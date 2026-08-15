/*!
 * @file rtsp_server.cpp
 */
#include "rtsp_server.h"
#include "../platform/platform_log.h"
#include "../util/plist.h"
#include <sstream>
#include <cstring>
#include <cstdio>     // std::snprintf（构建 RTSP/SDP 响应行）
#include <cstdlib>
#include <algorithm>

namespace airplay2 {
namespace net {

// ---- SDP parsing ----
static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\n') {
            std::string line = s.substr(start, i - start);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (!line.empty()) out.push_back(line);
            start = i + 1;
        }
    }
    return out;
}
/// 按 sep 切分字符串，兼容 fmtp / plist URL kv 列表
static std::vector<std::string> split_lines_and(const std::string& s, char sep = ';') {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            std::string p = s.substr(start, i - start);
            while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
            while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.pop_back();
            if (!p.empty()) out.push_back(p);
            start = i + 1;
        }
    }
    return out;
}

/*!
 * @brief 解析 AirPlay 2 SDP（支持 m=audio 和 m=video 双媒体行）
 *
 * AirPlay SDP 一般格式：
 *   v=0
 *   o=AppleID ...
 *   s=...
 *   c=IN IP4 0.0.0.0
 *   t=0 0
 *   m=audio 49152 RTP/AVP 96
 *   a=rtpmap:96 AppleLossless/44100/2
 *   a=fmtp:96 ...
 *   m=video 49170 RTP/AVP 97
 *   a=rtpmap:97 H264/90000
 *   a=fmtp:97 profile-level-id=42e01f;sprop-parameter-sets=...
 *   ...
 */
bool parse_sdp(const std::string& sdp, SdpInfo& out) {
    auto lines = split_lines(sdp);
    std::string current_media = "";   // "audio" / "video" / ""
    int current_pt_audio = 96;
    int current_pt_video = 97;
    for (auto& line : lines) {
        if (line.size() < 2 || line[1] != '=') continue;
        char type = line[0];
        std::string val = line.substr(2);
        switch (type) {
            case 'o': {
                size_t p1 = val.find(' ');
                if (p1 != std::string::npos) {
                    size_t p2 = val.find(' ', p1 + 1);
                    if (p2 == std::string::npos) p2 = val.size();
                    out.session_id = val.substr(p1 + 1, p2 - (p1 + 1));
                }
                break;
            }
            case 'm': {
                if (val.compare(0, 5, "audio") == 0) {
                    current_media = "audio";
                    size_t p1 = val.find(' ');
                    if (p1 != std::string::npos) {
                        size_t p2 = val.find(' ', p1 + 1);
                        std::string ports = val.substr(p1 + 1, (p2 == std::string::npos ? std::string::npos : p2 - (p1 + 1)));
                        size_t dash = ports.find('-');
                        if (dash != std::string::npos) {
                            try {
                                out.server_port_min = std::stoi(ports.substr(0, dash));
                                out.server_port_max = std::stoi(ports.substr(dash + 1));
                            } catch (...) {}
                        } else {
                            try { out.server_port_min = out.server_port_max = std::stoi(ports); } catch (...) {}
                        }
                        if (p2 != std::string::npos) {
                            size_t p3 = val.rfind(' ');
                            if (p3 != std::string::npos && p3 > p2) {
                                try { out.audio_pt = current_pt_audio = std::stoi(val.substr(p3 + 1)); } catch (...) {}
                            }
                        }
                    }
                } else if (val.compare(0, 5, "video") == 0) {
                    current_media = "video";
                    out.has_video = true;
                    size_t p3 = val.rfind(' ');
                    if (p3 != std::string::npos) {
                        try { out.video_pt = current_pt_video = std::stoi(val.substr(p3 + 1)); } catch (...) {}
                    }
                } else {
                    current_media = "";
                }
                break;
            }
            case 'a': {
                size_t eq = val.find(':');
                std::string key = (eq == std::string::npos) ? val : val.substr(0, eq);
                std::string vv  = (eq == std::string::npos) ? ""  : val.substr(eq + 1);
                std::string base_key = key;
                size_t col = base_key.find(':');
                int pt_filter = -1;       // 若 rtpmap:97 / fmtp:96 带了 PT，则做过滤
                if (col != std::string::npos) {
                    std::string pt_s = base_key.substr(col + 1);
                    base_key = base_key.substr(0, col);
                    try { pt_filter = std::stoi(pt_s); } catch (...) {}
                }
                if (base_key == "rtpmap") {
                    // 按 PT 判断归属到音频或视频
                    size_t sp = vv.find(' ');
                    if (sp == std::string::npos) break;
                    std::string codec_def = vv.substr(sp + 1);
                    bool is_video = (pt_filter == current_pt_video)
                                 || (pt_filter == -1 && current_media == "video");
                    if (is_video) {
                        size_t s1 = codec_def.find('/');
                        if (s1 != std::string::npos) {
                            std::string codec_name = codec_def.substr(0, s1);
                            std::string lower_name; lower_name.resize(codec_name.size());
                            std::transform(codec_name.begin(), codec_name.end(), lower_name.begin(), ::tolower);
                            if (lower_name.find("h265") != std::string::npos || lower_name.find("hevc") != std::string::npos) {
                                out.video_codec = VideoCodec::H265_HEVC;
                            } else if (lower_name.find("jpeg") != std::string::npos) {
                                out.video_codec = VideoCodec::MJPEG;
                            } else {
                                out.video_codec = VideoCodec::H264_AVC;
                            }
                            size_t s2 = codec_def.find('/', s1 + 1);
                            try {
                                out.video_clock = std::stoi(codec_def.substr(s1 + 1,
                                    (s2 == std::string::npos ? std::string::npos : s2 - (s1 + 1))));
                            } catch (...) {}
                        }
                    } else {
                        size_t s1 = codec_def.find('/');
                        if (s1 != std::string::npos) {
                            out.audio_mode = codec_def.substr(0, s1);
                            size_t s2 = codec_def.find('/', s1 + 1);
                            try { out.sample_rate = std::stoi(codec_def.substr(s1 + 1, s2 - (s1 + 1))); } catch (...) {}
                            if (s2 != std::string::npos) {
                                try { out.channels = std::stoi(codec_def.substr(s2 + 1)); } catch (...) {}
                            }
                        }
                    }
                } else if (base_key == "fmtp") {
                    bool to_video = (pt_filter == current_pt_video)
                                 || (pt_filter == -1 && current_media == "video");
                    if (to_video) {
                        out.video_fmtp = vv;
                        // 简单提取 width / height (x-dim-width / framesize)
                        for (auto kv : split_lines_and(vv, ';')) {
                            if (kv.find("framesize:") != std::string::npos) {
                                // framesize:97 1920-1080
                                size_t sp0 = kv.find(' ');
                                if (sp0 != std::string::npos) {
                                    std::string dim = kv.substr(sp0 + 1);
                                    size_t dash0 = dim.find('-');
                                    if (dash0 != std::string::npos) {
                                        try {
                                            out.video_width = std::stoi(dim.substr(0, dash0));
                                            out.video_height = std::stoi(dim.substr(dash0 + 1));
                                        } catch (...) {}
                                    }
                                }
                            } else if (kv.find("x-framerate:") != std::string::npos) {
                                size_t e = kv.find(':');
                                if (e != std::string::npos) {
                                    try { out.video_fps = std::stoi(kv.substr(e + 1)); } catch (...) {}
                                }
                            }
                        }
                    } else {
                        out.fmtp = vv;
                    }
                } else if (base_key == "control") {
                    // a=control:rtsp://.../sessionid
                } else if (base_key == "ts-clk" || base_key == "ts-refclk") {
                    try { out.rtp_time_base = std::stoull(vv); } catch (...) {}
                } else if (base_key == "aeskey") {
                    out.aes_key = vv;
                } else if (base_key == "aesiv") {
                    out.aes_iv = vv;
                } else if (base_key == "es-charset" || base_key == "es-parameters") {
                    if (out.aes_key.empty()) {
                        size_t p = vv.find("aeskey=");
                        if (p != std::string::npos) {
                            size_t s = p + 7;
                            size_t e = vv.find(';', s);
                            out.aes_key = vv.substr(s, (e == std::string::npos ? std::string::npos : e - s));
                        }
                    }
                    if (out.aes_iv.empty()) {
                        size_t p = vv.find("aesiv=");
                        if (p != std::string::npos) {
                            size_t s = p + 6;
                            size_t e = vv.find(';', s);
                            out.aes_iv = vv.substr(s, (e == std::string::npos ? std::string::npos : e - s));
                        }
                    }
                } else if (base_key == "range") {
                    // 存在时意味着 sender 支持 HLS URL 拉流（airplay play 接口）
                }
                break;
            }
            case 'c': {
                if (val.compare(0, 7, "IN IP4 ") == 0) {
                    out.source_ip = val.substr(7);
                }
                break;
            }
        }
    }
    return !out.session_id.empty() || out.sample_rate > 0 || out.has_video;
}

// ---- Minimal plist helpers for /info (XML-style, not binary) ----
static std::string xml_escape(const std::string& s) {
    std::string out; out.reserve(s.size() + 10);
    for (char c : s) {
        switch (c) {
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '&':  out += "&amp;"; break;
            case '"':  out += "&quot;"; break;
            default:   out += c; break;
        }
    }
    return out;
}

std::string RtspServer::build_info_plist(const DeviceInfo& dev) {
    char features[32];
    std::snprintf(features, sizeof(features), "0x%08X", dev.features);
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    oss << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    oss << "<plist version=\"1.0\"><dict>\n";
    auto kv = [&](const char* k, const std::string& v) {
        oss << "<key>" << k << "</key><string>" << xml_escape(v) << "</string>\n";
    };
    auto kvi = [&](const char* k, int v) {
        oss << "<key>" << k << "</key><integer>" << v << "</integer>\n";
    };
    kv("deviceid", dev.device_id);
    kv("name", dev.name);
    kv("model", dev.model);
    kv("features", features);
    kv("manufacturer", dev.manufacturer);
    if (!dev.serial_number.empty()) kv("serialNumber", dev.serial_number);
    kv("protovers", "1.1");
    kv("srcvers", "605.30.1");
    kv("vv", "2"); // AirPlay 2
    kv("os", "13.4.1");
    kvi("pw", dev.requires_encryption ? 1 : 0);
    kvi("pk", 0);
    kvi("acl", 0);
    oss << "</dict></plist>\n";
    return oss.str();
}

// ---- RtspServer ----
RtspServer::RtspServer() = default;
RtspServer::~RtspServer() { stop(); }

bool RtspServer::start(const ServerConfig& cfg, RtspHandlers handlers) {
    stop();
    cfg_ = cfg;
    handlers_ = std::move(handlers);
    install_routes(cfg.device);
    running_.store(http_.start(cfg.bind_address, cfg.control_port));
    return running_.load();
}

void RtspServer::stop() {
    running_.store(false);
    http_.stop();
}

void RtspServer::install_routes(const DeviceInfo& dev) {
    // /info - GET
    http_.add_route("GET", "/info", [this, dev](const HttpRequest& req, Connection& c) -> HttpResponse {
        (void)dev;
        std::vector<uint8_t> body;
        if (handlers_.on_info) body = handlers_.on_info(c.id());
        if (body.empty()) {
            std::string s = build_info_plist(cfg_.device);
            body.assign(s.begin(), s.end());
        }
        return make_rtsp_ok(req.cseq(), {}, body, "text/x-apple-plist+xml");
    }, true);

    // /pair-setup, /pair-verify (POST)
    http_.add_route("POST", "/pair-setup", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        std::vector<uint8_t> resp_body;
        if (handlers_.on_pair_setup) resp_body = handlers_.on_pair_setup(c.id(), req.body.data(), req.body.size());
        HeaderMap extra;
        extra["Content-Type"] = "application/octet-stream";
        return make_rtsp_ok(req.cseq(), extra, std::move(resp_body));
    }, true);
    http_.add_route("POST", "/pair-verify", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        std::vector<uint8_t> resp_body;
        if (handlers_.on_pair_verify) resp_body = handlers_.on_pair_verify(c.id(), req.body.data(), req.body.size());
        HeaderMap extra;
        extra["Content-Type"] = "application/octet-stream";
        return make_rtsp_ok(req.cseq(), extra, std::move(resp_body));
    }, true);

    // OPTIONS
    http_.add_route("OPTIONS", "*", [](const HttpRequest& req, Connection&) -> HttpResponse {
        HeaderMap extra;
        extra["Public"] = "ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER, POST, GET";
        return make_rtsp_ok(req.cseq(), extra);
    }, true);
    http_.add_route("OPTIONS", "/", [](const HttpRequest& req, Connection&) -> HttpResponse {
        HeaderMap extra;
        extra["Public"] = "ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER, POST, GET";
        return make_rtsp_ok(req.cseq(), extra);
    }, true);

    // ANNOUNCE rtsp://.../ RTSP/1.0   (body = SDP)
    http_.add_route("ANNOUNCE", "", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        SdpInfo info{};
        std::string sdp(req.body.begin(), req.body.end());
        parse_sdp(sdp, info);
        bool ok = false;
        if (handlers_.on_announce) {
            RtspAuthContext auth;
            auth.client_ip = c.peer_ip();
            auth.client_user_agent = req.header("User-Agent");
            ok = handlers_.on_announce(c.id(), info, auth);
        }
        HeaderMap extra;
        extra["Audio-Jack-Status"] = "connected; type=analog";
        return make_rtsp_ok(req.cseq(), extra);
    }, false);

    // SETUP
    http_.add_route("SETUP", "", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        // Client tells us remote ports via Transport: header
        // Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;client_port=6002-6003
        int remote_ports[3] = {0,0,0};
        std::string tr = req.header("Transport");
        // Look for client_port=X-Y
        size_t pos = tr.find("client_port=");
        if (pos != std::string::npos) {
            size_t p = pos + 12;
            size_t e = tr.find_first_of(";\r\n", p);
            std::string pr = tr.substr(p, (e == std::string::npos ? std::string::npos : e - p));
            size_t dash = pr.find('-');
            if (dash != std::string::npos) {
                try {
                    remote_ports[0] = std::stoi(pr.substr(0, dash));
                    remote_ports[1] = std::stoi(pr.substr(dash + 1));
                } catch (...) {}
            }
        }
        // Timing port from AppleSession header or separate
        int local_ports[3] = {0,0,0};
        if (handlers_.on_setup) {
            handlers_.on_setup(c.id(), remote_ports, local_ports);
        }
        char tbuf[256];
        std::snprintf(tbuf, sizeof(tbuf),
            "RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;server_port=%d-%d;client_port=%d-%d",
            local_ports[0], local_ports[1], remote_ports[0], remote_ports[1]);
        HeaderMap extra;
        extra["Transport"] = tbuf;
        extra["Session"] = "airplay2lib-" + std::to_string(c.id());
        return make_rtsp_ok(req.cseq(), extra);
    }, false);

    // RECORD = start
    http_.add_route("RECORD", "", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        if (handlers_.on_record) handlers_.on_record(c.id());
        HeaderMap extra;
        extra["Session"] = "airplay2lib-" + std::to_string(c.id());
        extra["Audio-Latency"] = "2205"; // ~50ms at 44100
        return make_rtsp_ok(req.cseq(), extra);
    }, false);

    // PAUSE
    http_.add_route("PAUSE", "", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        if (handlers_.on_pause) handlers_.on_pause(c.id());
        HeaderMap extra;
        extra["Session"] = "airplay2lib-" + std::to_string(c.id());
        return make_rtsp_ok(req.cseq(), extra);
    }, false);

    // FLUSH
    http_.add_route("FLUSH", "", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        if (handlers_.on_teardown) handlers_.on_teardown(c.id(), true);
        HeaderMap extra;
        extra["Session"] = "airplay2lib-" + std::to_string(c.id());
        return make_rtsp_ok(req.cseq(), extra);
    }, false);

    // TEARDOWN
    http_.add_route("TEARDOWN", "", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        if (handlers_.on_teardown) handlers_.on_teardown(c.id(), false);
        return make_rtsp_ok(req.cseq(), {});
    }, false);

    // GET_PARAMETER, SET_PARAMETER (volume, etc.)
    http_.add_route("GET_PARAMETER", "", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        std::string body_in(req.body.begin(), req.body.end());
        std::string body_out;
        if (handlers_.on_get_param) body_out = handlers_.on_get_param(c.id(), body_in);
        HeaderMap extra;
        extra["Content-Type"] = "text/parameters";
        std::vector<uint8_t> b(body_out.begin(), body_out.end());
        return make_rtsp_ok(req.cseq(), extra, std::move(b), "text/parameters");
    }, false);

    http_.add_route("SET_PARAMETER", "", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        std::string body_in(req.body.begin(), req.body.end());
        if (handlers_.on_set_param) handlers_.on_set_param(c.id(), body_in);
        return make_rtsp_ok(req.cseq());
    }, false);

    // POST /action — AirPlay 2 播放控制（binary plist 主体）
    // 常见字段：category="playback" / command="play" / params={...}
    // AirPlay 2 多房间扩展：command="setOutput" / "removeOutput" / "groupJoin"
    http_.add_route("POST", "/action", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        util::PlistValue dict;
        if (!req.body.empty()) {
            util::parse_plist(req.body.data(), req.body.size(), dict);
        }
        std::vector<uint8_t> resp_body;
        if (handlers_.on_action) {
            resp_body = handlers_.on_action(c.id(), dict, req.body.data(), req.body.size());
        }
        HeaderMap extra;
        if (!resp_body.empty()) {
            // 默认返回二进制 plist；上层给空就用空 body 200
            extra["Content-Type"] = "application/x-apple-binary-plist";
        }
        return make_rtsp_ok(req.cseq(), extra, std::move(resp_body));
    }, true);

    // PUT /rate — 播放速度/时间轴请求
    // 典型请求：x-apple-rate: 1.0 / x-apple-post-sync-time: ...
    // 缺少 handler 时也必须返回 200（否则发送端可能认为设备"卡住"）
    http_.add_route("PUT", "/rate", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        double rate = 1.0;
        std::string r = req.header("x-apple-rate");
        if (r.empty()) r = req.header("Rate");
        if (!r.empty()) {
            try { rate = std::stod(r); } catch (...) { rate = 1.0; }
        }
        if (handlers_.on_rate) handlers_.on_rate(c.id(), rate);
        AP2_LOGD("rtsp: /rate conn=%lu rate=%.3f", (unsigned long)c.id(), rate);
        HeaderMap extra;
        extra["Session"] = "airplay2lib-" + std::to_string(c.id());
        return make_rtsp_ok(req.cseq(), extra);
    }, false);

    // POST /feedback — 某些 AirPlay 发送端周期发起 feedback 通道，
    // 内容是一个 binary plist，包含网络质量 / 抖动 / RTT 估计。
    // 接收端不需要解析就可以回 200，空 response 即可。
    http_.add_route("POST", "/feedback", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        std::vector<uint8_t> resp_body;
        if (handlers_.on_feedback) {
            resp_body = handlers_.on_feedback(c.id(), req.body.data(), req.body.size());
        }
        HeaderMap extra;
        extra["Content-Type"] = "application/x-apple-binary-plist";
        return make_rtsp_ok(req.cseq(), extra, std::move(resp_body));
    }, true);

    // POST /event — 播放事件（volume changed / nowPlaying / queue update 等）
    http_.add_route("POST", "/event", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        util::PlistValue dict;
        if (!req.body.empty()) {
            util::parse_plist(req.body.data(), req.body.size(), dict);
        }
        if (handlers_.on_event) {
            handlers_.on_event(c.id(), dict, req.body.data(), req.body.size());
        }
        return make_rtsp_ok(req.cseq());
    }, true);

    // PUT /metadata + POST /metadata — 正在播放的曲目元信息
    // 常见字段：artist / album / title / duration / artwork（二进制 data）
    http_.add_route("PUT", "/metadata", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        util::PlistValue dict;
        if (!req.body.empty()) {
            util::parse_plist(req.body.data(), req.body.size(), dict);
        }
        if (handlers_.on_metadata) {
            handlers_.on_metadata(c.id(), dict, req.body.data(), req.body.size());
        }
        return make_rtsp_ok(req.cseq());
    }, true);
    http_.add_route("POST", "/metadata", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        util::PlistValue dict;
        if (!req.body.empty()) {
            util::parse_plist(req.body.data(), req.body.size(), dict);
        }
        if (handlers_.on_metadata) {
            handlers_.on_metadata(c.id(), dict, req.body.data(), req.body.size());
        }
        return make_rtsp_ok(req.cseq());
    }, true);

    // ---- Video / URL playback ----
    // POST /play — 视频播放请求：body 是 binary plist，包含 Content-Location/Start-Position
    http_.add_route("POST", "/play", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        util::PlistValue dict;
        if (!req.body.empty()) util::parse_plist(req.body.data(), req.body.size(), dict);
        std::vector<uint8_t> resp_body;
        if (handlers_.on_play_url) {
            resp_body = handlers_.on_play_url(c.id(), dict, req.body.data(), req.body.size());
        }
        HeaderMap extra;
        extra["Session"] = "airplay2lib-" + std::to_string(c.id());
        if (!resp_body.empty())
            extra["Content-Type"] = "application/x-apple-binary-plist";
        return make_rtsp_ok(req.cseq(), extra, std::move(resp_body));
    }, false);

    // POST /stop — 停止 URL 拉流
    http_.add_route("POST", "/stop", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        if (handlers_.on_stop) handlers_.on_stop(c.id());
        return make_rtsp_ok(req.cseq());
    }, false);

    // POST /scrub — seek (header: position=<seconds>)
    http_.add_route("POST", "/scrub", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        std::string p = req.header("position");
        if (p.empty()) p = req.header("Position");
        double pos = 0.0;
        try { if (!p.empty()) pos = std::stod(p); } catch (...) {}
        if (handlers_.on_scrub) handlers_.on_scrub(c.id(), pos);
        return make_rtsp_ok(req.cseq());
    }, false);

    // GET /scrub — 返回当前播放位置
    http_.add_route("GET", "/scrub", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        double pos = 0.0;
        if (handlers_.on_get_scrub_pos) pos = handlers_.on_get_scrub_pos(c.id());
        char tbuf[64];
        std::snprintf(tbuf, sizeof(tbuf), "duration: 0.000\nposition: %.3f\n", pos);
        std::string body(tbuf);
        std::vector<uint8_t> b(body.begin(), body.end());
        HeaderMap extra; extra["Content-Type"] = "text/parameters";
        return make_rtsp_ok(req.cseq(), extra, std::move(b));
    }, false);

    // PUT /reverse — 镜像翻转控制（header value=0/1/2）
    http_.add_route("PUT", "/reverse", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        std::string v = req.header("value");
        int mode = 0;
        try { if (!v.empty()) mode = std::stoi(v); } catch (...) {}
        if (handlers_.on_reverse) handlers_.on_reverse(c.id(), mode);
        return make_rtsp_ok(req.cseq());
    }, false);

    // PUT /setProperty — body: name=value
    http_.add_route("PUT", "/setProperty", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        std::string name = req.header("if");
        if (name.empty()) name = req.header("If");
        std::vector<uint8_t> val = req.body;
        if (handlers_.on_set_property) handlers_.on_set_property(c.id(), name, val);
        return make_rtsp_ok(req.cseq());
    }, true);

    // GET /getProperty — 以 ?if=xxx 或 path?name=xxx
    http_.add_route("GET", "/getProperty", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        std::string name = req.header("if");
        if (name.empty()) name = req.header("If");
        // HttpRequest 没有单独的 query() 接口；uri 形如 /getProperty?if=volume
        const std::string& uri = req.uri;
        size_t qmark = uri.find('?');
        if (name.empty() && qmark != std::string::npos) {
            std::string q = uri.substr(qmark + 1);
            size_t eq = q.find("if=");
            size_t adv = 0;
            if (eq != std::string::npos) adv = 3;
            else { eq = q.find("name="); if (eq != std::string::npos) adv = 5; }
            if (eq != std::string::npos) name = q.substr(eq + adv);
        }
        std::vector<uint8_t> body;
        if (handlers_.on_get_property) body = handlers_.on_get_property(c.id(), name);
        HeaderMap extra; extra["Content-Type"] = "application/octet-stream";
        return make_rtsp_ok(req.cseq(), extra, std::move(body));
    }, true);

    // PUT /photo — 上传照片投射
    http_.add_route("PUT", "/photo", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        if (handlers_.on_photo_upload) {
            handlers_.on_photo_upload(c.id(), req.body, req.header("Content-Type"));
        }
        return make_rtsp_ok(req.cseq());
    }, false);
    // POST /photo — 某些发送端用 POST
    http_.add_route("POST", "/photo", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        if (handlers_.on_photo_upload) {
            handlers_.on_photo_upload(c.id(), req.body, req.header("Content-Type"));
        }
        return make_rtsp_ok(req.cseq());
    }, false);

    // Default handler
    http_.set_default_handler([this](const HttpRequest& req, Connection& c) -> HttpResponse {
        if (handlers_.on_unknown) return handlers_.on_unknown(req, c);
        HttpResponse r; r.code = 404; r.reason = "Not Found";
        r.headers["CSeq"] = std::to_string(req.cseq());
        r.headers["Server"] = "airplay2lib/1.0";
        return r;
    });
}

} // namespace net
} // namespace airplay2
