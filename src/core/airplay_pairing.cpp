/*!
 * @file airplay_pairing.cpp
 */
#include "airplay_pairing.h"
#include "../platform/platform_log.h"
#include "../platform/platform_time.h"
#include "../util/plist.h"      // util::base64_encode
#include "../crypto/sha512.h"   // Sha512（pair-verify 密钥派生）
#include "../crypto/aes_ctr.h"  // AesCtr（pair-verify 签名加密）
#include <cstring>
#include <fstream>              // std::ifstream/ofstream（身份种子文件读写）
#include <random>
#include <algorithm>            // std::min（种子分块填充）
#include <cerrno>
#if defined(_WIN32)
#include <direct.h>             // ::_mkdir（Windows 创建身份目录）
#else
#include <sys/stat.h>           // ::mkdir（POSIX 创建身份目录）
#endif

namespace airplay2 {

/* ================================================================
 *                Ed25519 身份（持久化）
 * ================================================================ */

void AirPlayPairing::load_or_create_identity(const std::string& path) {
    std::vector<uint8_t> seed(32, 0);
    bool loaded = false;

    if (!path.empty()) {
        // 优先从已有种子文件恢复：保证重启后公钥不变，避免 iOS 反复要求重新配对
        std::ifstream in(path, std::ios::binary);
        if (in) {
            in.read(reinterpret_cast<char*>(seed.data()), (std::streamsize)seed.size());
            if (in.gcount() == (std::streamsize)seed.size()) loaded = true;
        }
    }

    if (!loaded) {
        // 生成新的 32 字节随机种子（std::random_device + 时钟做熵混合）
        std::random_device rd;
        std::mt19937_64 gen(rd() ^ (uint64_t)platform::time_now_us());
        for (size_t i = 0; i < seed.size(); i += 8) {
            uint64_t v = gen();
            std::memcpy(seed.data() + i, &v, std::min<size_t>(8, seed.size() - i));
        }
        if (!path.empty()) {
            // 先确保父目录存在（mkdir -p 语义；失败不致命，写盘只是优化）
            std::string dir = path;
            auto slash = dir.find_last_of('/');
            if (slash != std::string::npos) {
                dir = dir.substr(0, slash);
#if defined(_WIN32)
                ::_mkdir(dir.c_str());
#else
                ::mkdir(dir.c_str(), 0755);
#endif
            }
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (out) {
                out.write(reinterpret_cast<const char*>(seed.data()),
                          (std::streamsize)seed.size());
            } else {
                AP2_LOGW("pairing: cannot persist identity seed to %s", path.c_str());
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        identity_ = crypto::ed25519_generate(seed);
        have_identity_ = true;
    }
    AP2_LOGI("pairing: ed25519 identity %s (pk64=%s)",
             loaded ? "loaded" : "generated",
             public_key_b64().c_str());
}

std::string AirPlayPairing::public_key_b64() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!have_identity_ || identity_.pk.empty()) return "";
    return util::base64_encode(identity_.pk.data(), identity_.pk.size());
}

/* ================================================================
 *                        白名单
 * ================================================================ */

bool AirPlayPairing::is_paired(const std::string& client_ip) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = paired_ips_.find(client_ip);
    return it != paired_ips_.end() && it->second;
}

void AirPlayPairing::mark_paired(const std::string& client_ip) {
    std::lock_guard<std::mutex> lk(mu_);
    paired_ips_[client_ip] = true;
    AP2_LOGI("pairing: marked paired %s", client_ip.c_str());
}

void AirPlayPairing::unpair(const std::string& client_ip) {
    std::lock_guard<std::mutex> lk(mu_);
    paired_ips_.erase(client_ip);
}

/* ================================================================
 *                   legacy 配对（Ed25519）
 * ================================================================ */

// SHA512(salt || ecdh_secret) 前 16 字节 → pair-verify 的 AES 密钥/IV
// （shairplay derive_key_internal 同款，但用我们的 Sha512 实现）
static void derive_verify_key(const uint8_t ecdh_secret[32], const char* salt, uint8_t out[16]) {
    std::string s(salt);
    std::vector<uint8_t> buf(s.begin(), s.end());
    buf.insert(buf.end(), ecdh_secret, ecdh_secret + 32);
    uint8_t hash[64];
    crypto::Sha512::hash(buf.data(), buf.size(), hash);
    std::memcpy(out, hash, 16);
}

void AirPlayPairing::clear_verify_state(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(mu_);
    verify_states_.erase(conn_id);
}

std::vector<uint8_t> AirPlayPairing::handle_pair_setup(
    uint64_t conn_id, const std::string& client_ip,
    const uint8_t* body, size_t len, bool& needs_pin) {
    needs_pin = false;
    std::lock_guard<std::mutex> lk(mu_);

    if (!pin_.empty()) {
        // 有 PIN：旧占位流程，返回"需要 PIN"标记
        needs_pin = true;
        return {0x01, 0x00};
    }

    // legacy M1：客户端发 32 字节 Ed25519 公钥，服务端回自己的 32 字节公钥。
    // 有些客户端带 4 字节头部（如 pair-verify 的 header 风格），只取前 32 字节。
    if (have_identity_ && identity_.pk.size() == crypto::kEd25519KeySize && len >= 32 && body) {
        paired_ips_[client_ip] = true;
        verify_states_[conn_id].setup_done = true;  // UxPlay 语义：setup 完成后同连接 pair-verify 空 200
        AP2_LOGI("pairing: legacy pair-setup M1 from %s (conn %llu, %zuB) -> M2 sent",
                 client_ip.c_str(), (unsigned long long)conn_id, len);
        return identity_.pk;
    }
    // 无法识别：返回空，让上层尝试 FairPlay 路径
    return {};
}

std::vector<uint8_t> AirPlayPairing::handle_pair_verify(
    uint64_t conn_id, const std::string& client_ip,
    const uint8_t* body, size_t len, bool& pin_ok) {
    pin_ok = false;
    std::lock_guard<std::mutex> lk(mu_);

    if (!pin_.empty()) {
        // PIN 流程（旧行为）：从 body 提取 ASCII 数字比较
        std::string sent;
        for (size_t i = 0; i < len; ++i) {
            if (body && body[i] >= '0' && body[i] <= '9') sent.push_back((char)body[i]);
        }
        if (!sent.empty() && sent == pin_) {
            pin_ok = true;
            paired_ips_[client_ip] = true;
        } else if (pin_cb_) {
            if (pin_cb_(client_ip, sent)) {
                pin_ok = true;
                paired_ips_[client_ip] = true;
            }
        }
        if (pin_ok) return {0x00, 0x01}; // ok
        return {0x03, 0x00}; // fail (requires re-pair)
    }

    auto& st = verify_states_[conn_id];
    // 同连接 pair-setup 已完成：UxPlay no-PIN 行为，直接空 200
    if (st.setup_done) {
        pin_ok = true;
        AP2_LOGD("pairing: pair-verify %zuB from %s (conn %llu) -> ok(setup done)",
                 len, client_ip.c_str(), (unsigned long long)conn_id);
        return {};
    }
    if (!body || len < 68 || !have_identity_) {
        pin_ok = true;  // 无法识别也放行（避免老客户端卡死）
        return {};
    }

    if (body[0] == 1 && len >= 4 + 32 + 32) {
        // ---- M1：客户端 ECDH 握手请求 ----
        st.ecdh_theirs.assign(body + 4, body + 4 + 32);
        st.ed_theirs.assign(body + 36, body + 36 + 32);

        // 生成临时 X25519 密钥对并计算共享密钥
        std::vector<uint8_t> seed(32), sk(32), pk(32);
        std::random_device rd;
        std::mt19937_64 gen(rd() ^ (uint64_t)platform::time_now_us());
        for (size_t i = 0; i < 32; i += 8) {
            uint64_t v = gen();
            std::memcpy(seed.data() + i, &v, std::min<size_t>(8, 32 - i));
        }
        crypto::x25519_keygen(seed.data(), sk.data(), pk.data());
        st.ecdh_ours.assign(pk.begin(), pk.end());
        uint8_t shared[32];
        if (!crypto::x25519_shared(sk.data(), st.ecdh_theirs.data(), shared)) {
            AP2_LOGW("pairing: pair-verify M1 weak key from %s", client_ip.c_str());
            return {};
        }
        st.ecdh_secret.assign(shared, shared + 32);
        st.stage = 1;

        // 签名：Ed25519 长期私钥 签 (server_ecdh_pk || client_ecdh_pk)
        std::vector<uint8_t> msg;
        msg.insert(msg.end(), st.ecdh_ours.begin(), st.ecdh_ours.end());
        msg.insert(msg.end(), st.ecdh_theirs.begin(), st.ecdh_theirs.end());
        std::vector<uint8_t> sig = crypto::ed25519_sign(identity_, msg.data(), msg.size());

        // AES-128-CTR 加密签名（key/iv 由 ecdh_secret 派生）
        uint8_t key[16], iv[16];
        derive_verify_key(st.ecdh_secret.data(), "Pair-Verify-AES-Key", key);
        derive_verify_key(st.ecdh_secret.data(), "Pair-Verify-AES-IV", iv);
        crypto::AesCtr ctr;
        ctr.set_key(key, iv);
        ctr.process(sig.data(), sig.data(), sig.size());

        std::vector<uint8_t> resp = st.ecdh_ours;
        resp.insert(resp.end(), sig.begin(), sig.end());
        AP2_LOGI("pairing: pair-verify M1 from %s (conn %llu) -> M2(96B) sent",
                 client_ip.c_str(), (unsigned long long)conn_id);
        return resp;  // 96B：ecdh_pk(32) + 加密签名(64)
    }

    if (body[0] == 0 && st.stage == 1) {
        // ---- M2：验客户端签名 ----
        const uint8_t* client_sig = body + 4;
        uint8_t key[16], iv[16];
        derive_verify_key(st.ecdh_secret.data(), "Pair-Verify-AES-Key", key);
        derive_verify_key(st.ecdh_secret.data(), "Pair-Verify-AES-IV", iv);
        crypto::AesCtr ctr;
        ctr.set_key(key, iv);
        // fake round：双方共享同一密钥流，我们发 M2 已用掉 0..63，先跳过一档
        uint8_t junk[64] = {0};
        ctr.process(junk, junk, sizeof(junk));
        uint8_t dec[64];
        ctr.process(client_sig, dec, sizeof(dec));

        // 验签 msg = (client_ecdh_pk || server_ecdh_pk)（与 M1 相反）
        std::vector<uint8_t> msg;
        msg.insert(msg.end(), st.ecdh_theirs.begin(), st.ecdh_theirs.end());
        msg.insert(msg.end(), st.ecdh_ours.begin(), st.ecdh_ours.end());
        if (crypto::ed25519_verify(st.ed_theirs, msg.data(), msg.size(), dec)) {
            st.stage = 2;
            st.setup_done = true;  // 本连接已完成 verify 全流程，后续 pair-verify 空 200
            paired_ips_[client_ip] = true;
            pin_ok = true;
            AP2_LOGI("pairing: pair-verify M2 signature OK from %s (conn %llu)",
                     client_ip.c_str(), (unsigned long long)conn_id);
            return {};  // 200 空 body 即成功
        }
        AP2_LOGW("pairing: pair-verify M2 signature FAILED from %s", client_ip.c_str());
        return {0x03, 0x00};
    }

    pin_ok = true;  // 其它未知格式兜底放行
    return {};
}

} // namespace airplay2
