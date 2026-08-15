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
    // /info 响应键名与类型对齐 openairplay/airplay-spec GET /info 规范：
    //   features/vv/statusFlags 必须是 <integer>（十进制），不能是字符串；
    //   键名用 deviceID / macAddress 等驼峰写法（旧实现 deviceid= 字符串，
    //   features=字符串 已被 iOS 容忍但非规范，此处一并修正）。
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    oss << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    oss << "<plist version=\"1.0\"><dict>\n";
    auto kv = [&](const char* k, const std::string& v) {
        oss << "<key>" << k << "</key><string>" << xml_escape(v) << "</string>\n";
    };
    auto kvi = [&](const char* k, int64_t v) {
        oss << "<key>" << k << "</key><integer>" << v << "</integer>\n";
    };
    kv("deviceID", dev.device_id);
    kv("macAddress", dev.device_id);
    kv("name", dev.name);
    kv("model", dev.model);
    kvi("features", dev.features);  // integer 十进制（spec 要求，不能是 "0x.." 字符串）
    kv("manufacturer", dev.manufacturer);
    if (!dev.serial_number.empty()) kv("serialNumber", dev.serial_number);
    kv("protocolVersion", "1.1");
    kv("srcvers", "220.68");
    kv("pi", dev.device_id);
    kvi("vv", 2);                 // AirPlay 2
    kvi("statusFlags", 68);       // 0x44，对齐 UxPlay（表示支持音量控制等）
    kvi("pw", dev.requires_encryption ? 1 : 0);
    // XML plist 的 <data> 元素内容就是 base64；iOS 通过它拿到本机 Ed25519 公钥
    // 进行 legacy 配对。为空时（未配置身份）退化为 0 占位。
    if (!dev.public_key_b64.empty()) {
        oss << "<key>pk</key><data>" << dev.public_key_b64 << "</data>\n";
    } else {
        kvi("pk", 0);
    }
    kvi("acl", 0);

    // ---- 镜像关键字段：声明"本机有显示器"（对齐 UxPlay raop_handler_info）----
    // 缺少 displays 数组时，iOS 认为设备没有视频输出能力：
    // 屏幕镜像列表不显示本设备 / 点了也连不上（日志里从无 type=110 SETUP）。
    oss << "<key>displays</key><array><dict>\n";
    oss << "<key>uuid</key><string>e0ff8a27-6738-3d56-8a16-cc53aacee925</string>\n";
    kvi("widthPhysical", 0);
    kvi("heightPhysical", 0);
    kvi("width", 1920);
    kvi("height", 1080);
    kvi("widthPixels", 1920);
    kvi("heightPixels", 1080);
    oss << "<key>rotation</key><false/>\n";      // bit8(ScreenRotate) 关闭时用 false（AppleTV gen3 为 true）
    oss << "<key>refreshRate</key><real>0.016667</real>\n";  // 1/60s
    kvi("maxFPS", 60);
    oss << "<key>overscanned</key><false/>\n";
    kvi("features", 14);                         // 对齐 UxPlay displays features
    oss << "</dict></array>\n";

    // 保活与版本（iOS 用这些判断设备类型/能力）
    kvi("keepAliveLowPower", 1);
    oss << "<key>keepAliveSendStatsAsBody</key><true/>\n";
    kv("sourceVersion", "220.68");
    oss << "<key>initialVolume</key><real>1.0</real>\n";

    // audioLatencies / audioFormats（AirPlay 2 设备能力声明，UxPlay 同款）
    oss << "<key>audioLatencies</key><array>";
    oss << "<dict><key>type</key><integer>100</integer>"
        << "<key>inputLatencyMicros</key><integer>0</integer>"
        << "<key>audioType</key><string>default</string>"
        << "<key>outputLatencyMicros</key><integer>0</integer></dict>";
    oss << "<dict><key>type</key><integer>101</integer>"
        << "<key>inputLatencyMicros</key><integer>0</integer>"
        << "<key>audioType</key><string>default</string>"
        << "<key>outputLatencyMicros</key><integer>0</integer></dict>";
    oss << "</array>\n";
    oss << "<key>audioFormats</key><array>";
    oss << "<dict><key>audioOutputFormats</key><integer>67108860</integer>"   // 0x3FFFFFC
        << "<key>type</key><integer>100</integer>"
        << "<key>audioInputFormats</key><integer>67108860</integer></dict>";
    oss << "<dict><key>audioOutputFormats</key><integer>67108860</integer>"
        << "<key>type</key><integer>101</integer>"
        << "<key>audioInputFormats</key><integer>67108860</integer></dict>";
    oss << "</array>\n";

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

/* ================================================================
 *  FairPlay SAP（/fp-setup 用）—— 音频流媒体加密协商
 *
 *  setup 消息 1（seq=1，16 字节请求）：req[4]=版本(3) req[6]=seq(1)
 *      req[14]=mode(0..3) → 回 142 字节 FPLY server hello（固定应答，
 *      UxPlay/shairport-sync 同款硬编码；公开协议数据，非设备私密）
 *  setup 消息 2（seq=3，164 字节请求）：
 *      → 回 12 字节 FPLY 头 + 请求末尾 20 字节 = 32 字节
 * ================================================================ */

// FPLY server hello 回复（mode 0..3），与 UxPlay / shairport-sync 完全一致
static const uint8_t kFplyReply[4][142] = {
    {0x46,0x50,0x4c,0x59,0x03,0x01,0x02,0x00,0x00,0x00,0x00,0x82,0x02,0x00,0x0f,0x9f,0x3f,0x9e,0x0a,0x25,0x21,0xdb,0xdf,0x31,0x2a,0xb2,0xbf,0xb2,0x9e,0x8d,0x23,0x2b,0x63,0x76,0xa8,0xc8,0x18,0x70,0x1d,0x22,0xae,0x93,0xd8,0x27,0x37,0xfe,0xaf,0x9d,0xb4,0xfd,0xf4,0x1c,0x2d,0xba,0x9d,0x1f,0x49,0xca,0xaa,0xbf,0x65,0x91,0xac,0x1f,0x7b,0xc6,0xf7,0xe0,0x66,0x3d,0x21,0xaf,0xe0,0x15,0x65,0x95,0x3e,0xab,0x81,0xf4,0x18,0xce,0xed,0x09,0x5a,0xdb,0x7c,0x3d,0x0e,0x25,0x49,0x09,0xa7,0x98,0x31,0xd4,0x9c,0x39,0x82,0x97,0x34,0x34,0xfa,0xcb,0x42,0xc6,0x3a,0x1c,0xd9,0x11,0xa6,0xfe,0x94,0x1a,0x8a,0x6d,0x4a,0x74,0x3b,0x46,0xc3,0xa7,0x64,0x9e,0x44,0xc7,0x89,0x55,0xe4,0x9d,0x81,0x55,0x00,0x95,0x49,0xc4,0xe2,0xf7,0xa3,0xf6,0xd5,0xba},
    {0x46,0x50,0x4c,0x59,0x03,0x01,0x02,0x00,0x00,0x00,0x00,0x82,0x02,0x01,0xcf,0x32,0xa2,0x57,0x14,0xb2,0x52,0x4f,0x8a,0xa0,0xad,0x7a,0xf1,0x64,0xe3,0x7b,0xcf,0x44,0x24,0xe2,0x00,0x04,0x7e,0xfc,0x0a,0xd6,0x7a,0xfc,0xd9,0x5d,0xed,0x1c,0x27,0x30,0xbb,0x59,0x1b,0x96,0x2e,0xd6,0x3a,0x9c,0x4d,0xed,0x88,0xba,0x8f,0xc7,0x8d,0xe6,0x4d,0x91,0xcc,0xfd,0x5c,0x7b,0x56,0xda,0x88,0xe3,0x1f,0x5c,0xce,0xaf,0xc7,0x43,0x19,0x95,0xa0,0x16,0x65,0xa5,0x4e,0x19,0x39,0xd2,0x5b,0x94,0xdb,0x64,0xb9,0xe4,0x5d,0x8d,0x06,0x3e,0x1e,0x6a,0xf0,0x7e,0x96,0x56,0x16,0x2b,0x0e,0xfa,0x40,0x42,0x75,0xea,0x5a,0x44,0xd9,0x59,0x1c,0x72,0x56,0xb9,0xfb,0xe6,0x51,0x38,0x98,0xb8,0x02,0x27,0x72,0x19,0x88,0x57,0x16,0x50,0x94,0x2a,0xd9,0x46,0x68,0x8a},
    {0x46,0x50,0x4c,0x59,0x03,0x01,0x02,0x00,0x00,0x00,0x00,0x82,0x02,0x02,0xc1,0x69,0xa3,0x52,0xee,0xed,0x35,0xb1,0x8c,0xdd,0x9c,0x58,0xd6,0x4f,0x16,0xc1,0x51,0x9a,0x89,0xeb,0x53,0x17,0xbd,0x0d,0x43,0x36,0xcd,0x68,0xf6,0x38,0xff,0x9d,0x01,0x6a,0x5b,0x52,0xb7,0xfa,0x92,0x16,0xb2,0xb6,0x54,0x82,0xc7,0x84,0x44,0x11,0x81,0x21,0xa2,0xc7,0xfe,0xd8,0x3d,0xb7,0x11,0x9e,0x91,0x82,0xaa,0xd7,0xd1,0x8c,0x70,0x63,0xe2,0xa4,0x57,0x55,0x59,0x10,0xaf,0x9e,0x0e,0xfc,0x76,0x34,0x7d,0x16,0x40,0x43,0x80,0x7f,0x58,0x1e,0xe4,0xfb,0xe4,0x2c,0xa9,0xde,0xdc,0x1b,0x5e,0xb2,0xa3,0xaa,0x3d,0x2e,0xcd,0x59,0xe7,0xee,0xe7,0x0b,0x36,0x29,0xf2,0x2a,0xfd,0x16,0x1d,0x87,0x73,0x53,0xdd,0xb9,0x9a,0xdc,0x8e,0x07,0x00,0x6e,0x56,0xf8,0x50,0xce},
    {0x46,0x50,0x4c,0x59,0x03,0x01,0x02,0x00,0x00,0x00,0x00,0x82,0x02,0x03,0x90,0x01,0xe1,0x72,0x7e,0x0f,0x57,0xf9,0xf5,0x88,0x0d,0xb1,0x04,0xa6,0x25,0x7a,0x23,0xf5,0xcf,0xff,0x1a,0xbb,0xe1,0xe9,0x30,0x45,0x25,0x1a,0xfb,0x97,0xeb,0x9f,0xc0,0x01,0x1e,0xbe,0x0f,0x3a,0x81,0xdf,0x5b,0x69,0x1d,0x76,0xac,0xb2,0xf7,0xa5,0xc7,0x08,0xe3,0xd3,0x28,0xf5,0x6b,0xb3,0x9d,0xbd,0xe5,0xf2,0x9c,0x8a,0x17,0xf4,0x81,0x48,0x7e,0x3a,0xe8,0x63,0xc6,0x78,0x32,0x54,0x22,0xe6,0xf7,0x8e,0x16,0x6d,0x18,0xaa,0x7f,0xd6,0x36,0x25,0x8b,0xce,0x28,0x72,0x6f,0x66,0x1f,0x73,0x88,0x93,0xce,0x44,0x31,0x1e,0x4b,0xe6,0xc0,0x53,0x51,0x93,0xe5,0xef,0x72,0xe8,0x68,0x62,0x33,0x72,0x9c,0x22,0x7d,0x82,0x0c,0x99,0x94,0x45,0xd8,0x92,0x46,0xc8,0xc3,0x59}
};

// FPLY setup 消息 2 的回复头（12 字节）
static const uint8_t kFplyHeader[12] = {0x46,0x50,0x4c,0x59,0x03,0x01,0x04,0x00,0x00,0x00,0x00,0x14};

/*!
 * @brief 处理 FairPlay SAP setup 请求（/fp-setup 用）
 *
 * @param body 请求体
 * @param len  请求长度
 * @return 响应体；空表示无法识别（调用方回空 200）
 */
std::vector<uint8_t> handle_fairplay_setup(const uint8_t* body, size_t len) {
    std::vector<uint8_t> out;
    if (!body || len < 12) return out;
    // 协议字段：req[4]=版本(须为 3)，req[6]=seq（1=setup1，3=setup2），
    //           req[14]=mode（0..3，仅 setup1 用）
    AP2_LOGI("rtsp: fairplay setup len=%zu ver=%u seq=%u mode=%u",
             len, body[4], body[6], len > 14 ? body[14] : 0);
    if (body[4] != 3) return out;  // 不支持的 FairPlay 版本
    if (body[6] == 1) {
        // setup 消息 1：回 142 字节 FPLY server hello（按 mode 选）
        uint8_t mode = len > 14 ? body[14] : 0;
        if (mode > 3) return out;
        out.assign(kFplyReply[mode], kFplyReply[mode] + 142);
    } else if (body[6] == 3) {
        // setup 消息 2：回 12 字节头 + 请求末尾 20 字节
        const size_t kSuffix = 20;
        if (len < kSuffix) return out;
        out.assign(kFplyHeader, kFplyHeader + 12);
        out.insert(out.end(), body + len - kSuffix, body + len);
    }
    return out;
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
        if (handlers_.on_pair_setup)
            resp_body = handlers_.on_pair_setup(c.id(), c.peer_ip(), req.body.data(), req.body.size());
        HeaderMap extra;
        extra["Content-Type"] = "application/octet-stream";
        return make_rtsp_ok(req.cseq(), extra, std::move(resp_body));
    }, true);
    http_.add_route("POST", "/pair-verify", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        std::vector<uint8_t> resp_body;
        if (handlers_.on_pair_verify)
            resp_body = handlers_.on_pair_verify(c.id(), c.peer_ip(), req.body.data(), req.body.size());
        HeaderMap extra;
        extra["Content-Type"] = "application/octet-stream";
        return make_rtsp_ok(req.cseq(), extra, std::move(resp_body));
    }, true);

    // /auth-setup（MFi 握手）。正常情况下 iOS 只在 features bit26
    // (HasUnifiedAdvertiserInfo) 置位时才发起；我们 bit26=0，iOS 不会走到这里。
    // 万一有客户端仍发起：对齐 UxPlay 行为——未注册路由回空 200，
    // 而不是回"空证书+空签名"的 40B（后者已被实测导致 iOS 断开）。
    http_.add_route("POST", "/auth-setup", [this](const HttpRequest& req, Connection&) -> HttpResponse {
        (void)this;
        HeaderMap extra;
        extra["Content-Type"] = "application/octet-stream";
        return make_rtsp_ok(req.cseq(), extra);
    }, true);

    // /fp-setup（音频）—— FairPlay SAP 媒体加密握手
    http_.add_route("POST", "/fp-setup", [this](const HttpRequest& req, Connection& c) -> HttpResponse {
        std::vector<uint8_t> resp_body;
        if (handlers_.on_fairplay_setup)
            resp_body = handlers_.on_fairplay_setup(c.id(), req.body.data(), req.body.size());
        else
            resp_body = handle_fairplay_setup(req.body.data(), req.body.size());
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
        // AirPlay 2 的 SETUP 请求体是 binary plist（ekey/eiv/streams/timingPort），
        // 响应也必须是 binary plist。legacy AP1 用 Transport: 头。
        // 判断依据：body 以 "bplist00" 开头（Apple 官方 plist 魔数）。
        bool is_ap2 = (req.body.size() >= 8 &&
                       memcmp(req.body.data(), "bplist00", 8) == 0);
        if (is_ap2) {
            util::PlistValue root;
            if (!util::parse_binary_plist(req.body.data(), req.body.size(), root) ||
                !root.is_dict()) {
                AP2_LOGW("rtsp: SETUP(AP2) body not a bplist dict, len=%zu", req.body.size());
                return make_rtsp_ok(req.cseq(), {}, {}, "application/x-apple-binary-plist");
            }
            // 解析请求字段
            Ap2SetupRequest ap2req;
            const auto& ek = root.get("ekey");
            if (ek.is_data()) ap2req.ekey = ek.as_data();
            const auto& eiv = root.get("eiv");
            if (eiv.is_data()) ap2req.eiv = eiv.as_data();
            ap2req.timing_port = (uint64_t)root.get_int("timingPort");
            ap2req.timing_protocol = root.get_string("timingProtocol");
            ap2req.is_remote_control_only = root.get_bool("isRemoteControlOnly");
            const util::PlistValue& streams = root.get("streams");
            if (streams.is_array()) {
                for (const auto& s : streams.array()) {
                    if (!s.is_dict()) continue;
                    Ap2StreamReq sr;
                    sr.type = (uint64_t)s.get_int("type");
                    sr.control_port = (uint64_t)s.get_int("controlPort");
                    sr.stream_connection_id = (uint64_t)s.get_int("streamConnectionID");
                    sr.ct = (uint64_t)s.get_int("ct");
                    sr.spf = (uint64_t)s.get_int("spf");
                    sr.sr  = (uint64_t)s.get_int("sr");
                    ap2req.streams.push_back(sr);
                }
            }
            AP2_LOGI("rtsp: SETUP(AP2) streams=%zu timingPort=%llu proto=%s ekey=%zuB eiv=%zuB",
                     ap2req.streams.size(), (unsigned long long)ap2req.timing_port,
                     ap2req.timing_protocol.c_str(), ap2req.ekey.size(), ap2req.eiv.size());
            for (auto& s : ap2req.streams)
                AP2_LOGI("rtsp:   stream type=%llu controlPort=%llu streamConnectionID=%llu ct=%llu spf=%llu sr=%llu",
                         (unsigned long long)s.type, (unsigned long long)s.control_port,
                         (unsigned long long)s.stream_connection_id,
                         (unsigned long long)s.ct, (unsigned long long)s.spf,
                         (unsigned long long)s.sr);

            Ap2SetupResponse resp;
            if (handlers_.on_setup_ap2) resp = handlers_.on_setup_ap2(c.id(), ap2req);

            // 构建 binary plist 响应
            auto out = util::PlistValue::make_dict();
            if (!resp.ok) {
                return make_rtsp_ok(req.cseq(), {}, {}, "application/x-apple-binary-plist");
            }
            out.dict()["timingPort"] = util::PlistValue::make_int(resp.timing_port);
            out.dict()["eventPort"]  = util::PlistValue::make_int(resp.event_port);
            auto streams_out = util::PlistValue::make_array();
            for (auto& s : resp.streams) {
                auto sd = util::PlistValue::make_dict();
                sd.dict()["dataPort"] = util::PlistValue::make_int(s.data_port);
                if (s.type == 96 && s.control_port > 0)
                    sd.dict()["controlPort"] = util::PlistValue::make_int(s.control_port);
                sd.dict()["type"] = util::PlistValue::make_int((int64_t)s.type);
                streams_out.array().push_back(std::move(sd));
            }
            out.dict()["streams"] = std::move(streams_out);

            std::vector<uint8_t> body;
            util::serialize_binary_plist(out, body);
            HeaderMap extra;
            extra["Session"] = "airplay2lib-" + std::to_string(c.id());
            return make_rtsp_ok(req.cseq(), extra, std::move(body),
                                "application/x-apple-binary-plist");
        }

        // ---- legacy AP1 SETUP（Transport: header）----
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

    // Default handler：UxPlay 对未注册路径也回 200 空 body，iOS 才继续走
    // ANNOUNCE/SETUP；回 404 会让 iOS 直接断开连接。
    http_.set_default_handler([this](const HttpRequest& req, Connection& c) -> HttpResponse {
        if (handlers_.on_unknown) return handlers_.on_unknown(req, c);
        AP2_LOGI("rtsp: unhandled %s %s -> 200 empty", req.method.c_str(), req.uri.c_str());
        return make_rtsp_ok(req.cseq());
    });
}

} // namespace net
} // namespace airplay2
