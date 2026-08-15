/*!
 * @file audio_renderer.h
 * @brief Audio renderer interface - user implements this for playback
 */
#ifndef AIRPLAY2_AUDIO_RENDERER_H
#define AIRPLAY2_AUDIO_RENDERER_H

#include "airplay_config.h"
#include <cstddef>
#include <cstdint>

namespace airplay2 {

/*!
 * @brief Audio renderer interface
 *
 * Users of the library must implement this interface to receive
 * decoded PCM audio samples for playback.
 */
class IAudioRenderer {
public:
    virtual ~IAudioRenderer() = default;

    /*!
     * @brief Called when audio format/config changes
     * @param config New audio configuration
     * @return Status OK on success
     */
    virtual Status on_config(const AudioConfig& config) = 0;

    /*!
     * @brief Deliver decoded PCM audio samples for playback
     * @param pcm_data Pointer to raw PCM buffer (format matches last on_config)
     * @param num_bytes Size of the PCM buffer in bytes
     * @param timestamp_us Reference timestamp in microseconds (for sync)
     * @return Status OK on success
     */
    virtual Status on_pcm(const uint8_t* pcm_data, size_t num_bytes,
                          uint64_t timestamp_us) = 0;

    /*!
     * @brief Playback state change notifications
     */
    virtual void on_play()  {}
    virtual void on_pause() {}
    virtual void on_stop()  {}
    virtual void on_flush() {}

    /*!
     * @brief 压缩音频格式配置通知（AAC-ELD / AAC-LC 等库未内置解码器的格式）
     *
     * 当会话协商到库无法解码的压缩格式（屏幕镜像常用 AAC-ELD）时，
     * 库会把原始压缩帧通过 on_compressed_audio 透传给渲染器，由渲染器
     * 用平台解码器（macOS/iOS 的 AudioToolbox、Android MediaCodec、
     * Linux 的 FFmpeg 等）自行解码播放。
     *
     * 该回调在 on_config() 之后、收到数据之前调用一次。
     *
     * @param codec SDP 里的编码名（如 "mpeg4-generic" / "aac"）
     * @param fmtp  SDP fmtp 原串（可能含 config= 十六进制 AudioSpecificConfig、
     *              sizelength= / indexlength= / indexdeltaLength= 等 RFC 3640 参数）
     * @param cfg   期望输出的 PCM 参数（sample_rate / channels / format）
     */
    virtual void on_compressed_config(const std::string& codec,
                                      const std::string& fmtp,
                                      const AudioConfig& cfg) {
        (void)codec; (void)fmtp; (void)cfg;
    }

    /*!
     * @brief 交付一包压缩音频帧（原始 RTP 负载，可能带 RFC 3640 AU-header）
     *
     * 仅当协商到 on_compressed_config() 提到的编码时才会调用。渲染器负责
     * 解析 AU-header、调用平台解码器得到 PCM 并播放。
     *
     * @param data         原始压缩帧数据
     * @param len          字节数
     * @param timestamp_us 参考时间戳（微秒）
     * @return Status::OK 表示接受
     */
    virtual Status on_compressed_audio(const uint8_t* data, size_t len,
                                       uint64_t timestamp_us) {
        (void)data; (void)len; (void)timestamp_us;
        return Status::OK;
    }

    /*!
     * @brief Query current playback volume (0.0 - 1.0)
     */
    virtual float get_volume() const { return 1.0f; }

    /*!
     * @brief Set playback volume (0.0 - 1.0)
     */
    virtual void  set_volume(float /*volume*/) {}

    /*!
     * @brief Get playback latency in microseconds (for A/V sync)
     */
    virtual uint64_t get_playback_latency_us() const { return 0; }
};

/*!
 * @brief A simple in-memory audio buffer renderer (useful for testing)
 */
class MemoryAudioRenderer : public IAudioRenderer {
public:
    Status on_config(const AudioConfig& config) override {
        config_ = config;
        return Status::OK;
    }

    Status on_pcm(const uint8_t* pcm_data, size_t num_bytes,
                  uint64_t timestamp_us) override {
        (void)timestamp_us;
        buffer_.insert(buffer_.end(), pcm_data, pcm_data + num_bytes);
        return Status::OK;
    }

    void on_flush() override { buffer_.clear(); }

    const std::vector<uint8_t>& buffer() const { return buffer_; }
    const AudioConfig& config() const { return config_; }
    size_t total_bytes() const { return buffer_.size(); }

private:
    AudioConfig config_;
    std::vector<uint8_t> buffer_;
};

} // namespace airplay2

#endif // AIRPLAY2_AUDIO_RENDERER_H
