/*!
 * @file curve25519.h
 * @brief X25519 ECDH + Ed25519 签名/验签
 *
 * 这两个算法共享同一个 Montgomery 曲线 Curve25519：
 *   y² = x³ + 486662x² + x   模 p = 2²⁵⁵ - 19
 *
 * AirPlay FairPlay MFi-SAP 流程里：
 *   1. pair-setup 阶段：两端用 X25519 做 ECDH，得到共享密钥
 *   2. pair-verify 阶段：用 Ed25519 证书签名确认设备身份（或反方向）
 *
 * 本实现基于公开的 ref10 规范，无外部依赖，64 位 limb 数学运算，
 * 每次 X25519 约几微秒，完全满足 AirPlay 使用频次。
 */
#ifndef AIRPLAY2_CURVE25519_H
#define AIRPLAY2_CURVE25519_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace airplay2 {
namespace crypto {

/*!
 * @brief X25519 密钥长度（32 字节 = 256 位）
 */
static constexpr size_t kX25519KeySize = 32;
static constexpr size_t kEd25519KeySize = 32;
static constexpr size_t kEd25519SigSize = 64;

/* ====================================================================
 *                              X25519 (ECDH)
 * ==================================================================== */

/*!
 * @brief 从 32 字节随机种子生成 X25519 私钥
 *
 * @param seed   任意 32 字节随机熵
 * @param sk_out 输出 32 字节标量（已"夹紧"clamp：按 RFC 7748 5.2 节
 *               清零 bit0..bit2、置位 bit254、清零 bit255）
 * @param pk_out 输出 32 字节公钥 = clamp(seed) · G
 */
void x25519_keygen(const uint8_t seed[kX25519KeySize],
                   uint8_t sk_out[kX25519KeySize],
                   uint8_t pk_out[kX25519KeySize]);

/*!
 * @brief X25519 标量乘法：shared = sk · their_pk
 *
 * @param sk           自己 32 字节私钥（已 clamp）
 * @param their_pk     对端 32 字节公钥
 * @param shared_out   输出 32 字节共享密钥
 *
 * @return false 仅当 their_pk 是弱密钥（导致 shared 全零）；此时应丢弃该结果。
 */
bool x25519_shared(const uint8_t sk[kX25519KeySize],
                   const uint8_t their_pk[kX25519KeySize],
                   uint8_t shared_out[kX25519KeySize]);

/* ====================================================================
 *                             Ed25519 (签名)
 * ==================================================================== */

/*!
 * @brief Ed25519 密钥生成
 *
 * @param seed   32 字节随机熵
 * @param sk_out 输出 32 字节私钥标量
 * @param pk_out 输出 32 字节公钥（= 点 A 的 Y 坐标压缩）
 * @param prefix_out 可选：输出 32 字节"前缀"，签名时要用。
 *                   Ed25519 把 SHA-512(seed) 前 32 字节做 clamp 成私钥，
 *                   后 32 字节做签名 nonce 前缀。调用者可以缓存这个前缀避免
 *                   每次签名再做一次 SHA-512。
 */
void ed25519_keygen(const uint8_t seed[kEd25519KeySize],
                    uint8_t sk_out[kEd25519KeySize],
                    uint8_t pk_out[kEd25519KeySize],
                    uint8_t prefix_out[kEd25519KeySize] = nullptr);

/*!
 * @brief Ed25519 签名
 *
 * @param sk          32 字节私钥（已 clamp，来自 ed25519_keygen）
 * @param prefix      32 字节 nonce 前缀（keygen 的 SHA-512 后半）
 * @param pk          32 字节公钥
 * @param msg         消息
 * @param msg_len     消息字节数
 * @param sig_out     输出 64 字节签名 (R, s)
 */
void ed25519_sign(const uint8_t sk[kEd25519KeySize],
                  const uint8_t prefix[kEd25519KeySize],
                  const uint8_t pk[kEd25519KeySize],
                  const uint8_t* msg, size_t msg_len,
                  uint8_t sig_out[kEd25519SigSize]);

/*!
 * @brief Ed25519 验签
 *
 * @param pk    32 字节公钥
 * @param msg   消息
 * @param msg_len 消息字节数
 * @param sig   64 字节签名
 * @return true 表示签名通过
 */
bool ed25519_verify(const uint8_t pk[kEd25519KeySize],
                    const uint8_t* msg, size_t msg_len,
                    const uint8_t sig[kEd25519SigSize]);

/* 便捷 vector 版本 */
struct X25519Key {
    std::vector<uint8_t> sk; ///< 32 字节私钥
    std::vector<uint8_t> pk; ///< 32 字节公钥
};
X25519Key x25519_generate(const std::vector<uint8_t>& seed);
std::vector<uint8_t> x25519_shared(const std::vector<uint8_t>& sk,
                                   const std::vector<uint8_t>& their_pk);

struct Ed25519Key {
    std::vector<uint8_t> sk;     ///< 32 字节私钥
    std::vector<uint8_t> pk;     ///< 32 字节公钥
    std::vector<uint8_t> prefix; ///< 32 字节 nonce 前缀
};
Ed25519Key ed25519_generate(const std::vector<uint8_t>& seed);
std::vector<uint8_t> ed25519_sign(const Ed25519Key& key, const uint8_t* msg, size_t len);
inline std::vector<uint8_t> ed25519_sign(const Ed25519Key& key, const std::vector<uint8_t>& msg) {
    return ed25519_sign(key, msg.empty() ? nullptr : msg.data(), msg.size());
}
bool ed25519_verify(const std::vector<uint8_t>& pk, const uint8_t* msg, size_t len,
                    const uint8_t sig[kEd25519SigSize]);

} // namespace crypto
} // namespace airplay2

#endif // AIRPLAY2_CURVE25519_H
