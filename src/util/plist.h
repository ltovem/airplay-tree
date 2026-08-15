/*!
 * @file plist.h
 * @brief Apple Property List (plist) 解析器与序列化器
 *
 * AirPlay 2 协议大量使用 plist 格式传递结构化数据：
 *   - /info 响应：设备能力描述
 *   - /pair-setup /pair-verify：配对握手参数
 *   - /action：播放控制（play/pause/seek/volume）
 *   - /metadata：正在播放的曲目信息
 *   - /event：事件通知
 *
 * Apple 定义了两种编码：
 *   1. XML plist — 文本格式，可读性好，用于 /info 响应
 *   2. Binary plist (bplist) — 紧凑二进制格式，用于 /action 等请求
 *
 * 本模块实现了两种格式的解析和 XML 格式的序列化。
 *
 * @note 线程安全：所有方法都是纯函数（无静态状态），可并发调用。
 */
#ifndef AIRPLAY2_PLIST_H
#define AIRPLAY2_PLIST_H

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <variant>

namespace airplay2 {
namespace util {

/*!
 * @brief plist 值类型枚举
 *
 * 对应 Apple Foundation 框架的 NSObject 子类：
 *   NSString → string
 *   NSNumber → int / real / bool
 *   NSData   → data
 *   NSDate   → date (Unix timestamp, double)
 *   NSArray  → array
 *   NSDictionary → dict
 */
enum class PlistType : uint8_t {
    NONE   = 0,  ///< 未设置（默认值）
    STRING = 1,  ///< UTF-8 字符串
    INT    = 2,  ///< 64 位有符号整数
    REAL   = 3,  ///< 64 位浮点数
    BOOL   = 4,  ///< 布尔值
    DATA   = 5,  ///< 二进制数据
    DATE   = 6,  ///< 日期（Unix 时间戳秒数）
    ARRAY  = 7,  ///< 数组（子 PlistValue 列表）
    DICT   = 8,  ///< 字典（string → PlistValue 映射）
};

class PlistValue;
using PlistArray  = std::vector<PlistValue>;
using PlistDict   = std::map<std::string, PlistValue>;

/*!
 * @brief plist 节点值（tagged union 风格）
 *
 * 使用 type_ 字段区分当前存储的是哪种类型，
 * 通过 as_string() / as_int() 等便捷方法取出。
 * 未设置时 type_ == NONE，所有 as_xxx() 返回默认值。
 */
class PlistValue {
public:
    PlistValue() : type_(PlistType::NONE) {}
    ~PlistValue() = default;

    PlistValue(const PlistValue&) = default;
    PlistValue(PlistValue&&) = default;
    PlistValue& operator=(const PlistValue&) = default;
    PlistValue& operator=(PlistValue&&) = default;

    // ---- 工厂构造 ----
    static PlistValue make_string(const std::string& s);
    static PlistValue make_int(int64_t v);
    static PlistValue make_real(double v);
    static PlistValue make_bool(bool v);
    static PlistValue make_data(const uint8_t* data, size_t len);
    static PlistValue make_data(const std::vector<uint8_t>& data);
    static PlistValue make_date(double unix_timestamp);
    static PlistValue make_array();
    static PlistValue make_dict();

    // ---- 类型查询 ----
    PlistType type() const { return type_; }
    bool is_string() const { return type_ == PlistType::STRING; }
    bool is_int()    const { return type_ == PlistType::INT; }
    bool is_real()   const { return type_ == PlistType::REAL; }
    bool is_bool()   const { return type_ == PlistType::BOOL; }
    bool is_data()   const { return type_ == PlistType::DATA; }
    bool is_array()  const { return type_ == PlistType::ARRAY; }
    bool is_dict()   const { return type_ == PlistType::DICT; }

    // ---- 取值（类型不匹配时返回默认值）----
    const std::string& as_string() const;
    int64_t   as_int() const;
    double    as_real() const;
    bool      as_bool() const;
    const std::vector<uint8_t>& as_data() const;
    double    as_date() const;

    // ---- 容器访问 ----
    PlistArray& array();
    const PlistArray& array() const;
    PlistDict& dict();
    const PlistDict& dict() const;

    /// 字典便捷取值：找不到返回 NONE 类型的空 PlistValue
    const PlistValue& get(const std::string& key) const;

    /// 字典便捷取字符串：找不到返回空串
    std::string get_string(const std::string& key) const;

    /// 字典便捷取整数：找不到返回 0
    int64_t get_int(const std::string& key) const;

    /// 字典便捷取布尔：找不到返回 false
    bool get_bool(const std::string& key) const;

private:
    PlistType type_;
    std::string str_;
    int64_t int_   = 0;
    double  real_  = 0.0;
    bool    bool_  = false;
    std::vector<uint8_t> data_;
    std::shared_ptr<PlistArray> array_;
    std::shared_ptr<PlistDict>  dict_;
};

// 静态空值，供 get() 返回引用
const PlistValue& plist_none();

/*!
 * @brief 从 XML plist 文本解析为 PlistValue
 *
 * 支持标准 Apple XML plist 格式：
 *   <?xml version="1.0" encoding="UTF-8"?>
 *   <plist version="1.0">
 *     <dict>
 *       <key>name</key><string>Living Room</string>
 *       <key>volume</key><real>0.5</real>
 *     </dict>
 *   </plist>
 *
 * @param xml XML 文本
 * @param out  解析结果（成功时为 DICT 或 ARRAY）
 * @return true 解析成功
 */
bool parse_xml_plist(const std::string& xml, PlistValue& out);

/*!
 * @brief 从二进制 plist (bplist00) 解析为 PlistValue
 *
 * 支持 bplist00 格式 v1r0：
 *   - 支持 token 类型 0x00-0x09 (null/bool/fill/int/real/date/data/UTF8)
 *   - 支持 token 0xA0-0xAF (UTF-8 string)
 *   - 支持 token 0xC0-0xCF (UID)
 *   - 支持 token 0xD0-0xDF (array)
 *   - 支持 token 0xE0-0xEF (dict — set 形式)
 *
 * @note 不支持 bplist 中的 offset table trailier offset > 2^32 的情况（极少见）
 */
bool parse_binary_plist(const uint8_t* data, size_t len, PlistValue& out);

/*!
 * @brief 自动检测格式并解析 plist
 *
 * 检查头部：
 *   - "<?xml" 或 "<plist" → XML plist
 *   - "bplist00"          → Binary plist
 *
 * 用于处理 Content-Type 不明确或 body 是 raw bytes 的情况。
 */
bool parse_plist(const uint8_t* data, size_t len, PlistValue& out);

bool parse_plist(const std::string& data, PlistValue& out);

/*!
 * @brief 将 PlistValue 序列化为 XML plist 文本
 *
 * 输出标准 Apple XML plist 格式，包含 XML 声明和 DOCTYPE。
 */
std::string serialize_xml_plist(const PlistValue& value);

} // namespace util
} // namespace airplay2

#endif // AIRPLAY2_PLIST_H
