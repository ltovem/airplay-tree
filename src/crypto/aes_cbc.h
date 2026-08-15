/*!
 * @file aes_cbc.h
 * @brief AES-128-CBC 加解密（AirPlay FairPlay 握手消息加密用）
 *
 * FairPlay pair-setup / pair-verify 的交换消息体用 AES-128-CBC
 * 加解密，PKCS#7 填充。密钥由 X25519 ECDH 派生的会话密钥经 HKDF
 * 得出。接口同时支持 in-place 操作，便于减少一份拷贝。
 *
 * 复用 aes_ctr.h 里的 AES-128 核心模块（aes128_encrypt_block /
 * aes128_decrypt_block 导出到这里使用）。这样只写一次 S-Box，
 * 避免二进制膨胀。
 */
#ifndef AIRPLAY2_AES_CBC_H
#define AIRPLAY2_AES_CBC_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace airplay2 {
namespace crypto {

/*!
 * @brief AES-128-CBC 加解密器（PKCS#7 填充）
 *
 * 注意：CBC 不提供完整性保护。FairPlay 协议在消息末尾额外附
 * HMAC-SHA-256 摘要作认证（外层由 fairplay.cpp 调用方负责）。
 */
class AesCbc {
public:
    AesCbc();
    ~AesCbc();
    AesCbc(const AesCbc&) = delete;
    AesCbc& operator=(const AesCbc&) = delete;

    /*!
     * @brief 设置 16 字节 AES-128 密钥
     */
    bool set_key(const uint8_t key[16]);

    /// 是否已 set_key
    bool is_ready() const { return ready_; }

    /*!
     * @brief AES-128-CBC 加密（PKCS#7 填充）
     *
     * @param iv     16 字节 IV（任意、不可重；调用方负责生成随机 IV）
     * @param in     明文
     * @param in_len 明文字节数
     * @param out    输出密文；长度 >= in_len + 16（最坏情况：1 字节明文 → 32 字节密文）
     * @param out_len 传入 out 容量；返回实际写出字节数
     * @return false 表示密钥未设置或缓冲区不够
     */
    bool encrypt(const uint8_t iv[16],
                 const uint8_t* in, size_t in_len,
                 uint8_t* out, size_t* out_len);

    /*!
     * @brief AES-128-CBC 解密（去掉 PKCS#7 填充）
     *
     * @param iv      16 字节 IV（由加密方传过来）
     * @param in      密文（长度必须是 16 倍数）
     * @param in_len  密文字节数
     * @param out     输出明文；容量 >= in_len 即可
     * @param out_len 传入容量；返回实际明文字节数
     * @return false 表示密钥未设置 / 长度非 16 倍数 / 填充非法
     */
    bool decrypt(const uint8_t iv[16],
                 const uint8_t* in, size_t in_len,
                 uint8_t* out, size_t* out_len);

    /*!
     * @brief 便捷 vector 版：加密
     */
    std::vector<uint8_t> encrypt_vec(const uint8_t iv[16], const uint8_t* in, size_t len);
    std::vector<uint8_t> encrypt_vec(const uint8_t iv[16], const std::vector<uint8_t>& in) {
        return encrypt_vec(iv, in.empty() ? nullptr : in.data(), in.size());
    }

    /*!
     * @brief 便捷 vector 版：解密；失败返回空 vector
     */
    std::vector<uint8_t> decrypt_vec(const uint8_t iv[16], const uint8_t* in, size_t len);
    std::vector<uint8_t> decrypt_vec(const uint8_t iv[16], const std::vector<uint8_t>& in) {
        return decrypt_vec(iv, in.empty() ? nullptr : in.data(), in.size());
    }

private:
    // 与 AesCtr 同源的轮密钥（AES-128 11 轮 = 44 * 4 = 176 字节）
    uint32_t round_keys_[44];
    bool ready_ = false;

    static void aes128_encrypt_block(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16]);
    static void aes128_decrypt_block(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16]);
};

} // namespace crypto
} // namespace airplay2

#endif // AIRPLAY2_AES_CBC_H
