/*!
 * @file test_plist.cpp
 * @brief util::plist 单元测试：XML/binary 解析 + 序列化 + 构建 + 容器
 */
#include "test_harness.h"
#include "util/plist.h"

#include <cstring>
#include <string>
#include <vector>
#include <cstdint>

using namespace airplay2::util;

/* ====================================================================
 *                       PlistValue 基本构建与取值
 * ==================================================================== */

TEST(PlistValue, DefaultCtor_IsNone) {
    PlistValue v;
    EXPECT_EQ(v.type(), PlistType::NONE);
    EXPECT_FALSE(v.is_string());
    EXPECT_FALSE(v.is_int());
    EXPECT_FALSE(v.is_real());
    EXPECT_FALSE(v.is_bool());
    EXPECT_FALSE(v.is_data());
    EXPECT_FALSE(v.is_array());
    EXPECT_FALSE(v.is_dict());
}

TEST(PlistValue, MakeString_TypeAndValue) {
    auto v = PlistValue::make_string("hello");
    EXPECT_TRUE(v.is_string());
    EXPECT_STREQ(v.as_string().c_str(), "hello");
}

TEST(PlistValue, MakeInt_AndTypeChecks) {
    auto v = PlistValue::make_int(-12345678901234LL);
    EXPECT_TRUE(v.is_int());
    EXPECT_FALSE(v.is_real());
    EXPECT_EQ(v.as_int(), int64_t(-12345678901234LL));
}

TEST(PlistValue, MakeReal_Double) {
    auto v = PlistValue::make_real(3.1415926535);
    EXPECT_TRUE(v.is_real());
    double d = v.as_real();
    EXPECT_GT(d, 3.1415);
    EXPECT_LT(d, 3.1416);
}

TEST(PlistValue, MakeBool_TrueFalse) {
    auto vt = PlistValue::make_bool(true);
    auto vf = PlistValue::make_bool(false);
    EXPECT_TRUE(vt.is_bool());
    EXPECT_TRUE(vt.as_bool());
    EXPECT_TRUE(vf.is_bool());
    EXPECT_FALSE(vf.as_bool());
}

TEST(PlistValue, MakeData_PointerAndVector) {
    const uint8_t bytes[] = {0x01,0x02,0x03,0x04,0x05};
    auto a = PlistValue::make_data(bytes, 5);
    auto b = PlistValue::make_data(std::vector<uint8_t>(bytes, bytes + 5));
    EXPECT_TRUE(a.is_data());
    EXPECT_TRUE(b.is_data());
    EXPECT_EQ(a.as_data().size(), size_t(5));
    EXPECT_BYTES_EQ(a.as_data().data(), b.as_data().data(), 5);
}

TEST(PlistValue, MakeDate) {
    auto v = PlistValue::make_date(1.25e9);
    EXPECT_EQ(v.type(), PlistType::DATE);
    EXPECT_GT(v.as_date(), 1.2e9);
}

TEST(PlistValue, WrongTypeGetter_ReturnsDefault) {
    // 字符串用 as_int 应该返回 0，不崩溃
    auto v = PlistValue::make_string("xyz");
    EXPECT_EQ(v.as_int(), int64_t(0));
    EXPECT_FALSE(v.as_bool());
    EXPECT_TRUE(v.as_string() == "xyz");
}

TEST(PlistValue, CopyAndAssign) {
    auto a = PlistValue::make_int(42);
    PlistValue b(a);  // copy ctor
    PlistValue c;
    c = a;            // copy assign
    PlistValue d(std::move(b)); // move
    PlistValue e;
    e = std::move(c); // move assign
    EXPECT_EQ(a.as_int(), int64_t(42));
    EXPECT_EQ(d.as_int(), int64_t(42));
    EXPECT_EQ(e.as_int(), int64_t(42));
}

/* ====================================================================
 *                       Array / Dict 容器访问
 * ==================================================================== */

TEST(PlistArray, PushBackAndIndex) {
    auto arr = PlistValue::make_array();
    EXPECT_TRUE(arr.is_array());
    EXPECT_EQ(arr.array().size(), size_t(0));
    arr.array().push_back(PlistValue::make_int(1));
    arr.array().push_back(PlistValue::make_int(2));
    arr.array().push_back(PlistValue::make_string("three"));
    EXPECT_EQ(arr.array().size(), size_t(3));
    EXPECT_EQ(arr.array()[0].as_int(), int64_t(1));
    EXPECT_EQ(arr.array()[1].as_int(), int64_t(2));
    EXPECT_STREQ(arr.array()[2].as_string().c_str(), "three");
}

TEST(PlistDict, InsertAndGet) {
    auto d = PlistValue::make_dict();
    EXPECT_TRUE(d.is_dict());
    d.dict()["name"]   = PlistValue::make_string("Alice");
    d.dict()["age"]    = PlistValue::make_int(30);
    d.dict()["alive"]  = PlistValue::make_bool(true);
    EXPECT_EQ(d.dict().size(), size_t(3));
    // 用 get() 查存在的 key
    EXPECT_STREQ(d.get("name").as_string().c_str(), "Alice");
    EXPECT_EQ(d.get("age").as_int(), int64_t(30));
    EXPECT_TRUE(d.get("alive").as_bool());
    // 便捷 get_string / get_int / get_bool
    EXPECT_STREQ(d.get_string("name").c_str(), "Alice");
    EXPECT_EQ(d.get_int("age"), int64_t(30));
    EXPECT_TRUE(d.get_bool("alive"));
    // 不存在的 key
    EXPECT_EQ(d.get("nonexistent").type(), PlistType::NONE);
    EXPECT_STREQ(d.get_string("nonexistent").c_str(), "");
    EXPECT_EQ(d.get_int("nonexistent"), int64_t(0));
    EXPECT_FALSE(d.get_bool("nonexistent"));
}

TEST(PlistDict, NestedContainers) {
    auto root = PlistValue::make_dict();
    auto info = PlistValue::make_dict();
    info.dict()["model"] = PlistValue::make_string("AppleTV6,2");
    info.dict()["features"] = PlistValue::make_int(0x78FF);
    auto tags = PlistValue::make_array();
    tags.array().push_back(PlistValue::make_string("video"));
    tags.array().push_back(PlistValue::make_string("audio"));
    root.dict()["info"] = info;
    root.dict()["tags"] = tags;
    // 嵌套访问
    EXPECT_STREQ(root.get("info").get_string("model").c_str(), "AppleTV6,2");
    EXPECT_EQ(root.get("tags").array().size(), size_t(2));
    EXPECT_STREQ(root.get("tags").array()[1].as_string().c_str(), "audio");
}

TEST(PlistNone, ReturnsStableDefault) {
    // plist_none() 是静态 singleton，多次调用地址一致
    auto const& a = plist_none();
    auto const& b = plist_none();
    EXPECT_EQ(&a, &b);
    EXPECT_EQ(a.type(), PlistType::NONE);
}

/* ====================================================================
 *                      XML plist 解析 + 序列化
 * ==================================================================== */

TEST(XmlPlist, ParseSimpleDict) {
    const char* xml = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>name</key>
    <string>Living Room</string>
    <key>volume</key>
    <real>0.75</real>
    <key>count</key>
    <integer>42</integer>
    <key>enabled</key>
    <true/>
    <key>muted</key>
    <false/>
    <key>empty_str</key>
    <string></string>
</dict>
</plist>)XML";
    PlistValue out;
    EXPECT_TRUE(parse_xml_plist(xml, out));
    EXPECT_TRUE(out.is_dict());
    EXPECT_STREQ(out.get_string("name").c_str(), "Living Room");
    EXPECT_EQ(out.get_int("count"), int64_t(42));
    EXPECT_TRUE(out.get_bool("enabled"));
    EXPECT_FALSE(out.get_bool("muted"));
    EXPECT_STREQ(out.get_string("empty_str").c_str(), "");
    double vol = out.get("volume").as_real();
    EXPECT_GT(vol, 0.74);
    EXPECT_LT(vol, 0.76);
}

TEST(XmlPlist, ParseArray) {
    const char* xml = R"XML(<?xml version="1.0"?>
<plist version="1.0"><array>
    <integer>1</integer>
    <integer>2</integer>
    <string>three</string>
</array></plist>)XML";
    PlistValue out;
    EXPECT_TRUE(parse_xml_plist(xml, out));
    EXPECT_TRUE(out.is_array());
    EXPECT_EQ(out.array().size(), size_t(3));
    EXPECT_EQ(out.array()[0].as_int(), int64_t(1));
    EXPECT_EQ(out.array()[1].as_int(), int64_t(2));
    EXPECT_STREQ(out.array()[2].as_string().c_str(), "three");
}

TEST(XmlPlist, ParseNestedDict) {
    const char* xml = R"XML(<plist version="1.0">
<dict>
    <key>info</key>
    <dict>
        <key>model</key><string>AppleTV6,2</string>
        <key>ids</key>
        <array>
            <string>aa:bb:cc</string>
            <string>dd:ee:ff</string>
        </array>
    </dict>
</dict>
</plist>)XML";
    PlistValue out;
    EXPECT_TRUE(parse_xml_plist(xml, out));
    auto info = out.get("info");
    EXPECT_TRUE(info.is_dict());
    EXPECT_STREQ(info.get_string("model").c_str(), "AppleTV6,2");
    auto ids = info.get("ids");
    EXPECT_TRUE(ids.is_array());
    EXPECT_EQ(ids.array().size(), size_t(2));
    EXPECT_STREQ(ids.array()[0].as_string().c_str(), "aa:bb:cc");
}

TEST(XmlPlist, ParseDataHex) {
    const char* xml = R"XML(<plist version="1.0"><dict>
<key>bytes</key><data>AAECAwQF</data>
</dict></plist>)XML";
    PlistValue out;
    EXPECT_TRUE(parse_xml_plist(xml, out));
    auto& d = out.get("bytes").as_data();
    // base64 "AAECAwQF" = bytes 00 01 02 03 04 05
    EXPECT_EQ(d.size(), size_t(6));
    const uint8_t expected[] = {0,1,2,3,4,5};
    EXPECT_BYTES_EQ(d.data(), expected, 6);
}

TEST(XmlPlist, Parse_InvalidEmptyString) {
    PlistValue out;
    EXPECT_FALSE(parse_xml_plist("", out));
}

TEST(XmlPlist, Parse_InvalidNotPlist) {
    PlistValue out;
    EXPECT_FALSE(parse_xml_plist("<html></html>", out));
}

TEST(XmlPlist, Serialize_AndParseBack_Roundtrip) {
    auto root = PlistValue::make_dict();
    root.dict()["s"] = PlistValue::make_string("hello world");
    root.dict()["i"] = PlistValue::make_int(-12345);
    root.dict()["b"] = PlistValue::make_bool(true);
    root.dict()["d"] = PlistValue::make_real(0.125);
    // 嵌套数组
    auto a = PlistValue::make_array();
    a.array().push_back(PlistValue::make_int(1));
    a.array().push_back(PlistValue::make_string("two"));
    root.dict()["arr"] = a;
    std::string xml = serialize_xml_plist(root);
    // 必须包含头部
    EXPECT_TRUE(xml.find("<?xml") != std::string::npos);
    EXPECT_TRUE(xml.find("<plist") != std::string::npos);
    // 重新解析
    PlistValue back;
    EXPECT_TRUE(parse_xml_plist(xml, back));
    EXPECT_TRUE(back.is_dict());
    EXPECT_STREQ(back.get_string("s").c_str(), "hello world");
    EXPECT_EQ(back.get_int("i"), int64_t(-12345));
    EXPECT_TRUE(back.get_bool("b"));
    EXPECT_TRUE(back.get("d").is_real());
    EXPECT_EQ(back.get("arr").array().size(), size_t(2));
    EXPECT_EQ(back.get("arr").array()[0].as_int(), int64_t(1));
    EXPECT_STREQ(back.get("arr").array()[1].as_string().c_str(), "two");
}

TEST(XmlPlist, Serialize_EmptyDict) {
    auto d = PlistValue::make_dict();
    std::string x = serialize_xml_plist(d);
    EXPECT_FALSE(x.empty());
    PlistValue back;
    EXPECT_TRUE(parse_xml_plist(x, back));
    EXPECT_TRUE(back.is_dict());
    EXPECT_EQ(back.dict().size(), size_t(0));
}

/* ====================================================================
 *                        Binary plist 解析
 * ==================================================================== */

// 手工构造一个最小合法 bplist00：
//   Header: "bplist00" (8 bytes)
//   Objects:
//     obj0: 空 dict 0x10 (marker 0xE 低 nybble count=0 → 0xE0)
//   Offset table: [0x08]  (1 entry, 1 byte offset)
//   Trailer:
//     6 bytes unused (0)
//     offset_int_size = 1
//     ref_size = 0
//     num_objects = 1  (8 字节大端)
//     top_obj = 0
//     offset_table_offset = 9
static std::vector<uint8_t> make_minimal_bplist() {
    std::vector<uint8_t> b;
    b.insert(b.end(), {'b','p','l','i','s','t','0','0'}); // 8 bytes header
    // object 0 (offset 8): 空 dict
    // 标准 bplist（Apple CFBinaryPlist）：array=0xA0-0xAF，dict=0xD0-0xDF
    b.push_back(0xD0);        // 空 dict = 0xD0
    // offset table (offset 9): 单字节 0x08 (obj0 在偏移 8)
    b.push_back(0x08);
    // Trailer 起始 = offset 10
    while (b.size() < 10 + 32) b.push_back(0); // 先填充到 trailer 起始
    // Trailer 从偏移 10 (共 32 字节)
    size_t t = 10;
    // 6 bytes unused: keep 0
    // offset_int_size: b[t+6]
    b[t+6] = 1;
    // ref_size: b[t+7]
    b[t+7] = 0; // num_objects=1 不需要引用 (空 dict 无 key/value)
    // num_objects: 8 bytes big-endian
    for (int i = 0; i < 7; ++i) b[t+8+i] = 0;
    b[t+15] = 1;
    // top_object: 8 bytes
    for (int i = 0; i < 7; ++i) b[t+16+i] = 0;
    b[t+23] = 0;
    // offset_table_offset: 8 bytes big-endian (= 9)
    for (int i = 0; i < 7; ++i) b[t+24+i] = 0;
    b[t+31] = 9;
    return b;
}

TEST(BinaryPlist, MinimalEmptyDict) {
    auto b = make_minimal_bplist();
    PlistValue out;
    bool ok = parse_binary_plist(b.data(), b.size(), out);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(out.is_dict());
    EXPECT_EQ(out.dict().size(), size_t(0));
}

TEST(BinaryPlist, AutoDetect_FromParsePlist) {
    auto b = make_minimal_bplist();
    PlistValue out;
    EXPECT_TRUE(parse_plist(b.data(), b.size(), out));
    EXPECT_TRUE(out.is_dict());
}

TEST(BinaryPlist, AutoDetect_Xml_FromString) {
    const char* xml = R"XML(<plist version="1.0"><dict><key>k</key><string>v</string></dict></plist>)XML";
    PlistValue out;
    EXPECT_TRUE(parse_plist(xml, out));
    EXPECT_STREQ(out.get_string("k").c_str(), "v");
}

TEST(BinaryPlist, Invalid_TooShort) {
    const uint8_t bad[] = {'b','p','l','i','s','t'};
    PlistValue out;
    EXPECT_FALSE(parse_binary_plist(bad, sizeof(bad), out));
}

TEST(BinaryPlist, Invalid_WrongMagic) {
    const uint8_t bad[] = "nolist00ABCDEFGHIJKLMNOPQRSTUVW";
    PlistValue out;
    EXPECT_FALSE(parse_binary_plist(bad, sizeof(bad), out));
}

/* ====================================================================
 *                   构造典型 AirPlay plist 场景
 * ==================================================================== */

TEST(AirplayPlist, InfoResponseStruct) {
    // 模拟 /info 响应
    auto info = PlistValue::make_dict();
    info.dict()["deviceid"] = PlistValue::make_string("AA:BB:CC:DD:EE:FF");
    info.dict()["features"] = PlistValue::make_int(0x78FF);
    info.dict()["model"]    = PlistValue::make_string("AppleTV6,2");
    info.dict()["srcvers"]  = PlistValue::make_string("605.30.1");
    info.dict()["vv"]       = PlistValue::make_int(2);
    std::string xml = serialize_xml_plist(info);
    EXPECT_TRUE(xml.find("AppleTV6,2") != std::string::npos);
    // parse 回来确认无损失
    PlistValue back;
    EXPECT_TRUE(parse_xml_plist(xml, back));
    EXPECT_STREQ(back.get_string("deviceid").c_str(), "AA:BB:CC:DD:EE:FF");
    EXPECT_EQ(back.get_int("vv"), int64_t(2));
    EXPECT_STREQ(back.get_string("model").c_str(), "AppleTV6,2");
}

TEST(AirplayPlist, ActionDict_PlayPause) {
    // 模拟 /action 控制命令（二进制 plist 风格，这里用 value 构建即可）
    auto act = PlistValue::make_dict();
    act.dict()["category"] = PlistValue::make_string("media");
    act.dict()["command"]  = PlistValue::make_string("playPause");
    act.dict()["timestamp"] = PlistValue::make_int(0x12345678LL);
    EXPECT_STREQ(act.get_string("command").c_str(), "playPause");
    EXPECT_EQ(act.get_int("timestamp"), int64_t(0x12345678LL));
}

/* ====================================================================
 *                    Binary plist 序列化 roundtrip
 * ==================================================================== */

TEST(BinaryPlist, SerializeRoundtrip_SetupResponse) {
    // 模拟 AP2 SETUP 响应：timingPort / eventPort / streams[{dataPort,type}]
    auto resp = PlistValue::make_dict();
    resp.dict()["timingPort"] = PlistValue::make_int(5003);
    resp.dict()["eventPort"]  = PlistValue::make_int(0);
    auto streams = PlistValue::make_array();
    auto s1 = PlistValue::make_dict();
    s1.dict()["dataPort"] = PlistValue::make_int(5000);
    s1.dict()["type"]     = PlistValue::make_int(96);
    streams.array().push_back(s1);
    auto s2 = PlistValue::make_dict();
    s2.dict()["dataPort"] = PlistValue::make_int(5004);
    s2.dict()["type"]     = PlistValue::make_int(110);
    streams.array().push_back(s2);
    resp.dict()["streams"] = streams;

    std::vector<uint8_t> bytes;
    EXPECT_TRUE(serialize_binary_plist(resp, bytes));
    EXPECT_GT(bytes.size(), (size_t)40);  // header + trailer
    EXPECT_EQ(memcmp(bytes.data(), "bplist00", 8), 0);

    // 用自家解析器 roundtrip，确认结构无损失
    PlistValue back;
    EXPECT_TRUE(parse_binary_plist(bytes.data(), bytes.size(), back));
    EXPECT_TRUE(back.is_dict());
    EXPECT_EQ(back.get_int("timingPort"), int64_t(5003));
    EXPECT_EQ(back.get_int("eventPort"), int64_t(0));
    const PlistValue& bs = back.get("streams");
    EXPECT_TRUE(bs.is_array());
    EXPECT_EQ(bs.array().size(), size_t(2));
    EXPECT_EQ(bs.array()[0].get_int("dataPort"), int64_t(5000));
    EXPECT_EQ(bs.array()[0].get_int("type"), int64_t(96));
    EXPECT_EQ(bs.array()[1].get_int("dataPort"), int64_t(5004));
    EXPECT_EQ(bs.array()[1].get_int("type"), int64_t(110));
}

TEST(BinaryPlist, ParseUnsignedInt_HighBitSet_SampleRate) {
    // 回归：iOS AP2 SETUP 的 sr=44100 以 2 字节 bplist int 编码（0xAC44，
    // 最高位为 1）。此前解析器按有符号扩展成 -21436，导致采样率变成
    // 42.9 亿、播放线程永不输出 PCM。bplist 1/2/4 字节 int 必须按无符号读。
    auto d = PlistValue::make_dict();
    d.dict()["sr"]  = PlistValue::make_int(44100);   // 0xAC44
    d.dict()["spf"] = PlistValue::make_int(352);
    d.dict()["ct"]  = PlistValue::make_int(2);

    std::vector<uint8_t> bytes;
    EXPECT_TRUE(serialize_binary_plist(d, bytes));

    PlistValue back;
    EXPECT_TRUE(parse_binary_plist(bytes.data(), bytes.size(), back));
    EXPECT_EQ(back.get_int("sr"),  int64_t(44100));
    EXPECT_EQ(back.get_int("spf"), int64_t(352));
    EXPECT_EQ(back.get_int("ct"),  int64_t(2));
}

TEST(BinaryPlist, SerializeRoundtrip_DataAndLongString) {
    // 覆盖 data 类型和 >=15 字节长字符串（扩展长度编码路径）
    auto d = PlistValue::make_dict();
    std::vector<uint8_t> payload(72);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = (uint8_t)i;
    d.dict()["ekey"] = PlistValue::make_data(payload);
    d.dict()["deviceID"] = PlistValue::make_string("AA:BB:CC:DD:EE:FF-0123456789");

    std::vector<uint8_t> bytes;
    EXPECT_TRUE(serialize_binary_plist(d, bytes));
    PlistValue back;
    EXPECT_TRUE(parse_binary_plist(bytes.data(), bytes.size(), back));
    EXPECT_TRUE(back.is_dict());
    EXPECT_EQ(back.get("ekey").as_data().size(), size_t(72));
    EXPECT_EQ(back.get("ekey").as_data()[0], (uint8_t)0);
    EXPECT_EQ(back.get("ekey").as_data()[71], (uint8_t)71);
    EXPECT_STREQ(back.get_string("deviceID").c_str(),
                 "AA:BB:CC:DD:EE:FF-0123456789");
}

