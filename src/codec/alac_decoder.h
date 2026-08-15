/*!
 * @file alac_decoder.h
 * @brief Apple Lossless (ALAC) 解码器封装
 *
 * 内部直接使用 Apple 官方开源实现（macosforge/alac，Apache-2.0，
 * 代码 vendored 在 src/codec/alac/ 下）：ALACDecoder 是 Apple 参考解码器，
 * 与 iOS/Apple TV 的编码端保证互操作。此前自研解码器对真实 iOS 帧
 * 解码输出为噪音，故整体替换为官方实现。
 *
 * 解码流程：
 *   RTP 负载（已 AES-CBC 解密）→ Apple ALACDecoder::Decode() → PCM16LE
 */
#ifndef AIRPLAY2_ALAC_DECODER_H
#define AIRPLAY2_ALAC_DECODER_H

#include "../include/airplay2/airplay_config.h"
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace airplay2 {
namespace codec {

/*!
 * @brief ALAC "magic cookie"（AP2 SETUP 无 fmtp 时用默认值 / SDP fmtp 解析）
 *
 * AP2 纯音频的编解码参数来自 SETUP stream dict（ct=2/spf/sr），
 * configure() 时据此构造 ALACSpecificConfig。AirPlay 1 的 ANNOUNCE SDP
 * fmtp 形如 "0 16 4096 40 10 14 2 255 0 0 44100"，用 parse_alac_fmtp 解析。
 */
struct AlacMagicCookie {
    int frame_length = 4096;
    int compatible_version = 0;
    int bit_depth = 16;
    int pb = 40;           // predictor initial
    int mb = 10;           // max_run_length
    int kb = 14;           // initial rice_history
    int num_channels = 2;
    int max_run = 255;
    int max_frame_bytes = 0;
    int avg_bit_rate = 0;
    int sample_rate = 44100;
    int channel_layout_tag = 0;   // 0 = stereo LR interleaved
    uint32_t channel_layout_info = 0;
};

/// Parse the 11-space-separated fields of the AirPlay fmtp line into cookie.
bool parse_alac_fmtp(const std::string& fmtp, AlacMagicCookie& out);

/*!
 * @brief ALAC 帧解码器（Apple 官方 ALACDecoder 的薄封装）
 *
 * 线程安全：每个会话独立持有实例，实例内状态仅在 receiver 线程访问。
 */
class AlacDecoder {
public:
    AlacDecoder();
    ~AlacDecoder();
    AlacDecoder(const AlacDecoder&) = delete;
    AlacDecoder& operator=(const AlacDecoder&) = delete;

    /// 用 magic cookie 初始化 Apple 解码器（构造 ALACSpecificConfig）
    bool configure(const AlacMagicCookie& cookie);

    /// 输出格式（sample_rate / channels / PCM16LE）
    const AudioConfig& output_config() const { return out_cfg_; }

    /// 复位（seek/flush 时调用；Apple 解码器无跨帧状态，空实现）
    void reset();

    /*!
     * @brief 解码一帧 ALAC
     * @param data 已解密的 ALAC 帧负载（不含 RTP 头）
     * @param len  字节数
     * @param out_pcm 输出的交错 PCM16LE（自动 resize）
     * @return 消耗的输入字节数；-1 表示输入非法/未配置
     */
    int64_t decode_frame(const uint8_t* data, size_t len,
                         std::vector<uint8_t>& out_pcm);

    bool is_configured() const { return configured_; }
    const AlacMagicCookie& cookie() const { return cookie_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;   ///< Apple ALACDecoder 实例（pimpl 隔离头文件依赖）
    bool configured_ = false;
    AlacMagicCookie cookie_;
    AudioConfig out_cfg_;
};

} // namespace codec
} // namespace airplay2

#endif // AIRPLAY2_ALAC_DECODER_H
