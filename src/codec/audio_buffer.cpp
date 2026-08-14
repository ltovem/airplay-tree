/*!
 * @file audio_buffer.cpp
 */
#include "audio_buffer.h"
#include <cstring>
#include <algorithm>

namespace airplay2 {
namespace codec {

static size_t format_bytes(AudioFormat f) {
    switch (f) {
        case AudioFormat::PCM16LE:   return 2;
        case AudioFormat::PCM24LE:   return 3;
        case AudioFormat::PCM32LE:   return 4;
        case AudioFormat::PCM_FLOAT: return 4;
        default: return 0;
    }
}

AudioBuffer::AudioBuffer(size_t max_frames, const AudioConfig& cfg)
    : max_frames_(max_frames) {
    set_config(cfg);
}

void AudioBuffer::set_config(const AudioConfig& cfg) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = cfg;
    size_t bs = format_bytes(cfg.format);
    if (bs == 0) bs = 2;
    if (cfg.channels == 0) cfg_.channels = 2;
    bpf_ = bs * cfg_.channels;
    bytes_cap_ = max_frames_ * bpf_;
    data_.assign(bytes_cap_, 0);
    rd_ = wr_ = bytes_used_ = 0;
}

size_t AudioBuffer::available_frames() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (bpf_ == 0) return 0;
    return bytes_used_ / bpf_;
}

size_t AudioBuffer::free_frames() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (bpf_ == 0) return 0;
    return (bytes_cap_ - bytes_used_) / bpf_;
}

size_t AudioBuffer::write_bytes(const uint8_t* bytes, size_t num_bytes) {
    std::unique_lock<std::mutex> lk(mu_);
    if (bpf_ == 0 || bytes_cap_ == 0) return 0;
    size_t frames = num_bytes / bpf_;
    if (frames == 0) return 0;
    size_t frame_bytes = frames * bpf_;
    // Wait for space? We drop oldest if full.
    if (bytes_cap_ - bytes_used_ < frame_bytes) {
        // Advance read pointer to make room
        size_t need = frame_bytes - (bytes_cap_ - bytes_used_);
        size_t skip = ((need + bpf_ - 1) / bpf_) * bpf_;
        rd_ = (rd_ + skip) % bytes_cap_;
        bytes_used_ -= skip;
    }
    // Copy first chunk (rd to end, or partial)
    size_t first_chunk = std::min(frame_bytes, bytes_cap_ - wr_);
    std::memcpy(data_.data() + wr_, bytes, first_chunk);
    size_t remaining = frame_bytes - first_chunk;
    if (remaining > 0) {
        std::memcpy(data_.data(), bytes + first_chunk, remaining);
    }
    wr_ = (wr_ + frame_bytes) % bytes_cap_;
    bytes_used_ += frame_bytes;
    total_written_ += frames;
    cv_.notify_all();
    return frames;
}

size_t AudioBuffer::read_frames(uint8_t* out_buf, size_t num_frames, bool block_until) {
    std::unique_lock<std::mutex> lk(mu_);
    if (bpf_ == 0 || bytes_cap_ == 0) return 0;
    if (block_until) {
        cv_.wait(lk, [this, num_frames] { return bytes_used_ >= num_frames * bpf_; });
    }
    size_t have_frames = bytes_used_ / bpf_;
    size_t take = std::min(have_frames, num_frames);
    if (take == 0) return 0;
    size_t take_bytes = take * bpf_;
    size_t first = std::min(take_bytes, bytes_cap_ - rd_);
    std::memcpy(out_buf, data_.data() + rd_, first);
    size_t second = take_bytes - first;
    if (second > 0) std::memcpy(out_buf + first, data_.data(), second);
    rd_ = (rd_ + take_bytes) % bytes_cap_;
    bytes_used_ -= take_bytes;
    total_read_ += take;
    return take;
}

size_t AudioBuffer::peek_frames(uint8_t* out_buf, size_t num_frames) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (bpf_ == 0) return 0;
    size_t have_frames = bytes_used_ / bpf_;
    size_t take = std::min(have_frames, num_frames);
    if (take == 0) return 0;
    size_t take_bytes = take * bpf_;
    size_t first = std::min(take_bytes, bytes_cap_ - rd_);
    std::memcpy(out_buf, data_.data() + rd_, first);
    size_t second = take_bytes - first;
    if (second > 0) std::memcpy(out_buf + first, data_.data(), second);
    return take;
}

void AudioBuffer::flush() {
    std::lock_guard<std::mutex> lk(mu_);
    rd_ = wr_ = bytes_used_ = 0;
}

} // namespace codec
} // namespace airplay2
