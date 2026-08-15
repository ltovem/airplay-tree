/*!
 * @file airplay_server_impl.cpp
 */
#include "airplay_server_impl.h"
#include "../platform/platform_log.h"
#include "../platform/platform_time.h"
#include "../util/plist.h"
#include <cstring>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <functional>
#include <cctype>     // std::tolower（设备名 / 参数名归一化）
#include <random>     // std::random_device（auth-setup 临时 X25519 种子）

namespace airplay2 {

ServerImpl::ServerImpl(const ServerConfig& cfg, ServerCallbacks cbs,
                       IAudioRenderer* audio_renderer, IVideoRenderer* video_renderer)
    : cfg_(cfg), cbs_(std::move(cbs)),
      audio_renderer_(audio_renderer), video_renderer_(video_renderer) {
    platform::set_log_level(cfg_.enable_logging ? cfg_.log_level : -1);
    if (cbs_.on_log) {
        platform::set_log_callback([this](int lvl, const std::string& s) {
            if (cbs_.on_log) cbs_.on_log(lvl, s);
        });
    }
    if (!cfg_.device.pin_code.empty()) {
        pairing_.set_pin(cfg_.device.pin_code);
        fairplay_.set_pin(cfg_.device.pin_code);
    }
    if (cbs_.on_pin_request) {
        pairing_.set_pin_callback(cbs_.on_pin_request);
    }
    // 加载持久 Ed25519 身份（pair-setup/pair-verify 用），并把公钥同步给
    // DeviceInfo：/info plist 与 mDNS TXT 的 pk= 都会读取该字段。
    pairing_.load_or_create_identity(cfg_.identity_key_path);
    cfg_.device.public_key_b64 = pairing_.public_key_b64();
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
    h.on_setup_ap2 = [this](uint64_t conn, const net::Ap2SetupRequest& req) {
        return on_setup_ap2(conn, req);
    };
    h.on_record    = [this](uint64_t c) { on_record(c); };
    h.on_pause     = [this](uint64_t c) { on_pause(c); };
    h.on_teardown  = [this](uint64_t c, bool f) { on_teardown(c, f); };
    h.on_get_param = [this](uint64_t c, const std::string& s) { return on_get_param(c, s); };
    h.on_set_param = [this](uint64_t c, const std::string& s) { on_set_param(c, s); };
    h.on_pair_setup   = [this](uint64_t c, const std::string& ip, const uint8_t* b, size_t l) { return on_pair_setup(c, ip, b, l); };
    h.on_pair_verify  = [this](uint64_t c, const std::string& ip, const uint8_t* b, size_t l) { return on_pair_verify(c, ip, b, l); };
    h.on_auth_setup   = [this](uint64_t c, const std::string& ip, const uint8_t* b, size_t l) { return on_auth_setup(c, ip, b, l); };
    h.on_fairplay_setup = [this](uint64_t c, const uint8_t* b, size_t l) {
        return on_fairplay_setup(c, b, l);
    };
    h.on_info         = [this](uint64_t c) { return on_info(c); };
    h.on_rate         = [this](uint64_t c, double r) { auto s = get_impl(c); if (s) s->set_rate(r); };
    h.on_play_url     = [this](uint64_t c, const util::PlistValue& d, const uint8_t* raw, size_t len) {
        auto s = get_impl(c); std::vector<uint8_t> out;
        if (s) on_play_url_to(s, d, raw, len);
        (void)raw; (void)len;
        return out;
    };
    h.on_stop     = [this](uint64_t c) { auto s = get_impl(c); if (s) s->stop_streaming(); };
    h.on_scrub    = [this](uint64_t c, double p) { auto s = get_impl(c); if (s) s->seek(p); };
    h.on_get_scrub_pos = [this](uint64_t c) -> double {
        auto s = get_impl(c); return s ? s->current_pos_sec() : 0.0;
    };
    h.on_photo_upload = [this](uint64_t c, const std::vector<uint8_t>& data, const std::string& ct) {
        auto s = get_impl(c); if (s) on_photo(s, data, ct);
    };

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
    auto impl = std::make_unique<SessionImpl>(conn_id, this, audio_renderer_, video_renderer_);
    if (!ip.empty()) impl->set_client(ip, "");
    // 把全局 fairplay 对象引用同步给会话（目前每个会话独立 fairplay 状态机）
    (void)fairplay_;
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
    // 清理该连接的配对握手状态（防止 verify_states_ 无限增长）
    pairing_.clear_verify_state(conn_id);
    // 清理 FairPlay keymsg
    {
        std::lock_guard<std::mutex> lk(fp_mu_);
        fp_keymsg_.erase(conn_id);
    }
    if (cbs_.on_session_disconnected) cbs_.on_session_disconnected(conn_id);
    AP2_LOGI("server: session %lu destroyed", (unsigned long)conn_id);
}

// ---- RTSP handlers ----

bool ServerImpl::on_announce(uint64_t conn_id, const net::SdpInfo& info, net::RtspAuthContext& auth) {
    auto* sess = ensure_session(conn_id, auth.client_ip);
    if (!sess) return false;
    sess->set_client(auth.client_ip, auth.client_user_agent);
    // FairPlay + legacy PIN 校验二选一
    bool fp_complete = sess->fairplay().is_complete();
    bool paired = pairing_.is_paired(auth.client_ip) || cfg_.device.pin_code.empty() || fp_complete;
    if (!paired) {
        AP2_LOGW("server: session %lu rejected (unpaired client %s)",
                 (unsigned long)conn_id, auth.client_ip.c_str());
        return false;
    }
    sess->configure_audio(info);
    if (info.has_video) sess->configure_video(info);
    return true;
}

bool ServerImpl::on_setup(uint64_t conn_id, int remote[3], int allocated_ports[3]) {
    auto* sess = get_impl(conn_id);
    if (!sess) sess = ensure_session(conn_id, "");
    if (!sess) return false;
    // 先分配音频 3 端口（data/rtcp/timing）
    bool ok = sess->allocate_ports(remote, cfg_.rtp_port_min, cfg_.rtp_port_max, allocated_ports);
    if (ok && sess->has_video()) {
        // 另外在池中再找一个视频 data 端口
        int video_remote = (remote && remote[0]) ? remote[0] + 2 : 0; // best-effort
        uint16_t vp = sess->allocate_video_port(cfg_.rtp_port_min, cfg_.rtp_port_max, video_remote);
        AP2_LOGI("server: session %lu video RTP port = %u", (unsigned long)conn_id, (unsigned)vp);
    }
    return ok;
}

net::Ap2SetupResponse ServerImpl::on_setup_ap2(uint64_t conn_id,
                                               const net::Ap2SetupRequest& req) {
    net::Ap2SetupResponse out;
    auto* sess = get_impl(conn_id);
    if (!sess) sess = ensure_session(conn_id, "");
    if (!sess) return out;

    // 保存 FairPlay 密钥材料与流信息，供 RECORD 后解密 RTP 用
    sess->set_ap2_keys(req.ekey, req.eiv);
    for (const auto& s : req.streams) {
        if (s.type == 110 || s.type == 98)
            sess->set_stream_connection_id(s.stream_connection_id);
    }
    // 分配（或复用已分配的）音频 3 端口 data/ctrl/timing。
    // 关键：iOS 镜像流程在 SETUP(110) 后校验 timingPort，必须给真实端口，
    // 0 会直接 TEARDOWN（音频流程碰巧在 SETUP 96 补上真实端口所以能过）。
    // 端口在首次 SETUP（空流+ekey，或首个带流 SETUP）分配，后续复用。
    int remote[3] = {0, 0, 0};
    int local[3] = {0, 0, 0};
    bool have_audio = false;
    for (const auto& s : req.streams) {
        if (s.type == 96) {
            have_audio = true;
            remote[1] = (int)s.control_port; // 客户端 RTCP 端口
            // AP2 音频没有 ANNOUNCE：codec 信息（ct/spf/sr）在 stream dict 里，
            // 这里必须配置好 ALAC/AAC，否则渲染器/解码器永远不初始化。
            sess->configure_ap2_audio(s.ct, s.spf, s.sr);
            break;
        }
    }
    if (have_audio || !req.ekey.empty() || !req.streams.empty()) {
        if (!sess->allocate_ports(remote, cfg_.rtp_port_min, cfg_.rtp_port_max, local)) {
            AP2_LOGW("server: AP2 SETUP port allocation failed");
            return out; // ok=false
        }
        out.timing_port = local[2];   // 真实 timing 端口（NTP）
        if (have_audio) {
            net::Ap2StreamResp sr;
            sr.type = 96;
            sr.data_port = local[0];
            sr.control_port = local[1];
            out.streams.push_back(sr);
        }
    }

    // 视频/mirroring 流：单 data 端口。
    // AirPlay 2 镜像视频走 TCP Data Push：iOS 会主动 connect 到该端口推流，
    // 所以必须绑 TCP listener（UDP 端口 iOS 连不上会立即 TEARDOWN）。
    for (const auto& s : req.streams) {
        if (s.type == 110 || s.type == 98) {
            // streamConnectionID 只在 SETUP(110) 里携带，而 RECORD 可能已过：
            // 拿到真 ID 后重新派生视频解密密钥（RECORD 时派生的是 ID=0 的错 key）。
            sess->set_stream_connection_id(s.stream_connection_id);
            sess->update_video_media_key();
            uint16_t vp = sess->allocate_video_port(cfg_.rtp_port_min, cfg_.rtp_port_max,
                                                    (int)s.control_port, /*use_tcp=*/true);
            if (vp == 0) {
                AP2_LOGW("server: AP2 SETUP video port allocation failed");
                continue;
            }
            net::Ap2StreamResp sr;
            sr.type = s.type;
            sr.data_port = vp;
            out.streams.push_back(sr);
            AP2_LOGI("server: AP2 SETUP video type=%llu dataPort=%u (TCP push) streamConnectionID=%llu",
                     (unsigned long long)s.type, (unsigned)vp,
                     (unsigned long long)s.stream_connection_id);
            // RECORD 早于 SETUP(110) 时视频接收线程还没启动，这里补上
            if (sess->state() == AirPlaySession::State::PLAYING)
                sess->start_video_streaming();
        }
    }

    out.event_port = 0; // UxPlay 同款：不支持事件端口
    out.ok = true;
    return out;
}

// ---- 视频 / FairPlay / 照片 handler 实现 ----
void ServerImpl::on_play_url_to(SessionImpl* s, const util::PlistValue& dict,
                                const uint8_t* raw, size_t len) {
    (void)raw; (void)len;
    VideoPlaybackCmd cmd;
    cmd.type = VideoPlaybackCmd::PLAY;
    if (dict.is_dict()) {
        const auto& cl = dict.get("Content-Location");
        if (cl.is_string()) cmd.content_url = cl.as_string();
        if (cmd.content_url.empty()) {
            const auto& loc = dict.get("Location");
            if (loc.is_string()) cmd.content_url = loc.as_string();
        }
        const auto& sp = dict.get("Start-Position");
        if (sp.is_real()) cmd.start_pos_sec = sp.as_real();
        else if (sp.is_int()) cmd.start_pos_sec = (double)sp.as_int();
        const auto& rate = dict.get("rate");
        if (rate.is_real()) cmd.rate = rate.as_real();
    }
    s->play_url(cmd);
}
void ServerImpl::on_photo(SessionImpl* s, const std::vector<uint8_t>& data, const std::string& ct) {
    (void)s; (void)ct;
    if (!video_renderer_) return;
    VideoFrame f;
    f.codec = VideoCodec::MJPEG;
    f.pts_us = platform::time_now_us();
    f.is_key = true;
    f.width = 0;
    f.height = 0;
    // 用 annex_b 字段承载 JPEG payload（对 MJPEG 不强制 start_code）
    f.annex_b = data;
    video_renderer_->on_frame(f);
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

std::vector<uint8_t> ServerImpl::on_pair_setup(uint64_t conn_id, const std::string& peer_ip,
                                               const uint8_t* body, size_t len) {
    auto* sess = ensure_session(conn_id, peer_ip);
    std::string ip = sess ? sess->client_address() : std::string();
    if (ip.empty()) ip = peer_ip;
    AP2_LOGD("pairing: /pair-setup from %s body=%zuB", ip.c_str(), len);

    // 顺序：先 legacy Ed25519 配对（iOS 非 HomeKit 设备用这个，body=32B 公钥），
    // 解析不了再试 FairPlay SAP（TLV 格式），两者都不认才回空 200。
    std::vector<uint8_t> resp;
    bool need = false;
    resp = pairing_.handle_pair_setup(conn_id, ip, body, len, need);
    if (!resp.empty()) {
        if (need && cbs_.on_log) cbs_.on_log(2, "pairing(legacy): client asked to pair");
        return resp;
    }

    if (sess) {
        bool need_pin = false;
        bool pin_ok = false;
        int rc = sess->fairplay().handle_pair_setup(body, len, resp, need_pin, pin_ok);
        if (rc == 0 && !resp.empty()) {
            if (need_pin && cbs_.on_log) cbs_.on_log(2, "fairplay: client asked PIN pairing");
            if (pin_ok) AP2_LOGI("fairplay: client %s PIN OK, pairing complete", ip.c_str());
            return resp;
        }
    }
    AP2_LOGD("pairing: /pair-setup unhandled (len=%zu) -> empty 200", len);
    return resp;
}

std::vector<uint8_t> ServerImpl::on_pair_verify(uint64_t conn_id, const std::string& peer_ip,
                                                const uint8_t* body, size_t len) {
    auto* s = get_impl(conn_id);
    std::string ip = s ? s->client_address() : std::string();
    if (ip.empty()) ip = peer_ip;
    // 打印 body 前 4 字节，便于判断 iOS 发的是 M1(0x01) 还是 M2(0x00) 签名步
    if (body && len > 0) {
        AP2_LOGD("pairing: /pair-verify from %s body=%zuB head=%02x %02x %02x %02x",
                 ip.c_str(), len, body[0], len > 1 ? body[1] : 0,
                 len > 2 ? body[2] : 0, len > 3 ? body[3] : 0);
    } else {
        AP2_LOGD("pairing: /pair-verify from %s body=0B", ip.c_str());
    }

    // legacy 优先：无 PIN 时直接空 200（UxPlay 行为，iOS 接受）
    std::vector<uint8_t> resp;
    bool ok = false;
    resp = pairing_.handle_pair_verify(conn_id, ip, body, len, ok);
    if (ok) {
        AP2_LOGI("pairing(legacy): client %s verified", ip.c_str());
        return resp;
    }
    if (!resp.empty()) return resp;  // PIN 失败等明确响应

    if (s) {
        int rc = s->fairplay().handle_pair_verify(body, len, resp);
        if (rc == 0 && !resp.empty()) {
            if (s->fairplay().is_complete())
                AP2_LOGI("fairplay: client %s verify complete", ip.c_str());
            return resp;
        }
    }
    return resp;
}

std::vector<uint8_t> ServerImpl::on_auth_setup(uint64_t conn_id, const std::string& peer_ip,
                                               const uint8_t* body, size_t len) {
    (void)conn_id;
    (void)peer_ip;
    // AirPlay 2 视频镜像的媒体加密协商（MFi auth-setup）：
    //   请求 = <1: 加密类型(0x01=不加密)> + <32: 客户端 X25519 公钥>
    //   响应 = <32: 服务端 X25519 公钥> + <4: 证书长度> + <证书> + <4: 签名长度> + <签名>
    // 开源接收器没有 Apple MFi 证书，按 airplay2-rs 的做法回"空证书 + 空签名"，
    // 客户端（iOS）在 unencrypted 模式下接受该结构并继续。
    std::vector<uint8_t> out;
    if (!body || len < 33) {
        AP2_LOGW("pairing: /auth-setup too short (%zuB) from %s", len, peer_ip.c_str());
        return out;
    }
    AP2_LOGI("pairing: /auth-setup from %s len=%zu type=0x%02x", peer_ip.c_str(), len, body[0]);

    // 生成临时 X25519 密钥对，只把公钥发出去
    std::vector<uint8_t> seed(32), sk(32), pk(32);
    std::random_device rd;
    std::mt19937_64 gen(rd() ^ (uint64_t)platform::time_now_us());
    for (size_t i = 0; i < 32; i += 8) {
        uint64_t v = gen();
        std::memcpy(seed.data() + i, &v, std::min<size_t>(8, 32 - i));
    }
    crypto::x25519_keygen(seed.data(), sk.data(), pk.data());
    (void)sk;
    out.insert(out.end(), pk.begin(), pk.end());
    // 证书长度 = 0
    out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(0);
    // 签名长度 = 0
    out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(0);
    return out;  // 40B：pk(32) + cert_len(4) + sig_len(4)
}

std::vector<uint8_t> ServerImpl::on_fairplay_setup(uint64_t conn_id,
                                                   const uint8_t* body, size_t len) {
    std::vector<uint8_t> out = net::handle_fairplay_setup(body, len);
    if (!body || len < 12) return out;
    // seq==3（setup 消息 2）：保存 164B keymsg，供 RECORD 后解密媒体 AES 密钥
    // （fairplay_sap_decrypt(keymsg, ekey) → raw_aeskey）。
    if (body[6] == 3 && len >= 164) {
        std::lock_guard<std::mutex> lk(fp_mu_);
        fp_keymsg_[conn_id].assign(body, body + len);
        AP2_LOGI("fairplay: saved keymsg(%zuB) for conn %lu", len, (unsigned long)conn_id);
    }
    return out;
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
AirPlayServer::AirPlayServer(const ServerConfig& cfg, ServerCallbacks cbs,
                             IAudioRenderer* audio_renderer, IVideoRenderer* video_renderer)
    : impl_(std::make_unique<ServerImpl>(cfg, std::move(cbs), audio_renderer, video_renderer)) {}
AirPlayServer::~AirPlayServer() = default;

Status AirPlayServer::start()                          { return impl_->start(); }
void   AirPlayServer::stop()                           { impl_->stop(); }
bool   AirPlayServer::is_running() const               { return impl_->is_running(); }
std::vector<uint64_t> AirPlayServer::active_session_ids() const { return impl_->active_session_ids(); }
AirPlaySession* AirPlayServer::get_session(uint64_t id)         { return impl_->get_session(id); }
const ServerConfig& AirPlayServer::config() const               { return impl_->config(); }
void AirPlayServer::set_audio_renderer(IAudioRenderer* r)       { impl_->set_audio_renderer(r); }
void AirPlayServer::set_video_renderer(IVideoRenderer* r)       { impl_->set_video_renderer(r); }
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
