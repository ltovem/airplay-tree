/*!
 * @file alac_decoder.cpp
 *
 * ALAC（Apple Lossless）解码器，按 FFmpeg libavcodec/alac.c（David Hammerton）
 * 的位流结构移植实现：
 *
 *   1. 每包（一个 ALAC 帧）由若干"语法元素"组成，每个元素以 3-bit 类型开头：
 *        TYPE_SCE=0（单声道） / TYPE_CPE=1（立体声对） / TYPE_LFE=3 / TYPE_END=7
 *   2. 元素头（decode_element）：
 *        元素实例标签 4b + 保留 12b + has_size 1b + extra_bits 2b(<<3)
 *        + bps + is_compressed 1b + [输出采样数 32b]
 *   3. 压缩帧：decorr_shift 8b + decorr_left_weight 8b
 *        每声道：prediction_type 4b + lpc_quant 4b + rice_history_mult 3b
 *                 + lpc_order 5b + [lpc_order 个 16b 有符号系数]
 *        然后 Rice 解码残差 → LPC 预测重建 → 立体声去相关
 *   4. 以 TYPE_END 结束
 *
 * 之前这版实现按"16 位采样数 + 声道数"的假想帧头解析，与真实位流完全不符，
 * 输出为满幅噪声（已用 afconvert 生成的真实 ALAC 帧验证：min/max ±32767）。
 */
#include "alac_decoder.h"
#include "../platform/platform_log.h"
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <sstream>

namespace airplay2 {
namespace codec {

// ALAC 语法元素类型（FFmpeg alac.h AlacRawDataBlockType）
enum AlacElementType : uint8_t {
    kTypeSce = 0,   // 单声道元素
    kTypeCpe = 1,   // 立体声对元素
    kTypeLfe = 3,   // 低频效果声道
    kTypeEnd = 7,   // 帧结束
};

bool parse_alac_fmtp(const std::string& fmtp, AlacMagicCookie& out) {
    // AirPlay SDP 的 ALAC fmtp 实际是 ALACMagicCookieDescription 的数字序列，
    // 常见两种字段顺序（区别在于 frameLength 是否位于最前）：
    //   A: [PT] compatibleVersion bitDepth frameLength pb mb kb numChannels
    //      maxRun maxFrameBytes avgBitRate sampleRate        （AirPlay 1 风格）
    //   B: [PT] frameLength compatibleVersion bitDepth pb mb kb numChannels
    //      maxRun maxFrameBytes avgBitRate sampleRate        （AirPlay 2 风格）
    // 其中 [PT]（payload type，如 96）由 "a=fmtp:96 ..." 带出，解析后可能残留。
    //
    // 判别技巧：bitDepth 必须是 16/20/24/32，用它定位两种布局中 bitDepth 的下标：
    //   下标 1 → 布局 A（无前导 frameLength）
    //   下标 2 → 布局 B（带前导 frameLength）
    // 其余字段做范围兜底，避免个别发送端缺字段时产生荒谬配置。
    std::vector<int> nums;
    std::istringstream iss(fmtp);
    std::string tok;
    while (std::getline(iss, tok, ' ')) {
        if (tok.empty()) continue;
        try { nums.push_back(std::stoi(tok)); } catch (...) { nums.push_back(0); }
    }
    if (nums.size() < 9) return false;

    // 去掉 payload type 前缀：首字段为 1..127 且字段数足够（≥12）时视为 PT。
    // 真实 frameLength（如 352）远大于 127，不会被误删。
    if (nums.size() >= 12 && nums[0] >= 1 && nums[0] <= 127) nums.erase(nums.begin());

    // 定位 bitDepth 下标（只看前 4 个字段，避免把 sampleRate 等误判）
    int bd_pos = -1;
    for (int i = 0; i < 4 && i < (int)nums.size(); ++i) {
        if (nums[i] == 16 || nums[i] == 20 || nums[i] == 24 || nums[i] == 32) { bd_pos = i; break; }
    }
    if (bd_pos == 1) {
        // 布局 A: version bitDepth frameLength pb mb kb ch maxRun maxFB avgBR sr
        out.compatible_version = (0 < (int)nums.size()) ? nums[0] : 0;
        out.bit_depth          = (1 < (int)nums.size()) ? nums[1] : 16;
        out.frame_length       = (2 < (int)nums.size()) ? nums[2] : 4096;
        out.pb                 = (3 < (int)nums.size()) ? nums[3] : 40;
        out.mb                 = (4 < (int)nums.size()) ? nums[4] : 10;
        out.kb                 = (5 < (int)nums.size()) ? nums[5] : 14;
        out.num_channels       = (6 < (int)nums.size() && nums[6] > 0) ? nums[6] : 2;
        out.max_run            = (7 < (int)nums.size()) ? nums[7] : 255;
        out.max_frame_bytes    = (8 < (int)nums.size()) ? nums[8] : 0;
        out.avg_bit_rate       = (9 < (int)nums.size()) ? nums[9] : 0;
        out.sample_rate        = (10 < (int)nums.size()) ? nums[10] : 44100;
    } else {
        // 布局 B（含默认）：frameLength version bitDepth pb mb kb ch maxRun maxFB avgBR sr
        out.frame_length       = (0 < (int)nums.size()) ? nums[0] : 4096;
        out.compatible_version = (1 < (int)nums.size()) ? nums[1] : 0;
        out.bit_depth          = (2 < (int)nums.size()) ? nums[2] : 16;
        out.pb                 = (3 < (int)nums.size()) ? nums[3] : 40;
        out.mb                 = (4 < (int)nums.size()) ? nums[4] : 10;
        out.kb                 = (5 < (int)nums.size()) ? nums[5] : 14;
        out.num_channels       = (6 < (int)nums.size() && nums[6] > 0) ? nums[6] : 2;
        out.max_run            = (7 < (int)nums.size()) ? nums[7] : 255;
        out.max_frame_bytes    = (8 < (int)nums.size()) ? nums[8] : 0;
        out.avg_bit_rate       = (9 < (int)nums.size()) ? nums[9] : 0;
        out.sample_rate        = (10 < (int)nums.size()) ? nums[10] : 44100;
    }
    // 兜底：非法/缺失值使用安全默认（参考 ALACSpecificConfig 合理范围）
    if (out.bit_depth != 16 && out.bit_depth != 20 && out.bit_depth != 24 && out.bit_depth != 32)
        out.bit_depth = 16;
    if (out.frame_length < 32 || out.frame_length > 65536) out.frame_length = 4096;
    if (out.pb < 1)  out.pb = 40;
    if (out.mb < 1)  out.mb = 10;
    if (out.kb < 1)  out.kb = 14;
    if (out.num_channels < 1 || out.num_channels > 8) out.num_channels = 2;
    if (out.sample_rate < 8000 || out.sample_rate > 192000) out.sample_rate = 44100;
    return true;
}

// ---- BitReader ----
uint32_t AlacDecoder::BitReader::read(unsigned nbits) {
    if (nbits == 0 || pos_bits + nbits > total * 8) {
        pos_bits += nbits;
        return 0;
    }
    uint32_t out = 0;
    unsigned bit = 0;
    while (bit < nbits) {
        unsigned byte_i = (unsigned)(pos_bits >> 3);
        unsigned shift  = (unsigned)(7 - (pos_bits & 7));
        unsigned take   = std::min(nbits - bit, shift + 1u);
        uint8_t  mask   = (uint8_t)((1u << take) - 1u);
        uint32_t v      = (base[byte_i] >> (shift + 1u - take)) & mask;
        out = (out << take) | v;
        pos_bits += take;
        bit += take;
    }
    return out;
}

uint32_t AlacDecoder::BitReader::peek(unsigned nbits) const {
    BitReader tmp = *this;
    return tmp.read(nbits);
}

/*! floor(log2(v))，v>0 */
static int alac_log2(unsigned v) {
    int r = 0;
    while (v > 1) { r++; v >>= 1; }
    return r;
}

/*! 有符号数按 bits 位符号扩展（FFmpeg sign_extend）。
 * 注意：必须对 int32 做"算术右移"才能正确扩展符号；
 * 若在 uint32 上右移（逻辑右移），负值会被掩成正值（例如 -666 变 130406）。
 * 先左移（unsigned 无 UB）再转 int32 算术右移，兼容 GCC/Clang/MSVC。 */
static inline int32_t sign_extend(int32_t v, int bits) {
    if (bits >= 32) return v;
    int32_t shifted = (int32_t)((uint32_t)v << (32 - bits));
    return shifted >> (32 - bits);
}

/*!
 * Rice 值解码（FFmpeg decode_scalar + get_unary_0_9）
 * @param k   当前 rice 参数（0..rice_limit）
 * @param bps 每样本位数（超过阈值时的直读位数）
 */
uint32_t AlacDecoder::decode_scalar(BitReader& br, int k, int bps) {
    // unary：数连续 1，最多 9 位
    unsigned x = 0;
    while (x < 9 && br.bits_left() > 0 && br.read(1) == 1) x++;
    if (x > 8) {
        // 超过阈值：直接用 bps 位读取
        x = br.read((unsigned)bps);
    } else if (k != 1) {
        uint32_t extrabits = br.peek((unsigned)k);
        x = (x << k) - x; // x * (2^k - 1)
        if (extrabits > 1) {
            x += extrabits - 1;
            br.skip((unsigned)k);
        } else {
            br.skip((unsigned)(k - 1));
        }
    }
    return x;
}

int AlacDecoder::rice_decompress(BitReader& br, int rice_limit, int32_t* output,
                                 int nb_samples, int bps, int rice_history_mult,
                                 int initial_history) {
    unsigned history = (unsigned)initial_history; // rice_initial_history
    int sign_modifier = 0;
    for (int i = 0; i < nb_samples; i++) {
        if (br.bits_left() <= 0) return i;
        // k = av_log2((history >> 9) + 3)，并受 rice_limit 限制
        int k = alac_log2((history >> 9) + 3);
        k = std::min(k, rice_limit);
        unsigned x = decode_scalar(br, k, bps);
        x += (unsigned)sign_modifier;
        sign_modifier = 0;
        // 折半编码：x 偶 → 0，奇 → 负
        output[i] = (int32_t)((x >> 1) ^ (uint32_t)-(int32_t)(x & 1));
        // 更新历史
        if (x > 0xffff) history = 0xffff;
        else history += x * (unsigned)rice_history_mult -
                        ((history * (unsigned)rice_history_mult) >> 9);
        // 零块特殊处理
        if ((history < 128) && (i + 1 < nb_samples)) {
            int block_size;
            int kk = 7 - alac_log2(history) + ((int)(history + 16) >> 6);
            kk = std::min(kk, rice_limit);
            block_size = (int)decode_scalar(br, kk, 16);
            if (block_size > 0) {
                if (block_size >= nb_samples - i) block_size = nb_samples - i - 1;
                std::memset(&output[i + 1], 0, (size_t)block_size * sizeof(int32_t));
                i += block_size;
            }
            if (block_size <= 0xffff) sign_modifier = 1;
            history = 0;
        }
    }
    return nb_samples;
}

/*!
 * LPC 预测重建（FFmpeg lpc_prediction）
 */
static void lpc_prediction(const int32_t* error_buffer, int32_t* buffer_out,
                           int nb_samples, int bps, int16_t* lpc_coefs,
                           int lpc_order, int lpc_quant) {
    buffer_out[0] = error_buffer[0];
    if (nb_samples <= 1) return;
    if (lpc_order == 0) {
        for (int i = 1; i < nb_samples; i++) buffer_out[i] = error_buffer[i];
        return;
    }
    if (lpc_order == 31) {
        // 简单一阶预测
        for (int i = 1; i < nb_samples; i++)
            buffer_out[i] = sign_extend(buffer_out[i - 1] + error_buffer[i], bps);
        return;
    }
    // warm-up 样本
    for (int i = 1; i <= lpc_order && i < nb_samples; i++)
        buffer_out[i] = sign_extend(buffer_out[i - 1] + error_buffer[i], bps);
    // 主循环：pred 是游走指针（从 buffer_out[0] 起，每迭代前进一个），
    // 与 FFmpeg 一致——引用的是"之前的"样本，绝不能包含当前未写出的样本
    const int32_t* pred = buffer_out;
    for (int i = lpc_order + 1; i < nb_samples; i++) {
        int j;
        int val = 0;
        unsigned error_val = (unsigned)error_buffer[i];
        int error_sign;
        int d = *pred++;
        for (j = 0; j < lpc_order; j++)
            val += (pred[j] - d) * lpc_coefs[j];
        val = (val + (1LL << (lpc_quant - 1))) >> lpc_quant;
        val += d + (int)error_val;
        buffer_out[i] = sign_extend(val, bps);
        // 自适应调整 LPC 系数
        // 注意：error_val 是 unsigned，符号必须从"有符号"原始残差取，
        // 否则负残差在 unsigned 下变成巨大正数，error_sign 恒为正（参考 FFmpeg
        // sign_only(error_val)，其通过 int 参数转换保留真实符号）
        error_sign = (error_buffer[i] > 0) - (error_buffer[i] < 0);
        if (error_sign) {
            for (j = 0; j < lpc_order && (int)(error_val * (unsigned)error_sign) > 0; j++) {
                int sign;
                val = d - pred[j];
                sign = ((val > 0) - (val < 0)) * error_sign;
                lpc_coefs[j] = (int16_t)(lpc_coefs[j] - sign);
                val *= (unsigned)sign;
                error_val -= (unsigned)((val >> lpc_quant) * (j + 1));
            }
        }
    }
}

AlacDecoder::AlacDecoder() = default;
AlacDecoder::~AlacDecoder() = default;

bool AlacDecoder::configure(const AlacMagicCookie& cookie) {
    cookie_ = cookie;
    out_cfg_.sample_rate = (uint32_t)cookie.sample_rate;
    out_cfg_.channels    = (uint8_t)cookie.num_channels;
    switch (cookie.bit_depth) {
        case 16: out_cfg_.format = AudioFormat::PCM16LE; break;
        case 24: out_cfg_.format = AudioFormat::PCM24LE; break;
        case 32: out_cfg_.format = AudioFormat::PCM32LE; break;
        case 20: out_cfg_.format = AudioFormat::PCM32LE; break; // 20bit 提升到 32bit 输出
        default:
            out_cfg_.format = AudioFormat::PCM16LE;
            cookie_.bit_depth = 16;
            break;
    }
    configured_ = true;
    reset();
    AP2_LOGI("alac: configured sr=%u ch=%u bits=%d",
             out_cfg_.sample_rate, out_cfg_.channels, cookie_.bit_depth);
    return true;
}

void AlacDecoder::reset() {
    // ALAC 帧是自包含的（每帧携带 LPC 系数与 warm-up），无需跨帧状态
}

int64_t AlacDecoder::decode_frame(const uint8_t* data, size_t len,
                                  std::vector<uint8_t>& out_pcm) {
    if (!configured_ || len < 3) return -1;

    BitReader br;
    br.init(data, len);

    const int sample_size = cookie_.bit_depth;
    const int max_samples = (cookie_.frame_length > 0 && cookie_.frame_length <= 65536)
                                ? cookie_.frame_length : 4096;

    // 本帧解码结果（最多 2 声道）
    std::vector<int32_t> decoded[2];
    uint32_t out_samples = 0;
    int out_channels = cookie_.num_channels > 0 ? cookie_.num_channels : 2;
    bool decoded_any = false;

    // 逐元素解码：TYPE_END 结束；遇到未知元素/位不足/元素解析失败则结束本帧。
    // 注意：部分编码器（如 CAF 流）帧间可能没有 TYPE_END 标签且按字节对齐，
    // 因此不能因"没找到 END"就丢弃整帧——只要解出至少一个完整元素即可。
    while (br.bits_left() >= 3) {
        uint8_t element = (uint8_t)br.read(3);
        if (element == kTypeEnd) break;
        if (element != kTypeSce && element != kTypeCpe && element != kTypeLfe) {
            break; // 未知元素类型（可能已进入下一帧/填充）
        }
        int channels = (element == kTypeCpe) ? 2 : 1;
        out_channels = channels;

        // ---- 元素头（decode_element） ----
        br.skip(4);  // element instance tag
        br.skip(12); // unused header bits
        int has_size = (int)br.read(1);
        int extra_bits = (int)br.read(2) << 3;
        int bps = sample_size - extra_bits + channels - 1;
        if (bps > 32 || bps < 1) break;
        int is_compressed = !(int)br.read(1);
        uint32_t output_samples;
        if (has_size) output_samples = br.read(32);
        else          output_samples = (uint32_t)max_samples;
        if (output_samples == 0 || output_samples > (uint32_t)max_samples) break;

        for (int c = 0; c < channels; c++) decoded[c].assign(output_samples, 0);
        int32_t* out_ptr[2] = { decoded[0].data(), decoded[1].data() };

        if (is_compressed) {
            int decorr_shift = (int)br.read(8);
            int decorr_left_weight = (int)br.read(8);
            if (channels == 2 && decorr_left_weight && decorr_shift > 31) break;

            int16_t lpc_coefs[2][32] = {{0},{0}};
            int lpc_order[2] = {0, 0};
            int lpc_quant[2] = {0, 0};
            int rice_history_mult[2] = {0, 0};
            int prediction_type[2] = {0, 0};

            for (int ch = 0; ch < channels; ch++) {
                prediction_type[ch] = (int)br.read(4);
                lpc_quant[ch]       = (int)br.read(4);
                rice_history_mult[ch] = (int)br.read(3);
                lpc_order[ch]       = (int)br.read(5);
                if (lpc_order[ch] >= max_samples || !lpc_quant[ch]) break;
                // 系数按逆序存储（FFmpeg: for i=order-1..0）
                for (int i = lpc_order[ch] - 1; i >= 0; i--)
                    lpc_coefs[ch][i] = (int16_t)br.read(16);
            }

            // extra bits（20/24/32 位编码的附加低位）——当前输出不携带，直接跳过
            if (extra_bits) {
                if (br.bits_left() < (int)(output_samples * channels * extra_bits)) break;
                for (uint32_t i = 0; i < output_samples; i++)
                    for (int ch = 0; ch < channels; ch++)
                        br.skip((unsigned)extra_bits);
            }

            for (int ch = 0; ch < channels; ch++) {
                std::vector<int32_t> err(output_samples);
                // cookie 映射（与 FFmpeg alac_set_extradata 一致）：
                //   pb=rice_history_mult 基准(40)，mb=rice_initial_history(10)，
                //   kb=rice_limit(14)
                int got = rice_decompress(br, cookie_.kb, err.data(),
                                          (int)output_samples, bps,
                                          rice_history_mult[ch] * cookie_.pb / 4,
                                          cookie_.mb);
                if (got != (int)output_samples) break;
                // prediction_type 15：先做一次 31 阶自适应 FIR（参考编码器不用）
                if (prediction_type[ch] == 15) {
                    lpc_prediction(err.data(), err.data(), (int)output_samples,
                                   bps, nullptr, 31, 0);
                }
                lpc_prediction(err.data(), out_ptr[ch], (int)output_samples, bps,
                               lpc_coefs[ch], lpc_order[ch], lpc_quant[ch]);
            }

            // 立体声去相关（FFmpeg decorrelate_stereo）
            if (channels == 2 && decorr_left_weight) {
                for (uint32_t i = 0; i < output_samples; i++) {
                    int32_t a = decoded[0][i];
                    int32_t b = decoded[1][i];
                    a -= (b * decorr_left_weight) >> decorr_shift;
                    decoded[0][i] = a;
                    decoded[1][i] = a + b;
                }
            }
        } else {
            // 未压缩（verbatim）：直接读样本
            if (br.bits_left() < (int)(output_samples * channels * sample_size)) break;
            for (uint32_t i = 0; i < output_samples; i++)
                for (int ch = 0; ch < channels; ch++)
                    out_ptr[ch][i] = (int32_t)br.read((unsigned)sample_size);
        }

        // 20/24 位提升（FFmpeg：20<<12, 24<<8）
        if (sample_size == 20) {
            for (int c = 0; c < channels; c++)
                for (uint32_t i = 0; i < output_samples; i++) decoded[c][i] <<= 12;
        } else if (sample_size == 24) {
            for (int c = 0; c < channels; c++)
                for (uint32_t i = 0; i < output_samples; i++) decoded[c][i] <<= 8;
        }

        out_samples = output_samples;
        decoded_any = true;
    }
    if (!decoded_any || out_samples == 0) return -1;

    // ---- 交织输出 PCM ----
    const int bytes_per_sample = (sample_size == 24) ? 3 : 4; // 16/20/32 → 4 字节，24 → 3 字节
    // 16 位输出 2 字节
    const size_t out_bytes = (size_t)out_samples * out_channels *
                             ((sample_size == 16) ? 2 : bytes_per_sample);
    out_pcm.resize(out_bytes);
    size_t off = 0;
    if (sample_size == 16) {
        for (uint32_t i = 0; i < out_samples; i++) {
            for (int c = 0; c < out_channels; c++) {
                int32_t s = decoded[c][i];
                if (s >  32767) s =  32767;
                if (s < -32768) s = -32768;
                int16_t v = (int16_t)s;
                out_pcm[off++] = (uint8_t)v;
                out_pcm[off++] = (uint8_t)((uint16_t)v >> 8);
            }
        }
    } else if (sample_size == 24) {
        for (uint32_t i = 0; i < out_samples; i++) {
            for (int c = 0; c < out_channels; c++) {
                int32_t s = decoded[c][i];
                if (s >  0x7FFFFF) s =  0x7FFFFF;
                if (s < -0x800000) s = -0x800000;
                uint32_t u = (uint32_t)s;
                out_pcm[off++] = (uint8_t)u;
                out_pcm[off++] = (uint8_t)(u >> 8);
                out_pcm[off++] = (uint8_t)(u >> 16);
            }
        }
    } else { // 20 → 32bit 或原生 32bit
        for (uint32_t i = 0; i < out_samples; i++) {
            for (int c = 0; c < out_channels; c++) {
                uint32_t u = (uint32_t)decoded[c][i];
                out_pcm[off++] = (uint8_t)u;
                out_pcm[off++] = (uint8_t)(u >> 8);
                out_pcm[off++] = (uint8_t)(u >> 16);
                out_pcm[off++] = (uint8_t)(u >> 24);
            }
        }
    }
    return (int64_t)br.bytes_used();
}

} // namespace codec
} // namespace airplay2
