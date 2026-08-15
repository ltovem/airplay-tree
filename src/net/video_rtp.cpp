/*!
 * @file video_rtp.cpp
 * @brief 视频 RTP 接收器实现（单线程 select，低延时设计）
 */
#include "video_rtp.h"
#include "rtp.h"
#include "../crypto/aes_ctr.h"
#include "../platform/platform_log.h"
#include "../platform/platform_time.h"
#include <cstring>
#include <memory>

namespace airplay2 {
namespace net {

// VideoRtpReceiver 里喂给 NalReassembler 的包类型是 codec::RtpVideoPacket
using codec::RtpVideoPacket;

struct VideoRtpReceiver::AesCtx {
    crypto::AesCtr ctr;
};

VideoRtpReceiver::VideoRtpReceiver() = default;

VideoRtpReceiver::~VideoRtpReceiver() {
    stop();
}

bool VideoRtpReceiver::set_decryption_params(const std::string& aes_key_hex,
                                             const std::string& aes_iv_hex) {
    auto kb = crypto::hex_to_vector(aes_key_hex);
    auto ivb = crypto::hex_to_vector(aes_iv_hex);
    if (kb.size() != 16 || ivb.size() != 16) return false;
    if (!aes_) aes_ = std::make_unique<AesCtx>();
    return aes_->ctr.set_key(kb.data(), ivb.data());
}

bool VideoRtpReceiver::set_decryption_key(const uint8_t key[16], const uint8_t iv[16]) {
    if (!key || !iv) return false;
    if (!aes_) aes_ = std::make_unique<AesCtx>();
    return aes_->ctr.set_key(key, iv);
}

bool VideoRtpReceiver::open(uint16_t data_port_min, uint16_t data_port_max,
                            uint16_t& out_data_port) {
    using platform::SocketProtocol;
    if (data_port_min > data_port_max) return false;
    for (uint32_t p = data_port_min; p <= data_port_max; ++p) {
        platform::Socket s;
        if (!s.create(SocketProtocol::UDP)) continue;
        s.set_option(platform::SOCK_OPT_REUSEADDR, 1);
        if (s.bind("0.0.0.0", (uint16_t)p)) {
            data_sock_ = std::move(s);
            tcp_mode_ = false;
            out_data_port = (uint16_t)p;
            // start() 在绑定前被调用过（RECORD 早于镜像 SETUP）→ 补启动
            if (start_deferred_) {
                start_deferred_ = false;
                start();
            }
            return true;
        }
    }
    return false;
}

bool VideoRtpReceiver::open_tcp(uint16_t port_min, uint16_t port_max,
                                uint16_t& out_data_port) {
    using platform::SocketProtocol;
    if (port_min > port_max) return false;
    for (uint32_t p = port_min; p <= port_max; ++p) {
        platform::Socket s;
        if (!s.create(SocketProtocol::TCP)) continue;
        s.set_option(platform::SOCK_OPT_REUSEADDR, 1);
        if (!s.bind("0.0.0.0", (uint16_t)p)) continue;
        if (!s.listen(4)) continue;
        data_sock_ = std::move(s);
        tcp_mode_ = true;
        out_data_port = (uint16_t)p;
        AP2_LOGI("video: TCP Data Push listener on port %u", (unsigned)p);
        if (start_deferred_) {
            start_deferred_ = false;
            start();
        }
        return true;
    }
    return false;
}

void VideoRtpReceiver::start() {
    if (!data_sock_.valid()) {
        // 端口还没绑定：记下延迟启动请求，open() 成功后自动拉起。
        start_deferred_ = true;
        return;
    }
    start_deferred_ = false;
    if (running_.exchange(true)) return;
    if (tcp_mode_) {
        worker_.start([this] { tcp_push_worker(); }, "ap2-vidpush");
    } else {
        worker_.start([this] { receiver_worker(); }, "ap2-video");
    }
}

void VideoRtpReceiver::stop() {
    start_deferred_ = false;
    if (running_.exchange(false)) {
        data_sock_.close();   // 先关 socket 让 select 立即返回，再 join
        push_conn_.close();   // TCP 模式：同时断开已 accept 的推流连接
        worker_.stop_and_join();
        return;
    }
    data_sock_.close();
    push_conn_.close();
}

void VideoRtpReceiver::receiver_worker() {
    std::vector<uint8_t> buf(65536);
    uint64_t base_clk = 0;
    uint32_t first_ts = 0;

    while (running_.load()) {
        if (!data_sock_.valid()) break;
        std::vector<platform::Socket*> socks = { &data_sock_ };
        std::vector<size_t> ready;
        if (!platform::select_read(socks, ready, 50)) {
            platform::sleep_ms(10);
            continue;
        }
        for (size_t idx : ready) {
            (void)idx;
            platform::SocketAddr from;
            auto r = data_sock_.recvfrom(buf.data(), buf.size(), &from);
            if (!r.ok || r.bytes <= 0) continue;
            if (sender_ip_.empty()) {
                sender_ip_ = from.ip;
                sender_port_ = from.port;
            }

            // 用 rtp.h 统一解析 RTP 头
            uint16_t seq = 0;
            uint32_t ts  = 0;
            uint32_t ssrc = 0;
            uint8_t  pt = 0;
            bool     marker = false;
            size_t   off = 0;
            if (!rtp_parse_header(buf.data(), (size_t)r.bytes, seq, ts, ssrc, pt, marker, off)) continue;
            size_t body_len = (size_t)r.bytes - off;
            if (body_len == 0) continue;
            const uint8_t* body = buf.data() + off;

            // AES-128-CTR 解密（若密钥已设）
            if (aes_ && aes_->ctr.is_ready() && body_len > 0) {
                // 拷贝到一个可写临时 buf，避免对 const 去 const 导致未定义行为
                std::vector<uint8_t> tmp(body, body + body_len);
                aes_->ctr.process(tmp.data(), tmp.data(), body_len);
                // 再塞回 buf.data() + off 便于下面使用
                std::memcpy(const_cast<uint8_t*>(body), tmp.data(), body_len);
            }

            // 封装 video packet 给 reassembler
            RtpVideoPacket vpkt;
            vpkt.seq  = seq;
            vpkt.ts   = ts;
            vpkt.ssrc = ssrc;
            vpkt.marker = marker;
            (void)pt;
            vpkt.payload.assign(body, body + body_len);

            // 初始化时间基准（微秒）
            if (base_clk == 0) {
                base_clk = platform::time_now_us();
                first_ts = ts;
            }
            // 90kHz → 微秒
            uint64_t ts_delta = (ts >= first_ts) ? (ts - first_ts)
                                                  : (0xFFFFFFFFULL - first_ts + 1 + ts);
            uint64_t pts_us = base_clk + (ts_delta * 1000000ULL / 90000ULL);

            auto vf = reassembler_.push(vpkt);
            if (vf && cb_) {
                vf->pts_us = pts_us;
                cb_(*vf);
            }
        }
    }
}

/* ================================================================
 * TCP Data Push（AirPlay 2 镜像默认传输）
 *
 * iOS 在 SETUP(110) 响应拿到 dataPort 后主动 TCP connect，然后按
 * "128 字节帧头 + payload" 的帧格式推流：
 *   frame[0:4]   = payload 长度（大端）
 *   frame[4]     = 类型：0x00=加密VCL(音视频数据)、0x01=未加密 SPS/PPS、
 *                  0x02=旧协议空包、0x05=streaming report
 *   frame[4..5]  = 0x00 0x00（加密非IDR）/ 0x00 0x10（加密IDR）
 *                  0x01 0x00（未加密 SPS/PPS）等
 *   frame[8:16]  = NTP 时间戳（自开机纳秒，无 1900 偏移）
 *   frame[16:128]= 宽高等附加元数据（float）
 *   payload      = [4B NAL 长度][NAL]... 序列；加密时 AES-CTR 从
 *                  "AirPlayStreamKey{id}" 派生的密钥/IV 逐帧解密
 * 参考：UxPlay raop_rtp_mirror.c（协议等价，仅端口从 TCP 读）
 * ================================================================ */
void VideoRtpReceiver::tcp_push_worker() {
    std::vector<uint8_t> hdr(128);
    std::vector<uint8_t> payload;
    uint64_t base_clk = 0;   // 首帧本地时刻（微秒）
    uint64_t first_ntp_ns = 0;
    uint64_t n_frames = 0;   // 诊断：累计回调的帧数
    uint64_t n_bytes  = 0;   // 诊断：累计收到的 payload 字节
    uint64_t last_stat_us = platform::time_now_us();

    while (running_.load()) {
        // ---- accept：iOS 随时可能断开重连 ----
        if (!push_conn_.valid()) {
            if (!data_sock_.valid()) break;
            std::vector<platform::Socket*> socks = { &data_sock_ };
            std::vector<size_t> ready;
            if (!platform::select_read(socks, ready, 100)) continue;
            if (ready.empty()) continue;
            platform::SocketAddr peer;
            auto conn = data_sock_.accept(&peer);
            if (!conn.valid()) {
                platform::sleep_ms(20);
                continue;
            }
            push_conn_ = std::move(conn);
            sender_ip_ = peer.ip;
            sender_port_ = peer.port;
            AP2_LOGI("video: TCP Data Push client connected (%s:%u)",
                     peer.ip.c_str(), (unsigned)peer.port);
            continue;
        }

        // ---- 读 128 字节帧头 ----
        size_t got = 0;
        while (got < hdr.size() && running_.load()) {
            auto r = push_conn_.recv(hdr.data() + got, hdr.size() - got);
            if (!r.ok) {
                if (r.disconnected) { push_conn_.close(); break; }
                platform::sleep_ms(5);
                if (!push_conn_.valid()) break;
                continue;
            }
            if (r.bytes == 0) { push_conn_.close(); break; }
            got += (size_t)r.bytes;
        }
        if (!push_conn_.valid() || got < hdr.size()) continue;

        // ---- 解析帧头 ----
        // 注意字节序：payload_size 是 **little-endian**（UxPlay byteutils_get_int
        // 直接解引用 uint32* 即主机序，x86 上小端）；NAL 长度前缀才是大端。
        // 之前按大端读把 37B 的 SPS/PPS 包读成 0x25000000=620MB，直接丢弃。
        uint32_t payload_size = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                                ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
        uint8_t type = hdr[4];
        uint8_t type2 = hdr[5];
        uint64_t ntp_ns = 0;
        // NTP 时间戳同样是小端（UxPlay byteutils_get_long 直接解引用）
        for (int i = 7; i >= 0; --i)
            ntp_ns = (ntp_ns << 8) | hdr[8 + i];

        if (payload_size == 0 || payload_size > 8 * 1024 * 1024) {
            // 类型 0x02（旧协议空包）与 0x05（streaming report）没有 payload
            if (type == 0x02 || type == 0x05) continue;
            AP2_LOGW("video: bad Data Push payload size %u type=%02x", payload_size, type);
            push_conn_.close();
            continue;
        }

        // ---- 读 payload ----
        payload.resize(payload_size);
        got = 0;
        while (got < payload_size && running_.load()) {
            auto r = push_conn_.recv(payload.data() + got, payload_size - got);
            if (!r.ok) {
                if (r.disconnected) { push_conn_.close(); break; }
                platform::sleep_ms(5);
                if (!push_conn_.valid()) break;
                continue;
            }
            if (r.bytes == 0) { push_conn_.close(); break; }
            got += (size_t)r.bytes;
        }
        if (!push_conn_.valid() || got < payload_size) continue;

        // ---- 类型 0x00：加密的视频数据（含 IDR/非 IDR VCL NAL）----
        // 解密后 payload 是 [4B NAL 长度][NAL] 序列，转成 Annex-B。
        if (type == 0x00) {
            std::vector<uint8_t> plain;
            if (aes_ && aes_->ctr.is_ready()) {
                plain.resize(payload_size);
                aes_->ctr.process(payload.data(), plain.data(), payload_size);
            } else {
                plain.swap(payload);
            }
            // 诊断：首帧 dump 解密结果，确认 AES-CTR 解密是否正确。
            // 正确时 payload 是 [4B len][NAL头=0x67(SPS)/0x65(IDR)/0x25...]；
            // 若前 4B 长度荒谬或 NAL 头是随机字节 → 视频 key/iv 派生或
            // streamConnectionID 时序有问题。
            if (n_frames < 3) {
                char hexbuf[96] = {0};
                size_t hb = std::min<size_t>(plain.size(), 32);
                for (size_t i = 0; i < hb; ++i)
                    std::snprintf(hexbuf + i * 2, 3, "%02X", plain[i]);
                AP2_LOGI("video: type=0x00 len=%u plain[0:32]=%s",
                         (unsigned)payload_size, hexbuf);
            }
            // PTS 必须反映**发送端的真实帧时间**，不能用到达时刻——
            // TCP 突发到达时相邻帧可能只差几百微秒，AVSampleBufferDisplayLayer
            // 会把这些帧当成"同时到达"只显示第一帧（实测症状）。
            // 正确做法（UxPlay raop_rtp_mirror.c 同款）：帧头 NTP 时间戳是
            // "自开机纳秒"（小端、无 1900 偏移），用首帧对齐本地单调时钟后
            // 每帧 PTS = base + (ntp_ns - first_ntp_ns)/1000，间隔即真实帧率。
            if (base_clk == 0) {
                base_clk = platform::time_now_us();
                first_ntp_ns = ntp_ns;
            }
            uint64_t pts_us = base_clk;
            if (ntp_ns >= first_ntp_ns)
                pts_us += (ntp_ns - first_ntp_ns) / 1000;

            // [4B 长度][NAL]... → Annex-B（0x00 0x00 0x00 0x01 起始码）
            std::vector<uint8_t> annex_b;
            annex_b.reserve(plain.size() + 16);
            size_t off = 0;
            bool is_key = (type2 == 0x10);   // 帧头 0x00 0x10 = 加密 IDR
            while (off + 4 <= plain.size()) {
                uint32_t nal_len = ((uint32_t)plain[off] << 24) | ((uint32_t)plain[off + 1] << 16) |
                                   ((uint32_t)plain[off + 2] << 8) | plain[off + 3];
                off += 4;
                if (nal_len == 0 || off + nal_len > plain.size()) break;
                // 起始码
                annex_b.insert(annex_b.end(), {0x00, 0x00, 0x00, 0x01});
                annex_b.insert(annex_b.end(), plain.begin() + off, plain.begin() + off + nal_len);
                // H.264 IDR slice = type 5；H.265 IRAP = 16..23
                uint8_t hdr_byte = plain[off];
                if (codec_ == VideoCodec::H265_HEVC) {
                    int nt = (hdr_byte >> 1) & 0x3F;
                    if (nt >= 16 && nt <= 23) is_key = true;
                } else if ((hdr_byte & 0x1F) == 5) {
                    is_key = true;
                }
                off += nal_len;
            }
            if (annex_b.empty()) continue;
            VideoFrame f;
            f.codec = codec_;
            f.annex_b = std::move(annex_b);
            f.pts_us = pts_us;
            f.is_key = is_key;
            n_frames++;
            if (cb_) cb_(f);
        } else if (type == 0x01) {
            // 未加密 SPS/PPS 参数集包。
            // 注意：这是 **AVCC/AVCDecoderConfigurationRecord 格式**，不是
            // [4B 长度][NAL] 格式！布局（UxPlay raop_rtp_mirror.c 同款）：
            //   [0:6]  固定头：01 <profile> <compat> <level> FF E1
            //   [6:8]  SPS 长度（大端 2B）
            //   [8:]   SPS 数据
            //   [8+sps_size]     numOfPPS (1B)
            //   [9+sps_size:11+sps_size] PPS 长度（大端 2B）
            //   [11+sps_size:]   PPS 数据
            // 之前按 [4B len][NAL] 解析，把 01 64 00 20 读成 2336 万字节长度，
            // annex_b 为空 → 渲染器永远拿不到 SPS/PPS → 无法建 fmt_desc_ → 无画面。
            if (n_frames < 3) {
                char hexbuf[64] = {0};
                size_t hb = std::min<size_t>(payload_size, 16);
                for (size_t i = 0; i < hb; ++i)
                    std::snprintf(hexbuf + i * 2, 3, "%02X", payload[i]);
                AP2_LOGI("video: type=0x01 SPS/PPS len=%u bytes[0:16]=%s",
                         (unsigned)payload_size, hexbuf);
            }
            std::vector<uint8_t> annex_b;
            annex_b.reserve(payload_size + 16);
            // 安全解析 AVCC：需要 >= 8 字节且有合法的 SPS 长度
            if (payload_size >= 8) {
                uint32_t sps_size = ((uint32_t)payload[6] << 8) | payload[7];
                if (sps_size > 0 && 8 + sps_size + 3 <= payload_size) {
                    uint32_t pps_off = 8 + sps_size + 1;              // 跳过 numOfPPS
                    uint32_t pps_size = 0;
                    if (pps_off + 2 <= payload_size)
                        pps_size = ((uint32_t)payload[pps_off] << 8) | payload[pps_off + 1];
                    uint32_t pps_data = pps_off + 2;
                    annex_b.insert(annex_b.end(), {0x00, 0x00, 0x00, 0x01});
                    annex_b.insert(annex_b.end(), payload.begin() + 8,
                                  payload.begin() + 8 + sps_size);
                    if (pps_size > 0 && pps_data + pps_size <= payload_size) {
                        annex_b.insert(annex_b.end(), {0x00, 0x00, 0x00, 0x01});
                        annex_b.insert(annex_b.end(), payload.begin() + pps_data,
                                      payload.begin() + pps_data + pps_size);
                    }
                }
            }
            if (annex_b.empty()) {
                AP2_LOGW("video: SPS/PPS AVCC parse failed len=%u", (unsigned)payload_size);
                continue;
            }
            VideoFrame f;
            f.codec = codec_;
            f.annex_b = std::move(annex_b);
            f.pts_us = (base_clk != 0) ? base_clk : platform::time_now_us();
            f.is_key = false;
            if (cb_) cb_(f);
        }
        // 类型 0x02 / 0x05（空包 / streaming report）：直接忽略

        // 诊断：每 ~1s 打印一次累计统计，确认视频流是否仍在到达、
        // 以及各帧类型的占比（排查"帧冻结"和"无画面"）。
        n_bytes += payload_size;
        uint64_t now_us = platform::time_now_us();
        if (now_us - last_stat_us >= 1000000ULL) {
            AP2_LOGI("video: push stats %llu frames, %llu bytes (%llu B/s)",
                     (unsigned long long)n_frames, (unsigned long long)n_bytes,
                     (unsigned long long)(n_bytes * 1000000ULL / (now_us - last_stat_us)));
            last_stat_us = now_us;
        }
    }
}

} // namespace net
} // namespace airplay2
