/*!
 * @file airplay_session_impl.cpp
 */
#include "airplay_session_impl.h"
#include "airplay_server_impl.h"
#include "../platform/platform_log.h"
#include "../platform/platform_time.h"
#include "../platform/platform_socket.h"
#include "../crypto/fairplay_sap.h"
#include "../crypto/sha512.h"
#include <cstring>
#include <cstdio>      // std::snprintf（视频密钥派生字符串）
#include <cmath>       // std::abs（播放速率判断）
#include <utility>     // std::swap（声道交错转换）

namespace airplay2 {

// ===== AirPlay 镜像时钟同步（NTP 客户端，RPiPlay raop_rtp_mirror 同款）=====
// 接收端每 ~3 秒向 iPhone 的 timing 端口发一个 48 字节标准 NTP 客户端请求
// （VN=4, Mode=3, transmit timestamp 在 offset 40），iPhone 回一个 NTP
// 服务器响应；T2（Receive Timestamp, offset 32）即"iPhone 收到我们请求的
// 时刻"，用它把视频帧 NTP 时间戳换算成我们的时钟（A/V 同步）。
// 不做这个同步，iOS 等不到时钟校准会 ~30 秒后自行 TEARDOWN 会话。
namespace {

// NTP epoch（1900-01-01）与 Unix epoch（1970-01-01）秒差
constexpr uint64_t kNtpUnixOffsetSec = 2208988800ULL;

// 把"自 1900 epoch 的微秒数"写成 64 位 NTP 时间戳（秒<<32|分数，大端）
void put_ntp_ts(uint8_t* p, uint64_t us_since_1900) {
    uint64_t secs = us_since_1900 / 1000000ULL;
    uint64_t frac = ((us_since_1900 % 1000000ULL) << 32) / 1000000ULL;
    for (int i = 3; i >= 0; --i) p[i] = (uint8_t)((secs >> (8 * i)) & 0xFF);
    for (int i = 3; i >= 0; --i) p[4 + i] = (uint8_t)((frac >> (8 * i)) & 0xFF);
}

// 读 64 位 NTP 时间戳 → "自 1900 epoch 的微秒数"
uint64_t read_ntp_ts(const uint8_t* p) {
    uint64_t secs = 0, frac = 0;
    for (int i = 0; i < 4; ++i) secs = (secs << 8) | p[i];
    for (int i = 0; i < 4; ++i) frac = (frac << 8) | p[4 + i];
    return secs * 1000000ULL + (frac * 1000000ULL) / 0x100000000ULL;
}

} // namespace

// ISO 14496-3 AudioSpecificConfig（AAC-ELD, audioObjectType=39）构造。
// AP2 镜像音频（ct=8）的 SETUP 不带 RFC 3640 fmtp，渲染器必须靠 config=
// 创建解码器。
// 实测（AudioToolbox）：f8e85000 能被接受并解码（UxPlay GStreamer 同款
// codec_data）；f8e840 会被 AudioConverter 拒（'bada'）。spf=480 与
// frameLengthFlag 的关系 AudioToolbox 内部自行处理，这里复用 UxPlay 验证值。
static std::string build_eld_asc(uint32_t sample_rate, uint32_t channels) {
    (void)sample_rate; (void)channels;
    return "f8e85000";
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
            aac_fmtp_.clear();  // 切到 ALAC：清掉可能残留的 AAC fmtp
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
            aac_fmtp_.clear();
            audio_cfg_.format = AudioFormat::PCM16LE;
        } else if (lower.find("mpeg4") != std::string::npos || lower.find("aac") != std::string::npos) {
            // AAC / AAC-ELD（屏幕镜像音频）：库内置解码器解码。
            // fmtp 里带 config= AudioSpecificConfig，解码器需要它初始化。
            codec_mode_ = "mpeg4-generic";
            audio_cfg_.format = AudioFormat::AAC_ELD;
            aac_fmtp_ = sdp.fmtp;
        }
    }
    // 引擎（AVAudioEngine/player）对所有 codec 都要建（含 ALAC/AAC/PCM）。
    pcm_buffer_.set_config(audio_cfg_);
    if (audio_renderer_) audio_renderer_->on_config(audio_cfg_);
    // 初始化库内置 AAC 解码器（协商到 AAC 时；ALAC/PCM 分支已清空 aac_fmtp_）。
    if (!aac_fmtp_.empty()) {
        aac_.configure(aac_fmtp_, audio_cfg_.sample_rate, audio_cfg_.channels,
                       /*is_eld=*/audio_cfg_.format == AudioFormat::AAC_ELD);
    }
    transition(AirPlaySession::State::READY);
}

void SessionImpl::configure_ap2_audio(uint64_t ct, uint64_t spf, uint64_t sr) {
    // AP2 纯音频（音乐投送）没有 ANNOUNCE，编解码参数全在 SETUP stream dict：
    //   ct=2 → ALAC（spf=352，44.1kHz）；ct=8 → AAC-ELD（镜像音频，库内解码）。
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
        // AAC-ELD / 其他（屏幕镜像音频）：库内置解码器解码。
        // AP2 镜像音频（ct=8 AAC-ELD）没有 ANNOUNCE/fmtp，解码器拿不到
        // RFC 3640 的 config=，导致无法初始化。这里按 ISO 14496-3 手工
        // 构造 ELD AudioSpecificConfig 补上：
        //   bits: 11111(escape) 000111(AOT=39 ELD) SFI(4) CC(4)
        //         frameLengthFlag(1) dependsOnCoreCoder(1) extensionFlag(1)
        codec_mode_ = "mpeg4-generic";
        audio_cfg_.format = AudioFormat::AAC_ELD;
        std::string config_hex = build_eld_asc(audio_cfg_.sample_rate, audio_cfg_.channels);
        aac_fmtp_ = "config=" + config_hex;
    }
    // 引擎（AVAudioEngine/player）对所有 codec 都要建——必须放在通用尾部，
    // 否则 ct=2（ALAC 纯音频）不建引擎 → 没声音。
    pcm_buffer_.set_config(audio_cfg_);
    if (audio_renderer_) audio_renderer_->on_config(audio_cfg_);
    // 初始化库内置 AAC 解码器（若本轮协商的是 AAC；ALAC/PCM 则清除）。
    // 注意必须先于 on_config 之后的任何解码调用；aac_ 自身在 configure
    // 内部会先 reset 旧实例，因此重复 SETUP 也安全。
    if (!aac_fmtp_.empty()) {
        aac_.configure(aac_fmtp_, audio_cfg_.sample_rate, audio_cfg_.channels,
                       /*is_eld=*/audio_cfg_.format == AudioFormat::AAC_ELD);
    }
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
    // 镜像时钟同步：拿到客户端 timing 端口后周期发 NTP 请求（防止 iOS 自断）
    if (client_timing_port_ != 0 && !timing_stop_.load()) {
        timing_thread_.start([this] { timing_worker(); }, "ap2-timing");
    }
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
    // 停掉镜像时钟同步线程
    if (timing_stop_.exchange(true) == false) {
        timing_thread_.stop_and_join();
    }
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
    aac_.reset();
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
        // AAC-ELD / AAC（屏幕镜像音频）：用库内置解码器解成 PCM，与 ALAC
        // 共用同一条 pcm_buffer_ → playback_worker → on_pcm 播放链路。
        // UxPlay：AAC-ELD 流开头的 4 字节 "no_data_marker"(0x00 0x68 0x34 0x00)
        // 替换了 payload 的包不是音频，必须跳过，否则解码器吃垃圾。
        if (pkt.payload.size() == 4 &&
            pkt.payload[0] == 0x00 && pkt.payload[1] == 0x68 &&
            pkt.payload[2] == 0x34 && pkt.payload[3] == 0x00) {
            return;
        }
        if (!aac_.is_configured()) return;
        int64_t used = aac_.decode_frame(pkt.payload.data(), pkt.payload.size(), pcm);
        if (used < 0) return;
        (void)used;
        // 诊断（临时）：每 300 个 AAC 包打印一次，确认解码调用是否持续
        static uint64_t dbg_aac_call = 0;
        if ((dbg_aac_call++ % 300) == 0)
            AP2_LOGI("session %lu: aac decode call#%llu len=%zu out_pcm=%zu",
                     (unsigned long)id_, (unsigned long long)dbg_aac_call,
                     pkt.payload.size(), pcm.size());
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

void SessionImpl::timing_worker() {
    // 镜像时钟同步：周期向 iPhone 的 timing 端口发 48B NTP 客户端请求
    //（RPiPlay raop_rtp_mirror_thread_time 同款）。失败不影响主流程，
    // 但要保持请求节奏——iOS 等不到同步会 ~30s 后自断。
    platform::Socket sock;
    if (!sock.create(platform::SocketProtocol::UDP, false)) return;
    if (client_addr_.empty() || client_timing_port_ == 0) {
        sock.close();
        return;
    }
    uint8_t req[48] = {0};
    req[0] = 0x23;  // NTP: LI=0, VN=4, Mode=3(client)
    uint8_t buf[48];
    while (!timing_stop_.load()) {
        // transmit timestamp（offset 40）= 我们当前时钟（自 1900 的 µs）
        put_ntp_ts(req + 40, platform::wallclock_us() + kNtpUnixOffsetSec * 1000000ULL);
        sock.sendto(req, sizeof(req), client_addr_, client_timing_port_);
        // 等响应（最长 1s）：解析 T2（Receive Timestamp, offset 32）
        int rev = 0;
        if (sock.poll(1 /*POLLIN*/, 1000, rev) && (rev & 1)) {
            platform::SocketAddr from;
            auto r = sock.recvfrom(buf, sizeof(buf), &from);
            if (r.ok && r.bytes >= 48) {
                sync_clock_us_.store(read_ntp_ts(buf + 32));
            }
        }
        // 每 3 秒一轮
        for (int i = 0; i < 30 && !timing_stop_.load(); ++i)
            platform::sleep_ms(100);
    }
    sock.close();
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
