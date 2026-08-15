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

void VideoRtpReceiver::start() {
    if (!data_sock_.valid()) {
        // 端口还没绑定：记下延迟启动请求，open() 成功后自动拉起。
        start_deferred_ = true;
        return;
    }
    start_deferred_ = false;
    if (running_.exchange(true)) return;
    worker_.start([this] { receiver_worker(); }, "ap2-video");
}

void VideoRtpReceiver::stop() {
    start_deferred_ = false;
    if (running_.exchange(false)) {
        data_sock_.close();   // 先关 socket 让 select 立即返回，再 join
        worker_.stop_and_join();
        return;
    }
    data_sock_.close();
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

} // namespace net
} // namespace airplay2
