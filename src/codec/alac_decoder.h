/*!
 * @file alac_decoder.h
 * @brief Apple Lossless Audio Codec decoder implementation
 *
 * This implements the publicly documented ALAC bitstream format:
 *   - 32-bit predictor / LPC coefficient set
 *   - Rice parameter coding of residuals
 *   - Dynamic frame size support (up to 4096 samples per frame)
 *   - Support for 16/20/24/32-bit PCM, stereo / mono, 44.1/48 kHz
 *
 * Reference: Apple's open-source "Apple Lossless" codec, RFC-style specs
 * available from alac.co.uk and the alac-encoder/decoder GitHub projects.
 */
#ifndef AIRPLAY2_ALAC_DECODER_H
#define AIRPLAY2_ALAC_DECODER_H

#include "../include/airplay2/airplay_config.h"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>

namespace airplay2 {
namespace codec {

/*!
 * @brief ALAC "magic cookie" (fmtp data from SDP).
 *
 * In AirPlay SDP, the fmtp line contains:
 *   a=fmtp:96 0 16 4096 10 14 2 255 0 0 44100
 * Fields: [0] (unused), [1] bytes-per-sample, [2] max-frame,
 *         [3..9] predictor/lpc params, [10] sample-rate.
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
 * @brief ALAC frame decoder
 */
class AlacDecoder {
public:
    AlacDecoder();
    ~AlacDecoder();

    /// Initialize decoder from magic cookie
    bool configure(const AlacMagicCookie& cookie);

    /// Get output config
    const AudioConfig& output_config() const { return out_cfg_; }

    /// Reset predictor state (on seek / flush)
    void reset();

    /*!
     * @brief Decode one ALAC frame.
     * @param data Raw ALAC frame payload (starting with 32-bit header)
     * @param len  Length in bytes
     * @param out_pcm Output interleaved PCM buffer (will be resized)
     * @return bytes consumed, or -1 on error
     */
    int64_t decode_frame(const uint8_t* data, size_t len,
                         std::vector<uint8_t>& out_pcm);

    bool is_configured() const { return configured_; }
    const AlacMagicCookie& cookie() const { return cookie_; }

private:
    // Bit reader
    struct BitReader {
        const uint8_t* base = nullptr;
        size_t total = 0;
        size_t pos_bits = 0;

        void init(const uint8_t* d, size_t n) { base = d; total = n; pos_bits = 0; }
        uint32_t read(unsigned nbits);
        uint32_t peek(unsigned nbits) const;
        void     skip(unsigned nbits) { pos_bits += nbits; }
        size_t   bytes_used() const { return (pos_bits + 7) >> 3; }
        int      bits_left() const { return (int)(total * 8 - pos_bits); }
    };

    static int rice_decompress(BitReader& br, int k, int32_t* samples, int count,
                                int mod_shift, int max_samples);
    void decode_channel(BitReader& br, int32_t* out, int samples,
                        int predictor_num, int m, uint32_t* lpc_coefs,
                        int chan_bits, int chan_history);
    static int32_t idiv_shift(int32_t a, int shift);

    bool configured_ = false;
    AlacMagicCookie cookie_;
    AudioConfig out_cfg_;

    // Predictor history (per channel)
    static constexpr int kMaxChannels = 2;
    static constexpr int kMaxPredictor = 32;
    std::array<std::array<int32_t, kMaxPredictor>, kMaxChannels> predictor_buf_;
    std::array<int, kMaxChannels> predictor_used_;
};

} // namespace codec
} // namespace airplay2

#endif // AIRPLAY2_ALAC_DECODER_H
