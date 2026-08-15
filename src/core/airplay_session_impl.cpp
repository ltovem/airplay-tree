/*!
 * @file airplay_session_impl.cpp
 */
#include "airplay_session_impl.h"
#include "../platform/platform_log.h"
#include "../platform/platform_time.h"
#include <cstring>
#include <utility>     // std::swap（声道交错转换）

namespace airplay2 {

SessionImpl::SessionImpl(uint64_t id, ServerImpl* server,
                         IAudioRenderer* audio_renderer, IVideoRenderer* video_renderer)
    : id_(id), server_(server),
      audio_renderer_(audio_renderer), video_renderer_(video_renderer),
      pcm_buffer_(16384) {
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
    if (audio_renderer_) audio_renderer_->on_config(audio_cfg_);
    transition(AirPlaySession::State::READY);
}

void SessionImpl::start_streaming() {
    if (state_.load() == AirPlaySession::State::PLAYING) return;
    playback_stop_.store(false);
    rtp_.set_packet_callback([this](const net::RtpAudioPacket& p) { on_rtp_packet(p); });
    rtp_.start();
    playback_thread_.start([this] { playback_worker(); }, "ap2-playback");
    playing_.store(true);
    // 若会话已配置视频，也启动视频 RTP
    if (video_local_port_ != 0) start_video_streaming();
    transition(AirPlaySession::State::PLAYING);
    if (session_start_us_ == 0) session_start_us_ = platform::time_now_us();
    if (audio_renderer_) audio_renderer_->on_play();
    AP2_LOGI("session %lu: playback started", (unsigned long)id_);
}

void SessionImpl::pause_streaming() {
    if (!playing_.exchange(false)) return;
    transition(AirPlaySession::State::PAUSED);
    if (audio_renderer_) audio_renderer_->on_pause();
    if (video_renderer_ && video_playing_.load()) {
        VideoPlaybackCmd cmd; cmd.type = VideoPlaybackCmd::PAUSE;
        video_renderer_->on_playback(cmd);
    }
    AP2_LOGI("session %lu: paused", (unsigned long)id_);
}

void SessionImpl::resume_streaming() {
    if (playing_.load()) return;
    transition(AirPlaySession::State::PLAYING);
    playing_.store(true);
    if (audio_renderer_) audio_renderer_->on_play();
    if (video_renderer_ && video_playing_.load()) {
        VideoPlaybackCmd cmd; cmd.type = VideoPlaybackCmd::PLAY;
        cmd.rate = playback_rate_.load();
        video_renderer_->on_playback(cmd);
    }
}

void SessionImpl::stop_streaming() {
    playback_stop_.store(true);
    playing_.store(false);
    video_playing_.store(false);
    rtp_.stop();
    video_rtp_.stop();
    playback_thread_.stop_and_join();
    if (audio_renderer_) audio_renderer_->on_stop();
    if (video_renderer_) video_renderer_->on_stop();
    if (state_.load() != AirPlaySession::State::CLOSED)
        transition(AirPlaySession::State::IDLE);
}

void SessionImpl::flush_buffers() {
    rtp_.flush();
    video_rtp_.flush();
    pcm_buffer_.flush();
    alac_.reset();
    if (audio_renderer_) audio_renderer_->on_flush();
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
            if (got > 0 && audio_renderer_) {
                size_t bytes = got * bpf;
                audio_renderer_->on_pcm(tmp.data(), bytes, platform::time_now_us());
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
    if (remaining > 0 && audio_renderer_) {
        tmp.resize(remaining * bpf);
        size_t got = pcm_buffer_.read_frames(tmp.data(), remaining);
        if (got > 0) audio_renderer_->on_pcm(tmp.data(), got * bpf, platform::time_now_us());
    }
}

/* ================================================================
 *                    Video port allocation / configure
 * ================================================================ */
uint16_t SessionImpl::allocate_video_port(uint16_t port_min, uint16_t port_max,
                                           int remote_data_port) {
    uint16_t p = 0;
    if (!video_rtp_.open(port_min, port_max, p)) return 0;
    video_local_port_ = p;
    if (!client_addr_.empty() && remote_data_port > 0) {
        video_rtp_.set_remote_address(client_addr_, remote_data_port);
    }
    return p;
}

void SessionImpl::configure_video(const net::SdpInfo& sdp) {
    video_cfg_.codec = sdp.video_codec;
    video_cfg_.width  = (uint32_t)sdp.video_width;
    video_cfg_.height = (uint32_t)sdp.video_height;
    video_cfg_.fps_num = (uint32_t)sdp.video_fps;
    video_cfg_.fps_den = sdp.video_fps > 0 ? 1 : 0;
    video_rtp_.set_codec(sdp.video_codec);

    // 若 SDP video_fmtp 带 sprop-parameter-sets（H.264 SPS/PPS base64），解码保存
    // 简化：把 fmtp 直接作为 codec_extra（调用方也可自行解析）
    if (!sdp.video_fmtp.empty()) {
        video_cfg_.codec_extra.assign(sdp.video_fmtp.begin(), sdp.video_fmtp.end());
        video_rtp_.set_codec_data(video_cfg_.codec_extra);
    }
    // AES key 从同一 SDP 取（AirPlay 对音视频用同一把 key）
    if (!sdp.aes_key.empty() || !sdp.aes_iv.empty()) {
        video_rtp_.set_decryption_params(sdp.aes_key, sdp.aes_iv);
    }
    if (video_renderer_) video_renderer_->on_config(video_cfg_);
    AP2_LOGI("session %lu: video codec=%s w=%d h=%d fps=%d",
             (unsigned long)id_,
             (sdp.video_codec==VideoCodec::H264_AVC)?"H264":
             (sdp.video_codec==VideoCodec::H265_HEVC)?"H265":"MJPEG",
             sdp.video_width, sdp.video_height, sdp.video_fps);
}

void SessionImpl::start_video_streaming() {
    if (video_playing_.exchange(true)) return;
    video_rtp_.set_frame_callback([this](const VideoFrame& f){ on_video_frame(f); });
    video_rtp_.start();
    if (video_renderer_) {
        VideoPlaybackCmd c; c.type = VideoPlaybackCmd::PLAY; c.rate = playback_rate_.load();
        video_renderer_->on_playback(c);
    }
}

void SessionImpl::on_video_frame(const VideoFrame& f) {
    current_pos_sec_ = (double)f.pts_us / 1e6;
    if (video_renderer_) video_renderer_->on_frame(f);
}

/* ================================================================
 *              Playback control / URL pull
 * ================================================================ */
bool SessionImpl::play_url(const VideoPlaybackCmd& cmd) {
    current_pos_sec_ = cmd.start_pos_sec;
    if (video_renderer_) {
        return video_renderer_->on_url(cmd);
    }
    return true;
}
void SessionImpl::set_rate(double rate) {
    playback_rate_.store(rate);
    VideoPlaybackCmd cmd;
    cmd.type = std::abs(rate) < 1e-9 ? VideoPlaybackCmd::PAUSE : VideoPlaybackCmd::PLAY;
    cmd.rate = rate;
    cmd.start_pos_sec = current_pos_sec_;
    if (audio_renderer_) {
        if (cmd.type == VideoPlaybackCmd::PAUSE) audio_renderer_->on_pause();
        else audio_renderer_->on_play();
    }
    if (video_renderer_) video_renderer_->on_playback(cmd);
    if (cmd.type == VideoPlaybackCmd::PAUSE) pause_streaming();
    else {
        auto s = state_.load();
        if (s == AirPlaySession::State::PAUSED) resume_streaming();
    }
}
void SessionImpl::seek(double pos_sec) {
    current_pos_sec_ = pos_sec;
    VideoPlaybackCmd cmd;
    cmd.type = VideoPlaybackCmd::SEEK;
    cmd.start_pos_sec = pos_sec;
    if (video_renderer_) video_renderer_->on_playback(cmd);
}

} // namespace airplay2
