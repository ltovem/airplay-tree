/*!
 * @file rtp_receiver.cpp
 */
#include "rtp_receiver.h"
#include "../platform/platform_log.h"
#include "../platform/platform_time.h"
#include <cstring>

namespace airplay2 {
namespace net {

RtpReceiver::RtpReceiver() = default;
RtpReceiver::~RtpReceiver() { stop(); }

bool RtpReceiver::open(uint16_t port_min, uint16_t port_max, int ports[3]) {
    // Find 3 consecutive free UDP ports
    uint16_t bound[3] = {0,0,0};
    platform::Socket tmp[3];
    bool found = false;
    for (uint16_t base = port_min; base + 2 <= port_max; base += 2) {
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            tmp[i].close();
            if (!tmp[i].create(platform::SocketProtocol::UDP, false)) { ok = false; break; }
            tmp[i].set_option(platform::SOCK_OPT_REUSEADDR, 1);
            tmp[i].set_option(platform::SOCK_OPT_RCVBUF, 1 << 20); // 1MB
            if (!tmp[i].bind("0.0.0.0", base + i)) { ok = false; break; }
            bound[i] = base + i;
        }
        if (ok) { found = true; break; }
    }
    if (!found) return false;
    // Commit
    data_sock_   = std::move(tmp[0]);
    ctrl_sock_   = std::move(tmp[1]);
    timing_sock_ = std::move(tmp[2]);
    for (int i = 0; i < 3; ++i) ports[i] = bound[i];
    AP2_LOGI("rtp: bound ports %d-%d-%d", bound[0], bound[1], bound[2]);
    return true;
}

bool RtpReceiver::start() {
    if (!data_sock_.valid()) return false;
    running_.store(true);
    worker_.start([this] { receiver_worker(); }, "ap2-rtp");
    return true;
}

void RtpReceiver::stop() {
    if (!running_.exchange(false)) return;
    worker_.stop_and_join();
    data_sock_.close();
    ctrl_sock_.close();
    timing_sock_.close();
}

void RtpReceiver::flush() {
    std::lock_guard<std::mutex> lk(jbuf_mu_);
    jbuffer_.clear();
    has_started_ = false;
}

static inline uint16_t seq_diff(uint16_t a, uint16_t b) {
    // return (int16_t)(a - b); correct for wrap-around via int16_t cast
    return (uint16_t)(int16_t)(a - b);
}

void RtpReceiver::emit_ready() {
    std::vector<RtpAudioPacket> to_emit;
    {
        std::lock_guard<std::mutex> lk(jbuf_mu_);
        if (jbuffer_.empty()) return;
        if (!has_started_) {
            // Start from the lowest seq we have
            next_expected_seq_ = jbuffer_.begin()->first;
            has_started_ = true;
        }
        while (!jbuffer_.empty()) {
            auto it = jbuffer_.find(next_expected_seq_);
            if (it == jbuffer_.end()) {
                // Check if a huge gap exists; if the lowest seq is way ahead, we lost this one
                auto lowest = jbuffer_.begin();
                uint16_t diff = seq_diff(lowest->first, next_expected_seq_);
                if (diff > jbuf_max_ / 2) {
                    AP2_LOGW("rtp: jump seq %u -> %u (gap=%u)",
                             next_expected_seq_, lowest->first, diff);
                    stats_.lost += diff;
                    next_expected_seq_ = lowest->first;
                    continue;
                }
                break;
            }
            to_emit.push_back(std::move(it->second));
            jbuffer_.erase(it);
            next_expected_seq_++;
        }
    }
    if (packet_cb_) {
        for (auto& p : to_emit) packet_cb_(p);
    }
}

void RtpReceiver::receiver_worker() {
    uint8_t buf[4096];
    while (running_.load()) {
        std::vector<platform::Socket*> socks = { &data_sock_, &ctrl_sock_, &timing_sock_ };
        std::vector<size_t> ready;
        if (!platform::select_read(socks, ready, 100)) {
            platform::sleep_ms(20); continue;
        }
        for (size_t idx : ready) {
            platform::Socket* s = socks[idx];
            platform::SocketAddr from;
            auto r = s->recvfrom(buf, sizeof(buf), &from);
            if (!r.ok || r.bytes < 12) continue;
            stats_.packets++;
            stats_.bytes += (uint64_t)r.bytes;
            if (idx != 0) {
                // Control/timing packets are silently ignored in this minimal impl
                continue;
            }
            // RTP header: V=2, P, X, CC, M, PT (2B), seq (2B), ts (4B), ssrc (4B)
            uint8_t* p = buf;
            if ((p[0] & 0xC0) != 0x80) continue; // RTP version must be 2
            bool m = (p[1] & 0x80) != 0;
            uint8_t pt = p[1] & 0x7F;
            uint16_t seq = (uint16_t(p[2]) << 8) | p[3];
            uint32_t ts  = (uint32_t(p[4]) << 24) | (uint32_t(p[5]) << 16) |
                           (uint32_t(p[6]) << 8)  | p[7];
            uint32_t ssrc = (uint32_t(p[8]) << 24) | (uint32_t(p[9]) << 16) |
                            (uint32_t(p[10]) << 8) | p[11];
            uint8_t cc = p[0] & 0x0F;
            size_t hdr_len = 12 + cc * 4;
            // Extension header if X bit
            size_t off = hdr_len;
            if ((p[0] & 0x10) && (off + 4 <= (size_t)r.bytes)) {
                uint16_t ext_def = (uint16_t(buf[off]) << 8) | buf[off + 1];
                uint16_t ext_len = ((uint16_t(buf[off + 2]) << 8) | buf[off + 3]) * 4;
                (void)ext_def;
                off += 4 + ext_len;
            }
            if (off >= (size_t)r.bytes) continue;
            RtpAudioPacket pkt;
            pkt.seq = seq;
            pkt.timestamp = ts;
            pkt.ssrc = ssrc;
            pkt.pt = pt;
            pkt.marker = m;
            pkt.recv_us = platform::time_now_us();
            pkt.payload.assign(buf + off, buf + r.bytes);
            {
                std::lock_guard<std::mutex> lk(jbuf_mu_);
                if (has_started_) {
                    uint16_t d = seq_diff(seq, next_expected_seq_);
                    if ((int16_t)d < 0) {
                        stats_.reordered++;
                    } else if (d > 0 && d < 0x8000) {
                        // Could track hole; for now just insert
                    }
                }
                if (jbuffer_.size() >= jbuf_max_) {
                    // Emit what we can to free space
                }
                jbuffer_[seq] = std::move(pkt);
            }
            emit_ready();
        }
    }
}

} // namespace net
} // namespace airplay2
