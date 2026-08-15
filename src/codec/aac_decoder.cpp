/*!
 * @file aac_decoder.cpp
 * @brief AAC / AAC-ELD 解码器实现（单文件，全平台编译）
 *
 * Apple 平台：AudioToolbox（解码模式对齐 Fraunhofer《AAC-ELD based Audio
 * Communication on iOS》指南的逐帧流式用法——每帧一次
 * AudioConverterFillComplexBuffer，每次只请求 1 个输出包）。
 * 其他平台：编译为不支持占位（预留 FFmpeg / MediaCodec 接入点）。
 *
 * 三个实测教训（实现中均有对应注释）：
 *   1) npk 请求过大（如 8192）→ 解码器持续向输入回调要数据直到耗尽
 *      （回调返回 0 包 = EOS），把解码会话终止 → 只解出第一帧；
 *   2) 运行中 AudioConverterReset → 清掉 ELD lookahead 预测状态 → 杂音；
 *   3) 输入回调返回 0 包时必须完整初始化 AudioBufferList → 否则解码器
 *      对垃圾栈内存做合法性检查直接 CrashIfClientProvidedBogusAudioBufferList
 *      （SIGILL）。
 *
 * 注意：这里只 include 纯 C 的 AudioToolbox 头（AudioConverter.h /
 * CoreAudioTypes.h），**不要** #import 整个 <AudioToolbox/AudioToolbox.h>——
 * 那是 ObjC 伞头，在纯 C++ 编译单元里会拉进 ObjC 运行时并触发 libc++ 的
 * math.h 冲突（cos/cosh 未声明）。
 */
#include "aac_decoder.h"

#include <cstring>
#include <sstream>

namespace airplay2 {
namespace codec {

#if AP2_PLATFORM_MACOS || AP2_PLATFORM_IOS

#include <CoreAudio/CoreAudioTypes.h>
#include <AudioToolbox/AudioConverter.h>
#include <AudioToolbox/AudioFormat.h>

// kAppleSoftwareAudioCodecManufacturer（'appl'）在部分 SDK 的 C++ 编译上下文
// 里不会被 AudioFormat.h 带出（CF_ENUM 展开差异），这里手动兜底。
#ifndef kAppleSoftwareAudioCodecManufacturer
#define kAppleSoftwareAudioCodecManufacturer 'appl'
#endif

namespace {

/*! "k=v;k=v;..." fmtp → kv 表（值去引号/空白） */
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
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '"'))
                s.erase(s.begin());
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '"'))
                s.pop_back();
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

} // namespace

struct AacDecoder::Impl {
    AudioConverterRef conv = nullptr;      ///< AudioToolbox 解码器实例
    uint32_t sample_rate = 44100;
    uint32_t channels    = 2;
    bool     is_eld      = true;
    // 当前待喂入解码器的一帧（decode_frame 设置，输入回调消费后清空）
    const uint8_t* frame_data = nullptr;
    size_t         frame_len  = 0;
};

namespace {

/*! 输入回调：把当前帧交给解码器；无数据时返回 0 包。
 *  无论是否有数据都必须完整初始化 AudioBufferList——解码器（尤其 ELD，
 *  有 lookahead，会二次请求输入）会对回调结果做合法性检查（见文件头注 3）。 */
OSStatus AacInputProc(AudioConverterRef conv, UInt32* numPackets,
                      AudioBufferList* data,
                      AudioStreamPacketDescription** desc, void* user) {
    (void)conv;
    AacDecoder::Impl* s = static_cast<AacDecoder::Impl*>(user);
    data->mNumberBuffers = 1;
    data->mBuffers[0].mNumberChannels = 0;
    data->mBuffers[0].mData = nullptr;
    data->mBuffers[0].mDataByteSize = 0;
    if (!s || !s->frame_data || s->frame_len == 0) { *numPackets = 0; return noErr; }
    data->mBuffers[0].mData = const_cast<uint8_t*>(s->frame_data);
    data->mBuffers[0].mDataByteSize = (UInt32)s->frame_len;
    *numPackets = 1;
    if (desc) {
        static AudioStreamPacketDescription pd;
        pd.mStartOffset = 0;
        pd.mVariableFramesInPacket = 0;
        pd.mDataByteSize = (UInt32)s->frame_len;
        *desc = &pd;
    }
    s->frame_data = nullptr;
    s->frame_len  = 0;
    return noErr;
}

} // namespace

AacDecoder::AacDecoder() : impl_(new Impl) {}
AacDecoder::~AacDecoder() { reset(); }

bool AacDecoder::configure(const std::string& fmtp, uint32_t sample_rate,
                           uint32_t channels, bool is_eld) {
    reset();
    if (!impl_ || sample_rate == 0 || channels == 0) return false;
    impl_->sample_rate = sample_rate;
    impl_->channels    = channels;
    impl_->is_eld      = is_eld;

    // 从 fmtp 提取 config= 十六进制 AudioSpecificConfig
    auto kvs = ParseFmtpKvs(fmtp);
    std::string config_hex;
    for (auto& kv : kvs) {
        if (kv.first == "config") config_hex = kv.second;
    }
    auto asc = HexDecode(config_hex);
    if (asc.empty()) return false;
    auto cookie = BuildAacMagicCookie(asc);

    // 输入 = AAC（对象类型由 magic cookie 决定：ELD / LC），输出 = PCM16。
    // ELD 必须用 kAudioFormatMPEG4AAC_ELD（'aace'）而不是通用的
    // kAudioFormatMPEG4AAC（'aac '），否则 magic cookie 会被拒（实测 '!dat'）。
    // ELD 每帧 480 采样（44.1kHz，UxPlay spf=480）；LC 为 1024。显式声明
    // mFramesPerPacket 帮助 AudioToolbox 正确初始化（Fraunhofer 指南同款）。
    AudioStreamBasicDescription in{};
    in.mSampleRate = (Float64)sample_rate;
    in.mChannelsPerFrame = (UInt32)channels;
    in.mFormatID = is_eld ? kAudioFormatMPEG4AAC_ELD : kAudioFormatMPEG4AAC;
    in.mFramesPerPacket = is_eld ? 480 : 1024;

    AudioStreamBasicDescription out{};
    out.mSampleRate = in.mSampleRate;
    out.mChannelsPerFrame = in.mChannelsPerFrame;
    out.mFormatID = kAudioFormatLinearPCM;
    out.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    out.mBitsPerChannel = 16;
    out.mBytesPerFrame = 2 * (UInt32)channels;
    out.mBytesPerPacket = out.mBytesPerFrame;
    out.mFramesPerPacket = 1;

    // 显式指定**软件**解码器（Fraunhofer AAC-ELD iOS 指南：ELD 只有软件实现；
    // AudioConverterNew 自动选择时可能拿到硬件解码器，对 ELD 首帧后罢工——
    // "解出几帧就停"的常见诱因）。
    AudioClassDescription desc = {
        is_eld ? kAudioFormatMPEG4AAC_ELD : kAudioFormatMPEG4AAC,
        kAppleSoftwareAudioCodecManufacturer, 0
    };
    OSStatus st = AudioConverterNewSpecific(&in, &out, 1, &desc, &impl_->conv);
    if (st != noErr) {
        // 兜底：个别系统缺软件编解码器枚举时退回通用创建
        st = AudioConverterNew(&in, &out, &impl_->conv);
    }
    if (st != noErr || !impl_->conv) return false;

    st = AudioConverterSetProperty(impl_->conv, kAudioConverterDecompressionMagicCookie,
                                   (UInt32)cookie.size(), cookie.data());
    if (st != noErr) {
        reset();
        return false;
    }
    ready_ = true;
    return true;
}

int64_t AacDecoder::decode_frame(const uint8_t* data, size_t len,
                                 std::vector<uint8_t>& out_pcm) {
    if (!ready_ || !impl_ || !impl_->conv || !data || len == 0) return -1;
    impl_->frame_data = data;
    impl_->frame_len  = len;
    out_pcm.clear();

    // 每次只请求 1 个输出包（见文件头注 1）；输出缓冲按 1 帧 PCM16 立体声
    // 预留（480*2*2=1920B，留余量）。若解码器因 lookahead 暂存本帧而本次
    // 无输出，属正常流式语义，下一帧到达时自动补出。
    uint8_t outbuf[4096];
    AudioBufferList abl{};
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mData = outbuf;
    abl.mBuffers[0].mDataByteSize = sizeof(outbuf);
    // 一次调用输出**完整一帧**：ioOutputDataPacketSize 的单位是输出格式的 packet，
    // 而我们的输出 PCM 是 mFramesPerPacket=1（1 packet = 1 采样 = 4B），因此
    // npk 必须 = 每 AAC 帧的采样数（ELD=480 / LC=1024）。
    // 之前写死 npk=1 导致每次只输出 1 个采样（4B），一帧其余 479 个采样被丢弃，
    // 听感只剩零星噪音/卡碟——这是"解出几帧就停"表象的真正根因。
    UInt32 npk = impl_->is_eld ? 480 : 1024;
    OSStatus st = AudioConverterFillComplexBuffer(impl_->conv, AacInputProc,
                                                  impl_, &npk, &abl, nullptr);
    if (st != noErr) return -1;
    size_t produced = abl.mBuffers[0].mDataByteSize;
    if (produced > 0) out_pcm.assign(outbuf, outbuf + produced);
    return (int64_t)len;  // 单帧输入视为全部消耗
}

void AacDecoder::reset() {
    if (impl_) {
        if (impl_->conv) {
            AudioConverterDispose(impl_->conv);
            impl_->conv = nullptr;
        }
        impl_->frame_data = nullptr;
        impl_->frame_len  = 0;
    }
    ready_ = false;
}

#else  // 非 Apple 平台：暂不支持（预留 FFmpeg / MediaCodec 接入点）

AacDecoder::AacDecoder() = default;
AacDecoder::~AacDecoder() = default;

bool AacDecoder::configure(const std::string&, uint32_t, uint32_t, bool) { return false; }
int64_t AacDecoder::decode_frame(const uint8_t*, size_t, std::vector<uint8_t>&) { return -1; }
void AacDecoder::reset() {}

#endif

} // namespace codec
} // namespace airplay2
