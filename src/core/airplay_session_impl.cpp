/*!
 * @file airplay_session_impl.cpp
 */
#include "airplay_session_impl.h"
#include "../platform/platform_log.h"
#include "../platform/platform_time.h"
#include <cstring>

namespace airplay2 {

SessionImpl::SessionImpl(uint64_t id, ServerImpl* server, IAudioRenderer* renderer)
    : id_(id), server_(server), renderer_(renderer), pcm_buffer_(16384) {
    state_.store(AirPlaySession::State::CONNECTED);
}

SessionImpl::~SessionImpl() {
    stop_streaming();
}

SessionStats SessionImpl::stats() const {
    std::lock_guard<std::mutex> lk(stats_mu_);
    SessionStats s = stats_;
    s.session_duration_sec =
        session_start_us_ ? (platform::time_now_us() - session_start_us_) / 1e6 : 0.0;
    auto rstat = rtp_.stats();
    s.packets_received = rstat.packets;
    s.packets_lost     = rstat.lost;
    s.bytes_received   = rstat.bytes;
    s.client_ip   = client_addr_;
    s.client_name = client_name_;
    return s;
}

bool SessionImpl::allocate_ports(int remote_ports[3], uint16_t port_min, uint16_t port_max,
                                  int local_ports[3]) {
    if (!rtp_.open(port_min, port_max, rtp_local_ports_)) {
        AP2_LOGE("session: could not allocate RTP ports in %u-%u", port_min, port_max);
        return false;
    }
    // 把 SETUP 里拿到的 client_ip + client_port 三元组传给 RtpReceiver，
    // 供 RR / timing response 回送时使用（穿过 NAT）。
    if (!client_addr_.empty() && remote_ports) {
        rtp_.set_remote_address(client_addr_, remote_ports);
    }
    for (int i = 0; i < 3; ++i) local_ports[i] = rtp_local_ports_[i];
    return true;
}

void SessionImpl::configure_audio(const net::SdpInfo& sdp) {
    codec_mode_ = sdp.audio_mode;
    audio_cfg_.sample_rate = (uint32_t)sdp.sample_rate;
    audio_cfg_.channels    = (uint8_t)sdp.channels;
    first_sample_rate_     = audio_cfg_.sample_rate;
    AP2_LOGI("session %lu: configure audio %s sr=%u ch=%u",
             (unsigned long)id_, codec_mode_.c_str(),
             audio_cfg_.sample_rate, audio_cfg_.channels);

    // 若 SDP 里携带 AES key/iv，先配置给 RtpReceiver，后续 RTP 包到了就自动解密
    if (!sdp.aes_key.empty() || !sdp.aes_iv.empty()) {
        rtp_.set_decryption_params(sdp.aes_key, sdp.aes_iv);
    }

    // Default output format and decoder
    if (!codec_mode_.empty()) {
        std::string lower = codec_mode_;
        for (char& c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower.find("applelossless") != std::string::npos || lower == "alac") {
            codec::AlacMagicCookie cookie;
            if (!sdp.fmtp.empty() && codec::parse_alac_fmtp(sdp.fmtp, cookie)) {
                cookie.sample_rate = audio_cfg_.sample_rate;
                cookie.num_channels = audio_cfg_.channels;
                alac_.configure(cookie);
                audio_cfg_ = alac_.output_config();
            } else {
                cookie.sample_rate = audio_cfg_.sample_rate;
                cookie.num_channels = audio_cfg_.channels;
                cookie.bit_depth = 16;
                alac_.configure(cookie);
                audio_cfg_ = alac_.output_config();
            }
        } else if (lower.find("l16") != std::string::npos || lower == "pcm") {
            audio_cfg_.format = AudioFormat::PCM16LE;
        } else if (lower.find("mpeg4") != std::string::npos || lower == "aac") {
            // For AAC: output PCM16 stub (user can extend with external decoder)
            audio_cfg_.format = AudioFormat::PCM16LE;
        }
    }
    pcm_buffer_.set_config(audio_cfg_);
    if (renderer_) renderer_->on_config(audio_cfg_);
    transition(AirPlaySession::State::READY);
}

void SessionImpl::start_streaming() {
    if (state_.load() == AirPlaySession::State::PLAYING) return;
    playback_stop_.store(false);
    rtp_.set_packet_callback([this](const net::RtpAudioPacket& p) { on_rtp_packet(p); });
    rtp_.start();
    playback_thread_.start([this] { playback_worker(); }, "ap2-playback");
    playing_.store(true);
    transition(AirPlaySession::State::PLAYING);
    if (session_start_us_ == 0) session_start_us_ = platform::time_now_us();
    if (renderer_) renderer_->on_play();
    AP2_LOGI("session %lu: playback started", (unsigned long)id_);
}

void SessionImpl::pause_streaming() {
    if (!playing_.exchange(false)) return;
    transition(AirPlaySession::State::PAUSED);
    if (renderer_) renderer_->on_pause();
    AP2_LOGI("session %lu: paused", (unsigned long)id_);
}

void SessionImpl::resume_streaming() {
    if (playing_.load()) return;
    transition(AirPlaySession::State::PLAYING);
    playing_.store(true);
    if (renderer_) renderer_->on_play();
}

void SessionImpl::stop_streaming() {
    playback_stop_.store(true);
    playing_.store(false);
    rtp_.stop();
    playback_thread_.stop_and_join();
    if (renderer_) renderer_->on_stop();
    if (state_.load() != AirPlaySession::State::CLOSED)
        transition(AirPlaySession::State::IDLE);
}

void SessionImpl::flush_buffers() {
    rtp_.flush();
    pcm_buffer_.flush();
    alac_.reset();
    if (renderer_) renderer_->on_flush();
}

void SessionImpl::on_rtp_packet(const net::RtpAudioPacket& pkt) {
    std::vector<uint8_t> pcm;
    size_t samples = 0;
    std::string lower_mode = codec_mode_;
    for (char& c : lower_mode) c = (char)std::tolower((unsigned char)c);

    if (alac_.is_configured() &&
        (lower_mode.find("applelossless") != std::string::npos || lower_mode == "alac")) {
        int64_t used = alac_.decode_frame(pkt.payload.data(), pkt.payload.size(), pcm);
        if (used < 0) {
            AP2_LOGW("session %lu: alac decode failed", (unsigned long)id_);
            return;
        }
        (void)used;
        samples = pcm_buffer_.write_bytes(pcm.data(), pcm.size());
    } else if (lower_mode.find("l16") != std::string::npos || lower_mode == "pcm") {
        // Raw PCM 16-bit big-endian? AirPlay L16 is usually BE.
        pcm = pkt.payload;
        if (pcm.size() >= 2) {
            // Endian swap LE vs BE detection
            // Many senders use network byte order (BE), convert to LE:
            for (size_t i = 0; i + 1 < pcm.size(); i += 2) {
                std::swap(pcm[i], pcm[i + 1]);
            }
        }
        samples = pcm_buffer_.write_bytes(pcm.data(), pcm.size());
    } else {
        // Unknown: pass-through for now
        samples = pcm_buffer_.write_bytes(pkt.payload.data(), pkt.payload.size());
    }
    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.audio_frames_decoded += samples;
    }
    (void)pkt;
}

void SessionImpl::playback_worker() {
    // Periodically drain pcm_buffer_ into renderer.
    // We aim for ~20ms chunks at current sample rate.
    size_t bpf = pcm_buffer_.bytes_per_frame();
    std::vector<uint8_t> tmp;
    while (!playback_stop_.load()) {
        if (!playing_.load()) {
            platform::sleep_ms(50);
            continue;
        }
        size_t avail = pcm_buffer_.available_frames();
        size_t want = audio_cfg_.sample_rate / 50; // 20ms
        if (avail >= want) {
            tmp.resize(want * bpf);
            size_t got = pcm_buffer_.read_frames(tmp.data(), want);
            if (got > 0 && renderer_) {
                size_t bytes = got * bpf;
                renderer_->on_pcm(tmp.data(), bytes, platform::time_now_us());
            }
        } else if (avail == 0) {
            platform::sleep_ms(10);
            continue;
        } else {
            platform::sleep_ms(5);
            continue;
        }
    }
    // Drain remaining on stop
    size_t remaining = pcm_buffer_.available_frames();
    if (remaining > 0 && renderer_) {
        tmp.resize(remaining * bpf);
        size_t got = pcm_buffer_.read_frames(tmp.data(), remaining);
        if (got > 0) renderer_->on_pcm(tmp.data(), got * bpf, platform::time_now_us());
    }
}

} // namespace airplay2
