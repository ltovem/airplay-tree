/*!
 * @file rtp.h
 * @brief 共享 RTP 基础类型（音频 + 视频共用）
 *
 * 把视频 / 音频 RTP 都要用到的轻量结构、常量、解析辅助放在这里，
 * 避免 rtp_receiver.h 和 video_rtp.h 重复造轮子。
 */
#ifndef AIRPLAY2_NET_RTP_H
#define AIRPLAY2_NET_RTP_H

#include <cstdint>
#include <cstddef>
#include <vector>

namespace airplay2 {
namespace net {

/// @brief 基础 RTP 固定头大小（没有任何 CSRC / extension 时）
constexpr size_t kRtpFixedHeaderSize = 12;

/// @brief 最小的合法 RTP 报文长度
constexpr size_t kRtpMinPacketSize = kRtpFixedHeaderSize;

/*!
 * @brief 解析 RTP 固定头（不做 payload 解码；无 CRC/SSRC 校验）
 *
 * @param[in]  data      RTP 报文起始地址
 * @param[in]  len       报文总字节数
 * @param[out] out_seq   RTP sequence number
 * @param[out] out_ts    RTP timestamp
 * @param[out] out_ssrc  SSRC
 * @param[out] out_pt    Payload Type
 * @param[out] out_m     Marker bit
 * @param[out] out_off   从 data 到"纯 RTP payload"的偏移（含 CSRC list / 扩展头）
 * @return true  解析成功，out_off <= len
 * @return false 报文太短或 Version != 2
 */
static inline bool rtp_parse_header(const uint8_t* data, size_t len,
                                    uint16_t& out_seq, uint32_t& out_ts,
                                    uint32_t& out_ssrc, uint8_t& out_pt,
                                    bool& out_m, size_t& out_off) {
    if (len < kRtpFixedHeaderSize) return false;
    uint8_t ver = (data[0] & 0xC0) >> 6;
    if (ver != 2) return false;
    bool pad = (data[0] & 0x20) != 0;
    bool ext = (data[0] & 0x10) != 0;
    uint8_t cc  = data[0] & 0x0F;
    out_m  = (data[1] & 0x80) != 0;
    out_pt = data[1] & 0x7F;
    out_seq  = (uint16_t(data[2]) << 8) | data[3];
    out_ts   = (uint32_t(data[4]) << 24) | (uint32_t(data[5]) << 16)
             | (uint32_t(data[6]) << 8)  | uint32_t(data[7]);
    out_ssrc = (uint32_t(data[8])  << 24) | (uint32_t(data[9])  << 16)
             | (uint32_t(data[10]) << 8)  | uint32_t(data[11]);
    size_t off = kRtpFixedHeaderSize + 4 * (size_t)cc;
    if (off > len) return false;
    if (ext) {
        if (off + 4 > len) return false;
        // 扩展头：profile(2) + length(2)；length = 扩展 word 数
        uint16_t ext_words = (uint16_t(data[off + 2]) << 8) | data[off + 3];
        off += 4 + 4 * (size_t)ext_words;
        if (off > len) return false;
    }
    if (pad && len > off) {
        size_t pad_len = data[len - 1];
        if (pad_len <= (len - off)) {
            len -= pad_len;
        }
    }
    out_off = off;
    return true;
}

/// @brief 16-bit RTP seq 的"带符号差"，用于判断 wrap 时先后
static inline int16_t rtp_seq_delta(uint16_t a, uint16_t b) {
    return (int16_t)(a - b);
}

} // namespace net
} // namespace airplay2

#endif // AIRPLAY2_NET_RTP_H
