/*!
 * @file alac_decoder.cpp
 * @brief Apple Lossless (ALAC) 解码器——Apple 官方开源实现封装
 *
 * 自研 ALAC 解码器对真实 iOS 帧解码输出为噪音（帧头/参数解析与
 * Apple 编码端不完全兼容），故整体替换为 Apple 官方参考解码器
 * （macosforge/alac，Apache-2.0，vendored 在 src/codec/alac/）。
 *
 * 解码链路：
 *   RTP 负载（AES-CBC 解密后）→ BitBufferInit → ALACDecoder::Decode()
 *   → 交错 PCM16LE（Apple 解码器按 mConfig.bitDepth 输出原生端序 int16）
 */
#include "alac_decoder.h"
#include "alac/ALACDecoder.h"
#include "alac/ALACBitUtilities.h"
#include "../platform/platform_log.h"
#include <cstring>
#include <sstream>

namespace airplay2 {
namespace codec {

bool parse_alac_fmtp(const std::string& fmtp, AlacMagicCookie& out) {
    // fmtp examples:
    //   "96 0 16 4096 10 14 2 255 0 0 44100"
    //   "0 16 4096 10 14 2 255 0 0 44100"
    std::vector<int> nums;
    std::istringstream iss(fmtp);
    std::string tok;
    while (std::getline(iss, tok, ' ')) {
        if (tok.empty()) continue;
        try { nums.push_back(std::stoi(tok)); } catch (...) { nums.push_back(0); }
    }
    if (nums.size() < 10) return false;
    size_t i = 0;
    if (nums.size() == 11) i = 1; // skip payload type prefix
    out.compatible_version = nums[i + 0];
    out.bit_depth          = nums[i + 1];
    out.frame_length       = nums[i + 2];
    out.mb                 = nums[i + 3]; // max_run
    out.kb                 = nums[i + 4]; // rice_history_mult
    int pb0                = nums[i + 5]; // predictor_initial
    int max_run0           = nums[i + 6];
    (void)max_run0;
    out.avg_bit_rate       = nums[i + 7];
    out.sample_rate        = nums[i + 8];
    if (nums.size() > i + 9) out.num_channels = (nums[i + 9] ? nums[i + 9] : 2);
    else                     out.num_channels = 2;
    out.pb = pb0 ? pb0 : 40;
    out.max_run = max_run0 ? max_run0 : 255;
    return true;
}

// Apple 解码器实例（pimpl：把 ALACDecoder 的 include 隔离在 .cpp 里）
struct AlacDecoder::Impl {
    ALACDecoder decoder;
};

AlacDecoder::AlacDecoder() : impl_(std::make_unique<Impl>()) {}
AlacDecoder::~AlacDecoder() = default;

bool AlacDecoder::configure(const AlacMagicCookie& c) {
    cookie_ = c;
    // 重新创建 Impl 以彻底清掉 Apple 解码器的旧状态（ALACDecoder 持指针，
    // 不能直接拷贝赋值，否则双重释放）
    impl_ = std::make_unique<Impl>();

    // 构造 24 字节 ALACSpecificConfig（网络/大端字节序，Apple Init() 直接读）。
    // Init() 会先跳过可选 'frma'/'alac' atom 头（cookie 前 4 字节非 'frma'/'alac'
    // 时不跳），所以我们给裸配置即可。
    uint8_t cfg[24] = {0};
    auto put32 = [&](size_t off, uint32_t v) {
        cfg[off + 0] = (uint8_t)(v >> 24);
        cfg[off + 1] = (uint8_t)(v >> 16);
        cfg[off + 2] = (uint8_t)(v >> 8);
        cfg[off + 3] = (uint8_t)v;
    };
    auto put16 = [&](size_t off, uint16_t v) {
        cfg[off + 0] = (uint8_t)(v >> 8);
        cfg[off + 1] = (uint8_t)v;
    };
    put32(0,  (uint32_t)cookie_.frame_length);
    cfg[4] = (uint8_t)cookie_.compatible_version;
    cfg[5] = (uint8_t)cookie_.bit_depth;
    cfg[6] = (uint8_t)cookie_.pb;
    cfg[7] = (uint8_t)cookie_.mb;
    cfg[8] = (uint8_t)cookie_.kb;
    cfg[9] = (uint8_t)cookie_.num_channels;
    put16(10, (uint16_t)cookie_.max_run);
    put32(12, (uint32_t)cookie_.max_frame_bytes);
    put32(16, (uint32_t)cookie_.avg_bit_rate);
    put32(20, (uint32_t)cookie_.sample_rate);

    if (impl_->decoder.Init(cfg, sizeof(cfg)) != ALAC_noErr) {
        AP2_LOGW("alac: Apple ALACDecoder::Init failed (frame=%d bits=%d ch=%d sr=%d)",
                 cookie_.frame_length, cookie_.bit_depth,
                 cookie_.num_channels, cookie_.sample_rate);
        configured_ = false;
        return false;
    }
    out_cfg_.sample_rate = (uint32_t)cookie_.sample_rate;
    out_cfg_.channels    = (uint8_t)cookie_.num_channels;
    out_cfg_.format      = AudioFormat::PCM16LE;   // 16-bit 输出（Apple 按原生端序写 int16）
    out_cfg_.frame_size  = (uint16_t)cookie_.frame_length;
    configured_ = true;
    AP2_LOGI("alac: Apple ALAC configured sr=%d ch=%d bits=%d frame=%d",
             cookie_.sample_rate, cookie_.num_channels,
             cookie_.bit_depth, cookie_.frame_length);
    return true;
}

void AlacDecoder::reset() {
    // Apple 解码器每帧独立（LPC 预测器/混音缓冲按帧内重建），无跨帧状态
}

int64_t AlacDecoder::decode_frame(const uint8_t* data, size_t len,
                                  std::vector<uint8_t>& out_pcm) {
    if (!configured_ || !data || len == 0) return -1;
    uint32_t max_samples = (uint32_t)cookie_.frame_length;
    if (max_samples == 0) max_samples = 4096;

    // 输出缓冲按最多 frame_length 帧、2 字节/样本/声道预留
    out_pcm.assign(max_samples * (size_t)cookie_.num_channels * 2, 0);

    BitBuffer bits;
    BitBufferInit(&bits, const_cast<uint8_t*>(data), (uint32_t)len);
    uint32_t out_samples = 0;
    int32_t rc = impl_->decoder.Decode(&bits, out_pcm.data(), max_samples,
                                       (uint32_t)cookie_.num_channels,
                                       &out_samples);
    if (rc != ALAC_noErr || out_samples == 0) {
        // 输入非法（解密错误/非 ALAC）→ 返回 -1，调用方记录解码失败
        out_pcm.clear();
        return -1;
    }
    out_pcm.resize(out_samples * (size_t)cookie_.num_channels * 2);
    // 已消耗的输入字节数（AirPlay 一包一帧，正常应 ≈ len）
    size_t consumed = BitBufferGetPosition(&bits) / 8;
    if (consumed == 0) consumed = len;
    return (int64_t)consumed;
}

} // namespace codec
} // namespace airplay2
