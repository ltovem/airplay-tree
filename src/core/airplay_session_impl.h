/*!
 * @file airplay_session_impl.h
 */
#ifndef AIRPLAY2_AIRPLAY_SESSION_IMPL_H
#define AIRPLAY2_AIRPLAY_SESSION_IMPL_H

#include "../include/airplay2/airplay_session.h"
#include "../include/airplay2/audio_renderer.h"
#include "../net/rtp_receiver.h"
#include "../net/rtsp_server.h"
#include "../codec/alac_decoder.h"
#include "../codec/audio_buffer.h"
#include "../platform/platform_thread.h"
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
                         IAudioRenderer* renderer);
    ~SessionImpl();

    uint64_t id() const { return id_; }
    AirPlaySession::State state() const { return state_.load(); }
    std::string client_address() const { return client_addr_; }
    std::string client_name()   const { return client_name_; }
    AudioConfig audio_config()  const { return audio_cfg_; }

    void set_client(const std::string& ip, const std::string& ua) {
        client_addr_ = ip; client_name_ = ua;
    }

    SessionStats stats() const;

    // ---- RTP / codec lifecycle ----
    /// Allocate RTP ports given range; fills ports[3] and returns true
    bool allocate_ports(int remote_ports[3], uint16_t port_min, uint16_t port_max,
                        int local_ports[3]);

    /// Configure audio codec (called after ANNOUNCE SDP parsing)
    void configure_audio(const net::SdpInfo& sdp);

    /// Start the RTP receiver + playback thread
    void start_streaming();

    /// Pause / resume / teardown
    void pause_streaming();
    void resume_streaming();
    void stop_streaming();
    void flush_buffers();

    void disconnect() { stop_streaming(); state_.store(AirPlaySession::State::CLOSED); }

    /// Volume control
    void  set_volume(float v) { volume_.store(v); if (renderer_) renderer_->set_volume(v); }
    float get_volume() const { return volume_.load(); }

private:
    void on_rtp_packet(const net::RtpAudioPacket& pkt);
    void playback_worker();
    void transition(AirPlaySession::State s) { state_.store(s); }

    uint64_t const id_;
    ServerImpl* server_ = nullptr;
    IAudioRenderer* renderer_ = nullptr;
    std::atomic<AirPlaySession::State> state_{AirPlaySession::State::IDLE};

    std::string client_addr_;
    std::string client_name_;

    // Network
    net::RtpReceiver rtp_;
    int rtp_local_ports_[3] = {0,0,0};

    // Audio pipeline
    codec::AlacDecoder alac_;
    codec::AudioBuffer pcm_buffer_;
    AudioConfig audio_cfg_;
    std::string codec_mode_;
    std::atomic<bool> playing_{false};

    // Playback thread
    platform::Thread playback_thread_;
    std::atomic<bool> playback_stop_{false};

    // Volume
    std::atomic<float> volume_{1.0f};

    // Stats
    mutable std::mutex stats_mu_;
    SessionStats stats_;
    uint64_t session_start_us_ = 0;
    uint32_t first_sample_rate_ = 0;
};

} // namespace airplay2

#endif // AIRPLAY2_AIRPLAY_SESSION_IMPL_H
