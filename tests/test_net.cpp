/*!
 * @file test_net.cpp
 * @brief net 模块单元测试：HttpRequestParser / HttpResponse + RTCP SR/RR + Timing
 */
#include "test_harness.h"
#include "net/http_parser.h"
#include "net/rtcp.h"
#include "net/timing.h"

#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

using namespace airplay2::net;

/* ====================================================================
 *                  HttpRequestParser —— RTSP 请求
 * ==================================================================== */

TEST(HttpParser, RtspOptions_Simple) {
    // AirPlay 2 建立连接时第一个请求典型 OPTIONS
    const char* req =
        "OPTIONS * RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "DACP-ID: 12345\r\n"
        "Active-Remote: 98765\r\n"
        "User-Agent: AirPlay/605.30.1\r\n"
        "\r\n";
    HttpRequestParser p;
    size_t consumed = p.parse((const uint8_t*)req, std::strlen(req));
    EXPECT_EQ(consumed, std::strlen(req));
    EXPECT_TRUE(p.is_complete());
    EXPECT_FALSE(p.has_error());
    HttpRequest r = p.take_request();
    EXPECT_STREQ(r.method.c_str(), "OPTIONS");
    EXPECT_STREQ(r.uri.c_str(), "*");
    EXPECT_STREQ(r.protocol.c_str(), "RTSP/1.0");
    EXPECT_EQ(r.cseq(), 1);
    EXPECT_STREQ(r.header("User-Agent").c_str(), "AirPlay/605.30.1");
    // header 大小写不敏感
    EXPECT_STREQ(r.header("dacp-id").c_str(), "12345");
    EXPECT_STREQ(r.header("ACTIVE-REMOTE").c_str(), "98765");
    EXPECT_TRUE(r.body.empty());
    // parser 已重置
    EXPECT_EQ(p.state(), HttpRequestParser::S_METHOD);
}

TEST(HttpParser, RtspAnnounce_WithContentLength) {
    // ANNOUNCE 带 SDP body
    const char* sdp =
        "v=0\r\n"
        "o=iTunes 123 0 IN IP4 192.168.1.5\r\n"
        "s=iTunes\r\n";
    size_t sdp_len = std::strlen(sdp);
    std::string req_str =
        "ANNOUNCE rtsp://192.168.1.2/21345 RTSP/1.0\r\n"
        "CSeq: 3\r\n"
        "Content-Type: application/sdp\r\n";
    req_str += "Content-Length: " + std::to_string(sdp_len) + "\r\n";
    req_str += "\r\n";
    req_str += sdp;
    HttpRequestParser p;
    size_t consumed = p.parse((const uint8_t*)req_str.data(), req_str.size());
    EXPECT_EQ(consumed, req_str.size());
    EXPECT_TRUE(p.is_complete());
    HttpRequest r = p.take_request();
    EXPECT_STREQ(r.method.c_str(), "ANNOUNCE");
    EXPECT_EQ(r.body.size(), sdp_len);
    EXPECT_BYTES_EQ(r.body.data(), sdp, sdp_len);
}

TEST(HttpParser, HttpPost_ChunkedBody) {
    // chunked transfer encoding: AirPlay /action 用它传 binary plist
    // Chunks:
    //   4\r\nWiki\r\n
    //   6\r\npedia \r\n
    //   C\r\nin \r\nchunks.\r\n     ; 0xC = 12 bytes
    //   0\r\n\r\n
    const char* req =
        "POST /action HTTP/1.1\r\n"
        "CSeq: 7\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4\r\nWiki\r\n"
        "6\r\npedia \r\n"
        "C\r\nin \r\nchunks.\r\n"
        "0\r\n\r\n";
    HttpRequestParser p;
    size_t consumed = p.parse((const uint8_t*)req, std::strlen(req));
    EXPECT_EQ(consumed, std::strlen(req));
    EXPECT_TRUE(p.is_complete());
    HttpRequest r = p.take_request();
    EXPECT_STREQ(r.method.c_str(), "POST");
    EXPECT_STREQ(r.uri.c_str(), "/action");
    // 拼接结果 "Wikipedia in \r\nchunks."  = 4+6+12 = 22 bytes
    std::string expected = "Wikipedia in \r\nchunks.";
    EXPECT_EQ(r.body.size(), expected.size());
    EXPECT_BYTES_EQ(r.body.data(), expected.data(), expected.size());
}

TEST(HttpParser, Incremental_FeedByteAtATime) {
    // 模拟网络分片逐字节输入，验证状态机鲁棒
    const char* req =
        "SETUP rtsp://srv/1 RTSP/1.0\r\n"
        "CSeq: 5\r\n"
        "\r\n";
    HttpRequestParser p;
    size_t total = std::strlen(req);
    for (size_t i = 0; i < total; ++i) {
        size_t c = p.parse((const uint8_t*)req + i, 1);
        EXPECT_EQ(c, size_t(1));
    }
    EXPECT_TRUE(p.is_complete());
    HttpRequest r = p.take_request();
    EXPECT_STREQ(r.method.c_str(), "SETUP");
    EXPECT_EQ(r.cseq(), 5);
}

TEST(HttpParser, Incremental_FeedTwoRequestsBackToBack) {
    // 验证一次解析一个请求、下次继续解析剩余数据
    const char* req =
        "GET /info HTTP/1.1\r\n"
        "CSeq: 10\r\n"
        "\r\n"
        "POST /x HTTP/1.1\r\n"
        "Content-Length: 3\r\n"
        "\r\n"
        "abc";
    HttpRequestParser p;
    size_t total = std::strlen(req);
    size_t c1 = p.parse((const uint8_t*)req, total);
    EXPECT_TRUE(p.is_complete());
    EXPECT_GT(c1, size_t(0));
    HttpRequest r1 = p.take_request();
    EXPECT_STREQ(r1.method.c_str(), "GET");
    EXPECT_STREQ(r1.uri.c_str(), "/info");
    // 解析剩余字节
    size_t c2 = p.parse((const uint8_t*)req + c1, total - c1);
    EXPECT_EQ(c2, total - c1);
    EXPECT_TRUE(p.is_complete());
    HttpRequest r2 = p.take_request();
    EXPECT_STREQ(r2.method.c_str(), "POST");
    EXPECT_EQ(r2.body.size(), size_t(3));
    EXPECT_BYTES_EQ(r2.body.data(), "abc", 3);
}

TEST(HttpParser, Missing_CSeq_Defaults0) {
    const char* req = "OPTIONS * RTSP/1.0\r\n\r\n";
    HttpRequestParser p;
    p.parse((const uint8_t*)req, std::strlen(req));
    EXPECT_TRUE(p.is_complete());
    HttpRequest r = p.take_request();
    EXPECT_EQ(r.cseq(), 0);
}

TEST(HttpParser, MissingContentLength_BodyEmpty) {
    // 没有 Content-Length 也没 chunked → body 为空
    const char* req = "GET /x HTTP/1.1\r\n\r\n";
    HttpRequestParser p;
    p.parse((const uint8_t*)req, std::strlen(req));
    EXPECT_TRUE(p.is_complete());
    EXPECT_TRUE(p.take_request().body.empty());
}

/* ====================================================================
 *                        HttpResponse 序列化
 * ==================================================================== */

TEST(HttpResponse, MakeRtspOk_WithCseq) {
    auto resp = make_rtsp_ok(5);
    std::string s = resp.serialize();
    EXPECT_TRUE(s.find("RTSP/1.0 200 OK") != std::string::npos);
    EXPECT_TRUE(s.find("CSeq: 5") != std::string::npos);
}

TEST(HttpResponse, SetBodyStr_SetsContentLength) {
    HttpResponse r;
    r.code = 200;
    r.reason = "OK";
    r.set_body_str("hello world");
    EXPECT_EQ(r.body.size(), size_t(11));
    std::string s = r.serialize();
    EXPECT_TRUE(s.find("Content-Length: 11") != std::string::npos);
    // 末尾必须带 body
    auto pos = s.find("hello world");
    EXPECT_TRUE(pos != std::string::npos);
}

TEST(HttpResponse, SetBodyStr_AddsContentLengthHeader) {
    HttpResponse r;
    r.code = 404;
    r.reason = "Not Found";
    r.set_body_str("nope");
    EXPECT_EQ(r.body.size(), size_t(4));
    std::string s = r.serialize();
    EXPECT_TRUE(s.find("HTTP/1.1 404 Not Found") != std::string::npos ||
                s.find("RTSP/1.0 404 Not Found") != std::string::npos ||
                s.find("404 Not Found") != std::string::npos);
    EXPECT_TRUE(s.find("Content-Length: 4") != std::string::npos);
}

/* ====================================================================
 *                           RTCP 处理
 * ==================================================================== */

// 构造合法 SR 包（RFC 3550 §6.4.1）
//  Header (4 bytes): v=2 p=0 rc=0 → 0x80, PT=200(SR)=0xC8, length=6(words)
//  SSRC (4 bytes)
//  NTP MSW (4), NTP LSW (4), RTP ts (4), sender pkt count(4), sender octet count(4)
static std::vector<uint8_t> make_sr_packet(uint32_t ssrc,
                                           uint32_t ntp_msw, uint32_t ntp_lsw,
                                           uint32_t rtp_ts) {
    std::vector<uint8_t> b;
    // header
    b.push_back(0x80);   // v=2, p=0, rc=0
    b.push_back(200);    // PT=SR
    b.push_back(0x00);
    b.push_back(0x06);   // length = 6 (7 个 32-bit word - 1 = 6)
    // SSRC
    b.push_back((ssrc >> 24) & 0xFF);
    b.push_back((ssrc >> 16) & 0xFF);
    b.push_back((ssrc >> 8) & 0xFF);
    b.push_back(ssrc & 0xFF);
    // NTP MSW
    b.push_back((ntp_msw >> 24) & 0xFF);
    b.push_back((ntp_msw >> 16) & 0xFF);
    b.push_back((ntp_msw >> 8) & 0xFF);
    b.push_back(ntp_msw & 0xFF);
    // NTP LSW
    b.push_back((ntp_lsw >> 24) & 0xFF);
    b.push_back((ntp_lsw >> 16) & 0xFF);
    b.push_back((ntp_lsw >> 8) & 0xFF);
    b.push_back(ntp_lsw & 0xFF);
    // RTP ts
    b.push_back((rtp_ts >> 24) & 0xFF);
    b.push_back((rtp_ts >> 16) & 0xFF);
    b.push_back((rtp_ts >> 8) & 0xFF);
    b.push_back(rtp_ts & 0xFF);
    // sender's packet count
    for (int i = 0; i < 4; ++i) b.push_back(0x00);
    // sender's octet count
    for (int i = 0; i < 4; ++i) b.push_back(0x00);
    return b;
}

TEST(Rtcp, ParseSr_ExtractsTimestamps) {
    RtcpHandler h;
    EXPECT_FALSE(h.has_sr());
    auto pkt = make_sr_packet(0xDEADBEEF, 0xE8D0F280, 0x80000000, 0x00ABCDEF);
    h.handle_packet(pkt.data(), pkt.size());
    EXPECT_TRUE(h.has_sr());
    const auto& sr = h.last_sr();
    EXPECT_EQ(sr.ssrc, uint32_t(0xDEADBEEF));
    EXPECT_EQ(sr.ntp_msw, uint64_t(0xE8D0F280));
    EXPECT_EQ(sr.ntp_lsw, uint64_t(0x80000000));
    EXPECT_EQ(sr.rtp_ts, uint32_t(0x00ABCDEF));
}

TEST(Rtcp, BuildRr_LengthValid) {
    RtcpHandler h;
    auto rr = h.build_rr(0x11111111, 0x22222222, 1000, 5, 10);
    // RR: header 4 + SSRC 4 + Report block (24 bytes) = 32 bytes
    // length field in 32-bit words = 32/4 - 1 = 7
    EXPECT_FALSE(rr.empty());
    EXPECT_GE(rr.size(), size_t(8));
    EXPECT_EQ(rr[0] & 0xC0, 0x80); // v=2
    EXPECT_EQ(rr[1], 201);  // PT = RR = 201
    uint16_t len_word = (uint16_t(rr[2]) << 8) | rr[3];
    EXPECT_EQ(size_t(len_word) * 4 + 4, rr.size()); // RFC: "length" = 32-bit words - 1
}

TEST(Rtcp, HandleCompoundPacket_SrPlusUnknown) {
    // 构造 compound: SR + SDES (跳过 SDES)
    auto sr = make_sr_packet(0xAA, 1, 2, 3);
    std::vector<uint8_t> sdes;
    // SDES header: v=2 p=0 sc=1 → 0x81, PT=202=0xCA, length=3 words (header + SSRC + item)
    // Simplest SDES: SSRC + item END + pad
    sdes.push_back(0x81); sdes.push_back(202);
    sdes.push_back(0x00); sdes.push_back(0x03);  // 3 words (12 bytes) - 1? No length field = word count - 1
    // 调整：SDES length = word count - 1. 若 SDES 共 12 bytes (= 3 words)，则 length = 2.
    sdes[3] = 0x02;
    sdes.push_back(0x11); sdes.push_back(0x22); sdes.push_back(0x33); sdes.push_back(0x44); // SSRC
    // SDES item: CNAME type=1, length=1, 'x', and END
    sdes.push_back(0x01); sdes.push_back(0x01); sdes.push_back('x');
    sdes.push_back(0x00); // END (type=0)
    // compound
    std::vector<uint8_t> compound;
    compound.insert(compound.end(), sr.begin(), sr.end());
    compound.insert(compound.end(), sdes.begin(), sdes.end());
    RtcpHandler h;
    h.handle_packet(compound.data(), compound.size());
    EXPECT_TRUE(h.has_sr());
    EXPECT_EQ(h.last_sr().ssrc, uint32_t(0xAA));
}

TEST(Rtcp, HandleBadPacket_NoCrash) {
    RtcpHandler h;
    // 长度不足: 空包
    h.handle_packet(nullptr, 0);
    // 非 RTCP 包（长度 3 < 4）
    const uint8_t d[] = {0x00, 0x01, 0x02};
    h.handle_packet(d, 3);
    EXPECT_FALSE(h.has_sr());
    // 错误 version: 0x00 开头
    const uint8_t badvers[32] = {0x00};
    h.handle_packet(badvers, 32);
}

/* ====================================================================
 *                            Timing
 * ==================================================================== */

// 构造一个 timing request 包（真实协议：20 字节，length = (20/4)-1 = 4）
static std::vector<uint8_t> make_timing_req(uint32_t ssrc, uint32_t ts1) {
    std::vector<uint8_t> b;
    b.push_back(0x80);  // v=2, p=0
    b.push_back(0x53);  // PT = timing request = 0x53 = 83
    b.push_back(0x00);
    b.push_back(0x04);  // length = (20/4) - 1 = 4
    // SSRC
    b.push_back((ssrc >> 24) & 0xFF);
    b.push_back((ssrc >> 16) & 0xFF);
    b.push_back((ssrc >> 8) & 0xFF);
    b.push_back(ssrc & 0xFF);
    // timestamp1
    b.push_back((ts1 >> 24) & 0xFF);
    b.push_back((ts1 >> 16) & 0xFF);
    b.push_back((ts1 >> 8) & 0xFF);
    b.push_back(ts1 & 0xFF);
    // timestamp2 / timestamp3（请求里填 0）
    b.push_back(0); b.push_back(0); b.push_back(0); b.push_back(0);
    b.push_back(0); b.push_back(0); b.push_back(0); b.push_back(0);
    return b;
}

TEST(Timing, HandleRequest_ReturnsResponse) {
    TimingHandler h;
    auto req = make_timing_req(0xABCD, 0x12345678);
    auto resp = h.handle_packet(req.data(), req.size());
    EXPECT_FALSE(resp.empty());
    // response PT = 0x54
    EXPECT_EQ(resp[1], 0x54);
    // response 长度应为 20 bytes (header 4 + 16 body)
    EXPECT_EQ(resp.size(), size_t(20));
    // SSRC 应是我们的 (0x41503200) 或接收端回填 —— 不重要，
    // 但检查 length 字段 = (20/4) - 1 = 4 → 4 * 4 = 16 bytes body
    EXPECT_EQ(resp[3], 0x04);
    // timestamp1 (bytes 8..11) 必须等于请求的 0x12345678
    uint32_t ts1_back = (uint32_t(resp[8])<<24) | (uint32_t(resp[9])<<16) |
                        (uint32_t(resp[10])<<8) | uint32_t(resp[11]);
    EXPECT_EQ(ts1_back, uint32_t(0x12345678));
    // timestamp2 / 3 非零 (ntp_now 生成)
    uint64_t t2 = (uint64_t(resp[12])<<24) | (uint64_t(resp[13])<<16) |
                  (uint64_t(resp[14])<<8)  |  uint64_t(resp[15]);
    uint64_t t3 = (uint64_t(resp[16])<<24) | (uint64_t(resp[17])<<16) |
                  (uint64_t(resp[18])<<8)  |  uint64_t(resp[19]);
    // 只要 ts1 在请求值 0x12345678 附近即可，t2/t3 应接近当前时钟
    EXPECT_GT(t2 + t3, uint64_t(0));
}

TEST(Timing, HandleResponse_NoReply) {
    TimingHandler h;
    // 收到的是 response (PT=0x54)，不应返回任何内容
    auto req = make_timing_req(0x1, 0x1234);
    req[1] = 0x54;  // 改为 response PT
    auto resp = h.handle_packet(req.data(), req.size());
    EXPECT_TRUE(resp.empty());
}

TEST(Timing, HandleBadPacket_Empty) {
    TimingHandler h;
    auto r = h.handle_packet(nullptr, 0);
    EXPECT_TRUE(r.empty());
    const uint8_t tiny[] = {0x80};
    r = h.handle_packet(tiny, 1);
    EXPECT_TRUE(r.empty());
    // 错误 PT 不是 0x53 / 0x54
    const uint8_t badpt[20] = {0x80, 0xFF};
    r = h.handle_packet(badpt, 20);
    EXPECT_TRUE(r.empty());
    // 错 version
    const uint8_t badver[20] = {0x00, 0x53};
    r = h.handle_packet(badver, 20);
    EXPECT_TRUE(r.empty());
}

TEST(Timing, NtpNow_IsMonotonic) {
    // 连续两次 ntp_now 应该单调不减 (至少时间差 0)
    uint64_t t1 = TimingHandler::ntp_now();
    uint64_t t2 = TimingHandler::ntp_now();
    // 允许同一时钟值（低精度计时器）
    EXPECT_GE(t2, t1);
    // 高 32 位是秒，至少不应是 0 (除非机器 1900 年时钟)
    uint64_t seconds = (t1 >> 32);
    EXPECT_GT(seconds, uint64_t(0xDA000000 >> 0));  // 至少 1900 以来 ~2010 年值
    // 实际 2026 年 NTP 秒 ≈ 0xE900_0000 量级
}
