/*!
 * @file aes_ctr.h
 * @brief AES-128-CTR 解密器（AirPlay 音频加密用）
 *
 * AirPlay 发送端在 SDP 里通过 a=aeskey / a=aesiv 传递 16 字节 AES-128 密钥
 * 和 16 字节 IV。RTP 音频负载用 AES-128-CTR 模式加密，计数器从 IV 起始，
 * 每加密 16 字节后计数器 +1。
 *
 * 本实现是纯 C++ 的紧凑 AES-128（约 400 行），无外部依赖，
 * 适用于所有目标平台（桌面 / iOS / Android / 嵌入式）。
 *
 * @note 性能：单核约 50 MB/s（-O2），足够 44100Hz/16bit/2ch ~ 176 KB/s 的需求。
 *       如需更高性能可替换为 AES-NI / NEON 版本。
 */
#ifndef AIRPLAY2_AES_CTR_H
#define AIRPLAY2_AES_CTR_H

#include <cstdint>
#include <cstddef>
#include <vector>

namespace airplay2 {
namespace crypto {

/*!
 * @brief AES-128-CTR 加解密器
 *
 * CTR 模式是对称的：encrypt == decrypt，都是 "plaintext XOR keystream"。
 * AirPlay 只用解密方向，但接口同时支持两个方向。
 *
 * 线程安全：不可重入；每个会话应拥有独立的 AesCtr 实例。
 * 如果需要跨线程使用，调用方需自行加锁。
 */
class AesCtr {
public:
    AesCtr();
    ~AesCtr();

    AesCtr(const AesCtr&) = delete;
    AesCtr& operator=(const AesCtr&) = delete;

    /*!
     * @brief 初始化密钥和初始 IV
     * @param key  16 字节 AES-128 密钥（从 SDP aeskey hex 解码而来）
     * @param iv   16 字节初始计数器（从 SDP aesiv hex 解码而来）
     * @return false 表示 key/iv 长度不是 16
     */
    bool set_key(const uint8_t key[16], const uint8_t iv[16]);

    /*!
     * @brief 解密一段数据（in-place 安全）
     *
     * CTR 模式下 encrypt/decrypt 是同一个操作（XOR keystream）。
     * 内部计数器会自动递增，保证连续调用时 keystream 连续。
     *
     * @param in   输入密文
     * @param out  输出明文（可以 == in 做 in-place）
     * @param len  字节数
     */
    void process(const uint8_t* in, uint8_t* out, size_t len);

    /*!
     * @brief 重置计数器到初始 IV（用于 flush / 重新开始）
     */
    void reset_counter();

    /// 是否已成功 set_key
    bool is_ready() const { return ready_; }

private:
    /// AES-128 内部轮密钥（11 个 16 字节 = 176 字节）
    uint32_t round_keys_[44];
    /// 当前计数器（16 字节，大端）
    uint8_t counter_[16];
    /// 初始 IV（用于 reset）
    uint8_t iv_[16];
    /// keystream 缓存：每 16 字节生成一次，不足 16 时缓存剩余部分
    uint8_t keystream_[16];
    int keystream_pos_;
    bool ready_;

    /// AES-128 单块加密（用于生成 keystream）
    void aes128_encrypt_block(const uint8_t in[16], uint8_t out[16]);
    /// 计数器 +1（大端 128 位整数递增）
    void increment_counter();
};

/*!
 * @brief 将十六进制字符串解码为字节数组
 *
 * AirPlay SDP 中 aeskey / aesiv 以 hex 字符串传递，
 * 例如 "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6"。
 *
 * @param hex   输入 hex 字符串（长度必须是偶数）
 * @param out    输出缓冲区
 * @param out_len 输出缓冲区最大容量；返回时填入实际写入字节数
 * @return false 表示 hex 长度不是偶数或包含非法字符或缓冲区不够
 */
bool hex_to_bytes(const std::string& hex, uint8_t* out, size_t* out_len);

/*!
 * @brief 便捷版：hex string → vector<uint8_t>
 */
std::vector<uint8_t> hex_to_vector(const std::string& hex);

} // namespace crypto
} // namespace airplay2

#endif // AIRPLAY2_AES_CTR_H
