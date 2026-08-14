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
#include <functional>
#include <memory>
#include <string>

namespace airplay2 {
namespace net {

/*!
 * @brief Info about a parsed SDP from ANNOUNCE
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
