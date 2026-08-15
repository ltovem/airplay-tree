/*!
 * @file airplay_session_impl.h
 */
#ifndef AIRPLAY2_AIRPLAY_SESSION_IMPL_H
#define AIRPLAY2_AIRPLAY_SESSION_IMPL_H

#include "../include/airplay2/airplay_session.h"
#include "../include/airplay2/audio_renderer.h"
#include "../include/airplay2/video_renderer.h"
#include "../net/rtp_receiver.h"
#include "../net/rtsp_server.h"
#include "../net/video_rtp.h"
#include "../codec/alac_decoder.h"
#include "../codec/audio_buffer.h"
#include "../platform/platform_thread.h"
#include "fairplay.h"
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace airplay2 {

class ServerImpl;

class SessionImpl {
public:
    explicit SessionImpl(uint64_t id, ServerImpl* server,
                         IAudioRenderer* audio_renderer,
                         IVideoRenderer* video_renderer = nullptr);
    ~SessionImpl();

    uint64_t id() const { return id_; }
    AirPlaySession::State state() const { return state_.load(); }
    std::string client_address() const { return client_addr_; }
    std::string client_name()   const { return client_name_; }
    AudioConfig audio_config()  const { return audio_cfg_; }
    VideoConfig video_config()  const { return video_cfg_; }

    void set_client(const std::string& ip, const std::string& ua) {
        client_addr_ = ip; client_name_ = ua;
    }

    SessionStats stats() const;

    // ---- RTP / codec lifecycle ----
    /// Allocate audio RTP ports (data/ctrl/timing); fills ports[3]
    bool allocate_ports(int remote_ports[3], uint16_t port_min, uint16_t port_max,
                        int local_ports[3]);
    /// Allocate a video data port; returns 0 on fail
    uint16_t allocate_video_port(uint16_t port_min, uint16_t port_max,
                                  int remote_data_port = 0);

    /// Configure audio / video codec (after ANNOUNCE SDP parsing)
    void configure_audio(const net::SdpInfo& sdp);
    void configure_video(const net::SdpInfo& sdp);

    /// Start both streams (RECORD response)
    void start_streaming();
    /// Start only video stream (data push without audio)
    void start_video_streaming();

    /// Playback commands
    void pause_streaming();
    void resume_streaming();
    void stop_streaming();
    void flush_buffers();
    /// URL Pull 模式: 请求播放 URL
    bool play_url(const VideoPlaybackCmd& cmd);
    /// 速率控制
    void set_rate(double rate);
    /// Seek
    void seek(double pos_sec);

    void disconnect() { stop_streaming(); state_.store(AirPlaySession::State::CLOSED); }

    /// Volume / renderer swap
    void  set_volume(float v) { volume_.store(v); if (audio_renderer_) audio_renderer_->set_volume(v); }
    float get_volume() const { return volume_.load(); }
    void set_video_renderer(IVideoRenderer* r) { video_renderer_ = r; }
    void set_audio_renderer(IAudioRenderer* r) { audio_renderer_ = r; }

    /// 当前播放位置（秒），用于 /scrub GET
    double current_pos_sec() const { return current_pos_sec_.load(); }

    /// 是否已配置视频（SDP 中含 m=video）
    bool has_video() const { return video_local_port_ != 0 || video_cfg_.codec != VideoCodec::H264_AVC
                                      || video_cfg_.width != 0 || video_cfg_.height != 0; }

    /// FairPlay accessor
    FairPlaySap& fairplay() { return fp_; }

    // ---- AirPlay 2 SETUP 参数（RECORD 后解密 RTP 用）----
    /// 保存 AP2 SETUP 里携带的 FairPlay 加密 AES key(72B) 与 IV(16B)
    void set_ap2_keys(const std::vector<uint8_t>& ekey, const std::vector<uint8_t>& eiv) {
        ap2_ekey_ = ekey;
        ap2_eiv_ = eiv;
    }
    const std::vector<uint8_t>& ap2_ekey() const { return ap2_ekey_; }
    const std::vector<uint8_t>& ap2_eiv()  const { return ap2_eiv_; }
    /// 保存镜像/视频流的 streamConnectionID（视频 AES-CTR 密钥派生用）
    void set_stream_connection_id(uint64_t id) { stream_connection_id_ = id; }
    uint64_t stream_connection_id() const { return stream_connection_id_; }

    /// 派生媒体密钥（AP2）：fairplay_sap_decrypt(keymsg, ekey) → raw，
    /// 音频密钥 = SHA512(raw||ecdh_secret)[0:16]；
    /// 视频 key/iv = SHA512("AirPlayStreamKey/IV{id}"||audio_key)[0:16]。
    /// RECORD（start_streaming）前调用；成功后可配置 RTP 解密。
    void derive_media_keys();

private:
    void on_rtp_packet(const net::RtpAudioPacket& pkt);
    void on_video_frame(const VideoFrame& f);
    void playback_worker();
    void transition(AirPlaySession::State s) { state_.store(s); }

    uint64_t const id_;
    ServerImpl* server_ = nullptr;
    IAudioRenderer* audio_renderer_ = nullptr;
    IVideoRenderer* video_renderer_ = nullptr;
    std::atomic<AirPlaySession::State> state_{AirPlaySession::State::IDLE};

    std::string client_addr_;
    std::string client_name_;

    // Network: audio 3-port, video single data port
    net::RtpReceiver rtp_;
    int rtp_local_ports_[3] = {0,0,0};
    net::VideoRtpReceiver video_rtp_;
    uint16_t video_local_port_ = 0;

    // Audio pipeline
    codec::AlacDecoder alac_;
    codec::AudioBuffer pcm_buffer_;
    AudioConfig audio_cfg_;
    std::string codec_mode_;
    std::atomic<bool> playing_{false};
    std::atomic<bool> video_playing_{false};

    // Video pipeline
    VideoConfig video_cfg_;

    // Playback thread (audio)
    platform::Thread playback_thread_;
    std::atomic<bool> playback_stop_{false};

    // Volume
    std::atomic<float> volume_{1.0f};

    // Playback state
    std::atomic<double> playback_rate_{1.0};
    std::atomic<double> current_pos_sec_{0.0};
    std::mutex video_cmd_mu_;

    // FairPlay SAP
    FairPlaySap fp_;

    // ---- AirPlay 2 密钥材料（来自 SETUP bplist）----
    std::vector<uint8_t> ap2_ekey_;       ///< 72B FairPlay 加密 AES key
    std::vector<uint8_t> ap2_eiv_;        ///< 16B AES-CBC IV
    uint64_t stream_connection_id_ = 0;   ///< 镜像/视频流连接 ID
    uint8_t audio_key_[16] = {0};         ///< 最终音频 AES 密钥（CBC）
    uint8_t video_key_[16] = {0};         ///< 视频 AES-CTR key
    uint8_t video_iv_[16]  = {0};         ///< 视频 AES-CTR IV
    bool    media_keys_ready_ = false;    ///< 密钥派生是否成功

    // Stats
    mutable std::mutex stats_mu_;
    SessionStats stats_;
    uint64_t session_start_us_ = 0;
    uint32_t first_sample_rate_ = 0;
};

} // namespace airplay2

#endif // AIRPLAY2_AIRPLAY_SESSION_IMPL_H
