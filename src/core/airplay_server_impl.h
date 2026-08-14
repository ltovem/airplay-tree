/*!
 * @file airplay_server_impl.h
 */
#ifndef AIRPLAY2_AIRPLAY_SERVER_IMPL_H
#define AIRPLAY2_AIRPLAY_SERVER_IMPL_H

#include "../include/airplay2/airplay_server.h"
#include "../include/airplay2/audio_renderer.h"
#include "../mdns/mdns_publisher.h"
#include "../net/rtsp_server.h"
#include "airplay_session_impl.h"
#include "airplay_pairing.h"
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>

namespace airplay2 {

class ServerImpl {
public:
    ServerImpl(const ServerConfig& cfg, ServerCallbacks cbs, IAudioRenderer* renderer);
    ~ServerImpl();

    Status start();
    void stop();
    bool is_running() const { return running_.load(); }

    std::vector<uint64_t> active_session_ids() const;
    AirPlaySession* get_session(uint64_t id);
    const ServerConfig& config() const { return cfg_; }
    void set_audio_renderer(IAudioRenderer* r) { renderer_ = r; }
    void set_callbacks(ServerCallbacks cbs) { cbs_ = std::move(cbs); }

    IAudioRenderer* renderer() { return renderer_; }
    ServerCallbacks& callbacks() { return cbs_; }

    SessionImpl* get_impl(uint64_t conn_id);

private:
    // RTSP handler glue
    bool on_announce(uint64_t conn_id, const net::SdpInfo& info, net::RtspAuthContext& auth);
    bool on_setup(uint64_t conn_id, int remote[3], int allocated_ports[3]);
    void on_record(uint64_t conn_id);
    void on_pause(uint64_t conn_id);
    void on_teardown(uint64_t conn_id, bool flush);
    std::string on_get_param(uint64_t conn_id, const std::string& params);
    void on_set_param(uint64_t conn_id, const std::string& params);
    std::vector<uint8_t> on_pair_setup(uint64_t conn_id, const uint8_t* body, size_t len);
    std::vector<uint8_t> on_pair_verify(uint64_t conn_id, const uint8_t* body, size_t len);
    std::vector<uint8_t> on_info(uint64_t conn_id);

    SessionImpl* ensure_session(uint64_t conn_id, const std::string& ip = "");
    void drop_session(uint64_t conn_id);

    ServerConfig cfg_;
    ServerCallbacks cbs_;
    IAudioRenderer* renderer_ = nullptr;
    std::atomic<bool> running_{false};

    MdnsPublisher mdns_;
    net::RtspServer rtsp_;
    AirPlayPairing pairing_;

    mutable std::mutex mu_;
    std::map<uint64_t, std::unique_ptr<SessionImpl>> sessions_;
    std::map<uint64_t, std::unique_ptr<AirPlaySession>> session_wrappers_;
};

} // namespace airplay2

#endif // AIRPLAY2_AIRPLAY_SERVER_IMPL_H
