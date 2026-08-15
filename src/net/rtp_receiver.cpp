/*!
 * @file rtp_receiver.cpp
 *
 * AirPlay RTP 音频接收核心，功能分层：
 *   - 三 UDP 端口绑定（连续 data/ctrl/timing）
 *   - RTP data：解包 → AES-128-CTR 解密（可选） → 抖动缓冲重排序 → 回调有序包
 *   - RTCP ctrl：解析 SR → 定期构造 RR 回送
 *   - Timing：解析 timing request → 构造 timing response 回送
 *
 * 所有网络 I/O 在单条 receiver 线程里串行化，因此 rtcp_/timing_/aes_/jitter_*
 * 等内部状态完全不需要锁；唯一需要锁的是外部 flush() 调用和内部对 jbuffer_ 的
 * 访问（emit_ready / worker 插入）之间的互斥。
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
    uint16_t bound[3] = {0,0,0};
    platform::Socket tmp[3];
    bool found = false;
    for (uint16_t base = port_min; base + 2 <= port_max; ++base) {
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
    data_sock_   = std::move(tmp[0]);
    ctrl_sock_   = std::move(tmp[1]);
    timing_sock_ = std::move(tmp[2]);
    for (int i = 0; i < 3; ++i) ports[i] = bound[i];
    AP2_LOGI("rtp: bound ports %d-%d-%d", bound[0], bound[1], bound[2]);
    // 若 start() 在端口绑定前就被调用过（AP2：RECORD 早于带流 SETUP），
    // 现在端口就绪，补上收包线程。
    if (start_deferred_) {
        start_deferred_ = false;
        start();
    }
    return true;
}

bool RtpReceiver::start() {
    if (!data_sock_.valid()) {
        // 端口还没绑定：记下延迟启动请求，等 open() 成功后自动拉起。
        start_deferred_ = true;
        return true;
    }
    start_deferred_ = false;
    if (running_.exchange(true)) return true;
    worker_.start([this] { receiver_worker(); }, "ap2-rtp");
    return true;
}

void RtpReceiver::stop() {
    start_deferred_ = false;
    if (running_.exchange(false)) {
        worker_.stop_and_join();
    }
    // 即使从未真正启动（延迟启动期间被 TEARDOWN），也要关掉已绑定的 socket。
    data_sock_.close();
    ctrl_sock_.close();
    timing_sock_.close();
}

void RtpReceiver::flush() {
    std::lock_guard<std::mutex> lk(jbuf_mu_);
    jbuffer_.clear();
    has_started_ = false;
    aes_.reset_counter();
}

bool RtpReceiver::set_decryption_params(const std::string& aes_key_hex,
                                        const std::string& aes_iv_hex) {
    if (aes_key_hex.empty() || aes_iv_hex.empty()) {
        // 没有密钥表示不加密，视为"成功"但不启用解密
        return true;
    }
    auto key_bytes = crypto::hex_to_vector(aes_key_hex);
    auto iv_bytes  = crypto::hex_to_vector(aes_iv_hex);
    if (key_bytes.size() != 16 || iv_bytes.size() != 16) {
        AP2_LOGW("rtp: invalid AES key/iv length (need 16B each, got %zu/%zu B)",
                 key_bytes.size(), iv_bytes.size());
        return false;
    }
    bool ok = aes_.set_key(key_bytes.data(), iv_bytes.data());
    if (ok) {
        AP2_LOGI("rtp: AES-128-CTR decryption configured (key=%s...)",
                 aes_key_hex.substr(0, 8).c_str());
    }
    return ok;
}

void RtpReceiver::set_cbc_decryption(const uint8_t* key, const uint8_t* iv) {
    if (!key || !iv) { cbc_ready_ = false; return; }
    if (cbc_.set_key(key)) {
        std::memcpy(cbc_iv_, iv, 16);
        cbc_ready_ = true;
        AP2_LOGI("rtp: AES-128-CBC decryption configured (AP2 SETUP ekey/eiv)");
    }
}

void RtpReceiver::set_remote_address(const std::string& client_ip, int remote_ports[3]) {
    sender_ip_ = client_ip;
    // remote_ports[0] = audio data port on sender (接收端从不主动发 data)
    // remote_ports[1] = sender RTCP control port (回 RR)
    // remote_ports[2] = sender timing port (回 timing response)
    if (remote_ports) {
        remote_ctrl_port_   = remote_ports[1];
        remote_timing_port_ = remote_ports[2];
    }
}

static inline uint16_t seq_diff(uint16_t a, uint16_t b) {
    return (uint16_t)(int16_t)(a - b);
}

void RtpReceiver::emit_ready() {
    // 缺失包等待超时（微秒）。100ms ≈ 12 个包；等太久会让后续包把
    // 抖动缓冲占满，最终整批丢弃（~1 秒音频），远不如早跳损失 ~24ms。
    static constexpr uint64_t kGapTimeoutUs = 100000ULL;
    std::vector<RtpAudioPacket> to_emit;
    {
        std::lock_guard<std::mutex> lk(jbuf_mu_);
        if (jbuffer_.empty()) {
            gap_wait_start_us_ = 0;
            return;
        }
        if (!has_started_) {
            next_expected_seq_ = jbuffer_.begin()->first;
            has_started_ = true;
        }
        uint64_t now_us = platform::time_now_us();
        while (!jbuffer_.empty()) {
            auto it = jbuffer_.find(next_expected_seq_);
            if (it == jbuffer_.end()) {
                auto lowest = jbuffer_.begin();
                uint16_t diff = seq_diff(lowest->first, next_expected_seq_);
                // 缺口太大（> 半缓冲）或等待超时 → 按丢失跳过缺失段
                if (diff > jbuf_max_ / 2 || (gap_wait_start_us_ != 0 &&
                                             now_us - gap_wait_start_us_ >= kGapTimeoutUs)) {
                    if (diff > 0) {
                        AP2_LOGW("rtp: jump seq %u -> %u (gap=%u%s)",
                                 next_expected_seq_, lowest->first, diff,
                                 (diff > jbuf_max_ / 2) ? "" : ", timeout");
                        stats_.lost += diff;
                    }
                    next_expected_seq_ = lowest->first;
                    gap_wait_start_us_ = 0;
                    continue;
                }
                // 首次发现缺口：记录等待起点
                if (gap_wait_start_us_ == 0) gap_wait_start_us_ = now_us;
                break;
            }
            // 连续包来了，缺口已消除
            gap_wait_start_us_ = 0;
            to_emit.push_back(std::move(it->second));
            jbuffer_.erase(it);
            next_expected_seq_++;
        }
    }
    if (packet_cb_) {
        for (auto& p : to_emit) packet_cb_(p);
    }
}

void RtpReceiver::maybe_send_rr() {
    if (!ctrl_sock_.valid()) return;
    uint64_t now_us = platform::wallclock_us();
    // RR 间隔：~5 秒。AirPlay 音频是低带宽控制信道，RR 发得不用很频繁。
    if (last_rr_send_us_ != 0 && (now_us - last_rr_send_us_) < 5 * 1000000ULL) return;
    // 如果没收到过 SR，无法获取 sender_ssrc，但仍可构造一个"空 RR"；
    // 为了简化且不误导发送端，首次 SR 到之前不发。
    if (!rtcp_.has_sr()) return;
    const RtcpSrInfo& sr = rtcp_.last_sr();

    uint32_t highest_seq = 0;
    {
        std::lock_guard<std::mutex> lk(jbuf_mu_);
        if (!jbuffer_.empty()) {
            // jbuffer 里存的是未出队包，取最大 seq；否则用 next_expected_seq - 1
            highest_seq = jbuffer_.rbegin()->first;
        } else if (has_started_) {
            highest_seq = (uint32_t)(uint16_t)(next_expected_seq_ - 1);
        }
    }

    auto pkt = rtcp_.build_rr(sr.ssrc, our_ssrc_, highest_seq,
                              (uint32_t)stats_.lost, jitter_est_);
    if (pkt.empty()) return;
    // 优先用"最近一次收到 SR 时的源地址"，否则用 SETUP 声明的地址
    if (ctrl_peer_.valid()) {
        ctrl_sock_.sendto(pkt.data(), pkt.size(), ctrl_peer_);
    } else if (!sender_ip_.empty() && remote_ctrl_port_ > 0) {
        ctrl_sock_.sendto(pkt.data(), pkt.size(), sender_ip_.c_str(),
                          (uint16_t)remote_ctrl_port_);
    } else {
        return; // 没可用地址就不发（避免 sendto 抛错）
    }
    last_rr_send_us_ = now_us;
}

void RtpReceiver::receiver_worker() {
    // RTP 包 1500 MTU；AirPlay 音频每包 < 1400 字节，所以 4KB 足够
    uint8_t buf[4096];
    while (running_.load()) {
        std::vector<platform::Socket*> socks = { &data_sock_, &ctrl_sock_, &timing_sock_ };
        std::vector<size_t> ready;
        // select 100ms 超时 → 没数据时也能执行 maybe_send_rr / emit_ready / jbuffer 超时逻辑
        if (!platform::select_read(socks, ready, 100)) {
            platform::sleep_ms(20);
            maybe_send_rr();
            // 即使没有新包到达，也要定期检查"缺失包等待超时"，
            // 否则卡住的缺口要等下一个包到来才会触发跳包。
            emit_ready();
            continue;
        }
        for (size_t idx : ready) {
            platform::Socket* s = socks[idx];
            platform::SocketAddr from;
            auto r = s->recvfrom(buf, sizeof(buf), &from);
            if (!r.ok || r.bytes < 4) continue;

            // ======== data socket (RTP audio) ==================================
            if (idx == 0) {
                if (r.bytes < 12) continue;
                stats_.packets++;
                stats_.bytes += (uint64_t)r.bytes;
                uint8_t* p = buf;
                if ((p[0] & 0xC0) != 0x80) continue; // RTP V=2
                bool m = (p[1] & 0x80) != 0;
                uint8_t pt = p[1] & 0x7F;
                uint16_t seq = (uint16_t(p[2]) << 8) | p[3];
                uint32_t ts  = (uint32_t(p[4]) << 24) | (uint32_t(p[5]) << 16) |
                               (uint32_t(p[6]) << 8)  | p[7];
                uint32_t ssrc = (uint32_t(p[8]) << 24) | (uint32_t(p[9]) << 16) |
                                (uint32_t(p[10]) << 8) | p[11];
                uint8_t cc = p[0] & 0x0F;
                size_t hdr_len = 12 + cc * 4;
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

                // 解密（两种模式）：
                //   - AP2（SETUP bplist ekey/eiv）：AES-CBC，payload 从 0 起整块
                //     加密，最后不足 16B 的部分原样透传（UxPlay raop_buffer 同款）
                //   - AP1（ANNOUNCE SDP a=aeskey/a=aesiv）：AES-CTR 逐包
                if (cbc_ready_ && !pkt.payload.empty()) {
                    size_t enc = pkt.payload.size() / 16 * 16;
                    if (enc > 0) {
                        std::vector<uint8_t> plain(enc);
                        if (cbc_.decrypt_raw(cbc_iv_, pkt.payload.data(), enc, plain.data())) {
                            plain.insert(plain.end(), pkt.payload.begin() + enc, pkt.payload.end());
                            pkt.payload.swap(plain);
                        }
                    }
                } else if (aes_.is_ready() && !pkt.payload.empty()) {
                    aes_.process(pkt.payload.data(), pkt.payload.data(), pkt.payload.size());
                }

                // RFC 3550 A.8 Jitter 估算（粗略版）：
                //   D(i,j) = (Rj - Ri) - (Sj - Si)   （单位：采样时钟 tick）
                //   J(i)   = J(i-1) + (|D(i,i-1)| - J(i-1)) / 16
                // 由于我们不知道确切的采样时钟频率，简化为用"微秒差值"近似按 1/16 平滑，
                // 之后 jitter_est_ 直接填进 RR（AirPlay 不做精确校验）。
                if (last_arrival_us_ != 0) {
                    int64_t us_diff = (int64_t)(pkt.recv_us - last_arrival_us_);
                    int64_t ts_diff = (int64_t)((int32_t)(ts - (uint32_t)last_arrival_ts_));
                    // 用 44100Hz 近似把 ts_diff 转成微秒：ts_diff_us = ts_diff * 1e6 / 44100
                    // 简化：除以 44（1e6/44100 ≈ 22.67），取个整数近似就够了。
                    int64_t ts_diff_us = (ts_diff * 1000000LL) / 44100LL;
                    int64_t d = us_diff - ts_diff_us;
                    if (d < 0) d = -d;
                    jitter_est_ = jitter_est_ + (uint32_t)((d - (int64_t)jitter_est_) / 16);
                }
                last_arrival_ts_ = ts;
                last_arrival_us_ = pkt.recv_us;

                {
                    std::lock_guard<std::mutex> lk(jbuf_mu_);
                    if (has_started_) {
                        uint16_t d = seq_diff(seq, next_expected_seq_);
                        if ((int16_t)d < 0) {
                            stats_.reordered++;
                        }
                    }
                    if (jbuffer_.size() >= jbuf_max_) {
                        // 抖动缓冲满了：强制丢最旧的那个，记为丢包并推进
                        auto oldest = jbuffer_.begin();
                        AP2_LOGW("rtp: jbuffer full, drop oldest seq=%u", oldest->first);
                        stats_.lost++;
                        if (has_started_ && oldest->first == next_expected_seq_) {
                            next_expected_seq_++;
                        }
                        jbuffer_.erase(oldest);
                    }
                    jbuffer_[seq] = std::move(pkt);
                }
                emit_ready();
                // data 包处理完可能已经到了定期发 RR 的阈值
                maybe_send_rr();
            }
            // ======== control socket (RTCP) ====================================
            else if (idx == 1) {
                // 记录来自谁（RR 回给同一个 peer，能穿过 NAT）
                ctrl_peer_ = from;
                rtcp_.handle_packet(buf, (size_t)r.bytes);
            }
            // ======== timing socket (AirPlay timing) ==========================
            else if (idx == 2) {
                timing_peer_ = from;
                auto resp = timing_.handle_packet(buf, (size_t)r.bytes);
                if (!resp.empty()) {
                    // 优先发回给 recvfrom 的源地址
                    if (timing_peer_.valid()) {
                        timing_sock_.sendto(resp.data(), resp.size(), timing_peer_);
                    } else if (!sender_ip_.empty() && remote_timing_port_ > 0) {
                        timing_sock_.sendto(resp.data(), resp.size(),
                                            sender_ip_.c_str(),
                                            (uint16_t)remote_timing_port_);
                    }
                }
            }
        }
    }
}

} // namespace net
} // namespace airplay2
