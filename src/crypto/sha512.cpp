/*!
 * @file sha512.cpp
 * @brief SHA-512 / HMAC / HKDF / PBKDF2-HMAC-SHA512 实现
 *
 * 算法严格按 FIPS 180-4 和 RFC 5869 / RFC 8018 实现，
 * 用常量数组直接展开 80 轮压缩函数，避免查表分支。
 */
#include "sha512.h"
#include <cstring>

namespace airplay2 {
namespace crypto {

/* ---------- SHA-512 常量（FIPS 180-4 4.2.3）---------- */
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL,
};

static inline uint64_t rotr64(uint64_t x, unsigned n) {
    return (x >> n) | (x << (64 - n));
}

static inline uint64_t ch64(uint64_t x, uint64_t y, uint64_t z) {
    return (x & y) ^ (~x & z);
}
static inline uint64_t maj64(uint64_t x, uint64_t y, uint64_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}
static inline uint64_t sig0a(uint64_t x) { return rotr64(x, 28) ^ rotr64(x, 34) ^ rotr64(x, 39); }
static inline uint64_t sig1a(uint64_t x) { return rotr64(x, 14) ^ rotr64(x, 18) ^ rotr64(x, 41); }
static inline uint64_t sig0b(uint64_t x) { return rotr64(x,  1) ^ rotr64(x,  8) ^ (x >> 7);  }
static inline uint64_t sig1b(uint64_t x) { return rotr64(x, 19) ^ rotr64(x, 61) ^ (x >> 6);  }

Sha512::Sha512() { reset_state:
    state_[0] = 0x6a09e667f3bcc908ULL; state_[1] = 0xbb67ae8584caa73bULL;
    state_[2] = 0x3c6ef372fe94f82bULL; state_[3] = 0xa54ff53a5f1d36f1ULL;
    state_[4] = 0x510e527fade682d1ULL; state_[5] = 0x9b05688c2b3e6c1fULL;
    state_[6] = 0x1f83d9abfb41bd6bULL; state_[7] = 0x5be0cd19137e2179ULL;
    bitlen_hi_ = bitlen_lo_ = 0;
    buflen_ = 0;
}

void Sha512::compress(const uint8_t* block) {
    uint64_t W[80];
    for (int i = 0; i < 16; ++i) {
        W[i] = (uint64_t(block[i*8  ]) << 56) | (uint64_t(block[i*8+1]) << 48) |
               (uint64_t(block[i*8+2]) << 40) | (uint64_t(block[i*8+3]) << 32) |
               (uint64_t(block[i*8+4]) << 24) | (uint64_t(block[i*8+5]) << 16) |
               (uint64_t(block[i*8+6]) <<  8) | (uint64_t(block[i*8+7])      );
    }
    for (int i = 16; i < 80; ++i) {
        W[i] = sig1b(W[i-2]) + W[i-7] + sig0b(W[i-15]) + W[i-16];
    }
    uint64_t a=state_[0], b=state_[1], c=state_[2], d=state_[3];
    uint64_t e=state_[4], f=state_[5], g=state_[6], h=state_[7];
    for (int i = 0; i < 80; ++i) {
        uint64_t T1 = h + sig1a(e) + ch64(e,f,g) + K512[i] + W[i];
        uint64_t T2 = sig0a(a) + maj64(a,b,c);
        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha512::update(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    // 位长度更新（总 bits = len * 8，128 位）
    uint64_t add_lo = (uint64_t)len << 3;
    uint64_t add_hi = (uint64_t)len >> 61;
    bitlen_lo_ += add_lo;
    if (bitlen_lo_ < add_lo) bitlen_hi_++;
    bitlen_hi_ += add_hi;

    while (len > 0) {
        size_t space = sizeof(buffer_) - buflen_;
        size_t take  = (len < space) ? len : space;
        std::memcpy(buffer_ + buflen_, data, take);
        buflen_ += take;
        data    += take;
        len     -= take;
        if (buflen_ == sizeof(buffer_)) {
            compress(buffer_);
            buflen_ = 0;
        }
    }
}

void Sha512::final(uint8_t out[kSha512Size]) {
    // 填充：0x80 + 若干 0x00 + 16 字节 big-endian bitlen
    buffer_[buflen_++] = 0x80;
    if (buflen_ > 112) {
        while (buflen_ < 128) buffer_[buflen_++] = 0x00;
        compress(buffer_);
        buflen_ = 0;
    }
    while (buflen_ < 112) buffer_[buflen_++] = 0x00;
    // 写入 128 位 big-endian bitlen
    for (int i = 7; i >= 0; --i) buffer_[buflen_++] = (uint8_t)(bitlen_hi_ >> (i*8));
    for (int i = 7; i >= 0; --i) buffer_[buflen_++] = (uint8_t)(bitlen_lo_ >> (i*8));
    compress(buffer_);

    for (int i = 0; i < 8; ++i) {
        out[i*8  ] = (uint8_t)(state_[i] >> 56);
        out[i*8+1] = (uint8_t)(state_[i] >> 48);
        out[i*8+2] = (uint8_t)(state_[i] >> 40);
        out[i*8+3] = (uint8_t)(state_[i] >> 32);
        out[i*8+4] = (uint8_t)(state_[i] >> 24);
        out[i*8+5] = (uint8_t)(state_[i] >> 16);
        out[i*8+6] = (uint8_t)(state_[i] >>  8);
        out[i*8+7] = (uint8_t)(state_[i]      );
    }
    // 复用友好：重置内部状态
    state_[0] = 0x6a09e667f3bcc908ULL; state_[1] = 0xbb67ae8584caa73bULL;
    state_[2] = 0x3c6ef372fe94f82bULL; state_[3] = 0xa54ff53a5f1d36f1ULL;
    state_[4] = 0x510e527fade682d1ULL; state_[5] = 0x9b05688c2b3e6c1fULL;
    state_[6] = 0x1f83d9abfb41bd6bULL; state_[7] = 0x5be0cd19137e2179ULL;
    bitlen_hi_ = bitlen_lo_ = 0;
    buflen_ = 0;
}

void Sha512::hash(const uint8_t* data, size_t len, uint8_t out[kSha512Size]) {
    Sha512 s; s.update(data, len); s.final(out);
}

/* ---------- HMAC ---------- */
void hmac_sha512(const uint8_t* key, size_t key_len,
                 const uint8_t* msg, size_t msg_len,
                 uint8_t out[kSha512Size]) {
    // HMAC(K, m) = H( (K' ^ opad) || H( (K' ^ ipad) || m ) )
    // K' 是 key 归一化到 128 字节：若太长先哈希成 64 字节再补零
    uint8_t kprime[128];
    std::memset(kprime, 0, sizeof(kprime));
    if (key_len > 128) {
        Sha512::hash(key, key_len, kprime); // 写 64，后 64 已是 0
    } else {
        std::memcpy(kprime, key, key_len);
    }
    uint8_t ipad[128], opad[128];
    for (int i = 0; i < 128; ++i) {
        ipad[i] = kprime[i] ^ 0x36;
        opad[i] = kprime[i] ^ 0x5c;
    }
    Sha512 inner; inner.update(ipad, 128); inner.update(msg, msg_len);
    uint8_t inner_hash[kSha512Size]; inner.final(inner_hash);
    Sha512 outer; outer.update(opad, 128); outer.update(inner_hash, kSha512Size);
    outer.final(out);
}

std::vector<uint8_t> hmac_sha512(const std::vector<uint8_t>& key,
                                 const std::vector<uint8_t>& msg) {
    std::vector<uint8_t> out(kSha512Size);
    hmac_sha512(key.empty() ? nullptr : key.data(), key.size(),
                msg.empty() ? nullptr : msg.data(), msg.size(),
                out.data());
    return out;
}

/* ---------- HKDF ---------- */
void hkdf_extract(const uint8_t* salt, size_t salt_len,
                  const uint8_t* ikm,  size_t ikm_len,
                  uint8_t prk_out[kSha512Size]) {
    // RFC 5869: 缺省 salt = kSha512Size 字节零
    uint8_t zero_salt[kSha512Size];
    if (salt == nullptr || salt_len == 0) {
        std::memset(zero_salt, 0, sizeof(zero_salt));
        hmac_sha512(zero_salt, kSha512Size, ikm, ikm_len, prk_out);
    } else {
        hmac_sha512(salt, salt_len, ikm, ikm_len, prk_out);
    }
}

bool hkdf_expand(const uint8_t prk[kSha512Size],
                 const uint8_t* info, size_t info_len,
                 uint8_t* okm_out, size_t okm_len) {
    if (!prk || !okm_out || okm_len == 0) return false;
    size_t N = (okm_len + kSha512Size - 1) / kSha512Size;
    if (N > 255) return false;
    uint8_t T_prev[kSha512Size];
    size_t T_prev_len = 0;
    size_t written = 0;
    for (size_t i = 1; i <= N; ++i) {
        // T(i) = HMAC(PRK, T(i-1) || info || byte(i))
        Sha512 ctx;
        uint8_t salt_ = (uint8_t)i;
        // 构造 HMAC 输入：用一次性 hmac
        // 手动把 T(i-1) + info + i 拼接成临时缓冲
        std::vector<uint8_t> tmp;
        tmp.reserve(T_prev_len + info_len + 1);
        if (T_prev_len) tmp.insert(tmp.end(), T_prev, T_prev + T_prev_len);
        if (info_len) tmp.insert(tmp.end(), info, info + info_len);
        tmp.push_back(salt_);
        uint8_t Ti[kSha512Size];
        hmac_sha512(prk, kSha512Size, tmp.data(), tmp.size(), Ti);
        size_t take = (okm_len - written < kSha512Size) ? (okm_len - written) : kSha512Size;
        std::memcpy(okm_out + written, Ti, take);
        written += take;
        std::memcpy(T_prev, Ti, kSha512Size);
        T_prev_len = kSha512Size;
    }
    return true;
}

std::vector<uint8_t> hkdf_derive(const std::vector<uint8_t>& ikm,
                                 const std::vector<uint8_t>& salt,
                                 const std::vector<uint8_t>& info,
                                 size_t okm_len) {
    uint8_t prk[kSha512Size];
    hkdf_extract(salt.empty() ? nullptr : salt.data(), salt.size(),
                 ikm.empty() ? nullptr : ikm.data(), ikm.size(),
                 prk);
    std::vector<uint8_t> okm(okm_len);
    if (!hkdf_expand(prk, info.empty() ? nullptr : info.data(), info.size(),
                     okm.data(), okm_len)) {
        return {};
    }
    return okm;
}

/* ---------- PBKDF2-HMAC-SHA512 ---------- */
std::vector<uint8_t> pbkdf2_hmac_sha512(const std::vector<uint8_t>& password,
                                        const std::vector<uint8_t>& salt,
                                        uint32_t iterations, size_t dk_len) {
    // DK = T1 || T2 || ... || Tdklen/hlen
    // Ti = F(Password, Salt, c, i) = U1 ^ U2 ^ ... ^ Uc
    // U1 = HMAC(Password, Salt || INT_32_BE(i))
    // Uj = HMAC(Password, Uj-1)
    std::vector<uint8_t> dk(dk_len);
    size_t hlen = kSha512Size;
    size_t blocks = (dk_len + hlen - 1) / hlen;
    const uint8_t* pw = password.empty() ? nullptr : password.data();
    size_t pw_len = password.size();
    for (size_t i = 1; i <= blocks; ++i) {
        // U1
        std::vector<uint8_t> u1_input = salt;
        u1_input.push_back((uint8_t)(i >> 24));
        u1_input.push_back((uint8_t)(i >> 16));
        u1_input.push_back((uint8_t)(i >>  8));
        u1_input.push_back((uint8_t)(i      ));
        uint8_t U[kSha512Size];
        hmac_sha512(pw, pw_len, u1_input.data(), u1_input.size(), U);
        uint8_t T[kSha512Size];
        std::memcpy(T, U, hlen);
        for (uint32_t j = 1; j < iterations; ++j) {
            uint8_t Un[kSha512Size];
            hmac_sha512(pw, pw_len, U, hlen, Un);
            std::memcpy(U, Un, hlen);
            for (size_t k = 0; k < hlen; ++k) T[k] ^= U[k];
        }
        size_t offset = (i - 1) * hlen;
        size_t take = (dk_len - offset < hlen) ? (dk_len - offset) : hlen;
        std::memcpy(dk.data() + offset, T, take);
    }
    return dk;
}

} // namespace crypto
} // namespace airplay2
