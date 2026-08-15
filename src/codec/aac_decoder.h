/*!
 * @file aac_decoder.h
 * @brief 库内置 AAC / AAC-ELD 解码器（屏幕镜像音频，ct=8）
 *
 * 设计目标：与 AlacDecoder 平级——会话在 RTP 路径上直接
 * aac_.decode_frame() 解出 PCM，再走同一个 pcm_buffer_ → playback_worker
 * → on_pcm 播放链路，解码逻辑不再放在示例 demo 里。
 *
 * 平台实现：
 *   - macOS / iOS：AudioToolbox AudioConverter（kAudioFormatMPEG4AAC_ELD）
 *   - 其他平台：暂返回未支持（预留 FFmpeg / MediaCodec 接入点）
 *
 * 线程安全：非线程安全，由会话单线程（RTP 回调线程）调用。
 */
#ifndef AIRPLAY2_AAC_DECODER_H
#define AIRPLAY2_AAC_DECODER_H

#include "../include/airplay2/airplay_config.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace airplay2 {
namespace codec {

/*!
 * @brief AAC / AAC-ELD 解码器
 *
 * 输入为裸 AAC 帧（AirPlay 镜像音频每 RTP 负载一帧，无 RFC 3640 AU-header，
 * UxPlay 直接把整个 payload 送解码器）；输出 16-bit 交错 PCM。
 *
 * 关键约束（均经实测验证，见 aac_decoder.cpp）：
 *   - 每次 FillComplexBuffer 只请求 1 个输出包（npk=1）：请求过多会让解码器
 *     拉空输入（回调返回 0 包 = EOS）从而终止会话，只解出第一帧；
 *   - 运行中不能 AudioConverterReset：ELD 有 lookahead 预测状态，重置产生杂音；
 *   - 输入回调返回 0 包时也必须完整初始化 AudioBufferList，否则解码器对垃圾
 *     缓冲做合法性检查直接 SIGILL。
 */
class AacDecoder {
public:
    AacDecoder();
    ~AacDecoder();
    AacDecoder(const AacDecoder&) = delete;
    AacDecoder& operator=(const AacDecoder&) = delete;

    /*!
     * @brief 配置解码器
     * @param fmtp      RFC 3640 fmtp 串（需含 config= 十六进制 AudioSpecificConfig）
     * @param sample_rate 输出采样率（Hz，如 44100）
     * @param channels  输出声道数（如 2）
     * @param is_eld    是否为 AAC-ELD（决定输入格式 ID 与帧长）
     * @return true 成功；false 平台不支持或参数非法
     */
    bool configure(const std::string& fmtp, uint32_t sample_rate,
                   uint32_t channels, bool is_eld);

    /// 是否已配置成功（可解码）
    bool is_configured() const { return ready_; }

    /*!
     * @brief 解码一帧（一包一个 AU 的裸 AAC 帧）
     * @param data    压缩帧数据
     * @param len     字节数
     * @param out_pcm 输出 16-bit 交错 PCM（追加语义：本函数会清空后写入）
     * @return 消耗的输入字节数；<0 失败
     * @note 若解码器因 lookahead 暂存了本帧（本次无输出），下一次调用会
     *       补输出上一帧——流式语义，属正常现象，调用方无需处理。
     */
    int64_t decode_frame(const uint8_t* data, size_t len,
                         std::vector<uint8_t>& out_pcm);

    /// 重置（流结束时调用一次；运行中不要调用，会破坏 lookahead 状态）
    void reset();

    // 平台实现（pimpl）：前置声明公开，供 .mm 内静态输入回调访问；
    // 实际内容对用户不可见。
    struct Impl;

private:
    Impl* impl_ = nullptr;  // 平台实现（Apple 为 AudioConverterRef）
    bool  ready_ = false;   // 是否配置成功
};

} // namespace codec
} // namespace airplay2

#endif // AIRPLAY2_AAC_DECODER_H
