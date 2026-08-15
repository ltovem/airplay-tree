/*!
 * @file airplay_session_impl.cpp
 */
#include "airplay_session_impl.h"
#include "airplay_server_impl.h"
#include "../platform/platform_log.h"
#include "../platform/platform_time.h"
#include "../crypto/fairplay_sap.h"
#include "../crypto/sha512.h"
#include <cstring>
#include <cstdio>      // std::snprintf（视频密钥派生字符串）
#include <cmath>       // std::abs（播放速率判断）
#include <utility>     // std::swap（声道交错转换）

namespace airplay2 {

// ISO 14496-3 AudioSpecificConfig（AAC-ELD, audioObjectType=39）构造。
// AP2 镜像音频（ct=8）的 SETUP 不带 RFC 3640 fmtp，渲染器必须靠 config=
// 创建解码器；按规范把 ELD 的 ASC 拼成 3 字节十六进制字符串。
//   bit layout: 11111(escape) 000111(39-32=7) SFI(4) CC(4) frmLenFlag(1) depCore(1) extFlag(1)
//   SFI 表: 96000=0 88200=1 64000=2 48000=3 44100=4 32000=5 24000=6 22050=7 16000=8 12000=9 11025=10 8000=11 7350=12
static std::string build_eld_asc(uint32_t sample_rate, uint32_t channels) {
    static const uint32_t kSfiTable[] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
    uint32_t sfi = 4; // 默认 44100
    for (size_t i = 0; i < sizeof(kSfiTable)/sizeof(kSfiTable[0]); ++i) {
        if (sample_rate == kSfiTable[i]) { sfi = (uint32_t)i; break; }
    }
    if (channels == 0) channels = 2;
    uint32_t cc = (channels > 7) ? 2 : channels; // channelConfiguration 最多 4bit（7 声道规范内）
    // 逐位拼（27 位，含 escape 的 11 位 + SFI + CC + 3 个 flag）
    uint64_t bits = 0; int n = 0;
    auto put = [&](uint64_t v, int w) { bits = (bits << w) | (v & ((1ULL<<w)-1)); n += w; };
    put(31, 5);              // audioObjectType escape → 31
    put(39 - 32, 6);         // AOT 39 (ELD)
    put(sfi, 4);
    put(cc, 4);
    put(0, 1);               // frameLengthFlag = 0（480 samples/frame）
    put(0, 1);               // dependsOnCoreCoder
    put(0, 1);               // extensionFlag
    while (n % 8) { bits <<= 1; ++n; } // 补零到字节对齐
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%02X%02X%02X",
                  (unsigned)((bits >> 16) & 0xFF), (unsigned)((bits >> 8) & 0xFF),
                  (unsigned)(bits & 0xFF));
    return std::string(hex);
}

void SessionImpl::derive_media_keys() {
    if (!server_) return;
    media_keys_ready_ = false;
    auto keymsg = server_->fairplay_keymsg(id_);
    if (keymsg.size() < 164 || ap2_ekey_.size() < 72 || ap2_eiv_.size() < 16) {
        AP2_LOGW("session %lu: media keys unavailable (keymsg=%zu ekey=%zu eiv=%zu)",
                 (unsigned long)id_, keymsg.size(), ap2_ekey_.size(), ap2_eiv_.size());
        return;
    }
    uint8_t raw[16];
    if (!crypto::fairplay_sap_decrypt(keymsg.data(), ap2_ekey_.data(), raw)) {
        AP2_LOGW("session %lu: fairplay_sap_decrypt failed", (unsigned long)id_);
        return;
    }
    // 音频最终密钥 = SHA512(raw || ecdh_secret) 前 16B（UxPlay 同款）
    auto ecdh = server_->pairing().ecdh_secret(id_);
    crypto::Sha512 h;
    h.update(raw, 16);
    if (!ecdh.empty()) h.update(ecdh.data(), ecdh.size());
    uint8_t digest[64];
    h.final(digest);
    std::memcpy(audio_key_, digest, 16);

    media_keys_ready_ = true;
    AP2_LOGI("session %lu: media keys derived (audio CBC + video CTR, connID=%llu)",
             (unsigned long)id_, (unsigned long long)stream_connection_id_);
    // 音频密钥已就绪，视频密钥依赖 streamConnectionID（SETUP(110) 提供）；
    // 若 ID 已拿到就一并派生，否则等 update_video_media_key() 补。
    if (stream_connection_id_ != 0) update_video_media_key();
}

void SessionImpl::update_video_media_key() {
    // 视频 key/iv = SHA512("AirPlayStreamKey/IV{id}" || audio_key) 前 16B
    // （UxPlay mirror_buffer_init_aes 同款）。镜像流程里 SETUP(110) 常晚于
    // RECORD 到达，streamConnectionID 只有在 SETUP(110) 才有——所以 RECORD
    // 时派生的是错 key（ID=0），拿到真 ID 后必须在这里重算并重配解密。
    if (!media_keys_ready_ || stream_connection_id_ == 0) return;
    char kb[64], ib[64];
    std::snprintf(kb, sizeof(kb), "AirPlayStreamKey%llu",
                  (unsigned long long)stream_connection_id_);
    std::snprintf(ib, sizeof(ib), "AirPlayStreamIV%llu",
                  (unsigned long long)stream_connection_id_);
    uint8_t digest[64];
    crypto::Sha512 k, v;
    k.update((const uint8_t*)kb, std::strlen(kb));
    k.update(audio_key_, 16);
    k.final(digest);
    std::memcpy(video_key_, digest, 16);
    v.update((const uint8_t*)ib, std::strlen(ib));
    v.update(audio_key_, 16);
    v.final(digest);
    std::memcpy(video_iv_, digest, 16);
    if (video_local_port_ != 0)
        video_rtp_.set_decryption_key(video_key_, video_iv_);
    AP2_LOGI("session %lu: video media key updated (connID=%llu)",
             (unsigned long)id_, (unsigned long long)stream_connection_id_);
}

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
    // 幂等：端口已分配（SETUP 1 预分配）时直接复用，避免 AP2 多次 SETUP
    // （空流 SETUP / 音频 96 / 镜像 110）每次返回不同端口导致 iOS 混乱。
    if (rtp_local_ports_[0] != 0) {
        for (int i = 0; i < 3; ++i) local_ports[i] = rtp_local_ports_[i];
        return true;
    }
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
        } else if (lower.find("mpeg4") != std::string::npos || lower.find("aac") != std::string::npos) {
            // AAC / AAC-ELD：压缩帧原样透传给渲染器解码（demo 用 AudioConverter）。
            // fmtp 里带 config= AudioSpecificConfig，渲染器需要它来建解码器。
            codec_mode_ = "mpeg4-generic";
            audio_cfg_.format = AudioFormat::AAC_ELD;
            if (audio_renderer_) {
                audio_renderer_->on_compressed_config(codec_mode_, sdp.fmtp, audio_cfg_);
            }
        }
    }
    pcm_buffer_.set_config(audio_cfg_);
    if (audio_renderer_) audio_renderer_->on_config(audio_cfg_);
    transition(AirPlaySession::State::READY);
}

void SessionImpl::configure_ap2_audio(uint64_t ct, uint64_t spf, uint64_t sr) {
    // AP2 纯音频（音乐投送）没有 ANNOUNCE，编解码参数全在 SETUP stream dict：
    //   ct=2 → ALAC（spf=352，44.1kHz）；ct=8 → AAC-ELD（镜像音频，压缩透传）。
    // 与 UxPlay raop_handler_setup case 96 + audio_get_format 的映射一致。
    audio_cfg_.sample_rate = (sr > 0) ? (uint32_t)sr : 44100;
    audio_cfg_.channels    = 2;
    first_sample_rate_     = audio_cfg_.sample_rate;

    if (ct == 2) {
        codec_mode_ = "alac";
        codec::AlacMagicCookie cookie;
        cookie.sample_rate  = audio_cfg_.sample_rate;
        cookie.num_channels = audio_cfg_.channels;
        cookie.bit_depth    = 16;
        cookie.frame_length = (spf > 0) ? (int)spf : 4096;
        alac_.configure(cookie);
        audio_cfg_ = alac_.output_config();
    } else {
        // AAC-ELD / 其他：压缩帧透传给渲染器（demo 的 AudioConverter 解码）。
        // AP2 镜像音频（ct=8 AAC-ELD）没有 ANNOUNCE/fmtp，渲染器拿不到
        // RFC 3640 的 config=，导致解码器无法创建（日志 "no config in fmtp"）。
        // 这里按 ISO 14496-3 手工构造 ELD AudioSpecificConfig 补上：
        //   bits: 11111(escape) 000111(AOT=39 ELD) SFI(4) CC(4)
        //         frameLengthFlag(1) dependsOnCoreCoder(1) extensionFlag(1)
        codec_mode_ = "mpeg4-generic";
        audio_cfg_.format = AudioFormat::AAC_ELD;
        std::string config_hex = build_eld_asc(audio_cfg_.sample_rate, audio_cfg_.channels);
        std::string fmtp = "config=" + config_hex;
        if (audio_renderer_) {
            audio_renderer_->on_compressed_config(codec_mode_, fmtp, audio_cfg_);
        }
    }
    pcm_buffer_.set_config(audio_cfg_);
    if (audio_renderer_) audio_renderer_->on_config(audio_cfg_);
    // AP2 里 RECORD 可能先于带流 SETUP 到达：播放已启动但渲染队列尚未创建，
    // 这里补一次 on_play，否则 AudioQueue 建好后永远不 start → 没声音。
    if (playing_.load() && audio_renderer_) audio_renderer_->on_play();
    AP2_LOGI("session %lu: AP2 audio ct=%llu spf=%llu sr=%llu -> %s (out %u Hz %u ch)",
             (unsigned long)id_, (unsigned long long)ct, (unsigned long long)spf,
             (unsigned long long)sr, codec_mode_.c_str(),
             audio_cfg_.sample_rate, audio_cfg_.channels);
}

void SessionImpl::start_streaming() {
    if (state_.load() == AirPlaySession::State::PLAYING) return;
    playback_stop_.store(false);
    rtp_.set_packet_callback([this](const net::RtpAudioPacket& p) { on_rtp_packet(p); });
    // AP2 路径：SETUP 带了 ekey/eiv → 派生媒体密钥并配置 RTP 解密
    if (ap2_ekey_.size() >= 72) {
        derive_media_keys();
        if (media_keys_ready_) {
            rtp_.set_cbc_decryption(audio_key_, ap2_eiv_.data());
            if (video_local_port_ != 0)
                video_rtp_.set_decryption_key(video_key_, video_iv_);
        }
    }
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
    // 诊断：打印前几个包走哪个分支，确认 codec_mode_ 时序是否正确
    if (pkt_count_ < 3) {
        AP2_LOGI("session %lu: rtp pkt#%llu mode='%s' len=%zu", (unsigned long)id_,
                 (unsigned long long)pkt_count_, codec_mode_.c_str(), pkt.payload.size());
    }
    ++pkt_count_;

    if (alac_.is_configured() &&
        (lower_mode.find("applelossless") != std::string::npos || lower_mode == "alac")) {
        // UxPlay：ALAC 流开始时会先发几个 44 字节包（12B RTP 头 + 32B 加密负载），
        // 解密后是 ALAC 格式信息而非音频数据，必须跳过，否则解码器会报错刷屏。
        if (pkt.payload.size() == 32) return;
        int64_t used = alac_.decode_frame(pkt.payload.data(), pkt.payload.size(), pcm);
        if (used < 0) {
            AP2_LOGW("session %lu: alac decode failed", (unsigned long)id_);
            return;
        }
        (void)used;
        samples = pcm_buffer_.write_bytes(pcm.data(), pcm.size());
    } else if (lower_mode.find("mpeg4") != std::string::npos ||
               lower_mode.find("aac") != std::string::npos) {
        // AAC-ELD / AAC：压缩帧原样透传给渲染器（demo 用 AudioConverter 解码），
        // 不走 PCM 缓冲。未配置解码器时渲染器内部会丢弃。
        // UxPlay：AAC-ELD 流开头的 4 字节 "no_data_marker"(0x00 0x68 0x34 0x00)
        // 替换了 payload 的包不是音频，必须跳过，否则解码器吃垃圾。
        if (pkt.payload.size() == 4 &&
            pkt.payload[0] == 0x00 && pkt.payload[1] == 0x68 &&
            pkt.payload[2] == 0x34 && pkt.payload[3] == 0x00) {
            return;
        }
        if (audio_renderer_) {
            audio_renderer_->on_compressed_audio(pkt.payload.data(), pkt.payload.size(),
                                                 pkt.recv_us);
        }
        return;
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
                                           int remote_data_port, bool use_tcp) {
    uint16_t p = 0;
    bool ok = use_tcp ? video_rtp_.open_tcp(port_min, port_max, p)
                      : video_rtp_.open(port_min, port_max, p);
    if (!ok) return 0;
    video_local_port_ = p;
    if (!client_addr_.empty() && remote_data_port > 0) {
        video_rtp_.set_remote_address(client_addr_, remote_data_port);
    }
    // 若密钥已派生（RECORD 已过），把正确的视频解密密钥配给接收器
    if (media_keys_ready_) video_rtp_.set_decryption_key(video_key_, video_iv_);
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
