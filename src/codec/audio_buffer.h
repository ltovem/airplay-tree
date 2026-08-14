/*!
 * @file audio_buffer.h
 * @brief Thread-safe circular audio sample buffer
 */
#ifndef AIRPLAY2_AUDIO_BUFFER_H
#define AIRPLAY2_AUDIO_BUFFER_H

#include "../include/airplay2/airplay_config.h"
#include <cstddef>
#include <cstdint>
#include <vector>
#include <mutex>
#include <condition_variable>

namespace airplay2 {
namespace codec {

/*!
 * @brief Circular PCM audio buffer.
 *
 * Stores interleaved PCM samples as raw bytes; samples are sized by the
 * configured AudioConfig (e.g. 2ch*2bytes=4 bytes per sample frame).
 */
class AudioBuffer {
public:
    explicit AudioBuffer(size_t max_frames = 16384, const AudioConfig& cfg = {});

    void set_config(const AudioConfig& cfg);
    const AudioConfig& config() const { return cfg_; }

    /// Bytes per sample frame (channels * bytes_per_sample)
    size_t bytes_per_frame() const { return bpf_; }
    /// Capacity in sample frames
    size_t capacity_frames() const { return max_frames_; }
    /// Current readable sample frames
    size_t available_frames() const;
    /// Current writable sample frames
    size_t free_frames() const;

    /*!
     * @brief Write interleaved PCM bytes. Returns # frames actually written.
     */
    size_t write_bytes(const uint8_t* bytes, size_t num_bytes);

    /*!
     * @brief Read up to num_frames frames into out_buf. Returns frames read.
     */
    size_t read_frames(uint8_t* out_buf, size_t num_frames, bool block_until = false);

    /*!
     * @brief Peek without consuming
     */
    size_t peek_frames(uint8_t* out_buf, size_t num_frames) const;

    void flush();

    /// Total frames written lifetime (for stats)
    uint64_t total_written_frames() const { return total_written_; }
    uint64_t total_read_frames()    const { return total_read_; }

private:
    AudioConfig cfg_;
    size_t bpf_ = 0;               // bytes per frame
    size_t max_frames_ = 0;
    size_t bytes_cap_ = 0;
    std::vector<uint8_t> data_;
    size_t rd_ = 0;  // byte read offset
    size_t wr_ = 0;  // byte write offset
    size_t bytes_used_ = 0;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    uint64_t total_written_ = 0;
    uint64_t total_read_ = 0;
};

} // namespace codec
} // namespace airplay2

#endif // AIRPLAY2_AUDIO_BUFFER_H
