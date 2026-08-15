/*!
 * @file sha512.h
 * @brief SHA-512 哈希 + HMAC-SHA-512 + HKDF-SHA-512
 *
 * AirPlay FairPlay SAP 的密钥派生链大量使用 SHA-512 / HMAC / HKDF：
 *   - HKDF-Extract(salt, IKM) → PRK
 *   - HKDF-Expand(PRK, info, L) → OKM
 *
 * 本实现是 FIPS 180-4 合规的零依赖版本，无任何外部依赖，
 * 适合在 iOS / Android / 嵌入式 / 桌面 全平台使用。
 *
 * 性能：SHA-512 在 64 位机上非常快（每次压缩 128 字节块），
 * AirPlay 单会话只在 pair-setup / pair-verify 时用到几次，
 * 完全无需担心性能。
 */
#ifndef AIRPLAY2_SHA512_H
#define AIRPLAY2_SHA512_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace airplay2 {
namespace crypto {

/*!
 * @brief SHA-512 输出字节数（64）
 */
static constexpr size_t kSha512Size = 64;

/*!
 * @brief SHA-512 哈希器（单-shot 或增量式）
 *
 * 用法 1（单-shot）：
 *   uint8_t out[64];
 *   Sha512::hash(data, len, out);
 *
 * 用法 2（增量式）：
 *   Sha512 s; s.update(a, l1); s.update(b, l2); s.final(out);
 */
class Sha512 {
public:
    Sha512();
    /// 追加数据，可任意分片调用
    void update(const uint8_t* data, size_t len);
    /// 输出 64 字节摘要；重置哈希器以便复用
    void final(uint8_t out[kSha512Size]);
    /// 便捷单-shot
    static void hash(const uint8_t* data, size_t len, uint8_t out[kSha512Size]);

private:
    void compress(const uint8_t* block);
    uint64_t state_[8];
    uint64_t bitlen_hi_, bitlen_lo_;
    size_t   buflen_;
    uint8_t  buffer_[128]; ///< SHA-512 使用 1024-bit = 128 字节块
};

/*!
 * @brief HMAC-SHA-512
 * @param key   HMAC 密钥（任意长度）
 * @param key_len 密钥字节数
 * @param msg  消息
 * @param msg_len 消息字节数
 * @param out  输出 64 字节 MAC
 */
void hmac_sha512(const uint8_t* key, size_t key_len,
                 const uint8_t* msg, size_t msg_len,
                 uint8_t out[kSha512Size]);

/*!
 * @brief HMAC-SHA-512（便利 vector 版）
 */
std::vector<uint8_t> hmac_sha512(const std::vector<uint8_t>& key,
                                 const std::vector<uint8_t>& msg);

/*!
 * @brief HKDF-Extract (RFC 5869)
 *   PRK = HMAC-SHA-512(salt, IKM)
 *
 * @param salt      可选盐，空则用 64 字节零
 * @param salt_len  盐字节数
 * @param ikm       输入密钥材料
 * @param ikm_len   IKM 字节数
 * @param prk_out   输出 64 字节 PRK
 */
void hkdf_extract(const uint8_t* salt, size_t salt_len,
                  const uint8_t* ikm,  size_t ikm_len,
                  uint8_t prk_out[kSha512Size]);

/*!
 * @brief HKDF-Expand (RFC 5869)
 *   OKM = T(1) || T(2) || ... 其中 T(i) = HMAC(PRK, T(i-1) || info || 0x0i)
 *
 * @param prk        伪随机密钥（长度必须=64）
 * @param info       可选上下文信息（可空）
 * @param info_len   info 字节数
 * @param okm_out    输出缓冲区
 * @param okm_len    需要导出的字节数（<= 255 * 64）
 * @return false 表示参数非法
 */
bool hkdf_expand(const uint8_t prk[kSha512Size],
                 const uint8_t* info, size_t info_len,
                 uint8_t* okm_out, size_t okm_len);

/*!
 * @brief HKDF 一步导出（Extract + Expand）
 */
std::vector<uint8_t> hkdf_derive(const std::vector<uint8_t>& ikm,
                                 const std::vector<uint8_t>& salt,
                                 const std::vector<uint8_t>& info,
                                 size_t okm_len);

/*!
 * @brief PBKDF2-HMAC-SHA-512（RFC 8018）
 *
 * FairPlay 的 PIN 码派生会用它：DK = PBKDF2(PIN, salt, iters, dkLen)
 */
std::vector<uint8_t> pbkdf2_hmac_sha512(const std::vector<uint8_t>& password,
                                        const std::vector<uint8_t>& salt,
                                        uint32_t iterations, size_t dk_len);

} // namespace crypto
} // namespace airplay2

#endif // AIRPLAY2_SHA512_H
