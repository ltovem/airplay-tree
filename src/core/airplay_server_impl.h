/*!
 * @file airplay_server_impl.h
 */
#ifndef AIRPLAY2_AIRPLAY_SERVER_IMPL_H
#define AIRPLAY2_AIRPLAY_SERVER_IMPL_H

#include "../include/airplay2/airplay_server.h"
#include "../include/airplay2/audio_renderer.h"
#include "../include/airplay2/video_renderer.h"
#include "../mdns/mdns_publisher.h"
#include "../net/rtsp_server.h"
#include "airplay_session_impl.h"
#include "airplay_pairing.h"
#include "fairplay.h"
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>

namespace airplay2 {

class ServerImpl {
public:
    ServerImpl(const ServerConfig& cfg, ServerCallbacks cbs,
               IAudioRenderer* audio_renderer, IVideoRenderer* video_renderer);
    ~ServerImpl();

    Status start();
    void stop();
    bool is_running() const { return running_.load(); }

    std::vector<uint64_t> active_session_ids() const;
    AirPlaySession* get_session(uint64_t id);
    const ServerConfig& config() const { return cfg_; }
    void set_audio_renderer(IAudioRenderer* r) {
        std::lock_guard<std::mutex> lk(mu_);
        audio_renderer_ = r;
        for (auto& kv : sessions_) if (kv.second) kv.second->set_audio_renderer(r);
    }
    void set_video_renderer(IVideoRenderer* r) {
        std::lock_guard<std::mutex> lk(mu_);
        video_renderer_ = r;
        for (auto& kv : sessions_) if (kv.second) kv.second->set_video_renderer(r);
    }
    void set_callbacks(ServerCallbacks cbs) { cbs_ = std::move(cbs); }

    IAudioRenderer* audio_renderer() { return audio_renderer_; }
    IVideoRenderer* video_renderer() { return video_renderer_; }
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

    // 视频 / 照片 回调
    void on_play_url_to(SessionImpl* s, const util::PlistValue& dict,
                        const uint8_t* raw, size_t len);
    void on_photo(SessionImpl* s, const std::vector<uint8_t>& data, const std::string& ct);

    SessionImpl* ensure_session(uint64_t conn_id, const std::string& ip = "");
    void drop_session(uint64_t conn_id);

    ServerConfig cfg_;
    ServerCallbacks cbs_;
    IAudioRenderer* audio_renderer_ = nullptr;
    IVideoRenderer* video_renderer_ = nullptr;
    std::atomic<bool> running_{false};

    MdnsPublisher mdns_;
    net::RtspServer rtsp_;
    AirPlayPairing pairing_;
    FairPlaySap  fairplay_;    ///< 全局"参考"FairPlay 实例（配置了默认 PIN 等）；
                               ///< 真正的握手按会话在 SessionImpl::fairplay() 内

    mutable std::mutex mu_;
    std::map<uint64_t, std::unique_ptr<SessionImpl>> sessions_;
    std::map<uint64_t, std::unique_ptr<AirPlaySession>> session_wrappers_;
};

} // namespace airplay2

#endif // AIRPLAY2_AIRPLAY_SERVER_IMPL_H
