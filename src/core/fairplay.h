/*!
 * @file fairplay.h
 * @brief AirPlay FairPlay SAP (Secure Association Protocol) 握手状态机
 *
 * FairPlay 是 AirPlay 的"标准鉴权"：iOS / macOS 自带播放器默认会要求
 * 走 FairPlay；如果服务端没实现且发送端强制 FairPlay（pw=1）会拒绝连。
 *
 * 协议流程（完全遵循已公开的逆向文档）：
 *   1. POST /pair-setup (state=M1)
 *        client → server:  client_hello (TLV: method=1, session_id)
 *        server → client:  server_hello (TLV: server_pk_X25519)
 *   2. POST /pair-setup (state=M3)
 *        client → server:  auth_tag=HMAC(client_pk || server_pk),
 *                            client_pk_X25519 (32B)
 *        server:            shared = X25519(sk, client_pk)
 *                            session_key = HKDF(shared, "Pair-Setup", salt=[])
 *                            verify auth_tag
 *   3. POST /pair-setup (state=M5) [若要求 PIN]
 *        client → server:  PBKDF2(PIN, salt) 的 HMAC 证明
 *        server:            用 PBKDF2-HMAC-SHA512 校验
 *   4. POST /pair-setup (state=M7)
 *        server → client:  HMAC(session_key, "ok") 表示成功
 *
 * pair-verify 用 Ed25519 证书证明服务端持有已配对的私钥。
 *
 * 本实现为开源版本，不使用 Apple MFi 芯片，但与协议消息结构完全
 * 对齐，因此：
 *   - 未启用 PIN 时，发送端"不强制 FairPlay"的客户端（如 iTunes Win/Linux 版、
 *     shairport-sync 同代的客户端）能直接完成握手。
 *   - 强制 FairPlay 且没有 MFi 证书的 iOS 客户端会失败，此时会回落到
 *     简单 PIN（airplay_pairing.cpp 中的轻量 PIN）。
 */
#ifndef AIRPLAY2_FAIRPLAY_H
#define AIRPLAY2_FAIRPLAY_H

#include <cstdint>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../crypto/curve25519.h"   // X25519Key / Ed25519Key

namespace airplay2 {

/*!
 * @brief FairPlay TLV (Type-Length-Value) 常用类型号
 *
 * 参考 AirPlay2-Unofficial / openairplay 公开的 TLV 枚举。
 * 每个 item：[u8 type][u16 be length][bytes]
 */
enum class FpTlv : uint8_t {
    METHOD         = 0x00, ///< 0=pin, 1=sap, 2=cert, ...
    STATE          = 0x01, ///< M1..M7
    PUBLIC_KEY     = 0x02, ///< X25519 pk 32B
    SESSION_ID     = 0x03, ///< 8 字节会话 id
    ENCRYPTED_DATA = 0x04, ///< AES-128-CBC 密文
    AUTH_TAG       = 0x05, ///< HMAC-SHA256 认证
    SIGNATURE      = 0x06, ///< Ed25519 签名
    SALT           = 0x07, ///< PBKDF2 盐
    ITERATIONS     = 0x08, ///< PBKDF2 迭代次数 (u32 be)
    DEVICE_INFO    = 0x09, ///< 客户端设备标识字符串
    ERROR_CODE     = 0x0A, ///< 1=auth_failed 2=need_pin ...
    FLAG           = 0x0B, ///< 布尔位
    CERTIFICATE    = 0x0C, ///< Ed25519 公钥证书 (self-signed)
    NONCE          = 0x0D, ///< 16 字节随机 nonce
    AUX_DATA       = 0x0E, ///< 可选扩展数据
};

/*!
 * @brief 编解码 TLV 列表
 */
struct TlvItem {
    FpTlv type;
    std::vector<uint8_t> value;
};

/// 把 TLV 列表编码成字节流
std::vector<uint8_t> fp_tlv_encode(const std::vector<TlvItem>& items);

/// 从字节流解析一组 TLV
bool fp_tlv_decode(const uint8_t* data, size_t len, std::vector<TlvItem>& out);

/// 便捷：按 type 取第一个匹配（找不到返回 nullptr）
const std::vector<uint8_t>* fp_tlv_find(const std::vector<TlvItem>& items, FpTlv t);

/// 便捷：写 uint32 BE
static inline void put_be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x>>24)); v.push_back((uint8_t)(x>>16));
    v.push_back((uint8_t)(x>>8));  v.push_back((uint8_t)x);
}
static inline uint32_t get_be32(const uint8_t* p) {
    return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|uint32_t(p[3]);
}

/*!
 * @brief 单次 FairPlay 握手会话状态
 */
enum class FpState : int {
    IDLE = 0,   // 未开始
    M1_SENT,    // 已回 server_hello (含 pk)
    M3_DONE,    // 已完成 ECDH + 认证
    M5_DONE,    // PIN 通过
    COMPLETE,   // 握手完成，可用 session_key
    FAILED
};

/*!
 * @brief FairPlay 会话状态机
 *
 * 每 RTSP 连接 / 每会话一个实例。线程安全（一个内部 mutex）。
 */
class FairPlaySap {
public:
    FairPlaySap();
    ~FairPlaySap() = default;

    /// 重置到 IDLE（生成新的 X25519 密钥对）
    void reset();

    FpState state() const {
        std::lock_guard<std::mutex> lk(mu_);
        return state_;
    }

    bool is_complete() const { return state() == FpState::COMPLETE; }

    /// session_key：握手完成后可用于加密 RTP / RTSP 控制帧
    /// （标准 AirPlay 中 session_key 是 32 字节）
    const std::vector<uint8_t>& session_key() const { return session_key_; }

    /*!
     * @brief 处理 /pair-setup 请求 body 并返回 response
     *
     * @param body       原始请求（TLV 字节流）
     * @param len        body 长度
     * @param out_resp   输出响应字节（可直接 RTSP 返回）
     * @param need_pin   返回 true 表示需要 PIN（接下来要等 M5 PIN 证明）
     * @param pin_ok     若之前需要 PIN，这里返回校验结果
     * @return Status::OK 或 ERROR_AUTH_FAILED
     */
    int handle_pair_setup(const uint8_t* body, size_t len,
                          std::vector<uint8_t>& out_resp,
                          bool& need_pin, bool& pin_ok);

    /// 处理 /pair-verify（Ed25519 证书签名验证）
    int handle_pair_verify(const uint8_t* body, size_t len,
                           std::vector<uint8_t>& out_resp);

    /// 给该握手实例设置静态 PIN（4 位）；空表示不需要 PIN
    void set_pin(const std::string& pin) { pin_ = pin; }

    /// 设备级长期 Ed25519 证书密钥（可从外部加载持久化）；不设置会临时生成
    void set_device_cert(const crypto::Ed25519Key& cert) { cert_ = cert; have_cert_ = true; }

private:
    mutable std::mutex mu_;
    FpState state_ = FpState::IDLE;
    std::string pin_;

    // M1-M3 握手材料
    crypto::X25519Key ecdh_;       // 本端 ephemeral X25519
    uint64_t session_id_ = 0;
    std::vector<uint8_t> peer_pk_; // 32 字节对端 X25519
    std::vector<uint8_t> shared_;  // X25519 共享密钥 (32)
    std::vector<uint8_t> session_key_; // HKDF 派生的会话密钥 (32)

    // 证书
    bool have_cert_ = false;
    crypto::Ed25519Key cert_;

    // HKDF 派生常量
    static std::vector<uint8_t> kdf_sap_key(const uint8_t shared[32],
                                             const uint8_t info[], size_t info_len);
};

} // namespace airplay2

#endif // AIRPLAY2_FAIRPLAY_H
