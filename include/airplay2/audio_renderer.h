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
