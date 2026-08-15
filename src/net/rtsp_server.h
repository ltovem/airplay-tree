/*!
 * @file rtsp_server.h
 * @brief High-level AirPlay RTSP server: wraps HttpServer with
 *        routing for AirPlay-specific methods (ANNOUNCE, SETUP,
 *        RECORD, PAUSE, TEARDOWN, FLUSH, OPTIONS, GET_PARAMETER)
 *        and dispatches them to session handlers.
 */
#ifndef AIRPLAY2_RTSP_SERVER_H
#define AIRPLAY2_RTSP_SERVER_H

#include "http_server.h"
#include "../include/airplay2/airplay_config.h"
#include "../include/airplay2/video_renderer.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

// util::PlistValue 前向声明，避免 include plist.h 造成反向依赖
namespace airplay2 { namespace util { class PlistValue; } }

namespace airplay2 {
namespace net {

/*!
 * @brief Info about a parsed SDP from ANNOUNCE
 *
 * AirPlay 2 ANNOUNCE 里通常会包含多个 m= 行：
 *   m=audio 0 RTP/AVP 96 — 音频（ALAC/AAC）
 *   m=video 0 RTP/AVP 97 — 视频（H.264/H.265），屏幕镜像或投影片
 */
struct SdpInfo {
    std::string session_id;
    int         audio_pt = 96;       ///< RTP payload type for audio
    int         control_port = 0;    ///< Local RTSP port echoed
    int         server_port_min = 0;
    int         server_port_max = 0;
    int         timing_port = 0;
    std::string fmtp;                ///< fmtp config (ALAC magic cookie etc.)
    std::string audio_mode;          ///< "ALAC", "AAC", "PCM"
    int         sample_rate = 44100;
    int         channels = 2;
    uint64_t    rtp_time_base = 0;   ///< from a=rtpmap or a=ts-clk
    std::string aes_key;             ///< hex-encoded AES key (if encrypted)
    std::string aes_iv;              ///< hex-encoded AES IV
    std::string source_ip;           ///< client source IP for RTP (if offered)

    // ---- 视频 ----
    bool        has_video = false;
    int         video_pt = 97;       ///< H.264=96 或 H.265=98 由 SDP 决定
    int         video_clock = 90000; ///< 视频固定 90 kHz
    VideoCodec  video_codec = VideoCodec::H264_AVC;
    std::string video_fmtp;          ///< H.264 profile-level-id / sprop-parameter-sets
    int         video_width = 0;
    int         video_height = 0;
    int         video_fps = 0;
};

/// Parse SDP text into SdpInfo
bool parse_sdp(const std::string& sdp, SdpInfo& out);

/*!
 * @brief Pairing / Pin challenge context
 */
struct RtspAuthContext {
    bool        authenticated = false;
    std::string session_key;   ///< session id
    std::string client_user_agent;
    std::string client_ip;
};

/// Callback types
struct RtspHandlers {
    /// Called when client announces new stream; return true to accept
    std::function<bool(uint64_t conn_id, const SdpInfo& info, RtspAuthContext& auth)> on_announce;

    /// Called when client requests SETUP. Allocate 3 UDP ports (data, control, timing)
    /// and fill allocated_ports (size=3). Return true to accept.
    std::function<bool(uint64_t conn_id, int remote[3], int allocated_ports[3])> on_setup;

    /// Called on RECORD (playback start)
    std::function<void(uint64_t conn_id)> on_record;

    /// Called on PAUSE
    std::function<void(uint64_t conn_id)> on_pause;

    /// Called on TEARDOWN / FLUSH
    std::function<void(uint64_t conn_id, bool flush)> on_teardown;

    /// GET_PARAMETER / SET_PARAMETER for volume etc. Returns response body.
    std::function<std::string(uint64_t conn_id, const std::string& params)> on_get_param;
    std::function<void(uint64_t conn_id, const std::string& params)> on_set_param;

    /// Pair-setup: return plist body for response (airplay-specific)
    std::function<std::vector<uint8_t>(uint64_t conn_id, const uint8_t* body, size_t len)> on_pair_setup;
    std::function<std::vector<uint8_t>(uint64_t conn_id, const uint8_t* body, size_t len)> on_pair_verify;

    /// Info endpoint response body
    std::function<std::vector<uint8_t>(uint64_t conn_id)> on_info;

    /// Pin code callback: should we accept this code?
    std::function<bool(const std::string& client_ip, const std::string& pin)> on_pin;

    // --- AirPlay 2 控制端点 ---
    // POST /action — 带 binary plist 播放控制（play / pause / seek / volume /
    //                setOutput / removeOutput 等 AirPlay 2 多房间控制）
    //   第一个参数：已解析的 plist（dict）；未解析时 dict 为空
    //   第二个参数：原始 body 字节（方便用户自行扩展解析）
    //   返回：要回送的 response body（binary or xml plist；空则默认 200 OK + 空 body）
    std::function<std::vector<uint8_t>(uint64_t conn_id, const util::PlistValue& dict,
                                       const uint8_t* raw, size_t raw_len)> on_action;

    // PUT /rate — AirPlay 2 播放速度控制；header 带 value 参数
    //   on_rate(conn_id, rate)，rate=1.0 为正常播放，0.0 为暂停
    std::function<void(uint64_t conn_id, double rate)> on_rate;

    // POST /feedback — Apple 私有 TCP 反馈通道，接收端可以空响应 200
    std::function<std::vector<uint8_t>(uint64_t conn_id, const uint8_t* body, size_t len)> on_feedback;

    // POST /event — 发送端发事件（volume changed / nowPlaying 等）
    std::function<void(uint64_t conn_id, const util::PlistValue& dict,
                       const uint8_t* raw, size_t raw_len)> on_event;

    // PUT /metadata / POST /metadata — 正在播放的曲目/封面信息（通常是 binary plist）
    std::function<void(uint64_t conn_id, const util::PlistValue& dict,
                       const uint8_t* raw, size_t raw_len)> on_metadata;

    // ---- 视频 / URL 拉流 ----
    // POST /play — AirPlay 视频/HLS 播放请求：body 是 binary plist，
    //   带 Content-Location (URL) / Start-Position (0.0~1.0) / ...
    //   返回值：可选 response body（空即可）
    std::function<std::vector<uint8_t>(uint64_t conn_id, const util::PlistValue& dict,
                                       const uint8_t* raw, size_t raw_len)> on_play_url;

    // POST /stop — 停止 URL 拉流播放
    std::function<void(uint64_t conn_id)> on_stop;

    // POST /scrub — seek，header: position=<seconds>
    std::function<void(uint64_t conn_id, double pos_sec)> on_scrub;

    // GET  /scrub — 返回当前播放时间 "position: %.3f\n"
    std::function<double(uint64_t conn_id)> on_get_scrub_pos;

    // PUT /reverse — 控制镜像反向（可选）
    std::function<void(uint64_t conn_id, int mode)> on_reverse;

    // PUT /setProperty + GET /getProperty — 属性读写（volume / etc.）
    std::function<void(uint64_t conn_id, const std::string& name,
                       const std::vector<uint8_t>& value)> on_set_property;
    std::function<std::vector<uint8_t>(uint64_t conn_id,
                                       const std::string& name)> on_get_property;

    // PUT /photo — 上传照片（照片投射），body 为 JPEG/PNG 二进制
    //   第 2 参数为 Content-Type 头原始字符串
    std::function<void(uint64_t conn_id, const std::vector<uint8_t>& jpeg_data,
                       const std::string& content_type)> on_photo_upload;

    /// Fallback: allow embedding /play, /scrub, /property endpoints
    std::function<HttpResponse(const HttpRequest&, Connection&)> on_unknown;
};

class RtspServer {
public:
    RtspServer();
    ~RtspServer();

    bool start(const ServerConfig& cfg, RtspHandlers handlers);
    void stop();

    HttpServer& http() { return http_; }

private:
    void install_routes(const DeviceInfo& dev);
    static std::string build_info_plist(const DeviceInfo& dev);

    HttpServer http_;
    RtspHandlers handlers_;
    ServerConfig cfg_;
    std::atomic<bool> running_{false};
};

} // namespace net
} // namespace airplay2

#endif // AIRPLAY2_RTSP_SERVER_H
