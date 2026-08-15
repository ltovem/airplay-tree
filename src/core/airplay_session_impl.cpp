/*!
 * @file airplay_session_impl.cpp
 */
#include "airplay_session_impl.h"
#include "../platform/platform_log.h"
#include "../platform/platform_time.h"
#include "../util/plist.h"   // util::base64_decode（SDP sprop-parameter-sets）
#include <cstring>
#include <utility>     // std::swap（声道交错转换）
#include <vector>
#include <string>

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
            // AAC-ELD（屏幕镜像常用）：库没有内置 AAC 解码器，把原始压缩帧
            // 透传给渲染器（AudioToolbox / MediaCodec / FFmpeg）自行解码。
            // 这里仍把 audio_cfg_ 置为 PCM16LE，保证 pcm_buffer_ 配置有效；
            // 真正的压缩参数通过 on_compressed_config 告知渲染器。
            aac_compressed_ = true;
            audio_cfg_.format = AudioFormat::PCM16LE;
            if (audio_renderer_) {
                audio_renderer_->on_compressed_config(codec_mode_, sdp.fmtp, audio_cfg_);
            }
            AP2_LOGI("session %lu: AAC compressed passthrough enabled (fmtp=%s)",
                     (unsigned long)id_, sdp.fmtp.c_str());
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

    if (aac_compressed_) {
        // AAC-ELD：原样透传给渲染器解码（不写 PCM 缓冲，播放器由渲染器自管）
        if (audio_renderer_) {
            audio_renderer_->on_compressed_audio(pkt.payload.data(),
                                                 pkt.payload.size(), pkt.timestamp);
        }
        samples = pkt.payload.size();
    } else if (alac_.is_configured() &&
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

/*!
 * @brief 解析 fmtp 中的 sprop-parameter-sets（H.264）或 sprop-vps/sps/pps（H.265）
 *
 * AirPlay 屏幕镜像的 SDP 视频 fmtp 形如：
 *   a=fmtp:97 profile-level-id=42e01f;sprop-parameter-sets=Z0LAH5oBQBboQ==,aM4xEg==
 * SPS/PPS 是 base64 编码的裸 NAL 负载，需要逐个解码并包上 Annex-B 起始码，
 * 组成 CMFormatDescription 需要的参数集字节流（codec_extra）。
 *
 * @param fmtp  video_fmtp 原串（可能含多个 key=value; 段）
 * @param codec 视频编码类型（决定参数集 key 名）
 * @param out   Annex-B 拼接结果（追加到尾部；未解析到时保持原样）
 */
static void parse_sprop_parameter_sets(const std::string& fmtp,
                                       airplay2::VideoCodec codec,
                                       std::vector<uint8_t>& out) {
    using airplay2::util::base64_decode;
    // 切分 ';' 段，每段 "key=value"（值可能带引号）
    std::vector<std::pair<std::string, std::string>> kvs;
    size_t pos = 0;
    while (pos <= fmtp.size()) {
        size_t semi = fmtp.find(';', pos);
        std::string seg = fmtp.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        size_t eq = seg.find('=');
        if (eq != std::string::npos) {
            std::string k = seg.substr(0, eq);
            std::string v = seg.substr(eq + 1);
            // 去空白与首尾引号
            auto trim = [](std::string& s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '"')) s.erase(s.begin());
                while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '"')) s.pop_back();
            };
            trim(k); trim(v);
            if (!k.empty()) kvs.emplace_back(std::move(k), std::move(v));
        }
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }

    auto append_nal = [&out](const std::string& b64) {
        std::vector<uint8_t> nal;
        if (!base64_decode(b64, nal) || nal.empty()) return;
        // Annex-B start code（4 字节）
        out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
        out.insert(out.end(), nal.begin(), nal.end());
    };

    if (codec == airplay2::VideoCodec::H265_HEVC) {
        // 顺序：VPS → SPS → PPS
        for (const char* key : {"sprop-vps", "sprop-sps", "sprop-pps"}) {
            for (auto& kv : kvs) {
                if (kv.first != key) continue;
                size_t start = 0;
                while (start <= kv.second.size()) {
                    size_t comma = kv.second.find(',', start);
                    std::string b64 = kv.second.substr(
                        start, comma == std::string::npos ? std::string::npos : comma - start);
                    append_nal(b64);
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
            }
        }
    } else {
        // H.264 / MJPEG：sprop-parameter-sets=...,...
        for (auto& kv : kvs) {
            if (kv.first != "sprop-parameter-sets") continue;
            size_t start = 0;
            while (start <= kv.second.size()) {
                size_t comma = kv.second.find(',', start);
                std::string b64 = kv.second.substr(
                    start, comma == std::string::npos ? std::string::npos : comma - start);
                append_nal(b64);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
    }
}

void SessionImpl::configure_video(const net::SdpInfo& sdp) {
    video_cfg_.codec = sdp.video_codec;
    video_cfg_.width  = (uint32_t)sdp.video_width;
    video_cfg_.height = (uint32_t)sdp.video_height;
    video_cfg_.fps_num = (uint32_t)sdp.video_fps;
    video_cfg_.fps_den = sdp.video_fps > 0 ? 1 : 0;
    video_rtp_.set_codec(sdp.video_codec);

    // 若 SDP video_fmtp 带 sprop-parameter-sets（H.264 SPS/PPS base64），
    // 解码为 Annex-B 字节流存进 codec_extra，供渲染器直接构建格式描述符；
    // 若没有（部分发送端只在 RTP 里带内发送参数集），则留空走"带内缓存"回退。
    if (!sdp.video_fmtp.empty()) {
        video_cfg_.codec_extra.clear();
        parse_sprop_parameter_sets(sdp.video_fmtp, sdp.video_codec, video_cfg_.codec_extra);
        video_rtp_.set_codec_data(video_cfg_.codec_extra);
        if (!video_cfg_.codec_extra.empty()) {
            AP2_LOGI("session %lu: video fmtp sprop decoded -> %zu bytes codec_extra",
                     (unsigned long)id_, video_cfg_.codec_extra.size());
        }
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
