/*!
 * @file airplay_server_impl.cpp
 */
#include "airplay_server_impl.h"
#include "../platform/platform_log.h"
#include <cstring>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <functional>

namespace airplay2 {

ServerImpl::ServerImpl(const ServerConfig& cfg, ServerCallbacks cbs, IAudioRenderer* renderer)
    : cfg_(cfg), cbs_(std::move(cbs)), renderer_(renderer) {
    platform::set_log_level(cfg_.enable_logging ? cfg_.log_level : -1);
    if (cbs_.on_log) {
        platform::set_log_callback([this](int lvl, const std::string& s) {
            if (cbs_.on_log) cbs_.on_log(lvl, s);
        });
    }
    if (!cfg_.device.pin_code.empty()) pairing_.set_pin(cfg_.device.pin_code);
    if (cbs_.on_pin_request) pairing_.set_pin_callback(cbs_.on_pin_request);
}

ServerImpl::~ServerImpl() { stop(); }

Status ServerImpl::start() {
    if (running_.exchange(true)) return Status::ERROR_ALREADY_RUNNING;

    // Initialize RTSP server with all handlers wired to this
    net::RtspHandlers h;
    h.on_announce  = [this](uint64_t conn, const net::SdpInfo& info, net::RtspAuthContext& a) {
        return on_announce(conn, info, a);
    };
    h.on_setup     = [this](uint64_t conn, int r[3], int a[3]) {
        return on_setup(conn, r, a);
    };
    h.on_record    = [this](uint64_t c) { on_record(c); };
    h.on_pause     = [this](uint64_t c) { on_pause(c); };
    h.on_teardown  = [this](uint64_t c, bool f) { on_teardown(c, f); };
    h.on_get_param = [this](uint64_t c, const std::string& s) { return on_get_param(c, s); };
    h.on_set_param = [this](uint64_t c, const std::string& s) { on_set_param(c, s); };
    h.on_pair_setup   = [this](uint64_t c, const uint8_t* b, size_t l) { return on_pair_setup(c, b, l); };
    h.on_pair_verify  = [this](uint64_t c, const uint8_t* b, size_t l) { return on_pair_verify(c, b, l); };
    h.on_info         = [this](uint64_t c) { return on_info(c); };

    if (!rtsp_.start(cfg_, std::move(h))) {
        running_.store(false);
        if (cbs_.on_error) cbs_.on_error(Status::ERROR_BIND_FAILED, "RTSP bind failed");
        return Status::ERROR_BIND_FAILED;
    }
    // Read back actual port (if user passed 0)
    platform::SocketAddr local;
    if (cfg_.control_port == 0) {
        // Update cfg with the actual port bound
        cfg_.control_port = cfg_.device.port = rtsp_.http().port();
    }

    if (cfg_.publish_mdns) {
        mdns_.start(cfg_.device, cfg_.bind_address);
    }
    if (cbs_.on_started) cbs_.on_started();
    AP2_LOGI("AirPlay server started: %s (port %u)", cfg_.device.name.c_str(), cfg_.control_port);
    return Status::OK;
}

void ServerImpl::stop() {
    if (!running_.exchange(false)) return;
    mdns_.stop();
    rtsp_.stop();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : sessions_) kv.second->disconnect();
        sessions_.clear();
        session_wrappers_.clear();
    }
    if (cbs_.on_stopped) cbs_.on_stopped();
}

std::vector<uint64_t> ServerImpl::active_session_ids() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<uint64_t> out; out.reserve(sessions_.size());
    for (auto& kv : sessions_) out.push_back(kv.first);
    return out;
}

AirPlaySession* ServerImpl::get_session(uint64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = session_wrappers_.find(id);
    return it == session_wrappers_.end() ? nullptr : it->second.get();
}

SessionImpl* ServerImpl::get_impl(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sessions_.find(conn_id);
    return it == sessions_.end() ? nullptr : it->second.get();
}

SessionImpl* ServerImpl::ensure_session(uint64_t conn_id, const std::string& ip) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sessions_.find(conn_id);
    if (it != sessions_.end()) return it->second.get();
    if (sessions_.size() >= cfg_.max_sessions) return nullptr;
    auto impl = std::make_unique<SessionImpl>(conn_id, this, renderer_);
    if (!ip.empty()) impl->set_client(ip, "");
    SessionImpl* raw = impl.get();
    sessions_[conn_id] = std::move(impl);
    // Public wrapper
    session_wrappers_[conn_id] = std::unique_ptr<AirPlaySession>(new AirPlaySession(raw));
    AP2_LOGI("server: session %lu created (total=%zu)", (unsigned long)conn_id, sessions_.size());
    if (cbs_.on_session_connected) cbs_.on_session_connected(*session_wrappers_[conn_id]);
    return raw;
}

void ServerImpl::drop_session(uint64_t conn_id) {
    std::unique_ptr<SessionImpl> impl;
    std::unique_ptr<AirPlaySession> wrap;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it1 = sessions_.find(conn_id);
        auto it2 = session_wrappers_.find(conn_id);
        if (it1 != sessions_.end()) impl = std::move(it1->second);
        if (it2 != session_wrappers_.end()) wrap = std::move(it2->second);
        sessions_.erase(conn_id);
        session_wrappers_.erase(conn_id);
    }
    if (impl) impl->disconnect();
    if (cbs_.on_session_disconnected) cbs_.on_session_disconnected(conn_id);
    AP2_LOGI("server: session %lu destroyed", (unsigned long)conn_id);
}

// ---- RTSP handlers ----

bool ServerImpl::on_announce(uint64_t conn_id, const net::SdpInfo& info, net::RtspAuthContext& auth) {
    auto* sess = ensure_session(conn_id, auth.client_ip);
    if (!sess) return false;
    sess->set_client(auth.client_ip, auth.client_user_agent);
    if (!pairing_.is_paired(auth.client_ip) && !cfg_.device.pin_code.empty()) {
        AP2_LOGW("server: session %lu rejected (unpaired client %s)",
                 (unsigned long)conn_id, auth.client_ip.c_str());
        return false;
    }
    sess->configure_audio(info);
    return true;
}

bool ServerImpl::on_setup(uint64_t conn_id, int remote[3], int allocated_ports[3]) {
    auto* sess = get_impl(conn_id);
    if (!sess) sess = ensure_session(conn_id, "");
    if (!sess) return false;
    (void)remote;
    return sess->allocate_ports(remote, cfg_.rtp_port_min, cfg_.rtp_port_max, allocated_ports);
}

void ServerImpl::on_record(uint64_t conn_id) {
    auto* sess = get_impl(conn_id);
    if (!sess) return;
    sess->start_streaming();
}

void ServerImpl::on_pause(uint64_t conn_id) {
    auto* sess = get_impl(conn_id);
    if (!sess) return;
    sess->pause_streaming();
}

void ServerImpl::on_teardown(uint64_t conn_id, bool flush) {
    auto* sess = get_impl(conn_id);
    if (!sess) return;
    if (flush) sess->flush_buffers();
    else sess->stop_streaming();
    if (!flush) {
        drop_session(conn_id);
    }
}

std::string ServerImpl::on_get_param(uint64_t conn_id, const std::string& params) {
    (void)conn_id;
    // clients often ask for "volume:" or "progress:"
    std::ostringstream oss;
    std::string lower = params;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
    if (lower.find("volume") != std::string::npos) {
        auto* s = get_impl(conn_id);
        float v = s ? s->get_volume() : 1.0f;
        oss << "volume: " << v << "\r\n";
    }
    return oss.str();
}

void ServerImpl::on_set_param(uint64_t conn_id, const std::string& params) {
    // Parse "volume: -10.000000" or "volume: 0.5" etc.
    auto* s = get_impl(conn_id);
    if (!s) return;
    size_t vpos = params.find("volume:");
    if (vpos != std::string::npos) {
        std::string val = params.substr(vpos + 7);
        try {
            float db = std::stof(val);
            // Convert dB to linear (-30dB..0 range)
            float linear = std::pow(10.0f, db / 20.0f);
            if (linear < 0) linear = 0; if (linear > 1) linear = 1;
            if (db < -100) linear = 0; // mute
            s->set_volume(linear);
            AP2_LOGD("volume: db=%.2f linear=%.3f", db, linear);
        } catch (...) {}
    }
}

std::vector<uint8_t> ServerImpl::on_pair_setup(uint64_t conn_id, const uint8_t* body, size_t len) {
    std::string ip;
    if (auto* s = get_impl(conn_id)) ip = s->client_address();
    bool need = false;
    auto r = pairing_.handle_pair_setup(ip, body, len, need);
    if (need && cbs_.on_log) cbs_.on_log(2, "pairing: client asked to pair");
    return r;
}

std::vector<uint8_t> ServerImpl::on_pair_verify(uint64_t conn_id, const uint8_t* body, size_t len) {
    std::string ip;
    if (auto* s = get_impl(conn_id)) ip = s->client_address();
    bool ok = false;
    auto r = pairing_.handle_pair_verify(ip, body, len, ok);
    if (ok) AP2_LOGI("pairing: client %s verified", ip.c_str());
    return r;
}

std::vector<uint8_t> ServerImpl::on_info(uint64_t conn_id) {
    (void)conn_id;
    // Use default plist from rtsp_server; could customize per-device features here
    return {};
}

// ============== Public API bindings (airplay2.cpp helper implementations) ==============

Status AirPlayServer::global_init() {
    if (!platform::Socket::global_init()) return Status::ERROR_NETWORK;
    return Status::OK;
}
void AirPlayServer::global_cleanup() {
    platform::Socket::global_cleanup();
}

// AirPlayServer public object forwards to impl
AirPlayServer::AirPlayServer(const ServerConfig& cfg, ServerCallbacks cbs, IAudioRenderer* renderer)
    : impl_(std::make_unique<ServerImpl>(cfg, std::move(cbs), renderer)) {}
AirPlayServer::~AirPlayServer() = default;

Status AirPlayServer::start()                          { return impl_->start(); }
void   AirPlayServer::stop()                           { impl_->stop(); }
bool   AirPlayServer::is_running() const               { return impl_->is_running(); }
std::vector<uint64_t> AirPlayServer::active_session_ids() const { return impl_->active_session_ids(); }
AirPlaySession* AirPlayServer::get_session(uint64_t id)         { return impl_->get_session(id); }
const ServerConfig& AirPlayServer::config() const               { return impl_->config(); }
void AirPlayServer::set_audio_renderer(IAudioRenderer* r)       { impl_->set_audio_renderer(r); }
void AirPlayServer::set_callbacks(ServerCallbacks cbs)          { impl_->set_callbacks(std::move(cbs)); }

// AirPlaySession public object
AirPlaySession::AirPlaySession(SessionImpl* impl) : impl_(impl), owns_impl_(false) {}
AirPlaySession::~AirPlaySession() {
    if (owns_impl_ && impl_) {
        delete impl_;
        impl_ = nullptr;
    }
}
uint64_t    AirPlaySession::id() const { return impl_ ? impl_->id() : 0; }
std::string AirPlaySession::client_address() const { return impl_ ? impl_->client_address() : ""; }
std::string AirPlaySession::client_name() const    { return impl_ ? impl_->client_name() : ""; }
AirPlaySession::State AirPlaySession::state() const { return impl_ ? impl_->state() : State::ERROR; }
SessionStats AirPlaySession::stats() const         { return impl_ ? impl_->stats() : SessionStats{}; }
AudioConfig  AirPlaySession::audio_config() const  { return impl_ ? impl_->audio_config() : AudioConfig{}; }
void         AirPlaySession::disconnect()          { if (impl_) impl_->disconnect(); }

} // namespace airplay2
