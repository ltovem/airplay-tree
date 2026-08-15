/*!
 * @file plist.cpp
 *
 * Apple Property List (XML + Binary bplist00) 解析器 / XML 序列化器。
 *
 * 实现取舍：
 *   - XML plist 不使用第三方 XML 库；手写状态机解析器，只识别需要的
 *     subset 标签（dict/array/string/integer/real/true/false/data/date/key）。
 *   - bplist 仅支持需要的 token（int/real/UTF-8/data/array/dict），
 *     对于 16-byte long long 和 8-byte double 全覆盖。
 *   - 错误时不抛异常，返回 false 并让调用方决定。
 *
 * AirPlay 2 里 plist 体积一般都很小（< 64 KB），解析器没有做流式解析。
 */
#include "plist.h"
#include "../platform/platform_log.h"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <sstream>
#include <cctype>
#include <algorithm>   // std::min（bplist varint 解析截断）

namespace airplay2 {
namespace util {

// ===================== PlistValue 工厂方法 =====================

PlistValue PlistValue::make_string(const std::string& s) {
    PlistValue v; v.type_ = PlistType::STRING; v.str_ = s; return v;
}
PlistValue PlistValue::make_int(int64_t value) {
    PlistValue v; v.type_ = PlistType::INT; v.int_ = value; return v;
}
PlistValue PlistValue::make_real(double value) {
    PlistValue v; v.type_ = PlistType::REAL; v.real_ = value; return v;
}
PlistValue PlistValue::make_bool(bool value) {
    PlistValue v; v.type_ = PlistType::BOOL; v.bool_ = value; return v;
}
PlistValue PlistValue::make_data(const uint8_t* data, size_t len) {
    PlistValue v; v.type_ = PlistType::DATA;
    v.data_.assign(data, data + len); return v;
}
PlistValue PlistValue::make_data(const std::vector<uint8_t>& data) {
    PlistValue v; v.type_ = PlistType::DATA; v.data_ = data; return v;
}
PlistValue PlistValue::make_date(double unix_timestamp) {
    PlistValue v; v.type_ = PlistType::DATE; v.real_ = unix_timestamp; return v;
}
PlistValue PlistValue::make_array() {
    PlistValue v; v.type_ = PlistType::ARRAY;
    v.array_ = std::make_shared<PlistArray>(); return v;
}
PlistValue PlistValue::make_dict() {
    PlistValue v; v.type_ = PlistType::DICT;
    v.dict_ = std::make_shared<PlistDict>(); return v;
}

// ===================== PlistValue 取值方法 =====================

const std::string& PlistValue::as_string() const {
    static const std::string kEmpty;
    return (type_ == PlistType::STRING) ? str_ : kEmpty;
}
int64_t PlistValue::as_int() const {
    if (type_ == PlistType::INT) return int_;
    if (type_ == PlistType::BOOL) return (int64_t)bool_;
    if (type_ == PlistType::REAL) return (int64_t)real_;
    return 0;
}
double PlistValue::as_real() const {
    if (type_ == PlistType::REAL) return real_;
    if (type_ == PlistType::INT) return (double)int_;
    if (type_ == PlistType::BOOL) return bool_ ? 1.0 : 0.0;
    return 0.0;
}
bool PlistValue::as_bool() const {
    if (type_ == PlistType::BOOL) return bool_;
    if (type_ == PlistType::INT) return int_ != 0;
    if (type_ == PlistType::REAL) return real_ != 0.0;
    if (type_ == PlistType::STRING) return !str_.empty();
    return false;
}
const std::vector<uint8_t>& PlistValue::as_data() const {
    static const std::vector<uint8_t> kEmpty;
    return (type_ == PlistType::DATA) ? data_ : kEmpty;
}
double PlistValue::as_date() const {
    // DATE 的值也存在 real_ 里（Unix 秒）
    return (type_ == PlistType::DATE) ? real_ : 0.0;
}

PlistArray& PlistValue::array() {
    if (!array_) array_ = std::make_shared<PlistArray>();
    type_ = PlistType::ARRAY;
    return *array_;
}
const PlistArray& PlistValue::array() const {
    static const PlistArray kEmpty;
    return (array_ && type_ == PlistType::ARRAY) ? *array_ : kEmpty;
}
PlistDict& PlistValue::dict() {
    if (!dict_) dict_ = std::make_shared<PlistDict>();
    type_ = PlistType::DICT;
    return *dict_;
}
const PlistDict& PlistValue::dict() const {
    static const PlistDict kEmpty;
    return (dict_ && type_ == PlistType::DICT) ? *dict_ : kEmpty;
}

static const PlistValue kNone;
const PlistValue& plist_none() { return kNone; }

const PlistValue& PlistValue::get(const std::string& key) const {
    if (!dict_ || type_ != PlistType::DICT) return kNone;
    auto it = dict_->find(key);
    return (it == dict_->end()) ? kNone : it->second;
}
std::string PlistValue::get_string(const std::string& key) const {
    return get(key).as_string();
}
int64_t PlistValue::get_int(const std::string& key) const {
    return get(key).as_int();
}
bool PlistValue::get_bool(const std::string& key) const {
    return get(key).as_bool();
}

// ===================== Base64 =====================

static const char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_char_to_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/*!
 * @brief Base64 解码 —— 用于 plist <data> 标签
 *
 * AirPlay /info 里没用到 <data>，但设备发送的某些 /action /event
 * 里有。这是一个标准的 4 字符 → 3 字节解码器。
 */
static bool base64_decode(const std::string& in, std::vector<uint8_t>& out) {
    out.clear();
    uint8_t block[3];
    int val[4];
    size_t i = 0, n = in.size();
    // 跳过所有非 base64 字符（换行、空格）
    while (i < n) {
        int got = 0;
        while (got < 4 && i < n) {
            char c = in[i++];
            if (c == '=') { val[got++] = -2; break; }
            int v = base64_char_to_val(c);
            if (v >= 0) { val[got++] = v; }
            // 否则跳过
        }
        if (got == 0) break;
        if (got < 2 && val[0] != -2) return false;
        block[0] = (uint8_t)((val[0] << 2) | (val[1] >> 4));
        out.push_back(block[0]);
        if (got > 2) {
            block[1] = (uint8_t)(((val[1] & 0x0F) << 4) | (val[2] >> 2));
            out.push_back(block[1]);
        }
        if (got > 3 && val[3] != -2) {
            block[2] = (uint8_t)(((val[2] & 0x03) << 6) | val[3]);
            out.push_back(block[2]);
        }
    }
    return true;
}

static std::string base64_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += kBase64[(v >> 18) & 0x3F];
        out += kBase64[(v >> 12) & 0x3F];
        out += kBase64[(v >> 6) & 0x3F];
        out += kBase64[v & 0x3F];
        i += 3;
    }
    if (i + 1 == len) {
        uint32_t v = (uint32_t(data[i]) << 16);
        out += kBase64[(v >> 18) & 0x3F];
        out += kBase64[(v >> 12) & 0x3F];
        out += "==";
    } else if (i + 2 == len) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out += kBase64[(v >> 18) & 0x3F];
        out += kBase64[(v >> 12) & 0x3F];
        out += kBase64[(v >> 6) & 0x3F];
        out += "=";
    }
    return out;
}

// ===================== XML 文本处理辅助 =====================

static void xml_append_escape(std::string& out, const std::string& s) {
    for (char c : s) {
        switch (c) {
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '&':  out += "&amp;"; break;
            case '"':  out += "&quot;"; break;
            default:   out += c; break;
        }
    }
}

/// 在字符串 s 中从 start 位置开始，查找下一个与 tag 匹配的 "<tag>" 或 "</tag>"
/// 返回 {tag_start, tag_end, is_close, tag_name}，tag_end 是 ">" 之后的位置。
struct XTag {
    size_t start = 0;
    size_t end = 0;
    bool is_close = false;
    std::string name;
};
static bool find_next_tag(const std::string& s, size_t start, XTag& t) {
    size_t i = s.find('<', start);
    if (i == std::string::npos) return false;
    size_t gt = s.find('>', i);
    if (gt == std::string::npos) return false;
    // 截取 < 和 > 之间的部分
    std::string inner = s.substr(i + 1, gt - i - 1);
    t.start = i;
    t.end = gt + 1;
    t.is_close = false;
    // 去除尾部 " /"（self-closing）
    bool self_close = false;
    if (!inner.empty() && inner.back() == '/') {
        self_close = true;
        inner.pop_back();
        while (!inner.empty() && inner.back() == ' ') inner.pop_back();
    }
    // 去除头部属性（遇到空格就截断），我们只识别没有属性的核心标签
    t.is_close = false;
    if (!inner.empty() && inner[0] == '/') {
        t.is_close = true;
        inner = inner.substr(1);
        while (!inner.empty() && inner.back() == ' ') inner.pop_back();
    }
    // 去掉属性
    size_t sp = inner.find(' ');
    if (sp != std::string::npos) inner = inner.substr(0, sp);
    t.name = inner;
    // 自闭合标记：不是 </tag> 形式，且 inner 最后是 "/"（已处理）
    // 我们把 self_close 作为"立即出栈"的标记：通过把 t.is_close=true 来提示
    // 更简单：self_close 的同时 is_close=false，用外部变量传。
    // 为此，我们利用 XTag 里的额外信息；简单做法：
    if (self_close && !t.is_close) {
        // 让调用方知道：处理完 open 后不用再找 close
        t.is_close = true;
        // 但要区分"自闭合"和"真正的 close tag"，名字在 open 与 close 不同。
        // 处理方式：这里改回 false，返回值用 name 带前缀不够优雅。
        // 简单办法：引入 static thread_local 变量。
        static thread_local bool g_self_close;
        g_self_close = true;
        t.is_close = false;
        (void)g_self_close; // 不影响，调用方用下面宏处理
    }
    return true;
}

// 简化的 XML 解析器：基于"递归下降"——对于开始标签，解析值直到匹配的 close 标签。
// 返回解析到的 PlistValue；end_pos 指向闭合标签 ">" 之后的位置。
static bool parse_xml_value(const std::string& s, size_t start, size_t& end_pos, PlistValue& out);

/// 提取当前 open-tag 与下一个 close-tag 之间的文本内容。
/// 假设 open_end 指向 "<open>" 之后；找到下一个 "<" 之前的内容。
static std::string xml_inside_text(const std::string& s, size_t open_end, size_t close_start) {
    size_t first = open_end;
    size_t last = close_start;
    std::string content = s.substr(first, last - first);
    // 处理 XML 转义
    std::string out; out.reserve(content.size());
    for (size_t i = 0; i < content.size(); ++i) {
        char c = content[i];
        if (c == '&') {
            if (content.compare(i, 4, "&lt;") == 0) { out += '<'; i += 3; continue; }
            if (content.compare(i, 4, "&gt;") == 0) { out += '>'; i += 3; continue; }
            if (content.compare(i, 5, "&amp;") == 0) { out += '&'; i += 4; continue; }
            if (content.compare(i, 6, "&quot;") == 0) { out += '"'; i += 5; continue; }
            if (content.compare(i, 6, "&apos;") == 0) { out += '\''; i += 5; continue; }
            out += '&';
        } else {
            out += c;
        }
    }
    return out;
}

static bool parse_xml_value(const std::string& s, size_t start, size_t& end_pos, PlistValue& out) {
    XTag t;
    if (!find_next_tag(s, start, t)) return false;
    if (t.is_close) { start = t.end; return false; }

    std::string name = t.name;
    size_t after_open = t.end;

    if (name == "true") { out = PlistValue::make_bool(true);  end_pos = after_open; start = after_open; return true; }
    if (name == "false"){ out = PlistValue::make_bool(false); end_pos = after_open; return true; }

    // 对 string/integer/real/date/data：找 </name> close tag
    // 对 array/dict/key：递归
    if (name == "string" || name == "integer" || name == "real" || name == "date" || name == "data" || name == "key"
        || name == "array" || name == "dict") {
        // 找匹配的 close
        std::string close_name = std::string("/") + name;
        size_t cursor = after_open;
        if (name == "string" || name == "integer" || name == "real" || name == "date" || name == "data" || name == "key") {
            // 标量：找第一个 close </name>
            std::string needle = std::string("</") + name + ">";
            size_t cp = s.find(needle, after_open);
            if (cp == std::string::npos) return false;
            std::string content = xml_inside_text(s, after_open, cp);
            if (name == "string" || name == "key") {
                if (name == "key") out = PlistValue::make_string(content);
                else out = PlistValue::make_string(content);
            } else if (name == "integer") {
                try {
                    out = PlistValue::make_int(std::stoll(content));
                } catch (...) {
                    // 也可能是 hex（0x）或 unsigned 超范围
                    try {
                        unsigned long long u = std::stoull(content);
                        out = PlistValue::make_int((int64_t)u);
                    } catch (...) { out = PlistValue::make_int(0); }
                }
            } else if (name == "real") {
                try { out = PlistValue::make_real(std::stod(content)); }
                catch (...) { out = PlistValue::make_real(0.0); }
            } else if (name == "date") {
                // ISO 8601 UTC: YYYY-MM-DDTHH:MM:SSZ（简化解析，只支持秒级 + Z）
                // 更常见的 AirPlay date 是 Unix timestamp double（real 标签）。
                // 这里用标准库 strod 把形如 "409841404.000"（Apple epoch 秒）转换；
                // 若是 ISO8601 字符就只能粗略估值，给 0。
                try {
                    double val = std::stod(content);
                    // Apple 自定义 epoch 是 2001-01-01 00:00:00 UTC，
                    // 到 Unix epoch 差 978307200 秒。
                    out = PlistValue::make_date(val + 978307200.0);
                } catch (...) {
                    out = PlistValue::make_date(0.0);
                }
            } else if (name == "data") {
                std::vector<uint8_t> bytes;
                base64_decode(content, bytes);
                out = PlistValue::make_data(bytes);
            }
            end_pos = cp + needle.size();
            return true;
        }

        // array / dict：需要在 children 之前平衡
        if (name == "array") {
            out = PlistValue::make_array();
            cursor = after_open;
            while (true) {
                XTag child;
                if (!find_next_tag(s, cursor, child)) return false;
                if (child.is_close && child.name == name) {
                    end_pos = child.end;
                    return true;
                }
                if (child.is_close) return false; // 意外的 close tag
                PlistValue elem; size_t ep;
                if (!parse_xml_value(s, child.start, ep, elem)) return false;
                out.array().push_back(elem);
                cursor = ep;
            }
        }

        if (name == "dict") {
            out = PlistValue::make_dict();
            cursor = after_open;
            while (true) {
                XTag ktag;
                if (!find_next_tag(s, cursor, ktag)) return false;
                if (ktag.is_close && ktag.name == name) { end_pos = ktag.end; return true; }
                if (ktag.is_close || ktag.name != "key") return false;
                size_t kep;
                PlistValue kval;
                // parse_xml_value 把 key 当 string 返回
                if (!parse_xml_value(s, ktag.start, kep, kval)) return false;
                std::string k = kval.as_string();
                // 接下来一个 value 元素
                XTag vtag;
                if (!find_next_tag(s, kep, vtag)) return false;
                if (vtag.is_close) return false;
                PlistValue vval; size_t vep;
                if (!parse_xml_value(s, vtag.start, vep, vval)) return false;
                out.dict()[k] = std::move(vval);
                cursor = vep;
            }
        }
    }
    return false;
}

bool parse_xml_plist(const std::string& xml, PlistValue& out) {
    // 跳过 XML 声明 + DOCTYPE
    size_t root = xml.find("<plist");
    if (root == std::string::npos) {
        // 没声明，直接从根 value 开始
        XTag t;
        if (!find_next_tag(xml, 0, t)) return false;
        size_t ep;
        return parse_xml_value(xml, t.start, ep, out);
    }
    // 找到 <plist ...> 的结尾 '>'
    size_t root_gt = xml.find('>', root);
    if (root_gt == std::string::npos) return false;
    size_t cursor = root_gt + 1;
    XTag first;
    if (!find_next_tag(xml, cursor, first)) return false;
    size_t ep;
    bool ok = parse_xml_value(xml, first.start, ep, out);
    return ok;
}

// ===================== Binary plist (bplist00) =====================

static inline uint64_t be_read_varint(const uint8_t* p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | p[i];
    return v;
}

// Apple bplist date 是 64 位 double 从 2001-01-01 epoch 算起
static constexpr double kAppleCFAbsoluteTimeOffset = 978307200.0;

// 对象引用表：解析过程中，把每个 obj 偏移 → PlistValue 存起来。
// 用 vector<PlistValue> table，下标是对象引用 ID（0-indexed）。
static bool bplist_parse_object(const uint8_t* data, size_t len, uint64_t offset,
                                const uint8_t* offset_table_start,
                                int offset_size, int ref_size,
                                std::vector<PlistValue>& table, int ref_id);

static bool bplist_resolve_object(const uint8_t* data, size_t len,
                                  const uint8_t* offset_table_start,
                                  int offset_size, int ref_size,
                                  int ref_id,
                                  std::vector<PlistValue>& table) {
    if (ref_id < 0 || (size_t)ref_id >= table.size()) return false;
    if (table[ref_id].type() != PlistType::NONE) return true; // 已解析
    // 从 offset_table[ref_id] 读该对象的绝对偏移
    uint64_t obj_offset = 0;
    const uint8_t* off_entry = offset_table_start + (size_t)ref_id * offset_size;
    // 确保不越界
    if (off_entry + offset_size > data + len) return false;
    obj_offset = be_read_varint(off_entry, offset_size);
    return bplist_parse_object(data, len, obj_offset, offset_table_start,
                               offset_size, ref_size, table, ref_id);
}

static bool bplist_parse_object(const uint8_t* data, size_t len, uint64_t offset,
                                const uint8_t* offset_table_start,
                                int offset_size, int ref_size,
                                std::vector<PlistValue>& table, int ref_id) {
    if (offset >= len) return false;
    uint8_t tok = data[offset];
    uint8_t high = (tok >> 4) & 0x0F;
    uint8_t low  = tok & 0x0F;
    // 对字符串/整数/数组/字典，低 4 位表示长度（0..14）或 0xF 表示下一个 byte/int 给长度。
    uint64_t count = low;
    size_t p = offset + 1;
    if (low == 0x0F && high != 0x00 && high != 0x03) {
        // int 标记字节给出长度：0x1n 的 n 是 power-of-2 字节数
        if (p + 1 > len) return false;
        uint8_t len_tok = data[p++];
        int power = len_tok & 0x0F;
        int bytes = 1 << power;
        if (p + bytes > len) return false;
        count = be_read_varint(data + p, bytes);
        p += bytes;
    }

    switch (high) {
        case 0x00: { // null, bool, fill
            if (low == 0x08) table[ref_id] = PlistValue::make_bool(false);
            else if (low == 0x09) table[ref_id] = PlistValue::make_bool(true);
            else table[ref_id] = PlistValue(); // NONE
            return true;
        }
        case 0x01: { // signed int
            int bytes = 1 << (int)low; // low: 0=1B 1=2B 2=4B 3=8B
            if (p + bytes > len) return false;
            int64_t signed_val;
            if (bytes == 1) signed_val = (int8_t)data[p];
            else if (bytes == 2) {
                int16_t sv = (int16_t)(be_read_varint(data + p, 2));
                signed_val = sv;
            } else if (bytes == 4) {
                int32_t sv = (int32_t)(be_read_varint(data + p, 4));
                signed_val = sv;
            } else if (bytes == 8) {
                uint64_t u = be_read_varint(data + p, 8);
                signed_val = (int64_t)u;
            } else {
                // 更宽的不支持（rare）
                signed_val = (int64_t)be_read_varint(data + p, std::min(bytes, 8));
            }
            table[ref_id] = PlistValue::make_int(signed_val);
            return true;
        }
        case 0x02: { // real
            int bytes = 1 << (int)low;
            if (p + bytes > len) return false;
            if (bytes == 4) {
                uint32_t u = (uint32_t)be_read_varint(data + p, 4);
                float f; std::memcpy(&f, &u, 4);
                table[ref_id] = PlistValue::make_real(f);
            } else if (bytes == 8) {
                uint64_t u = be_read_varint(data + p, 8);
                double d; std::memcpy(&d, &u, 8);
                table[ref_id] = PlistValue::make_real(d);
            } else {
                table[ref_id] = PlistValue::make_real(0.0);
            }
            return true;
        }
        case 0x03: { // date（8 字节 double，Apple epoch）
            if (p + 8 > len) return false;
            uint64_t u = be_read_varint(data + p, 8);
            double d; std::memcpy(&d, &u, 8);
            table[ref_id] = PlistValue::make_date(d + kAppleCFAbsoluteTimeOffset);
            return true;
        }
        case 0x04: { // binary data
            if (p + count > len) return false;
            table[ref_id] = PlistValue::make_data(data + p, (size_t)count);
            return true;
        }
        case 0x05: { // ASCII string
            if (p + count > len) return false;
            table[ref_id] = PlistValue::make_string(
                std::string((const char*)(data + p), (size_t)count));
            return true;
        }
        case 0x06: { // UTF-16 string（每个字符 2 字节大端）
            // 简化：把高字节都是 0 的 ASCII 当普通字符串；否则逐字符转 UTF-8
            size_t chars = (size_t)count;
            if (p + chars * 2 > len) return false;
            std::string out;
            out.reserve(chars);
            for (size_t i = 0; i < chars; ++i) {
                uint16_t cp = (uint16_t)((uint16_t(data[p + 2*i]) << 8) | data[p + 2*i + 1]);
                if (cp < 0x80) {
                    out.push_back((char)cp);
                } else if (cp < 0x800) {
                    out.push_back((char)(0xC0 | (cp >> 6)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back((char)(0xE0 | (cp >> 12)));
                    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                }
            }
            table[ref_id] = PlistValue::make_string(out);
            return true;
        }
        case 0x08: { // UID（1 byte low = bytes - 1）
            int bytes = low + 1;
            if (p + bytes > len) return false;
            table[ref_id] = PlistValue::make_int((int64_t)be_read_varint(data + p, bytes));
            return true;
        }
        case 0x0A: { // UTF-8 string (AirPlay 发送端非常常用)
            if (p + count > len) return false;
            table[ref_id] = PlistValue::make_string(
                std::string((const char*)(data + p), (size_t)count));
            return true;
        }
        case 0x0D: { // array: count 个 ref
            // 每个 ref = ref_size 字节
            const uint8_t* ot_start = offset_table_start;
            size_t n = (size_t)count;
            PlistValue arr = PlistValue::make_array();
            for (size_t i = 0; i < n; ++i) {
                if (p + ref_size > len) return false;
                int child_ref = (int)be_read_varint(data + p, ref_size);
                p += ref_size;
                if (!bplist_resolve_object(data, len, ot_start, offset_size, ref_size, child_ref, table))
                    return false;
                arr.array().push_back(table[child_ref]);
            }
            table[ref_id] = std::move(arr);
            return true;
        }
        case 0x0E: { // dict (set form)：count keys + count vals refs
            const uint8_t* ot_start = offset_table_start;
            size_t n = (size_t)count;
            PlistValue dict = PlistValue::make_dict();
            std::vector<int> key_refs(n), val_refs(n);
            for (size_t i = 0; i < n; ++i) {
                if (p + ref_size > len) return false;
                key_refs[i] = (int)be_read_varint(data + p, ref_size);
                p += ref_size;
            }
            for (size_t i = 0; i < n; ++i) {
                if (p + ref_size > len) return false;
                val_refs[i] = (int)be_read_varint(data + p, ref_size);
                p += ref_size;
            }
            for (size_t i = 0; i < n; ++i) {
                if (!bplist_resolve_object(data, len, ot_start, offset_size, ref_size, key_refs[i], table))
                    return false;
                if (!bplist_resolve_object(data, len, ot_start, offset_size, ref_size, val_refs[i], table))
                    return false;
                std::string k = table[key_refs[i]].as_string();
                dict.dict()[k] = table[val_refs[i]];
            }
            table[ref_id] = std::move(dict);
            return true;
        }
        default:
            return false;
    }
}

bool parse_binary_plist(const uint8_t* data, size_t len, PlistValue& out) {
    if (len < 40) return false;   // 至少 8 字节 magic + 32 字节 trailer
    if (memcmp(data, "bplist00", 8) != 0) return false;

    // Trailer 从 len - 32 开始
    const uint8_t* trailer = data + len - 32;
    uint8_t  offset_size = trailer[6]; // 24th byte (offset=6 from trailer start)
    uint8_t  ref_size    = trailer[7];
    uint64_t num_objects   = be_read_varint(trailer + 8, 8);
    uint64_t top_object    = be_read_varint(trailer + 16, 8);
    uint64_t offset_table_off = be_read_varint(trailer + 24, 8);

    if (num_objects == 0) { out = PlistValue(); return true; }
    if (num_objects > (1ULL << 24)) return false; // 太大，拒绝

    if (offset_table_off + num_objects * offset_size > len) return false;
    if (offset_size == 0 || ref_size == 0) return false;
    if ((size_t)top_object >= num_objects) return false;

    std::vector<PlistValue> table(num_objects);  // 全部是 NONE
    const uint8_t* ot_start = data + offset_table_off;

    // 递归解析 top_object 以及其依赖
    if (!bplist_resolve_object(data, len, ot_start, offset_size, ref_size,
                               (int)top_object, table)) {
        return false;
    }
    out = table[(int)top_object];
    return true;
}

// ===================== 自动检测 parse_plist =====================

bool parse_plist(const uint8_t* data, size_t len, PlistValue& out) {
    if (!data || len == 0) return false;
    // 头部探测
    if (len >= 8 && memcmp(data, "bplist00", 8) == 0) {
        return parse_binary_plist(data, len, out);
    }
    std::string s((const char*)data, len);
    return parse_xml_plist(s, out);
}

bool parse_plist(const std::string& data, PlistValue& out) {
    return parse_plist(reinterpret_cast<const uint8_t*>(data.data()), data.size(), out);
}

// ===================== XML 序列化 =====================

static void xml_indent(std::string& out, int depth) {
    for (int i = 0; i < depth; ++i) out += "  ";
}

static void serialize_value(const PlistValue& v, std::string& out, int depth);

static void serialize_dict(const PlistDict& dict, std::string& out, int depth) {
    xml_indent(out, depth); out += "<dict>\n";
    for (const auto& kv : dict) {
        xml_indent(out, depth + 1);
        out += "<key>";
        xml_append_escape(out, kv.first);
        out += "</key>\n";
        serialize_value(kv.second, out, depth + 1);
    }
    xml_indent(out, depth); out += "</dict>\n";
}

static void serialize_array(const PlistArray& arr, std::string& out, int depth) {
    xml_indent(out, depth); out += "<array>\n";
    for (const auto& v : arr) serialize_value(v, out, depth + 1);
    xml_indent(out, depth); out += "</array>\n";
}

static void serialize_value(const PlistValue& v, std::string& out, int depth) {
    switch (v.type()) {
        case PlistType::STRING: {
            xml_indent(out, depth); out += "<string>";
            xml_append_escape(out, v.as_string());
            out += "</string>\n";
            break;
        }
        case PlistType::INT: {
            xml_indent(out, depth);
            out += "<integer>" + std::to_string(v.as_int()) + "</integer>\n";
            break;
        }
        case PlistType::REAL: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", v.as_real());
            xml_indent(out, depth);
            out += "<real>"; out += buf; out += "</real>\n";
            break;
        }
        case PlistType::BOOL: {
            xml_indent(out, depth);
            out += v.as_bool() ? "<true/>\n" : "<false/>\n";
            break;
        }
        case PlistType::DATA: {
            const auto& d = v.as_data();
            xml_indent(out, depth); out += "<data>\n";
            // 每行 76 字符 base64
            std::string b64 = base64_encode(d.data(), d.size());
            for (size_t i = 0; i < b64.size(); i += 76) {
                xml_indent(out, depth + 1);
                out += b64.substr(i, 76);
                out += "\n";
            }
            xml_indent(out, depth); out += "</data>\n";
            break;
        }
        case PlistType::DATE: {
            // Apple epoch = Unix - 978307200
            char buf[64];
            double apple_ts = v.as_date() - kAppleCFAbsoluteTimeOffset;
            std::snprintf(buf, sizeof(buf), "%.3f", apple_ts);
            xml_indent(out, depth);
            out += "<date>"; out += buf; out += "</date>\n";
            break;
        }
        case PlistType::ARRAY: {
            serialize_array(v.array(), out, depth);
            break;
        }
        case PlistType::DICT: {
            serialize_dict(v.dict(), out, depth);
            break;
        }
        case PlistType::NONE:
        default:
            break;
    }
}

std::string serialize_xml_plist(const PlistValue& value) {
    std::string out;
    out.reserve(1024);
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    out += "<plist version=\"1.0\">\n";
    serialize_value(value, out, 0);
    out += "</plist>\n";
    return out;
}

} // namespace util
} // namespace airplay2
