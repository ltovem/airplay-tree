/*!
 * @file rtsp_server.cpp
 */
#include "rtsp_server.h"
#include "../platform/platform_log.h"
#include <sstream>
#include <cstring>
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

bool parse_sdp(const std::string& sdp, SdpInfo& out) {
    auto lines = split_lines(sdp);
    for (auto& line : lines) {
        if (line.size() < 2 || line[1] != '=') continue;
        char type = line[0];
        std::string val = line.substr(2);
        switch (type) {
            case 'o': {
                // o=- <session_id> ...
                size_t p1 = val.find(' ');
                if (p1 != std::string::npos) {
                    size_t p2 = val.find(' ', p1 + 1);
                    if (p2 == std::string::npos) p2 = val.size();
                    out.session_id = val.substr(p1 + 1, p2 - (p1 + 1));
                }
                break;
            }
            case 'm': {
                // m=audio <port_min>-<port_max> RTP/AVP 96
                if (val.compare(0, 5, "audio") == 0) {
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
                        // Look for payload type
                        if (p2 != std::string::npos) {
                            size_t p3 = val.rfind(' ');
                            if (p3 != std::string::npos && p3 > p2) {
                                try { out.audio_pt = std::stoi(val.substr(p3 + 1)); } catch (...) {}
                            }
                        }
                    }
                }
                break;
            }
            case 'a': {
                size_t eq = val.find(':');
                std::string key = (eq == std::string::npos) ? val : val.substr(0, eq);
                std::string vv  = (eq == std::string::npos) ? ""  : val.substr(eq + 1);
                if (key == "rtpmap") {
                    // a=rtpmap:96 AppleLossless/44100/2
                    size_t sp = vv.find(' ');
                    if (sp != std::string::npos) {
                        std::string codec_def = vv.substr(sp + 1);
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
                } else if (key == "fmtp") {
                    out.fmtp = vv;
                    // fmtp:96 0 16 4096 10 14 2 255 0 0 44100 (ALAC magic cookie)
                } else if (key == "control") {
                    // a=control:rtsp://.../sessionid
                } else if (key == "ts-clk" || key == "ts-refclk") {
                    try { out.rtp_time_base = std::stoull(vv); } catch (...) {}
                } else if (key == "es-charset" || key == "es-parameters") {
                    // AES key material
                }
                break;
            }
            case 'c': {
                // c=IN IP4 192.168.1.5
                if (val.compare(0, 7, "IN IP4 ") == 0) {
                    out.source_ip = val.substr(7);
                }
                break;
            }
        }
    }
    return !out.session_id.empty() || out.sample_rate > 0;
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
