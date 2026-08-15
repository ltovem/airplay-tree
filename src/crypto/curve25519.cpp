/*!
 * @file curve25519.cpp
 * @brief X25519 ECDH + Ed25519 签名/验签
 *
 * 本文件移植自 TweetNaCl（tweetnacl.c，20140427 版本，
 * 作者 Daniel J. Bernstein / Peter Schwabe / Tanja Lange，
 * 公有领域 / CC0），并保持本库既有接口不变：
 *
 *   - x25519_keygen / x25519_shared   （RFC 7748）
 *   - ed25519_keygen / ed25519_sign / ed25519_verify （RFC 8032）
 *
 * TweetNaCl 的实现已在无数项目中被交叉验证（OpenSSH、WireGuard 等生态），
 * 相比手写曲线代码，安全性/正确性更有保障。SHA-512 直接复用本库
 * sha512.h 中已通过 RFC 向量测试的实现，不重复造轮子。
 *
 * 平台注意：全部使用标准 C++ 无符号类型，无 __int128 依赖，
 * 可在 Windows / macOS / Linux / iOS / Android 全平台编译。
 */
#include "curve25519.h"
#include "sha512.h"
#include <cstring>

namespace airplay2 {
namespace crypto {

namespace {

/* ===================== 类型别名（沿用 TweetNaCl 命名） ===================== */
using u8  = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;
using gf  = i64[16];

#define FOR(i, n) for (int (i) = 0; (i) < (n); ++(i))
#define sv static void

static const u8 _9[32] = {9};
static const gf gf0_v = {0};
static const gf gf1 = {1};
static const gf _121665 = {0xDB41, 1};
static const gf D = {
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203
};
static const gf D2 = {
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406
};
static const gf X = {
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169
};
static const gf Y = {
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666
};
static const gf I = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83
};

/* ===================== 常量时间比较 ===================== */
static int vn(const u8* x, const u8* y, int n) {
    u32 i, d = 0;
    FOR(i, n) d |= x[i] ^ y[i];
    return (1 & ((d - 1) >> 8)) - 1;
}
int crypto_verify_32(const u8* x, const u8* y) {
    return vn(x, y, 32);
}

/* ===================== 域元素基础运算 ===================== */
sv set25519(gf r, const gf a) {
    FOR(i, 16) r[i] = a[i];
}

sv car25519(gf o) {
    int i;
    i64 c;
    FOR(i, 16) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        // c 可能为负，负值 << 是未定义行为（UBSan: left shift of negative）；
        // 改用乘法（此处 c 量级很小，不会溢出）
        o[i] -= c * (1LL << 16);
    }
}

sv sel25519(gf p, gf q, int b) {
    i64 t, i, c = ~(b - 1);
    FOR(i, 16) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

sv pack25519(u8* o, const gf n) {
    int i, j, b;
    gf m, t;
    FOR(i, 16) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    FOR(j, 2) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    FOR(i, 16) {
        o[2 * i] = (u8)(t[i] & 0xff);
        o[2 * i + 1] = (u8)(t[i] >> 8);
    }
}

static int neq25519(const gf a, const gf b) {
    u8 c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    return crypto_verify_32(c, d);
}

static u8 par25519(const gf a) {
    u8 d[32];
    pack25519(d, a);
    return d[0] & 1;
}

sv unpack25519(gf o, const u8* n) {
    int i;
    FOR(i, 16) o[i] = n[2 * i] + ((i64)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

sv A(gf o, const gf a, const gf b) {
    FOR(i, 16) o[i] = a[i] + b[i];
}
sv Z(gf o, const gf a, const gf b) {
    FOR(i, 16) o[i] = a[i] - b[i];
}
sv M(gf o, const gf a, const gf b) {
    i64 i, j, t[31];
    FOR(i, 31) t[i] = 0;
    FOR(i, 16) FOR(j, 16) t[i + j] += a[i] * b[j];
    FOR(i, 15) t[i] += 38 * t[i + 16];
    FOR(i, 16) o[i] = t[i];
    car25519(o);
    car25519(o);
}
sv S(gf o, const gf a) {
    M(o, a, a);
}

sv inv25519(gf o, const gf i) {
    gf c;
    int a;
    FOR(a, 16) c[a] = i[a];
    for (a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    FOR(a, 16) o[a] = c[a];
}

sv pow2523(gf o, const gf i) {
    gf c;
    int a;
    FOR(a, 16) c[a] = i[a];
    for (a = 250; a >= 0; a--) {
        S(c, c);
        if (a != 1) M(c, c, i);
    }
    FOR(a, 16) o[a] = c[a];
}

/* ===================== X25519 ===================== */
int crypto_scalarmult(u8* q, const u8* n, const u8* p) {
    u8 z[32];
    i64 x[80], r, i;
    gf a, b, c, d, e, f;
    FOR(i, 31) z[i] = n[i];
    z[31] = (n[31] & 127) | 64;
    z[0] &= 248;
    unpack25519(x, p);
    FOR(i, 16) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (i = 254; i >= 0; --i) {
        r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, (int)r);
        sel25519(c, d, (int)r);
        A(e, a, c);
        Z(a, a, c);
        A(c, b, d);
        Z(b, b, d);
        S(d, e);
        S(f, a);
        M(a, c, a);
        M(c, b, e);
        A(e, a, c);
        Z(a, a, c);
        S(b, a);
        Z(c, d, f);
        M(a, c, _121665);
        A(a, a, d);
        M(c, c, a);
        M(a, d, f);
        M(d, b, x);
        S(b, e);
        sel25519(a, b, (int)r);
        sel25519(c, d, (int)r);
    }
    FOR(i, 16) {
        x[i + 16] = a[i];
        x[i + 32] = c[i];
        x[i + 48] = b[i];
        x[i + 64] = d[i];
    }
    inv25519(x + 32, x + 32);
    M(x + 16, x + 16, x + 32);
    pack25519(q, x + 16);
    return 0;
}

int crypto_scalarmult_base(u8* q, const u8* n) {
    return crypto_scalarmult(q, n, _9);
}

/* ===================== Ed25519 群运算 ===================== */
sv add(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]);
    Z(t, q[1], q[0]);
    M(a, a, t);
    A(b, p[0], p[1]);
    A(t, q[0], q[1]);
    M(b, b, t);
    M(c, p[3], q[3]);
    M(c, c, D2);
    M(d, p[2], q[2]);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);
    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

sv cswap(gf p[4], gf q[4], u8 b) {
    int i;
    FOR(i, 4)
        sel25519(p[i], q[i], (int)b);
}

sv pack(u8* r, gf p[4]) {
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= par25519(tx) << 7;
}

sv scalarmult(gf p[4], gf q[4], const u8* s) {
    int i;
    set25519(p[0], gf0_v);
    set25519(p[1], gf1);
    set25519(p[2], gf1);
    set25519(p[3], gf0_v);
    for (i = 255; i >= 0; --i) {
        u8 b = (u8)((s[i / 8] >> (i & 7)) & 1);
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}

sv scalarbase(gf p[4], const u8* s) {
    gf q[4];
    set25519(q[0], X);
    set25519(q[1], Y);
    set25519(q[2], gf1);
    M(q[3], X, Y);
    scalarmult(p, q, s);
}

static const u64 L_arr[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0x10
};

sv modL(u8* r, i64 x[64]) {
    i64 carry, i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * (i64)L_arr[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    FOR(j, 32) {
        x[j] += carry - (x[31] >> 4) * (i64)L_arr[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    FOR(j, 32) x[j] -= carry * (i64)L_arr[j];
    FOR(i, 32) {
        x[i + 1] += x[i] >> 8;
        r[i] = (u8)(x[i] & 255);
    }
}

sv reduce(u8* r) {
    i64 x[64], i;
    FOR(i, 64) x[i] = (i64)(u64)r[i];
    FOR(i, 64) r[i] = 0;
    modL(r, x);
}

static int unpackneg(gf r[4], const u8 p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], gf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, D);
    Z(num, num, r[2]);
    A(den, r[2], den);
    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t, den6, num);
    M(t, t, den);
    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(t, t, den);
    M(r[0], t, den);
    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) M(r[0], r[0], I);
    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) return -1;
    if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], gf0_v, r[0]);
    M(r[3], r[0], r[1]);
    return 0;
}

} // namespace

/* ====================================================================
 *                          对外接口
 * ==================================================================== */

void x25519_keygen(const uint8_t seed[kX25519KeySize],
                   uint8_t sk_out[kX25519KeySize],
                   uint8_t pk_out[kX25519KeySize]) {
    std::memcpy(sk_out, seed, 32);
    // RFC 7748 clamp
    sk_out[0] &= 0xF8;
    sk_out[31] &= 0x7F;
    sk_out[31] |= 0x40;
    crypto_scalarmult_base(pk_out, sk_out);
}

bool x25519_shared(const uint8_t sk[kX25519KeySize],
                   const uint8_t their_pk[kX25519KeySize],
                   uint8_t shared_out[kX25519KeySize]) {
    uint8_t bpk[32];
    std::memcpy(bpk, their_pk, 32);
    bpk[31] &= 0x7F; // 清 bit255
    crypto_scalarmult(shared_out, sk, bpk);
    // 弱密钥检测：共享结果全零则失败
    int allzero = 1;
    for (int i = 0; i < 32; ++i) if (shared_out[i] != 0) { allzero = 0; break; }
    return allzero == 0;
}

void ed25519_keygen(const uint8_t seed[kEd25519KeySize],
                    uint8_t sk_out[kEd25519KeySize],
                    uint8_t pk_out[kEd25519KeySize],
                    uint8_t prefix_out[kEd25519KeySize]) {
    uint8_t d[64];
    Sha512::hash(seed, 32, d);
    // 私钥标量 = clamp(SHA512(seed)[0..31])
    std::memcpy(sk_out, d, 32);
    sk_out[0] &= 0xF8;
    sk_out[31] &= 0x7F;
    sk_out[31] |= 0x40;
    // nonce 前缀 = SHA512(seed)[32..63]
    if (prefix_out) std::memcpy(prefix_out, d + 32, 32);
    // 公钥 = sk·B
    gf p[4];
    scalarbase(p, sk_out);
    pack(pk_out, p);
    std::memset(d, 0, sizeof(d));
}

void ed25519_sign(const uint8_t sk[kEd25519KeySize],
                  const uint8_t prefix[kEd25519KeySize],
                  const uint8_t pk[kEd25519KeySize],
                  const uint8_t* msg, size_t msg_len,
                  uint8_t sig_out[kEd25519SigSize]) {
    uint8_t r_hash[64], h_hash[64];
    uint8_t h_scalar[64], r_scalar[64];
    i64 x[64];

    // r = SHA512(prefix || msg) mod L
    {
        std::vector<uint8_t> buf;
        buf.reserve(32 + msg_len);
        buf.insert(buf.end(), prefix, prefix + 32);
        if (msg && msg_len > 0) buf.insert(buf.end(), msg, msg + msg_len);
        Sha512::hash(buf.data(), buf.size(), r_hash);
        std::memcpy(r_scalar, r_hash, 64);
        reduce(r_scalar); // r_scalar 前 32 字节为 r mod L
    }

    // R = r·B
    gf p[4];
    scalarbase(p, r_scalar);
    pack(sig_out, p); // sig[0..31] = R

    // h = SHA512(R || pk || msg) mod L
    {
        std::vector<uint8_t> buf;
        buf.reserve(32 + 32 + msg_len);
        buf.insert(buf.end(), sig_out, sig_out + 32);
        buf.insert(buf.end(), pk, pk + 32);
        if (msg && msg_len > 0) buf.insert(buf.end(), msg, msg + msg_len);
        Sha512::hash(buf.data(), buf.size(), h_hash);
        std::memcpy(h_scalar, h_hash, 64);
        reduce(h_scalar); // h_scalar 前 32 字节为 h mod L
    }

    // S = (r + h·sk) mod L
    FOR(i, 64) x[i] = 0;
    FOR(i, 32) x[i] = (i64)(u64)r_scalar[i];
    FOR(i, 32) FOR(j, 32) x[i + j] += (i64)(u64)h_scalar[i] * (i64)(u64)sk[j];
    modL(sig_out + 32, x);

    std::memset(r_hash, 0, sizeof(r_hash));
    std::memset(h_hash, 0, sizeof(h_hash));
    std::memset(h_scalar, 0, sizeof(h_scalar));
    std::memset(r_scalar, 0, sizeof(r_scalar));
}

bool ed25519_verify(const uint8_t pk[kEd25519KeySize],
                    const uint8_t* msg, size_t msg_len,
                    const uint8_t sig[kEd25519SigSize]) {
    if (!pk || !sig) return false;
    uint8_t h_hash[64], h_scalar[64], t[32];
    gf p[4], q[4];

    // 解压公钥；失败则验签失败
    if (unpackneg(q, pk)) return false;

    // h = SHA512(R || pk || msg) mod L
    {
        std::vector<uint8_t> buf;
        buf.reserve(32 + 32 + msg_len);
        buf.insert(buf.end(), sig, sig + 32);
        buf.insert(buf.end(), pk, pk + 32);
        if (msg && msg_len > 0) buf.insert(buf.end(), msg, msg + msg_len);
        Sha512::hash(buf.data(), buf.size(), h_hash);
        std::memcpy(h_scalar, h_hash, 64);
        reduce(h_scalar);
    }

    // p = h·A + S·B
    scalarmult(p, q, h_scalar);
    scalarbase(q, sig + 32);
    add(p, q);
    pack(t, p);

    // 常数时间比较 R 是否一致
    return crypto_verify_32(sig, t) == 0;
}

/* ====================================================================
 *                      便捷 vector 版本
 * ==================================================================== */
X25519Key x25519_generate(const std::vector<uint8_t>& seed) {
    X25519Key k;
    if (seed.size() != 32) return k;
    k.sk.resize(32);
    k.pk.resize(32);
    x25519_keygen(seed.data(), k.sk.data(), k.pk.data());
    return k;
}

std::vector<uint8_t> x25519_shared(const std::vector<uint8_t>& sk,
                                   const std::vector<uint8_t>& their_pk) {
    std::vector<uint8_t> out;
    if (sk.size() != 32 || their_pk.size() != 32) return out;
    out.resize(32);
    if (!x25519_shared(sk.data(), their_pk.data(), out.data())) return {};
    return out;
}

Ed25519Key ed25519_generate(const std::vector<uint8_t>& seed) {
    Ed25519Key k;
    if (seed.size() != 32) return k;
    k.sk.resize(32);
    k.pk.resize(32);
    k.prefix.resize(32);
    ed25519_keygen(seed.data(), k.sk.data(), k.pk.data(), k.prefix.data());
    return k;
}

std::vector<uint8_t> ed25519_sign(const Ed25519Key& key, const uint8_t* msg, size_t len) {
    std::vector<uint8_t> sig;
    if (key.sk.size() != 32 || key.pk.size() != 32 || key.prefix.size() != 32) return sig;
    sig.resize(64);
    ed25519_sign(key.sk.data(), key.prefix.data(), key.pk.data(), msg, len, sig.data());
    return sig;
}

bool ed25519_verify(const std::vector<uint8_t>& pk, const uint8_t* msg, size_t len,
                    const uint8_t sig[kEd25519SigSize]) {
    if (pk.size() != 32) return false;
    return ed25519_verify(pk.data(), msg, len, sig);
}

} // namespace crypto
} // namespace airplay2
