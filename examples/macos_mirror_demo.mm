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
#include <deque>
#include <mutex>
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

private:
    AVSampleBufferDisplayLayer* layer_ = nullptr;
    dispatch_queue_t q_ = nullptr;
    CMFormatDescriptionRef fmt_desc_ = nullptr;
    bool pending_flush_ = false; ///< 丢包后等待下一个关键帧再恢复显示

    void handle_config(const VideoConfig& cfg) {
        if (layer_) [layer_ flushAndRemoveImage];
        pending_flush_ = false;
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
        // 丢包：AVSampleBufferDisplayLayer 不会自动跳过坏帧，需要 flush，
        // 等下一个关键帧（IDR）再恢复显示
        if (frame.has_loss) {
            [layer_ flushAndRemoveImage];
            pending_flush_ = true;
        }
        if (pending_flush_ && !frame.is_key) return;
        if (!fmt_desc_) {
            // 帧自带参数集（关键帧会 prepend SPS/PPS），从帧数据构建描述符
            VideoConfig tmp;
            tmp.codec = frame.codec;
            tmp.codec_extra = frame.annex_b;
            fmt_desc_ = BuildFormatDescription(tmp);
            if (!fmt_desc_) return;
        }
        auto avcc = AnnexBToAvcc(frame.annex_b.data(), frame.annex_b.size());
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
        pending_.insert(pending_.end(), pcm_data, pcm_data + num_bytes);
        // 限制缓存上限（200ms），避免镜像延迟无限增长
        size_t max_pending = (size_t)(cfg_.sample_rate * asbd_.mBytesPerFrame * 200 / 1000);
        if (pending_.size() > max_pending) {
            pending_.erase(pending_.begin(), pending_.begin() + (pending_.size() - max_pending));
        }
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
        if (q_) AudioQueueFlush(q_);
        pending_.clear();
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

private:
    AudioQueueRef q_ = nullptr;
    AudioStreamBasicDescription asbd_{};
    AudioConfig cfg_;
    std::mutex mu_;
    std::deque<uint8_t> pending_;
    bool configured_ = false;
    bool playing_ = false;
    float volume_ = 1.0f;

    static void StaticQueueCallback(void* user, AudioQueueRef aq, AudioQueueBufferRef buf) {
        static_cast<MacAudioRenderer*>(user)->FillBuffer(aq, buf);
    }

    void FillBuffer(AudioQueueRef aq, AudioQueueBufferRef buf) {
        std::lock_guard<std::mutex> lk(mu_);
        size_t cap = buf->mAudioDataBytesCapacity;
        size_t n = std::min(cap, pending_.size());
        if (n > 0) {
            memcpy(buf->mAudioData, &pending_[0], n);
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
        if (q_) {
            AudioQueueStop(q_, true);
            AudioQueueDispose(q_, true);
            q_ = nullptr;
        }
        playing_ = false;
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
@property(nonatomic, strong) NSSlider* volumeSlider;
@end

static MirrorAppDelegate* g_delegate = nil;
static MacVideoRenderer* g_video = nullptr;
static MacAudioRenderer* g_audio = nullptr;
static std::atomic<bool> g_running{true};

@implementation MirrorAppDelegate {
    std::string device_name_;
    uint16_t port_;
    std::string pin_;
}

- (instancetype)initWithName:(std::string)name port:(uint16_t)port pin:(std::string)pin {
    self = [super init];
    if (self) {
        device_name_ = std::move(name);
        port_ = port;
        pin_ = std::move(pin);
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

    // ---- 视频显示层 ----
    NSView* content = self.window.contentView;
    content.wantsLayer = YES;
    self.displayLayer = [AVSampleBufferDisplayLayer layer];
    self.displayLayer.videoGravity = AVLayerVideoGravityResizeAspect;
    self.displayLayer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    self.displayLayer.frame = content.bounds;
    [content.layer addSublayer:self.displayLayer];

    // ---- 状态栏 ----
    self.statusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 12, 1000, 22)];
    self.statusLabel.stringValue = @"Status: idle — 请在 iPhone/Mac 控制中心选择本设备";
    self.statusLabel.editable = NO;
    self.statusLabel.bezeled = NO;
    self.statusLabel.drawsBackground = NO;
    self.statusLabel.textColor = [NSColor whiteColor];
    self.statusLabel.font = [NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightRegular];
    [content addSubview:self.statusLabel];

    // ---- 音量滑杆 ----
    self.volumeSlider = [[NSSlider alloc] initWithFrame:NSMakeRect(12, 40, 260, 20)];
    self.volumeSlider.minValue = 0.0;
    self.volumeSlider.maxValue = 1.0;
    self.volumeSlider.doubleValue = 1.0;
    self.volumeSlider.target = self;
    self.volumeSlider.action = @selector(volumeChanged:);
    [content addSubview:self.volumeSlider];
    NSTextField* volLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(280, 38, 60, 22)];
    volLabel.stringValue = @"音量";
    volLabel.editable = NO;
    volLabel.bezeled = NO;
    volLabel.drawsBackground = NO;
    volLabel.textColor = [NSColor whiteColor];
    [content addSubview:volLabel];

    // ---- 启动服务端 ----
    g_video = new MacVideoRenderer();
    g_audio = new MacAudioRenderer();
    g_video->attach(self.displayLayer);

    // 捕获 strong self：demo 进程生命周期 = app 生命周期，无循环引用问题
    std::thread([self] {
        [self server_thread];
    }).detach();
}

- (void)windowDidResize:(NSNotification*)note {
    self.displayLayer.frame = self.window.contentView.bounds;
}

- (void)windowWillClose:(NSNotification*)note {
    g_running.store(false);
    [NSApp terminate:nil];
}

- (void)applicationWillTerminate:(NSNotification*)note {
    g_running.store(false);
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
    cfg.device.model = "MacBookPro18,3";
    cfg.device.supports_audio = true;
    cfg.device.supports_video = true;
    cfg.device.supports_photo = true;
    cfg.device.features = 0x5F7FFFF7; // 音频 + 视频 (bit24) + H.264 (bit27)
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
    cfg.log_level = 2;

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
    cbs.on_log = [](int lvl, const std::string& m) {
        if (lvl <= 2) fprintf(stderr, "%s\n", m.c_str());
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
                      "投屏中  client=%s  audio=%llu B  latency=%u ms  jitter=%u ms",
                      st.client_ip.c_str(),
                      (unsigned long long)st.bytes_received,
                      st.current_latency_ms, st.jitter_ms);
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
    printf("  -n, --name <name>   Device display name (default: \"My Mac\")\n");
    printf("  -p, --port <port>   Control port (default: 7000)\n");
    printf("  -k, --pin <xxxx>    4-digit PIN required to pair (optional)\n");
    printf("  -h, --help          Show help\n");
}

int main(int argc, const char** argv) {
    std::string name = "My Mac";
    uint16_t port = 7000;
    std::string pin;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : "";
        };
        if (arg == "-n" || arg == "--name") name = next();
        else if (arg == "-p" || arg == "--port") port = (uint16_t)atoi(next().c_str());
        else if (arg == "-k" || arg == "--pin") pin = next();
        else if (arg == "-h" || arg == "--help") { print_help(argv[0]); return 0; }
    }

    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        g_delegate = [[MirrorAppDelegate alloc] initWithName:name port:port pin:pin];
        [app setDelegate:g_delegate];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }

    delete g_video;
    delete g_audio;
    return 0;
}
