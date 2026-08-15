/*!
 * @file macos_mirror_demo.mm
 * @brief macOS 投屏（Screen Mirroring）接收器 Demo
 *
 * 这是一个完整的 macOS Cocoa 应用，使用 airplay2lib 作为 AirPlay 2
 * 接收端，实现：
 *
 *   1. 屏幕镜像（Screen Mirroring / Data Push）：
 *      iPhone / iPad / Mac 的控制中心 → 屏幕镜像 → 选择本机（设备名）
 *      视频走 RTP H.264/H.265，库重组 NAL 后通过 IVideoRenderer 回调
 *      送给我们；我们用 AVSampleBufferDisplayLayer 解码并显示在窗口。
 *
 *   2. 音频：镜像时发送端把系统音频一起推过来（AAC-ELD / ALAC），
 *      库解码成 PCM 后通过 IAudioRenderer 回调送给我们；
 *      我们用 AudioQueue 播放。
 *
 * 构建（macOS 桌面）：
 *   cmake -B build -DCMAKE_BUILD_TYPE=Release
 *   cmake --build build --target airplay2_mac_mirror
 *   ./build/examples/airplay2_mac_mirror --name "My Mac" [--port 7000] [--pin 1234]
 *
 * 使用：
 *   启动后保持窗口在前台；iPhone 打开控制中心 → 屏幕镜像 → 选择设备名。
 *   首次可能要求输入 PIN（--pin 设置后才需要）。
 *
 * 说明：
 *   - AVSampleBufferDisplayLayer 内部走 VideoToolbox 硬解，支持
 *     H.264 / H.265，低延时。
 *   - 视频/音频回调在库的工作线程上被调用，通过串行队列串到
 *     渲染队列，避免竞态；UI 更新 dispatch 到主线程。
 */

#import <Cocoa/Cocoa.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <VideoToolbox/VideoToolbox.h>
#import <AudioToolbox/AudioToolbox.h>
#import <QuartzCore/QuartzCore.h>

#include <airplay2/airplay2.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace airplay2;

// ============================================================================
// 工具函数：Annex-B <-> AVCC 转换、NAL 解析
// ============================================================================

namespace {

/*! 在 Annex-B 字节流中查找所有 NAL 起始位置（3/4 字节 start code） */
std::vector<size_t> FindStartCodes(const uint8_t* data, size_t len) {
    std::vector<size_t> pos;
    size_t i = 0;
    while (i + 3 <= len) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            pos.push_back(i);
            i += 3;
        } else if (i + 4 <= len && data[i] == 0 && data[i + 1] == 0 &&
                   data[i + 2] == 0 && data[i + 3] == 1) {
            pos.push_back(i);
            i += 4;
        } else {
            ++i;
        }
    }
    return pos;
}

/*! 提取 Annex-B 流中的各个 NAL 负载（不含 start code） */
std::vector<std::vector<uint8_t>> ExtractNals(const uint8_t* data, size_t len) {
    std::vector<std::vector<uint8_t>> nals;
    auto starts = FindStartCodes(data, len);
    if (starts.empty()) {
        if (len > 0) nals.emplace_back(data, data + len);
        return nals;
    }
    for (size_t k = 0; k < starts.size(); ++k) {
        size_t begin = starts[k];
        size_t off = (begin + 3 <= len && data[begin] == 0 && data[begin + 1] == 0 &&
                      data[begin + 2] == 1) ? 3 : 4;
        size_t end = (k + 1 < starts.size()) ? starts[k + 1] : len;
        if (end > begin + off) nals.emplace_back(data + begin + off, data + end);
    }
    return nals;
}

/*! Annex-B → AVCC（每 NAL 前加 4 字节大端长度） */
std::vector<uint8_t> AnnexBToAvcc(const uint8_t* data, size_t len) {
    std::vector<uint8_t> out;
    auto nals = ExtractNals(data, len);
    for (auto& nal : nals) {
        uint32_t sz = (uint32_t)nal.size();
        out.push_back((uint8_t)(sz >> 24));
        out.push_back((uint8_t)(sz >> 16));
        out.push_back((uint8_t)(sz >> 8));
        out.push_back((uint8_t)(sz));
        out.insert(out.end(), nal.begin(), nal.end());
    }
    return out;
}

/*! H.264 NAL 类型（RFC 6184） */
enum H264NalType { kH264Sps = 7, kH264Pps = 8, kH264Aud = 9 };
/*! H.265 NAL 类型（RFC 7798，低 6 位） */
enum H265NalType { kH265Vps = 32, kH265Sps = 33, kH265Pps = 34, kH265Aud = 35 };

/*! 判断一个 NAL 是否是"参数集"（SPS/PPS/VPS/AUD），渲染时需剥离 */
bool IsParamSetNal(const uint8_t* nal, size_t len, VideoCodec codec) {
    if (!nal || len < 1) return false;
    if (codec == VideoCodec::H265_HEVC) {
        if (len < 2) return false;
        int type = (nal[0] >> 1) & 0x3F;   // H.265 NAL header: F(1)|Type(6)|...
        return type == kH265Vps || type == kH265Sps || type == kH265Pps || type == kH265Aud;
    }
    if (codec == VideoCodec::MJPEG) return false;
    int type = nal[0] & 0x1F;              // H.264 NAL header: F(2)|NRI(2)|Type(5)
    return type == kH264Sps || type == kH264Pps || type == kH264Aud;
}

/*! 从 Annex-B 帧中剥离参数集 NAL（保留 slice），返回只含 slice 的 Annex-B */
std::vector<uint8_t> StripParamSets(const uint8_t* data, size_t len, VideoCodec codec) {
    std::vector<uint8_t> out;
    auto starts = FindStartCodes(data, len);
    if (starts.empty()) return {data, data + len};
    for (size_t k = 0; k < starts.size(); ++k) {
        size_t begin = starts[k];
        size_t off = (begin + 3 <= len && data[begin] == 0 && data[begin + 1] == 0 &&
                      data[begin + 2] == 1) ? 3 : 4;
        size_t end = (k + 1 < starts.size()) ? starts[k + 1] : len;
        if (end <= begin + off) continue;
        const uint8_t* nal = data + begin + off;
        if (IsParamSetNal(nal, end - begin - off, codec)) continue;
        out.insert(out.end(), data + begin, data + end);
    }
    return out;
}

// ---- AAC-ELD 解码辅助（RFC 3640 AU-header 解析 + hex） ----

/*! 把 "k=v;k=v;..." 的 fmtp 切分成 kv 表（值去引号/空白） */
std::vector<std::pair<std::string, std::string>> ParseFmtpKvs(const std::string& fmtp) {
    std::vector<std::pair<std::string, std::string>> kvs;
    std::stringstream ss(fmtp);
    std::string seg;
    while (std::getline(ss, seg, ';')) {
        size_t eq = seg.find('=');
        if (eq == std::string::npos) continue;
        std::string k = seg.substr(0, eq);
        std::string v = seg.substr(eq + 1);
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '"')) s.erase(s.begin());
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '"')) s.pop_back();
        };
        trim(k); trim(v);
        if (!k.empty()) kvs.emplace_back(std::move(k), std::move(v));
    }
    return kvs;
}

/*! 十六进制字符串 → 字节 */
std::vector<uint8_t> HexDecode(const std::string& s) {
    std::vector<uint8_t> out;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) break;
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return out;
}

/*!
 * @brief 把 AudioSpecificConfig 包装成 AudioToolbox 期望的 MPEG-4 描述符 cookie
 *
 * 与 FFmpeg audiotoolboxdec.c ffat_get_magic_cookie 一致：kAudioFormatMPEG4AAC
 * 的 kAudioConverterDecompressionMagicCookie 不能直接给原始 ASC，而必须包在
 *   ES descriptor(0x03) → DecoderConfig(0x04) → DecoderSpecificInfo(0x05)
 * 三层描述符里，否则 AudioConverterSetProperty 会返回错误（'!dat'）。
 */
std::vector<uint8_t> BuildAacMagicCookie(const std::vector<uint8_t>& asc) {
    std::vector<uint8_t> out;
    auto put_descr = [&out](int tag, unsigned size) {
        out.push_back((uint8_t)tag);
        for (int i = 3; i > 0; --i) out.push_back((uint8_t)((size >> (7 * i)) | 0x80));
        out.push_back((uint8_t)(size & 0x7F));
    };
    auto put16 = [&out](uint16_t v) { out.push_back((uint8_t)(v >> 8)); out.push_back((uint8_t)v); };
    auto put32 = [&out](uint32_t v) {
        out.push_back((uint8_t)(v >> 24)); out.push_back((uint8_t)(v >> 16));
        out.push_back((uint8_t)(v >> 8));  out.push_back((uint8_t)v);
    };
    unsigned n = (unsigned)asc.size();
    put_descr(0x03, 3 + 5 + 13 + 5 + n);   // ES descriptor
    put16(0);                               // ES_ID
    out.push_back(0x00);                    // flags
    put_descr(0x04, 13 + 5 + n);            // DecoderConfig descriptor
    out.push_back(0x40);                    // objectTypeIndication = MPEG-4 Audio
    out.push_back(0x15);                    // streamType/flags = Audiostream
    out.push_back(0); out.push_back(0); out.push_back(0); // bufferSizeDB
    put32(0);                               // maxBitrate
    put32(0);                               // avgBitrate
    put_descr(0x05, n);                     // DecoderSpecificInfo
    out.insert(out.end(), asc.begin(), asc.end());
    return out;
}

/*! 从 codec_extra（Annex-B SPS/PPS/VPS）构建 CMFormatDescription */
CMFormatDescriptionRef BuildFormatDescription(const VideoConfig& cfg) {
    if (cfg.codec_extra.empty()) return nullptr;
    auto nals = ExtractNals(cfg.codec_extra.data(), cfg.codec_extra.size());
    if (nals.empty()) return nullptr;

    std::vector<uint8_t*> ptrs;
    std::vector<size_t> sizes;
    for (auto& n : nals) {
        ptrs.push_back(n.data());
        sizes.push_back(n.size());
    }

    CMFormatDescriptionRef desc = nullptr;
    if (cfg.codec == VideoCodec::H264_AVC) {
        if (ptrs.size() < 2) return nullptr; // 需要 SPS + PPS
        CMVideoFormatDescriptionCreateFromH264ParameterSets(
            kCFAllocatorDefault, (size_t)ptrs.size(),
            (const uint8_t* const*)ptrs.data(), sizes.data(),
            4, &desc);
    } else if (cfg.codec == VideoCodec::H265_HEVC) {
        if (ptrs.size() < 3) return nullptr; // VPS + SPS + PPS
        CMVideoFormatDescriptionCreateFromHEVCParameterSets(
            kCFAllocatorDefault, (size_t)ptrs.size(),
            (const uint8_t* const*)ptrs.data(), sizes.data(),
            0, nullptr, &desc);
    } else {
        CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_JPEG,
                                       (int32_t)cfg.width, (int32_t)cfg.height,
                                       NULL, &desc);
    }
    return desc;
}

/*! 把一个 NAL 缓冲包装成 CMSampleBuffer 并提交给显示层 */
void EnqueueSample(AVSampleBufferDisplayLayer* layer, CMFormatDescriptionRef desc,
                   const uint8_t* data, size_t len, uint64_t pts_us) {
    if (!layer || !desc || !data || len == 0) return;
    CMBlockBufferRef block = nullptr;
    if (CMBlockBufferCreateWithMemoryBlock(
            kCFAllocatorDefault, nullptr, len, kCFAllocatorDefault, nullptr,
            0, len, kCMBlockBufferAssureMemoryNowFlag, &block) != kCMBlockBufferNoErr)
        return;
    if (CMBlockBufferReplaceDataBytes(data, block, 0, len) != kCMBlockBufferNoErr) {
        CFRelease(block);
        return;
    }
    size_t sample_size = len;
    CMSampleTimingInfo timing{};
    timing.duration = kCMTimeInvalid;
    timing.presentationTimeStamp = CMTimeMake((int64_t)pts_us, 1000000);
    timing.decodeTimeStamp = kCMTimeInvalid;
    CMSampleBufferRef sb = nullptr;
    // CMSampleBufferCreateReady 的参数顺序：timing 在 size 之前
    if (CMSampleBufferCreateReady(kCFAllocatorDefault, block, desc, 1, 1,
                                  &timing, 1, &sample_size, &sb) == noErr) {
        [layer enqueueSampleBuffer:sb];
        CFRelease(sb);
    }
    CFRelease(block);
}

} // namespace

// ============================================================================
// MacVideoRenderer —— 用 AVSampleBufferDisplayLayer 显示投屏画面
// ============================================================================

class MacVideoRenderer : public IVideoRenderer {
public:
    MacVideoRenderer() {
        q_ = dispatch_queue_create("airplay2.mirror.video", DISPATCH_QUEUE_SERIAL);
    }
    ~MacVideoRenderer() override {
        dispatch_sync(q_, ^{
            if (fmt_desc_) { CFRelease(fmt_desc_); fmt_desc_ = nullptr; }
        });
        // ARC 模式下 dispatch 对象自动释放，无需 dispatch_release
    }

    void attach(AVSampleBufferDisplayLayer* layer) { layer_ = layer; }

    void on_config(const VideoConfig& cfg) override {
        VideoConfig c = cfg;
        dispatch_async(q_, ^{
            this->handle_config(c);
        });
    }

    void on_frame(const VideoFrame& frame) override {
        VideoFrame f = frame;
        dispatch_async(q_, ^{
            this->handle_frame(f);
        });
    }

    void on_playback(const VideoPlaybackCmd& cmd) override {
        if (cmd.type == VideoPlaybackCmd::STOP || cmd.type == VideoPlaybackCmd::SEEK) {
            dispatch_async(q_, ^{
                if (this->layer_) {
                    [this->layer_ flushAndRemoveImage];
                    this->pending_flush_ = false;
                }
            });
        }
    }

    void on_stop() override {
        dispatch_async(q_, ^{
            if (this->layer_) {
                [this->layer_ flushAndRemoveImage];
                this->pending_flush_ = false;
            }
            if (this->fmt_desc_) {
                CFRelease(this->fmt_desc_);
                this->fmt_desc_ = nullptr;
            }
        });
    }

    /// UI 统计：累计收到并提交显示层的视频帧数
    uint64_t frames_total() const { return frames_total_.load(); }

private:
    AVSampleBufferDisplayLayer* layer_ = nullptr;
    dispatch_queue_t q_ = nullptr;
    CMFormatDescriptionRef fmt_desc_ = nullptr;
    bool pending_flush_ = false; ///< 丢包后等待下一个关键帧再恢复显示
    std::atomic<uint64_t> frames_total_{0};

    // 带内参数集缓存：iOS 屏幕镜像的 SPS/PPS 常随 RTP 流周期性下发
    // （而非只在 SDP fmtp 里），这里把最新看到的参数集攒起来建格式描述符
    VideoCodec param_codec_ = VideoCodec::H264_AVC;
    std::vector<uint8_t> sps_, pps_, vps_;

    /// 从一帧 Annex-B 里提取并缓存 SPS/PPS/VPS（每次覆盖为最新）
    void CacheParamSets(const VideoFrame& frame) {
        param_codec_ = frame.codec;
        auto nals = ExtractNals(frame.annex_b.data(), frame.annex_b.size());
        for (auto& n : nals) {
            if (n.empty()) continue;
            if (frame.codec == VideoCodec::H265_HEVC) {
                int type = (n[0] >> 1) & 0x3F;
                if (type == kH265Vps) vps_ = n;
                else if (type == kH265Sps) sps_ = n;
                else if (type == kH265Pps) pps_ = n;
            } else if (frame.codec != VideoCodec::MJPEG) {
                int type = n[0] & 0x1F;
                if (type == kH264Sps) sps_ = n;
                else if (type == kH264Pps) pps_ = n;
            }
        }
    }

    /// 用缓存的参数集构建格式描述符（H.264: SPS+PPS；H.265: VPS+SPS+PPS）
    CMFormatDescriptionRef BuildFromCache() {
        if (param_codec_ == VideoCodec::H265_HEVC) {
            if (vps_.empty() || sps_.empty() || pps_.empty()) return nullptr;
            const uint8_t* ptrs[3] = {vps_.data(), sps_.data(), pps_.data()};
            size_t sizes[3] = {vps_.size(), sps_.size(), pps_.size()};
            CMFormatDescriptionRef desc = nullptr;
            CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                kCFAllocatorDefault, 3, ptrs, sizes, 0, nullptr, &desc);
            return desc;
        }
        if (sps_.empty() || pps_.empty()) return nullptr;
        const uint8_t* ptrs[2] = {sps_.data(), pps_.data()};
        size_t sizes[2] = {sps_.size(), pps_.size()};
        CMFormatDescriptionRef desc = nullptr;
        CMVideoFormatDescriptionCreateFromH264ParameterSets(
            kCFAllocatorDefault, 2, ptrs, sizes, 4, &desc);
        return desc;
    }

    void handle_config(const VideoConfig& cfg) {
        if (layer_) [layer_ flushAndRemoveImage];
        pending_flush_ = false;
        sps_.clear(); pps_.clear(); vps_.clear();
        if (fmt_desc_) { CFRelease(fmt_desc_); fmt_desc_ = nullptr; }
        if (cfg.codec_extra.empty()) return;
        fmt_desc_ = BuildFormatDescription(cfg);
        if (!fmt_desc_) return;
        // 提交只含参数集的样本，让解码器就绪
        EnqueueSample(layer_, fmt_desc_, cfg.codec_extra.data(),
                      cfg.codec_extra.size(), 0);
    }

    void handle_frame(const VideoFrame& frame) {
        if (!layer_ || frame.annex_b.empty()) return;
        frames_total_++; // UI 统计：计数到达的完整帧
        // 先把带内参数集缓存下来（无论是否已有 fmt_desc_，参数集更新后需重建）
        CacheParamSets(frame);
        // 丢包：AVSampleBufferDisplayLayer 不会自动跳过坏帧，需要 flush，
        // 等下一个关键帧（IDR）再恢复显示
        if (frame.has_loss) {
            [layer_ flushAndRemoveImage];
            pending_flush_ = true;
        }
        if (pending_flush_ && !frame.is_key) return;
        if (!fmt_desc_) {
            // 1) 优先用 SDP sprop-parameter-sets 构造的描述符（handle_config 已建）
            // 2) 否则用带内缓存的 SPS/PPS（可能比关键帧自身携带的更完整）
            fmt_desc_ = BuildFromCache();
            if (!fmt_desc_) return;
        }
        // 提交 slice 数据（参数集由 fmt_desc_ 携带，避免重复解码参数集）
        auto slices = StripParamSets(frame.annex_b.data(), frame.annex_b.size(),
                                     frame.codec);
        if (slices.empty()) return;
        auto avcc = AnnexBToAvcc(slices.data(), slices.size());
        if (avcc.empty()) return;
        EnqueueSample(layer_, fmt_desc_, avcc.data(), avcc.size(), frame.pts_us);
        if (pending_flush_ && frame.is_key) pending_flush_ = false;
    }
};

// ============================================================================
// MacAudioRenderer —— 用 AudioQueue 播放 PCM
// ============================================================================

class MacAudioRenderer : public IAudioRenderer {
public:
    MacAudioRenderer() = default;
    ~MacAudioRenderer() override { StopQueue(); }

    Status on_config(const AudioConfig& config) override {
        std::lock_guard<std::mutex> lk(mu_);
        StopQueueLocked();
        cfg_ = config;

        memset(&asbd_, 0, sizeof(asbd_));
        asbd_.mSampleRate = (double)config.sample_rate;
        asbd_.mChannelsPerFrame = config.channels;
        asbd_.mFormatID = kAudioFormatLinearPCM;
        asbd_.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        switch (config.format) {
            case AudioFormat::PCM_FLOAT:
                asbd_.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
                asbd_.mBitsPerChannel = 32;
                break;
            case AudioFormat::PCM24LE:
                asbd_.mBitsPerChannel = 24;
                break;
            case AudioFormat::PCM32LE:
                asbd_.mBitsPerChannel = 32;
                break;
            default: // PCM16LE
                asbd_.mBitsPerChannel = 16;
                break;
        }
        asbd_.mBytesPerFrame = (asbd_.mBitsPerChannel / 8) * config.channels;
        asbd_.mBytesPerPacket = asbd_.mBytesPerFrame;
        asbd_.mFramesPerPacket = 1;

        OSStatus st = AudioQueueNewOutput(&asbd_, &MacAudioRenderer::StaticQueueCallback,
                                          this, nullptr, nullptr, 0, &q_);
        if (st != noErr) { q_ = nullptr; return Status::ERROR_CODEC; }

        // 预分配 8 个 24ms buffer
        uint32_t buf_bytes = (uint32_t)(config.sample_rate * asbd_.mBytesPerFrame * 24 / 1000);
        if (buf_bytes < 4096) buf_bytes = 4096;
        for (int i = 0; i < 8; ++i) {
            AudioQueueBufferRef buf = nullptr;
            if (AudioQueueAllocateBuffer(q_, buf_bytes, &buf) == noErr && buf) {
                memset(buf->mAudioData, 0, buf_bytes);
                buf->mAudioDataByteSize = buf_bytes;
                AudioQueueEnqueueBuffer(q_, buf, 0, nullptr);
            }
        }
        configured_ = true;
        return Status::OK;
    }

    Status on_pcm(const uint8_t* pcm_data, size_t num_bytes,
                  uint64_t timestamp_us) override {
        (void)timestamp_us;
        std::lock_guard<std::mutex> lk(mu_);
        if (!configured_ || !q_ || !pcm_data || num_bytes == 0) return Status::OK;
        pcm_bytes_total_ += num_bytes; // UI 统计：累计解码出的 PCM 字节数
        pending_.insert(pending_.end(), pcm_data, pcm_data + num_bytes);
        // 限制缓存上限（200ms），避免镜像延迟无限增长
        size_t max_pending = (size_t)(cfg_.sample_rate * asbd_.mBytesPerFrame * 200 / 1000);
        if (pending_.size() > max_pending) {
            pending_.erase(pending_.begin(), pending_.begin() + (pending_.size() - max_pending));
        }
        return Status::OK;
    }

    // ---- AAC-ELD 压缩透传（屏幕镜像音频） ----

    void on_compressed_config(const std::string& codec, const std::string& fmtp,
                              const AudioConfig& cfg) override {
        std::lock_guard<std::mutex> lk(mu_);
        StopAacLocked();
        // 解析 fmtp 参数（RFC 3640）：config= 十六进制 AudioSpecificConfig 是必须的
        auto kvs = ParseFmtpKvs(fmtp);
        std::string config_hex;
        for (auto& kv : kvs) {
            if (kv.first == "config") config_hex = kv.second;
            else if (kv.first == "sizelength") aac_sizelength_ = atoi(kv.second.c_str());
            else if (kv.first == "indexlength") aac_indexlength_ = atoi(kv.second.c_str());
            else if (kv.first == "indexdeltaLength" || kv.first == "indexdelta") aac_indexdelta_ = atoi(kv.second.c_str());
        }
        auto asc = HexDecode(config_hex);
        if (asc.empty()) {
            fprintf(stderr, "[AAC] no config= in fmtp, cannot create decoder\n");
            return;
        }
        // AudioToolbox 需要 MPEG-4 描述符包装的 cookie（不是裸 ASC）
        auto cookie = BuildAacMagicCookie(asc);
        // 输入 = AAC（对象类型由 magic cookie 决定：ELD / LC），输出 = PCM16
        AudioStreamBasicDescription in{};
        in.mSampleRate = cfg.sample_rate ? cfg.sample_rate : 44100;
        in.mChannelsPerFrame = cfg.channels ? cfg.channels : 2;
        in.mFormatID = kAudioFormatMPEG4AAC;
        memset(&aac_out_asbd_, 0, sizeof(aac_out_asbd_));
        aac_out_asbd_.mSampleRate = in.mSampleRate;
        aac_out_asbd_.mChannelsPerFrame = in.mChannelsPerFrame;
        aac_out_asbd_.mFormatID = kAudioFormatLinearPCM;
        aac_out_asbd_.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        aac_out_asbd_.mBitsPerChannel = 16;
        aac_out_asbd_.mBytesPerFrame = 2 * in.mChannelsPerFrame;
        aac_out_asbd_.mBytesPerPacket = aac_out_asbd_.mBytesPerFrame;
        aac_out_asbd_.mFramesPerPacket = 1;

        OSStatus st = AudioConverterNew(&in, &aac_out_asbd_, &aac_conv_);
        if (st != noErr) { aac_conv_ = nullptr; return; }
        st = AudioConverterSetProperty(aac_conv_, kAudioConverterDecompressionMagicCookie,
                                       (UInt32)cookie.size(), cookie.data());
        if (st != noErr) {
            fprintf(stderr, "[AAC] magic cookie rejected (%d), disabling\n", (int)st);
            StopAacLocked();
            return;
        }
        aac_ready_ = true;
        fprintf(stderr, "[AAC] decoder ready: %s %.0f Hz %u ch cookie=%zu B\n",
                codec.c_str(), in.mSampleRate, (unsigned)in.mChannelsPerFrame, cookie.size());
    }

    Status on_compressed_audio(const uint8_t* data, size_t len,
                               uint64_t timestamp_us) override {
        (void)timestamp_us;
        if (!data || len == 0) return Status::OK;
        std::lock_guard<std::mutex> lk(mu_);
        if (!aac_ready_ || !aac_conv_) return Status::OK;
        aac_bytes_total_ += len;
        aac_packets_++;
        // 攒够一批再解码：AudioConverter 的 AAC 解码器有 1 帧 lookahead，
        // 单包一次调用会产生 0 输出（实测），批量喂 4 个包即可稳定输出。
        aac_batch_.push_back(std::vector<uint8_t>(data, data + len));
        if (aac_batch_.size() >= kAacBatchSize) DrainAacBatchLocked();
        return Status::OK;
    }

    void on_play() override {
        std::lock_guard<std::mutex> lk(mu_);
        if (q_ && !playing_) { AudioQueueStart(q_, nullptr); playing_ = true; }
    }
    void on_pause() override {
        std::lock_guard<std::mutex> lk(mu_);
        if (q_ && playing_) { AudioQueuePause(q_); playing_ = false; }
    }
    void on_flush() override {
        std::lock_guard<std::mutex> lk(mu_);
        // 先把攒的 AAC 包解出来，避免 FLUSH 丢尾音
        DrainAacBatchLocked();
        if (q_) AudioQueueFlush(q_);
        pending_.clear();
        aac_batch_.clear();
    }
    void on_stop() override {
        std::lock_guard<std::mutex> lk(mu_);
        StopQueueLocked();
    }

    float get_volume() const override { return volume_; }
    void set_volume(float v) override {
        std::lock_guard<std::mutex> lk(mu_);
        volume_ = v;
        if (q_) AudioQueueSetParameter(q_, kAudioQueueParam_Volume, v);
    }

    /// UI 统计：累计收到的 PCM 字节数
    uint64_t pcm_bytes_total() const { return pcm_bytes_total_.load(); }
    /// UI 统计：累计收到的 AAC 压缩包数 / 字节数
    uint64_t aac_packets() const { return aac_packets_.load(); }
    uint64_t aac_bytes_total() const { return aac_bytes_total_.load(); }
    /// UI 统计：当前队列状态（非 const：内部要加锁）
    bool queue_active() {
        std::lock_guard<std::mutex> lk(mu_);
        return configured_ && q_ && playing_;
    }

private:
    AudioQueueRef q_ = nullptr;
    AudioStreamBasicDescription asbd_{};
    AudioConfig cfg_;
    std::mutex mu_;
    // 注意：必须用连续存储（vector 而非 deque）——FillBuffer 会对
    // &pending_[0] 直接 memcpy n 字节；deque 的块存储不连续，跨块读取
    // 会越界（ASan: heap-buffer-overflow）
    std::vector<uint8_t> pending_;
    std::atomic<uint64_t> pcm_bytes_total_{0};
    bool configured_ = false;
    bool playing_ = false;
    float volume_ = 1.0f;

    // ---- AAC-ELD 解码状态（AudioConverter） ----
    AudioConverterRef aac_conv_ = nullptr;
    AudioStreamBasicDescription aac_out_asbd_{};
    bool aac_ready_ = false;
    int aac_sizelength_ = 13;   // RFC 3640 AU-size 位宽
    int aac_indexlength_ = 3;   // AU-index 位宽
    int aac_indexdelta_ = 3;    // AU-index-delta 位宽
    static constexpr size_t kAacBatchSize = 4;  // 攒 4 个包再解码（≈93ms @44.1k）
    std::vector<std::vector<uint8_t>> aac_batch_; // 待解码的原始 RTP 负载
    std::atomic<uint64_t> aac_packets_{0};
    std::atomic<uint64_t> aac_bytes_total_{0};
    std::atomic<uint64_t> aac_decode_errors_{0};

    // 批量解码输入状态：把所有 AU 拼成一个连续缓冲，回调按 AU 逐个喂
    struct AacBatchState {
        const uint8_t* data = nullptr;   // 连续 AU 数据
        std::vector<size_t> au_offsets;  // 每个 AU 在 data 中的偏移
        size_t total = 0;
        size_t idx = 0;
    };
    static OSStatus AacBatchInputProc(AudioConverterRef conv, UInt32* numPackets,
                                      AudioBufferList* data,
                                      AudioStreamPacketDescription** desc, void* user) {
        (void)conv;
        AacBatchState* s = (AacBatchState*)user;
        if (!s || s->idx >= s->au_offsets.size()) { *numPackets = 0; return noErr; }
        size_t start = s->au_offsets[s->idx];
        size_t end = (s->idx + 1 < s->au_offsets.size()) ? s->au_offsets[s->idx + 1] : s->total;
        data->mNumberBuffers = 1;
        data->mBuffers[0].mNumberChannels = 0;
        data->mBuffers[0].mData = (void*)(s->data + start);
        data->mBuffers[0].mDataByteSize = (UInt32)(end - start);
        *numPackets = 1;
        if (desc) {
            static AudioStreamPacketDescription pd;  // 变量包输入需要 packet description
            pd.mStartOffset = 0;
            pd.mVariableFramesInPacket = 0;
            pd.mDataByteSize = (UInt32)(end - start);
            *desc = &pd;
        }
        s->idx++;
        return noErr;
    }

    /*! 把一批原始负载解析成 AU 连续缓冲（调用方必须已持有 mu_） */
    void ParseBatchToAus(std::vector<uint8_t>& cont, std::vector<size_t>& offsets) {
        cont.clear();
        offsets.clear();
        for (auto& raw : aac_batch_) {
            if (raw.size() < 2) continue;
            unsigned hdr_bits = (unsigned)((raw[0] << 8) | raw[1]);
            size_t payload_off = 2 + (hdr_bits + 7) / 8;
            if (payload_off > raw.size()) payload_off = raw.size();
            if (hdr_bits == 0) {
                // 无 AU-header：整个剩余负载就是一个 AU
                if (raw.size() > 2) {
                    offsets.push_back(cont.size());
                    cont.insert(cont.end(), raw.begin() + 2, raw.end());
                }
                continue;
            }
            // RFC 3640：AU-size(sizelength) [+ AU-index] [+ AU-index-delta] ...
            unsigned pos = 0;
            size_t au_off = payload_off;
            int idx = 0;
            while (au_off < raw.size()) {
                if (pos + (unsigned)aac_sizelength_ > hdr_bits) break;
                size_t au_size = 0;
                for (int b = 0; b < aac_sizelength_ && pos < hdr_bits; ++b, ++pos) {
                    int byte_i = 2 + (int)(pos >> 3);
                    if (byte_i >= (int)raw.size()) { au_size = 0; break; }
                    au_size = (au_size << 1) | ((raw[byte_i] >> (7 - (pos & 7))) & 1);
                }
                int skip_bits = (idx == 0) ? aac_indexlength_ : aac_indexdelta_;
                for (int b = 0; b < skip_bits && pos < hdr_bits; ++b) ++pos;
                if (au_size == 0 || au_off + au_size > raw.size()) break;
                offsets.push_back(cont.size());
                cont.insert(cont.end(), raw.begin() + au_off, raw.begin() + au_off + au_size);
                au_off += au_size;
                idx++;
            }
            if (idx == 0) {
                // 头部解析异常：把 AU-headers 之后的数据整体当一个 AU
                offsets.push_back(cont.size());
                cont.insert(cont.end(), raw.begin() + payload_off, raw.end());
            }
        }
    }

    /*! 解码一批累积的 AAC 包，输出 PCM16 到 pending_（调用方必须已持有 mu_） */
    void DrainAacBatchLocked() {
        if (!aac_ready_ || !aac_conv_ || aac_batch_.empty()) return;
        std::vector<uint8_t> cont;
        std::vector<size_t> offsets;
        ParseBatchToAus(cont, offsets);
        aac_batch_.clear();
        if (offsets.empty()) return;

        AacBatchState st{cont.data(), offsets, cont.size(), 0};
        // 循环解码直到该批全部消费完
        for (int guard = 0; guard < 64; ++guard) {
            uint8_t outbuf[131072];
            AudioBufferList abl{};
            abl.mNumberBuffers = 1;
            abl.mBuffers[0].mData = outbuf;
            abl.mBuffers[0].mDataByteSize = sizeof(outbuf);
            UInt32 npk = 8192;
            OSStatus st2 = AudioConverterFillComplexBuffer(aac_conv_, AacBatchInputProc,
                                                           &st, &npk, &abl, nullptr);
            if (st2 != noErr) {
                if (aac_decode_errors_.fetch_add(1) < 8)
                    fprintf(stderr, "[AAC] decode error %d\n", (int)st2);
                break;
            }
            size_t produced = abl.mBuffers[0].mDataByteSize;
            if (produced > 0) {
                pcm_bytes_total_ += produced;
                pending_.insert(pending_.end(), outbuf, outbuf + produced);
            }
            if (st.idx >= offsets.size() && produced == 0) break;
        }
        // 限制缓存上限（200ms），避免镜像延迟无限增长
        size_t max_pending = (size_t)(cfg_.sample_rate * asbd_.mBytesPerFrame * 200 / 1000);
        if (max_pending > 0 && pending_.size() > max_pending) {
            pending_.erase(pending_.begin(), pending_.begin() + (pending_.size() - max_pending));
        }
    }

    void StopAacLocked() {
        aac_ready_ = false;
        if (aac_conv_) {
            AudioConverterDispose(aac_conv_);
            aac_conv_ = nullptr;
        }
    }

    static void StaticQueueCallback(void* user, AudioQueueRef aq, AudioQueueBufferRef buf) {
        static_cast<MacAudioRenderer*>(user)->FillBuffer(aq, buf);
    }

    void FillBuffer(AudioQueueRef aq, AudioQueueBufferRef buf) {
        std::lock_guard<std::mutex> lk(mu_);
        // 若还有攒下的 AAC 包，优先解码（backpressure：队列快空时补上）
        if (!aac_batch_.empty() && pending_.empty()) DrainAacBatchLocked();
        size_t cap = buf->mAudioDataBytesCapacity;
        size_t n = std::min(cap, pending_.size());
        if (n > 0) {
            memcpy(buf->mAudioData, pending_.data(), n);
            pending_.erase(pending_.begin(), pending_.begin() + n);
        } else {
            memset(buf->mAudioData, 0, cap);
            n = cap;
        }
        buf->mAudioDataByteSize = (UInt32)n;
        AudioQueueEnqueueBuffer(aq, buf, 0, nullptr);
    }

    void StopQueueLocked() {
        configured_ = false;
        pending_.clear();
        aac_batch_.clear();
        if (q_) {
            AudioQueueStop(q_, true);
            AudioQueueDispose(q_, true);
            q_ = nullptr;
        }
        playing_ = false;
        StopAacLocked();
    }
    void StopQueue() {
        std::lock_guard<std::mutex> lk(mu_);
        StopQueueLocked();
    }
};

// ============================================================================
// Cocoa App
// ============================================================================

@interface MirrorAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) AVSampleBufferDisplayLayer* displayLayer;
@property(nonatomic, strong) NSTextField* statusLabel;
@property(nonatomic, strong) NSTextField* statsLabel;
@property(nonatomic, strong) NSSlider* volumeSlider;
@property(nonatomic, strong) NSScrollView* logScroll;
@property(nonatomic, strong) NSTextView* logView;
@property(nonatomic, strong) NSTimer* statsTimer;
@end

static MirrorAppDelegate* g_delegate = nil;
static MacVideoRenderer* g_video = nullptr;
static MacAudioRenderer* g_audio = nullptr;
static std::atomic<bool> g_running{true};

@implementation MirrorAppDelegate {
    std::string device_name_;
    uint16_t port_;
    std::string pin_;
    std::string model_;
    std::string keyfile_;
}

- (instancetype)initWithName:(std::string)name port:(uint16_t)port pin:(std::string)pin
                       model:(std::string)model keyfile:(std::string)keyfile {
    self = [super init];
    if (self) {
        device_name_ = std::move(name);
        port_ = port;
        pin_ = std::move(pin);
        model_ = std::move(model);
        keyfile_ = std::move(keyfile);
    }
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    NSRect frame = NSMakeRect(0, 0, 1280, 800);
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:(NSWindowStyleMaskTitled |
                                                         NSWindowStyleMaskClosable |
                                                         NSWindowStyleMaskMiniaturizable |
                                                         NSWindowStyleMaskResizable)
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    [self.window setTitle:[NSString stringWithFormat:@"AirPlay 投屏接收器 — %s",
                                                      device_name_.c_str()]];
    [self.window center];
    [self.window setDelegate:self];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    // ---- 视频显示层（顶部） ----
    NSView* content = self.window.contentView;
    content.wantsLayer = YES;
    self.displayLayer = [AVSampleBufferDisplayLayer layer];
    self.displayLayer.videoGravity = AVLayerVideoGravityResizeAspect;
    self.displayLayer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    self.displayLayer.frame = NSMakeRect(0, 320, content.bounds.size.width,
                                         content.bounds.size.height - 320);
    [content.layer addSublayer:self.displayLayer];

    // ---- 状态栏（一行） ----
    self.statusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 296, 1000, 20)];
    self.statusLabel.stringValue = @"Status: idle — 请在 iPhone/Mac 控制中心选择本设备";
    self.statusLabel.editable = NO;
    self.statusLabel.bezeled = NO;
    self.statusLabel.drawsBackground = NO;
    self.statusLabel.textColor = [NSColor whiteColor];
    self.statusLabel.font = [NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightRegular];
    [content addSubview:self.statusLabel];

    // ---- 实时统计行 ----
    self.statsLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 272, 1000, 20)];
    self.statsLabel.stringValue = @"audio: - | video: -";
    self.statsLabel.editable = NO;
    self.statsLabel.bezeled = NO;
    self.statsLabel.drawsBackground = NO;
    self.statsLabel.textColor = [NSColor colorWithCalibratedWhite:0.75 alpha:1.0];
    self.statsLabel.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
    [content addSubview:self.statsLabel];

    // ---- 音量滑杆 ----
    self.volumeSlider = [[NSSlider alloc] initWithFrame:NSMakeRect(12, 246, 260, 20)];
    self.volumeSlider.minValue = 0.0;
    self.volumeSlider.maxValue = 1.0;
    self.volumeSlider.doubleValue = 1.0;
    self.volumeSlider.target = self;
    self.volumeSlider.action = @selector(volumeChanged:);
    [content addSubview:self.volumeSlider];
    NSTextField* volLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(280, 244, 60, 22)];
    volLabel.stringValue = @"音量";
    volLabel.editable = NO;
    volLabel.bezeled = NO;
    volLabel.drawsBackground = NO;
    volLabel.textColor = [NSColor whiteColor];
    [content addSubview:volLabel];

    // ---- 日志面板（底部，滚动） ----
    NSRect logRect = NSMakeRect(8, 8, content.bounds.size.width - 16, 228);
    self.logView = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0,
                                                  logRect.size.width - 14,
                                                  logRect.size.height)];
    self.logView.editable = NO;
    self.logView.selectable = YES;
    self.logView.backgroundColor = [NSColor colorWithCalibratedWhite:0.07 alpha:1.0];
    self.logView.textColor = [NSColor colorWithCalibratedWhite:0.82 alpha:1.0];
    self.logView.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    self.logView.string = @"== 日志 ==\n";
    self.logScroll = [[NSScrollView alloc] initWithFrame:logRect];
    self.logScroll.documentView = self.logView;
    self.logScroll.hasVerticalScroller = YES;
    self.logScroll.autohidesScrollers = YES;
    self.logScroll.borderType = NSBezelBorder;
    [content addSubview:self.logScroll];

    // ---- 启动服务端 ----
    g_video = new MacVideoRenderer();
    g_audio = new MacAudioRenderer();
    g_video->attach(self.displayLayer);

    // 每秒刷新统计行
    __strong MirrorAppDelegate* strongSelf = self;
    self.statsTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES
        block:^(NSTimer* timer) {
            [strongSelf updateStats];
        }];

    // 捕获 strong self：demo 进程生命周期 = app 生命周期，无循环引用问题
    std::thread([self] {
        [self server_thread];
    }).detach();
}

- (void)windowDidResize:(NSNotification*)note {
    NSRect bounds = self.window.contentView.bounds;
    self.displayLayer.frame = NSMakeRect(0, 320, bounds.size.width,
                                         bounds.size.height - 320);
    self.logScroll.frame = NSMakeRect(8, 8, bounds.size.width - 16, 228);
    self.logView.frame = NSMakeRect(0, 0, bounds.size.width - 30,
                                    self.logScroll.contentSize.height);
}

- (void)windowWillClose:(NSNotification*)note {
    g_running.store(false);
    [NSApp terminate:nil];
}

- (void)applicationWillTerminate:(NSNotification*)note {
    g_running.store(false);
}

// 禁用系统窗口状态恢复（NSPersistentUI）：投屏接收器不需要恢复窗口，
// 且 macOS 12 在状态目录异常时可能直接 abort 崩溃
- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)app {
    (void)app;
    return NO;
}

- (void)volumeChanged:(id)sender {
    if (g_audio) g_audio->set_volume((float)self.volumeSlider.doubleValue);
}

- (void)setStatus:(NSString*)text {
    NSString* t = text;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!self.statusLabel) return;
        self.statusLabel.stringValue = t;
    });
}

// 追加一行到日志面板（可从任意线程调用；内部切主线程 + 自动滚动）
- (void)appendLog:(NSString*)line {
    NSString* t = line;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!self.logView) return;
        static NSDateFormatter* fmt = nil;
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            fmt = [[NSDateFormatter alloc] init];
            fmt.dateFormat = @"HH:mm:ss.SSS";
        });
        NSString* full = [NSString stringWithFormat:@"[%@] %@\n",
                          [fmt stringFromDate:[NSDate date]], t];
        NSDictionary* attrs = @{
            NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:0.82 alpha:1.0],
            NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular],
        };
        // 限制日志长度（约 1500 行），防止长时间运行内存/文本无限增长
        if (self.logView.string.length > 200000) {
            NSUInteger cut = [self.logView.string lengthOfBytesUsingEncoding:NSUTF8StringEncoding] / 2;
            self.logView.string = [self.logView.string substringFromIndex:cut];
        }
        [self.logView.textStorage appendAttributedString:
            [[NSAttributedString alloc] initWithString:full attributes:attrs]];
        [self.logView scrollRangeToVisible:NSMakeRange(self.logView.string.length, 0)];
    });
}

// 每秒刷新统计行：音频 PCM 流量 / AAC 包数 / 队列状态 / 视频帧数
- (void)updateStats {
    if (!g_audio || !g_video || !self.statsLabel) return;
    const char* queue_state = g_audio->queue_active() ? "播放中" : "未播放";
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "audio: %llu B PCM | %llu AAC包 | 队列 %s | video: %llu 帧 | 音量 %.2f",
                  (unsigned long long)g_audio->pcm_bytes_total(),
                  (unsigned long long)g_audio->aac_packets(),
                  queue_state,
                  (unsigned long long)g_video->frames_total(),
                  g_audio->get_volume());
    self.statsLabel.stringValue = [NSString stringWithUTF8String:buf];
    // 本地调试：统计行也回显到终端
    fprintf(stderr, "stats: %s\n", buf);
}

- (void)server_thread {
    __strong MirrorAppDelegate* strongSelf = self;
    if (AirPlayServer::global_init() != Status::OK) {
        [strongSelf setStatus:@"global_init failed"];
        return;
    }

    ServerConfig cfg;
    cfg.device.name = device_name_;
    cfg.device.port = port_;
    cfg.control_port = port_;
    cfg.device.model = model_;  // mDNS TXT model：决定 iPhone 控制中心显示的图标
    // 常见取值：MacBookPro18,3=电脑 / AppleTV6,2|AppleTV11,1=AppleTV
    //           / AudioAccessory1,2|AudioAccessory5,1=音响(HomePod)
    cfg.device.supports_audio = true;
    cfg.device.supports_video = true;
    cfg.device.supports_photo = true;
    // 0x5A7FFFF7 = 真实 Apple TV 3 抓包值（含 bit0=Video/bit7=Screen/bit8=ScreenRotate），
    // iOS 判定"可镜像"需要 Video 位：只给 bit7 会被归为纯音频 → 镜像栏显示音响。
    // 注意 bit26(HasUnifiedAdvertiserInfo) 必须为 0：置 1 会让 iOS 强制走
    // /auth-setup MFi 握手，没有 MFi 证书时 iOS 直接断开（曾导致"投不上"）。
    cfg.device.features = 0x5A7FFFF7;
    {
        std::hash<std::string> h;
        size_t hv = h(device_name_);
        char mac[32];
        std::snprintf(mac, sizeof(mac), "AA:BB:CC:%02X:%02X:%02X",
                      (unsigned)(hv & 0xFF), (unsigned)((hv >> 8) & 0xFF),
                      (unsigned)((hv >> 16) & 0xFF));
        cfg.device.device_id = mac;
    }
    if (!pin_.empty()) {
        cfg.device.requires_encryption = true;
        cfg.device.pin_code = pin_;
    }
    cfg.bind_address = "0.0.0.0";
    cfg.rtp_port_min = 5000;
    cfg.rtp_port_max = 5100;
    cfg.max_sessions = 8;
    cfg.buffer_ms = 2000;
    cfg.enable_logging = true;
    cfg.log_level = 4;  // 4=trace：本地调试时把每个 RTSP 请求/响应都打进日志面板
    // 持久化 Ed25519 身份：重启后公钥不变，iOS 不会反复要求重新配对
    if (!keyfile_.empty()) {
        cfg.identity_key_path = keyfile_;
    } else {
        const char* home = getenv("HOME");
        if (home) {
            cfg.identity_key_path =
                std::string(home) + "/Library/Application Support/airplay2lib/identity.key";
        }
    }

    // 音频输出格式（解码后的 PCM 直接按此播放）
    cfg.audio.sample_rate = 48000;
    cfg.audio.channels = 2;
    cfg.audio.format = AudioFormat::PCM16LE;

    ServerCallbacks cbs;
    cbs.on_started = [strongSelf] {
        [strongSelf setStatus:@"Ready — 等待投屏连接（mDNS 已宣告）"];
    };
    cbs.on_stopped = [strongSelf] {
        [strongSelf setStatus:@"Stopped"];
    };
    cbs.on_session_connected = [strongSelf](AirPlaySession& s) {
        std::string msg = "客户端已连接: " + s.client_address() + " (" + s.client_name() + ")";
        [strongSelf setStatus:[NSString stringWithUTF8String:msg.c_str()]];
    };
    cbs.on_session_disconnected = [strongSelf](uint64_t) {
        [strongSelf setStatus:@"客户端已断开"];
    };
    cbs.on_pin_request = [](const std::string&, const std::string&) {
        return true; // demo：接受任意 PIN
    };
    cbs.on_error = [strongSelf](Status code, const std::string& msg) {
        std::string text = "错误 [" + std::to_string((int)code) + "]: " + msg;
        [strongSelf setStatus:[NSString stringWithUTF8String:text.c_str()]];
    };
    cbs.on_log = [strongSelf](int lvl, const std::string& m) {
        // 错误/警告/信息都显示到 UI 日志面板（0=E, 1=W, 2=I），调试更直观；
        // 同时全部回显到终端，便于不打开 UI 时本地排查（如自动化测试）
        fprintf(stderr, "[%c] %s\n", lvl == 0 ? 'E' : (lvl == 1 ? 'W' : 'I'), m.c_str());
        if (lvl <= 2) {
            [strongSelf appendLog:[NSString stringWithUTF8String:m.c_str()]];
        }
    };

    AirPlayServer server(cfg, std::move(cbs), g_audio, g_video);
    Status rc = server.start();
    if (rc != Status::OK) {
        // -5 端口被占用：最常见原因是上一个实例未退出，先退出再启动
        std::string text = "启动失败: " + std::to_string((int)rc) +
                           (rc == Status::ERROR_BIND_FAILED
                                ? " (端口 " + std::to_string(port_) +
                                      " 被占用，请先退出已运行的实例再启动)"
                                : "");
        [strongSelf setStatus:[NSString stringWithUTF8String:text.c_str()]];
        AirPlayServer::global_cleanup();
        return;
    }

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto ids = server.active_session_ids();
        if (ids.empty()) continue;
        auto* s = server.get_session(ids.front());
        if (!s) continue;
        auto st = s->stats();
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "投屏中  client=%s  audio=%llu B  pkts=%llu lost=%llu latency=%u ms jitter=%u ms",
                      st.client_ip.c_str(),
                      (unsigned long long)st.bytes_received,
                      (unsigned long long)st.packets_received,
                      (unsigned long long)st.packets_lost,
                      st.current_latency_ms, st.jitter_ms);
        fprintf(stderr, "sess: %s\n", buf);  // 本地调试：会话统计
        [strongSelf setStatus:[NSString stringWithUTF8String:buf]];
    }

    server.stop();
    AirPlayServer::global_cleanup();
}

@end

// ============================================================================
// main
// ============================================================================

static void print_help(const char* argv0) {
    printf("Usage: %s [options]\n", argv0);
    printf("Options:\n");
    printf("  -n, --name <name>    Device display name (default: \"My Mac\")\n");
    printf("  -p, --port <port>    Control port (default: 7000)\n");
    printf("  -k, --pin <xxxx>     4-digit PIN required to pair (optional)\n");
    printf("  -K, --keyfile <path> Ed25519 identity seed file (default:\n");
    printf("                         ~/Library/Application Support/airplay2lib/identity.key)\n");
    printf("  -m, --model <model>  mDNS TXT model (controls Control-Center icon):\n");
    printf("                         MacBookPro18,3    = computer\n");
    printf("                         AppleTV6,2        = Apple TV\n");
    printf("                         AppleTV11,1       = Apple TV 4K\n");
    printf("                         AudioAccessory1,2 = speaker (HomePod)\n");
    printf("                         AudioAccessory5,1 = speaker (HomePod mini)\n");
    printf("  -h, --help           Show help\n");
}

int main(int argc, const char** argv) {
    std::string name = "My Mac";
    uint16_t port = 7000;
    std::string pin;
    std::string keyfile;
    std::string model = "MacBookPro18,3";  // 默认：电脑图标

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : "";
        };
        if (arg == "-n" || arg == "--name") name = next();
        else if (arg == "-p" || arg == "--port") port = (uint16_t)atoi(next().c_str());
        else if (arg == "-k" || arg == "--pin") pin = next();
        else if (arg == "-K" || arg == "--keyfile") keyfile = next();
        else if (arg == "-m" || arg == "--model") model = next();
        else if (arg == "-h" || arg == "--help") { print_help(argv[0]); return 0; }
    }

    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        g_delegate = [[MirrorAppDelegate alloc] initWithName:name port:port pin:pin
                                                       model:model keyfile:keyfile];
        [app setDelegate:g_delegate];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }

    delete g_video;
    delete g_audio;
    return 0;
}
