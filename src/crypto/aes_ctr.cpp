/*!
 * @file aes_ctr.cpp
 * @brief AES-128-CTR 解密器实现（AirPlay 音频加密用）
 *
 * 纯 C++ 实现 FIPS-197 标准 AES-128 加密（10 轮），并在此基础上构建 CTR 模式。
 * 无外部依赖，仅使用标准 C++17，可在 macOS / iOS / Linux / Android / Windows
 * 上编译运行。
 *
 * CTR 模式原理（NIST SP 800-38A）：
 *   keystream = AES_encrypt(counter)
 *   plaintext = ciphertext XOR keystream
 *   每加密 16 字节后 counter +1
 *
 * 由于 CTR 模式加解密对称（都是 XOR keystream），本实现同一函数同时支持
 * 加密和解密方向。AirPlay 仅使用解密方向。
 */
#include "aes_ctr.h"

#include <cstring>  // memcpy / memset
#include <string>   // std::string（头文件中 hex_to_bytes 声明依赖）
#include <array>    // std::array（用于编译期计算逆 S-Box）
#include <vector>   // std::vector（hex_to_bytes 内部临时缓冲）

namespace airplay2 {
namespace crypto {

// ============================================================================
// AES-128 常量表
// ============================================================================

// FIPS-197 标准 S-Box：SubBytes 步骤中对每个字节做非线性替换。
// 安全性核心——实现 GF(2^8) 上的乘法逆元后接仿射变换，
// 打乱字节间的代数关系，抵抗线性 / 差分密码分析。
static constexpr uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5,
    0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc,
    0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a,
    0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b,
    0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85,
    0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17,
    0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9,
    0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6,
    0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94,
    0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68,
    0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

// 从 S-Box 编译期计算逆 S-Box，避免手工录入 256 字节带来的 transcription 错误。
// 逆 S-Box 用于 InvSubBytes（解密方向的 SubBytes 逆操作）。
// CTR 模式仅需前向加密，此表当前未使用，保留以备未来扩展为 CBC/ECB 解密。
static constexpr std::array<uint8_t, 256> compute_inv_sbox() {
    std::array<uint8_t, 256> inv{};
    for (int i = 0; i < 256; ++i) {
        inv[kSbox[i]] = static_cast<uint8_t>(i);
    }
    return inv;
}

[[maybe_unused]] static constexpr auto kInvSbox = compute_inv_sbox();

// KeyExpansion 轮常数：GF(2^8) 中 x^(i-1) 的值（i = 1..10）。
// 每个轮次的首个 word 与对应 Rcon 异或，打破密钥扩展的对称性，
// 防止弱密钥在扩展过程中产生重复模式（FIPS-197 §5.2）。
static constexpr uint8_t kRcon[10] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

// ============================================================================
// 字节级辅助函数
// ============================================================================

// 大端加载 4 字节为 uint32_t。
// AES 规范以大端序解释 word，统一使用大端可避免跨平台字节序差异导致
// 轮密钥排列错误。
static inline uint32_t load_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) |
           (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) <<  8) |
           (uint32_t(p[3]));
}

// GF(2^8) 乘以 2（即 xtime）。
// AES 不可约多项式为 x^8+x^4+x^3+x+1（0x11B）；
// 当最高位为 1 时移出，需异或 0x1B 实现模约减，保证结果仍在 GF(2^8) 内。
static inline uint8_t xtime(uint8_t x) {
    return uint8_t((x << 1) ^ (x & 0x80 ? 0x1b : 0x00));
}

// SubWord：对 32-bit word 的 4 个字节分别查 S-Box。
// KeyExpansion 中与 RotWord 组合形成 g() 函数，为密钥扩展引入非线性。
static inline uint32_t sub_word(uint32_t w) {
    return (uint32_t(kSbox[(w >> 24) & 0xFF]) << 24) |
           (uint32_t(kSbox[(w >> 16) & 0xFF]) << 16) |
           (uint32_t(kSbox[(w >>  8) & 0xFF]) <<  8) |
           (uint32_t(kSbox[ w        & 0xFF]));
}

// RotWord：word 左循环移 1 字节 [b0,b1,b2,b3] → [b1,b2,b3,b0]。
// KeyExpansion 中用于打乱字节位置，与 SubWord 组合增强扩散性。
static inline uint32_t rot_word(uint32_t w) {
    return (w << 8) | (w >> 24);
}

// ============================================================================
// AES-128 KeyExpansion
// ============================================================================

// 将 16 字节密钥扩展为 44 个 word（11 轮轮密钥，共 176 字节）。
// 算法见 FIPS-197 §5.2（Nk=4, Nr=10, 共 4*(Nr+1)=44 words）：
//   - 前 4 个 word 直接从密钥大端加载
//   - i % 4 == 0: word[i] = word[i-4] ⊕ SubWord(RotWord(word[i-1])) ⊕ Rcon[i/4]
//   - 否则:       word[i] = word[i-4] ⊕ word[i-1]
static void aes128_key_expansion(const uint8_t key[16], uint32_t w[44]) {
    // 前 4 个 word：直接从密钥字节大端加载
    for (int i = 0; i < 4; ++i) {
        w[i] = load_be32(key + 4 * i);
    }
    // 递推生成后续 40 个 word
    for (int i = 4; i < 44; ++i) {
        uint32_t temp = w[i - 1];
        if (i % 4 == 0) {
            // g(temp) = SubWord(RotWord(temp)) ⊕ Rcon
            // Rcon 仅影响最高字节（左移 24 位对齐到 word 的第 0 字节）
            temp = sub_word(rot_word(temp)) ^ (uint32_t(kRcon[i / 4 - 1]) << 24);
        }
        w[i] = w[i - 4] ^ temp;
    }
}

// ============================================================================
// AesCtr 构造 / 析构
// ============================================================================

AesCtr::AesCtr() {
    // 清零所有敏感状态，防止未初始化内存被误用或泄露前一次会话的残留
    std::memset(round_keys_, 0, sizeof(round_keys_));
    std::memset(counter_,     0, sizeof(counter_));
    std::memset(iv_,          0, sizeof(iv_));
    std::memset(keystream_,   0, sizeof(keystream_));
    // pos = 16 表示 keystream 尚未生成，首次 process 时自动触发生成
    keystream_pos_ = 16;
    ready_ = false;
}

AesCtr::~AesCtr() {
    // 析构时清零密钥材料，防止攻击者从进程内存 dump 中恢复密钥
    std::memset(round_keys_, 0, sizeof(round_keys_));
    std::memset(counter_,     0, sizeof(counter_));
    std::memset(iv_,          0, sizeof(iv_));
    std::memset(keystream_,   0, sizeof(keystream_));
}

// ============================================================================
// set_key / reset_counter / increment_counter
// ============================================================================

bool AesCtr::set_key(const uint8_t key[16], const uint8_t iv[16]) {
    if (key == nullptr || iv == nullptr) {
        return false;
    }
    // 先扩展轮密钥——AES-128 的 16 字节密钥总能成功扩展
    aes128_key_expansion(key, round_keys_);
    // 保存初始 IV，用于后续 reset_counter 恢复计数器
    std::memcpy(iv_, iv, 16);
    ready_ = true;
    // 重置计数器到初始 IV，并标记 keystream 需要重新生成
    reset_counter();
    return true;
}

void AesCtr::reset_counter() {
    // 将计数器恢复为初始 IV，用于重新解密某段数据或 flush 中间状态。
    // AirPlay 中 RTP 包可能乱序到达，需要按序号重置计数器重新解密。
    std::memcpy(counter_, iv_, 16);
    // pos = 16 表示当前 keystream 已 "用完"，下次 process 时会自动重新生成
    keystream_pos_ = 16;
}

void AesCtr::increment_counter() {
    // 大端 128 位整数 +1：从最低字节（counter_[15]）向前进位。
    // 这与 NIST SP 800-38A CTR 模式的标准计数器递增方式一致。
    // AirPlay 使用整个 128 位 IV 作为计数器初始值，递增覆盖全部 16 字节。
    for (int i = 15; i >= 0; --i) {
        if (++counter_[i] != 0) {
            break;  // 无进位，停止递增
        }
        // 该字节溢出为 0，继续向更高字节进位
    }
}

// ============================================================================
// AES-128 单块加密
// ============================================================================

// 对 16 字节输入执行 AES-128 加密，输出 16 字节。
// CTR 模式中此函数用于加密计数器值以生成 keystream。
//
// AES state 以列优先排列（FIPS-197 §3.4）：
//   state[0..3]   = 第 0 列 = in[0..3]
//   state[4..7]   = 第 1 列 = in[4..7]
//   state[8..11]  = 第 2 列 = in[8..11]
//   state[12..15] = 第 3 列 = in[12..15]
// 即 state[r + 4*c] 对应第 r 行第 c 列。
void AesCtr::aes128_encrypt_block(const uint8_t in[16], uint8_t out[16]) {
    uint8_t state[16];
    std::memcpy(state, in, 16);

    // --- 初始轮（round 0）：仅 AddRoundKey ---
    // round 0 的轮密钥是 w[0..3]，逐列异或到 state
    for (int c = 0; c < 4; ++c) {
        uint32_t rk = round_keys_[c];
        state[4 * c + 0] ^= uint8_t(rk >> 24);
        state[4 * c + 1] ^= uint8_t(rk >> 16);
        state[4 * c + 2] ^= uint8_t(rk >>  8);
        state[4 * c + 3] ^= uint8_t(rk);
    }

    // --- 第 1 ~ 9 轮：SubBytes → ShiftRows → MixColumns → AddRoundKey ---
    for (int round = 1; round <= 9; ++round) {
        // SubBytes：逐字节 S-Box 替换，引入非线性
        for (int i = 0; i < 16; ++i) {
            state[i] = kSbox[state[i]];
        }

        // ShiftRows：行 0 不移，行 1 左移 1，行 2 左移 2，行 3 左移 3。
        // 通过行间位移实现字节扩散，使单字节变化影响多列。
        uint8_t tmp;
        // 行 1 左移 1: [s1,s5,s9,s13] → [s5,s9,s13,s1]
        tmp         = state[1];
        state[1]    = state[5];
        state[5]    = state[9];
        state[9]    = state[13];
        state[13]   = tmp;
        // 行 2 左移 2: [s2,s6,s10,s14] → [s10,s14,s2,s6]
        tmp         = state[2];
        state[2]    = state[10];
        state[10]   = tmp;
        tmp         = state[6];
        state[6]    = state[14];
        state[14]   = tmp;
        // 行 3 左移 3 (= 右移 1): [s3,s7,s11,s15] → [s15,s3,s7,s11]
        tmp         = state[15];
        state[15]   = state[11];
        state[11]   = state[7];
        state[7]    = state[3];
        state[3]    = tmp;

        // MixColumns：对每列做 GF(2^8) 矩阵乘法（FIPS-197 §5.1.3）
        //   | 2 3 1 1 |
        //   | 1 2 3 1 |
        //   | 1 1 2 3 |
        //   | 3 1 1 2 |
        // 其中 3·a = xtime(a) ⊕ a（即 2·a ⊕ a），实现列内字节扩散。
        for (int c = 0; c < 4; ++c) {
            uint8_t a0 = state[4 * c + 0];
            uint8_t a1 = state[4 * c + 1];
            uint8_t a2 = state[4 * c + 2];
            uint8_t a3 = state[4 * c + 3];

            // 2·a0 ⊕ 3·a1 ⊕ a2 ⊕ a3
            state[4 * c + 0] = xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3;
            // a0 ⊕ 2·a1 ⊕ 3·a2 ⊕ a3
            state[4 * c + 1] = a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3;
            // a0 ⊕ a1 ⊕ 2·a2 ⊕ 3·a3
            state[4 * c + 2] = a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3);
            // 3·a0 ⊕ a1 ⊕ a2 ⊕ 2·a3
            state[4 * c + 3] = (xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3);
        }

        // AddRoundKey：将 round 轮密钥异或到 state
        for (int c = 0; c < 4; ++c) {
            uint32_t rk = round_keys_[4 * round + c];
            state[4 * c + 0] ^= uint8_t(rk >> 24);
            state[4 * c + 1] ^= uint8_t(rk >> 16);
            state[4 * c + 2] ^= uint8_t(rk >>  8);
            state[4 * c + 3] ^= uint8_t(rk);
        }
    }

    // --- 最终轮（round 10）：SubBytes → ShiftRows → AddRoundKey ---
    // 最终轮省略 MixColumns（FIPS-197 设计要求，不影响安全性）
    for (int i = 0; i < 16; ++i) {
        state[i] = kSbox[state[i]];
    }

    // ShiftRows（同主循环）
    uint8_t tmp;
    tmp       = state[1];
    state[1]  = state[5];
    state[5]  = state[9];
    state[9]  = state[13];
    state[13] = tmp;
    tmp       = state[2];
    state[2]  = state[10];
    state[10] = tmp;
    tmp       = state[6];
    state[6]  = state[14];
    state[14] = tmp;
    tmp       = state[15];
    state[15] = state[11];
    state[11] = state[7];
    state[7]  = state[3];
    state[3]  = tmp;

    // AddRoundKey（最终轮密钥 w[40..43]）
    for (int c = 0; c < 4; ++c) {
        uint32_t rk = round_keys_[40 + c];
        state[4 * c + 0] ^= uint8_t(rk >> 24);
        state[4 * c + 1] ^= uint8_t(rk >> 16);
        state[4 * c + 2] ^= uint8_t(rk >>  8);
        state[4 * c + 3] ^= uint8_t(rk);
    }

    std::memcpy(out, state, 16);
}

// ============================================================================
// CTR 模式处理
// ============================================================================

void AesCtr::process(const uint8_t* in, uint8_t* out, size_t len) {
    if (!ready_ || in == nullptr || out == nullptr) {
        return;
    }

    size_t i = 0;
    while (i < len) {
        // keystream 用完时重新生成：encrypt(counter) → keystream，然后 counter +1。
        // 初始状态 keystream_pos_ = 16，因此首次调用会立即生成第一块 keystream。
        if (keystream_pos_ >= 16) {
            aes128_encrypt_block(counter_, keystream_);
            increment_counter();
            keystream_pos_ = 0;
        }
        // CTR 核心操作：plaintext = ciphertext XOR keystream
        // 加解密对称，同一函数处理两个方向。
        // 逐字节 XOR 而非整块处理，是为了支持非 16 字节对齐的输入。
        out[i] = in[i] ^ keystream_[keystream_pos_];
        ++keystream_pos_;
        ++i;
    }
}

// ============================================================================
// hex 工具函数
// ============================================================================

// 将单个 hex 字符转换为数值 [0,15]，非法字符返回 -1。
static inline int hex_char_to_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hex_to_bytes(const std::string& hex, uint8_t* out, size_t* out_len) {
    if (out == nullptr || out_len == nullptr) {
        return false;
    }

    // 第一遍：收集所有有效 hex 数值，跳过空格和冒号。
    // AirPlay SDP 中 aesiv 可能以 "aa:bb:cc..." 或 "aa bb cc..." 格式传递，
    // 需要容忍这些分隔符。
    std::vector<int> digits;
    digits.reserve(hex.size());
    for (char c : hex) {
        if (c == ' ' || c == ':') continue;
        int v = hex_char_to_val(c);
        if (v < 0) {
            return false;  // 非法字符
        }
        digits.push_back(v);
    }

    // hex 字符数必须是偶数（每 2 个字符组成 1 字节）
    if (digits.size() % 2 != 0) {
        return false;
    }

    size_t byte_count = digits.size() / 2;
    if (byte_count > *out_len) {
        return false;  // 输出缓冲区不够
    }

    // 第二遍：每两个 hex 数值组合为一个字节
    for (size_t i = 0; i < byte_count; ++i) {
        out[i] = uint8_t((digits[2 * i] << 4) | digits[2 * i + 1]);
    }

    *out_len = byte_count;
    return true;
}

std::vector<uint8_t> hex_to_vector(const std::string& hex) {
    // 预分配最大可能长度（hex.size()/2 向上取整 +1 保证足够），
    // 解析完成后 resize 到实际长度。
    std::vector<uint8_t> result;
    result.resize(hex.size() / 2 + 1);
    size_t actual = result.size();
    if (!hex_to_bytes(hex, result.data(), &actual)) {
        return {};  // 解析失败返回空 vector
    }
    result.resize(actual);
    return result;
}

} // namespace crypto
} // namespace airplay2
