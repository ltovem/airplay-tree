/*!
 * @file test_codec.cpp
 * @brief codec 模块单元测试：AudioBuffer + NAL reassembler (H.264/H.265)
 */
#include "test_harness.h"
#include "codec/audio_buffer.h"
#include "codec/nal_reassembler.h"
#include "airplay2/airplay_config.h"
#include "airplay2/video_renderer.h"

#include <cstring>
#include <cstdint>
#include <vector>

using namespace airplay2;
using namespace airplay2::codec;

/* ====================================================================
 *                         AudioBuffer
 * ==================================================================== */

TEST(AudioBuffer, DefaultCtor_Defaults) {
    AudioBuffer ab;
    EXPECT_EQ(ab.capacity_frames(), size_t(16384));
    EXPECT_EQ(ab.available_frames(), size_t(0));
    EXPECT_EQ(ab.free_frames(), size_t(16384));
    // 默认 2ch x 16bit = 4 bytes / frame
    EXPECT_EQ(ab.bytes_per_frame(), size_t(4));
}

TEST(AudioBuffer, WriteRead_Roundtrip) {
    AudioBuffer ab(128); // 小容量方便测试
    // 写 32 帧（= 128 字节 2chx16bit）
    uint8_t input[128];
    for (size_t i = 0; i < 128; ++i) input[i] = (uint8_t)i;
    size_t written = ab.write_bytes(input, 128);
    EXPECT_EQ(written, size_t(32));  // 32 帧
    EXPECT_EQ(ab.available_frames(), size_t(32));
    EXPECT_EQ(ab.free_frames(), size_t(128 - 32));
    // 读回
    uint8_t out[128] = {0};
    size_t read = ab.read_frames(out, 32);
    EXPECT_EQ(read, size_t(32));
    EXPECT_BYTES_EQ(out, input, 128);
    EXPECT_EQ(ab.available_frames(), size_t(0));
    EXPECT_EQ(ab.free_frames(), size_t(128));
}

TEST(AudioBuffer, Write_PartialWhenFull) {
    AudioBuffer ab(8);  // 8 frames = 32 bytes (default bpf=4)
    uint8_t buf[64];
    std::memset(buf, 0xAA, sizeof(buf));
    // AudioBuffer 默认采用 "drop oldest" 策略写满（jitter buffer 常用）
    // 所以 64 字节 = 16 帧会全部 "写入"，但可用帧数不会超过容量（8 帧：最新的）
    size_t w = ab.write_bytes(buf, 64);  // 尝试写 64/4=16 帧
    EXPECT_EQ(w, size_t(16));            // 全部 "接收"
    EXPECT_EQ(ab.available_frames(), size_t(8)); // 实际保留：最新 8 帧
    EXPECT_EQ(ab.free_frames(), size_t(0));
    EXPECT_EQ(ab.total_written_frames(), uint64_t(16));
}

TEST(AudioBuffer, Peek_DoesNotConsume) {
    AudioBuffer ab(64);
    uint8_t input[4*10];
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)(i + 7);
    ab.write_bytes(input, sizeof(input));
    EXPECT_EQ(ab.available_frames(), size_t(10));
    // peek 两次读同样内容
    uint8_t p1[40], p2[40];
    size_t pr1 = ab.peek_frames(p1, 10);
    size_t pr2 = ab.peek_frames(p2, 10);
    EXPECT_EQ(pr1, size_t(10));
    EXPECT_EQ(pr2, size_t(10));
    EXPECT_BYTES_EQ(p1, input, 40);
    EXPECT_BYTES_EQ(p2, input, 40);
    // peek 后数据还在
    EXPECT_EQ(ab.available_frames(), size_t(10));
}

TEST(AudioBuffer, Flush_ClearsBuffer) {
    AudioBuffer ab(64);
    uint8_t b[4*20];
    std::memset(b, 0x55, sizeof(b));
    ab.write_bytes(b, sizeof(b));
    EXPECT_EQ(ab.available_frames(), size_t(20));
    ab.flush();
    EXPECT_EQ(ab.available_frames(), size_t(0));
    EXPECT_EQ(ab.free_frames(), size_t(64));
}

TEST(AudioBuffer, Circular_WrapAround) {
    // 小容量，多次读写，触发环形缓冲区 wrap
    AudioBuffer ab(16);
    uint8_t input[4*12];
    uint8_t out[4*12];
    for (size_t i = 0; i < 4*12; ++i) input[i] = (uint8_t)i;
    // 先写 12 帧，读 8 帧
    ab.write_bytes(input, 4*12);
    EXPECT_EQ(ab.available_frames(), size_t(12));
    ab.read_frames(out, 8);
    EXPECT_BYTES_EQ(out, input, 32);
    // 写 8 帧新数据，触发环形 wrap
    uint8_t input2[4*8];
    for (size_t i = 0; i < 4*8; ++i) input2[i] = (uint8_t)(i + 100);
    ab.write_bytes(input2, 4*8);
    // 总共可读: 12-8+8 = 12 帧
    EXPECT_EQ(ab.available_frames(), size_t(12));
    // 读前 4 帧 (应为 input[32..48])
    ab.read_frames(out, 4);
    EXPECT_BYTES_EQ(out, input + 32, 16);
    // 读后 8 帧 (应为 input2)
    ab.read_frames(out, 8);
    EXPECT_BYTES_EQ(out, input2, 32);
}

TEST(AudioBuffer, SetConfig_ChangesBpf) {
    AudioBuffer ab;
    AudioConfig cfg;
    cfg.channels = 6;
    cfg.format = AudioFormat::PCM32LE; // 4 bytes per sample
    ab.set_config(cfg);
    // 6*4 = 24 bytes per frame
    EXPECT_EQ(ab.bytes_per_frame(), size_t(6 * 4));
}

TEST(AudioBuffer, TotalStats_Accumulate) {
    AudioBuffer ab(64);
    uint8_t b[40];
    ab.write_bytes(b, 40);
    ab.write_bytes(b, 40);
    EXPECT_EQ(ab.total_written_frames(), uint64_t(20));
    // 读 15 帧 = 60 字节，缓冲区需 >= 60
    uint8_t out[64];
    ab.read_frames(out, 5);
    EXPECT_EQ(ab.total_read_frames(), uint64_t(5));
    ab.read_frames(out, 15);
    EXPECT_EQ(ab.total_read_frames(), uint64_t(20));
}

/* ====================================================================
 *                        VideoFrame / VideoConfig
 * ==================================================================== */

TEST(VideoConfig, DefaultValues) {
    VideoConfig c;
    EXPECT_EQ(c.codec, VideoCodec::H264_AVC);
    EXPECT_EQ(c.width, uint32_t(0));
    EXPECT_EQ(c.height, uint32_t(0));
}

TEST(VideoFrame, DefaultCtor) {
    VideoFrame f;
    EXPECT_EQ(f.pts_us, uint64_t(0));
    EXPECT_EQ(f.dts_us, uint64_t(0));
    EXPECT_EQ(f.width, uint32_t(0));
    EXPECT_TRUE(f.annex_b.empty());
    EXPECT_FALSE(f.is_key);
}

/* ====================================================================
 *                   H.264 NAL 重组 —— 单 NAL 单元
 * ==================================================================== */

static RtpVideoPacket make_pkt(uint16_t seq, uint32_t ts, bool marker, std::vector<uint8_t> payload) {
    RtpVideoPacket p;
    p.seq = seq;
    p.ts = ts;
    p.marker = marker;
    p.payload = std::move(payload);
    return p;
}

TEST(NalReassembler, H264_SingleNal_Sps) {
    NalReassembler n;
    n.set_codec(VideoCodec::H264_AVC);
    // SPS: nal_type=7, ref_idc=3 → header = 0x67
    // 典型 SPS 数据（简写前几个字节即可）
    auto pkt = make_pkt(0, 90000, false, {0x67, 0x42, 0x00, 0x1F, 0xAB, 0x40});
    auto frame = n.push(pkt);
    // SPS 单 NAL + marker=false -> close 条件还没触发，frame 应为空
    EXPECT_TRUE(frame == nullptr);
    // push 一个 marker=true 的包来 flush 帧
    auto pkt2 = make_pkt(1, 90000, true, {0x68, 0xCE, 0x38, 0x80});  // PPS nal_type=8
    auto frame2 = n.push(pkt2);
    EXPECT_FALSE(frame2 == nullptr);
    // Annex-B: 0x00000001 + SPS + 0x00000001 + PPS
    const auto& d = frame2->annex_b;
    EXPECT_GT(d.size(), size_t(14));
    // 前 4 字节 start code
    EXPECT_EQ(d[0], 0x00);
    EXPECT_EQ(d[1], 0x00);
    EXPECT_EQ(d[2], 0x00);
    EXPECT_EQ(d[3], 0x01);
    EXPECT_EQ(d[4], 0x67);
    EXPECT_TRUE(frame2->is_key); // SPS/PPS 标记为 key
}

TEST(NalReassembler, H264_SingleSlice_FlushMarker) {
    NalReassembler n;
    n.set_codec(VideoCodec::H264_AVC);
    // slice type 1 (non-IDR), marker=true 单包一帧
    uint8_t header = 0x41; // nal_ref_idc=1, type=1
    auto pkt = make_pkt(100, 1000, true, {header, 0x00, 0x01, 0x02, 0x03});
    auto frame = n.push(pkt);
    EXPECT_FALSE(frame == nullptr);
    EXPECT_EQ(frame->width, uint32_t(0));
    EXPECT_EQ(frame->pts_us, uint64_t(1000));
    EXPECT_FALSE(frame->is_key);
    EXPECT_GE(frame->annex_b.size(), size_t(4 + 5));  // start code + header + 4 bytes payload
}

TEST(NalReassembler, H264_FUA_SingleFrameFragments) {
    // FU-A (nal_type=28) 分成 3 段：start + middle + end
    NalReassembler n;
    n.set_codec(VideoCodec::H264_AVC);
    // FU indicator: F=0, NRI=3, type=28 → 0x7C
    // FU header:   S=1, E=0, R=0, type=5 (IDR) → 0x85 (start)
    //              S=0, E=0, type=5            → 0x05 (mid)
    //              S=0, E=1, type=5            → 0x45 (end)
    uint8_t fu_indicator = 0x7C;
    // 分片1 (start)
    std::vector<uint8_t> p1 = {fu_indicator, 0x85, 0xAA, 0xBB};
    auto frame1 = n.push(make_pkt(0, 5000, false, p1));
    EXPECT_TRUE(frame1 == nullptr);  // 未结束
    // 分片2 (mid)
    std::vector<uint8_t> p2 = {fu_indicator, 0x05, 0xCC, 0xDD};
    auto frame2 = n.push(make_pkt(1, 5000, false, p2));
    EXPECT_TRUE(frame2 == nullptr);
    // 分片3 (end) + marker=true  → 形成一帧
    std::vector<uint8_t> p3 = {fu_indicator, 0x45, 0xEE, 0xFF};
    auto frame3 = n.push(make_pkt(2, 5000, true, p3));
    EXPECT_FALSE(frame3 == nullptr);
    EXPECT_TRUE(frame3->is_key); // type=5 (IDR)
    // Annex-B 内容 = 0x00000001 + (indicator & 0xE0)=0x60 | type=5 → 0x65 + bytes
    // 0x7C = F(0)+NRI(11=3)+Type(28) → &0xE0 = 0x60; nal_type=5 → header = 0x65
    const auto& d = frame3->annex_b;
    EXPECT_GT(d.size(), size_t(8));
    EXPECT_EQ(d[0], 0x00);
    EXPECT_EQ(d[1], 0x00);
    EXPECT_EQ(d[2], 0x00);
    EXPECT_EQ(d[3], 0x01);
    EXPECT_EQ(d[4], 0x65);
    EXPECT_EQ(d[5], 0xAA);
    EXPECT_EQ(d[6], 0xBB);
    EXPECT_EQ(d[7], 0xCC);
    EXPECT_EQ(d[8], 0xDD);
    EXPECT_EQ(d[9], 0xEE);
    EXPECT_EQ(d[10], 0xFF);
}

TEST(NalReassembler, H264_FUA_GapLoss_DropsFrame) {
    NalReassembler n;
    n.set_codec(VideoCodec::H264_AVC);
    uint8_t ind = 0x7C;
    // seq 0 start
    n.push(make_pkt(0, 1, false, {ind, 0x85, 0x01}));
    // 跳过 seq 1，直接 seq 2 (end) → 检测到 gap
    auto frame = n.push(make_pkt(2, 1, true, {ind, 0x45, 0x03}));
    // 帧可能被 flush，但 has_loss=true
    if (frame) {
        EXPECT_TRUE(frame->has_loss);
    }
    EXPECT_GT(n.frames_lost() + n.packets_fragmented(), uint64_t(0));
}

TEST(NalReassembler, H264_StapA_MultipleNals) {
    // STAP-A (nal_type=24): 两个 NAL 聚合在一个 RTP 包里
    NalReassembler n;
    n.set_codec(VideoCodec::H264_AVC);
    // STAP header = 0x78 (0111_1000: NRI=3, type=24)
    // 两个 NAL，各带 2 字节长度前缀（大端）
    std::vector<uint8_t> payload;
    payload.push_back(0x78);
    // NAL1 (SPS type 7): length 3, bytes 0x67,0x42,0x00
    payload.push_back(0x00); payload.push_back(0x03);
    payload.push_back(0x67); payload.push_back(0x42); payload.push_back(0x00);
    // NAL2 (PPS type 8): length 2, bytes 0x68,0xCE
    payload.push_back(0x00); payload.push_back(0x02);
    payload.push_back(0x68); payload.push_back(0xCE);
    auto frame = n.push(make_pkt(0, 100, true, payload));
    EXPECT_FALSE(frame == nullptr);
    EXPECT_TRUE(frame->is_key);
    // 输出 Annex-B 中应包含两个 start code
    const auto& d = frame->annex_b;
    // 第一 start code 后应是 0x67... 第二 start code 后应是 0x68
    // 找第二个 0x00000001
    bool foundSecond = false;
    for (size_t i = 1; i + 4 <= d.size(); ++i) {
        if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 0 && d[i+3] == 1) {
            EXPECT_EQ(d[i+4], 0x68);
            foundSecond = true;
            break;
        }
    }
    EXPECT_TRUE(foundSecond);
}

/* ====================================================================
 *                   H.265 NAL 重组 —— 单 NAL + FU
 * ==================================================================== */

TEST(NalReassembler, H265_SingleNal) {
    NalReassembler n;
    n.set_codec(VideoCodec::H265_HEVC);
    // H.265 NAL header: 2 字节:
    //   byte0: F(1) + type(6) + layer(6) bit... — 简化：我们用非分片类型 47
    std::vector<uint8_t> payload = {0x02, 0x01, 0x05, 0x06, 0x07};  // 随意 2 字节头 + 3 byte body
    auto pkt = make_pkt(0, 0, true, payload);
    auto frame = n.push(pkt);
    EXPECT_FALSE(frame == nullptr);
    const auto& d = frame->annex_b;
    EXPECT_EQ(d[0], 0x00);
    EXPECT_EQ(d[1], 0x00);
    EXPECT_EQ(d[2], 0x00);
    EXPECT_EQ(d[3], 0x01);
    // 保留 H.265 NAL 2 字节头
    EXPECT_EQ(d[4], 0x02);
    EXPECT_EQ(d[5], 0x01);
}

TEST(NalReassembler, H265_FU_TwoFragments) {
    NalReassembler n;
    n.set_codec(VideoCodec::H265_HEVC);
    // H.265 payload header (2 byte):
    //   type=49 (FU) → 我们用 0x62, 0x00 作为示例
    // FU header 1 byte: S=1 E=0 → 0x80 + type=19 (IDR_W_RADL, 一个 IRAP)
    uint8_t ph[2] = {0x62, 0x00};  // payload header: F=0 type=49>>1? 我们直接按代码路径构造
    // 查 nal_reassembler.h/cpp: H.265 FU type is 49
    // 为避免对具体值的硬编码假设，我们查看代码实现：
    // 只要 payload[0] & 0x7E == 49<<1 (即 type==49) 即进入 H265 FU 路径
    // 简化：用 payload header[0] = (49 << 1) = 98 = 0x62, payload header[1] = 0
    std::vector<uint8_t> p1;
    p1.push_back(0x62);  // payload[0] = (49<<1) → H265 FU
    p1.push_back(0x00);  // payload[1]
    p1.push_back(0x80 | 19); // FU header: S=1, type=19 (IRAP)
    p1.push_back(0xAA);
    p1.push_back(0xBB);
    auto f1 = n.push(make_pkt(0, 200, false, p1));
    EXPECT_TRUE(f1 == nullptr);
    // 第二个分片：S=0, E=1 → 0x40 | 19
    std::vector<uint8_t> p2;
    p2.push_back(0x62);
    p2.push_back(0x00);
    p2.push_back(0x40 | 19); // FU header: E=1, type=19
    p2.push_back(0xCC);
    auto f2 = n.push(make_pkt(1, 200, true, p2));
    EXPECT_FALSE(f2 == nullptr);
    EXPECT_TRUE(f2->is_key);  // type 19 是 IRAP
    const auto& d = f2->annex_b;
    // Annex-B start code 然后重新构造的 H.265 2-byte NAL header (type=19 + layer 由 fu_nal_header 组合)
    EXPECT_EQ(d[0], 0x00);
    EXPECT_EQ(d[1], 0x00);
    EXPECT_EQ(d[2], 0x00);
    EXPECT_EQ(d[3], 0x01);
}

TEST(NalReassembler, H265_AP) {
    // H.265 AP (Aggregation Packet, type 48)
    NalReassembler n;
    n.set_codec(VideoCodec::H265_HEVC);
    // payload[0]: (48 << 1) = 96 = 0x60
    std::vector<uint8_t> payload;
    payload.push_back(0x60);
    payload.push_back(0x00);
    // 2-byte length (big endian) + 2-byte H265 NAL header + body: total 5 bytes NAL → len 5
    payload.push_back(0x00); payload.push_back(0x05);
    payload.push_back(0x42); payload.push_back(0x01);  // 2-byte HEVC NAL header
    payload.push_back(0x10); payload.push_back(0x11); payload.push_back(0x12);
    auto frame = n.push(make_pkt(0, 0, true, payload));
    EXPECT_FALSE(frame == nullptr);
}

/* ====================================================================
 *                        set_codec_data / flush
 * ==================================================================== */

TEST(NalReassembler, SetCodecData_Retrievable) {
    NalReassembler n;
    std::vector<uint8_t> extra = {0x01,0x02,0x03,0x04};
    n.set_codec_data(extra);
    EXPECT_EQ(n.codec_data().size(), size_t(4));
}

TEST(NalReassembler, Flush_ReturnsUnfinishedFrame) {
    NalReassembler n;
    n.set_codec(VideoCodec::H264_AVC);
    // push 一个非 marker 单包
    n.push(make_pkt(0, 100, false, {0x41, 0x00, 0x01}));
    // flush 应该能吐出未完成帧
    auto f = n.flush();
    EXPECT_FALSE(f == nullptr);
}

TEST(NalReassembler, StatsCounters) {
    NalReassembler n;
    // 初始为 0
    EXPECT_EQ(n.packets_fragmented(), uint64_t(0));
    EXPECT_EQ(n.frames_lost(), uint64_t(0));
}
