/*!
 * @file airplay_pairing.h
 * @brief AirPlay legacy 配对（Ed25519）+ PIN 回退
 *
 * 协议背景：
 *   现代 iOS/macOS（AirPlay 2，非 HomeKit 设备）对接收器执行 "legacy
 *   pairing"，这是 Ed25519 签名的两阶段握手（字节格式与 UxPlay 一致）：
 *
 *   pair-setup（一次）：
 *     M1: client → server   32 字节 client Ed25519 公钥
 *     M2: server → client   32 字节 server Ed25519 公钥
 *
 *   pair-verify（每条连接重新做）：
 *     M1: client → server   [0x01][0,0,0] + client_X25519(32) + client_Ed25519(32) = 68B
 *     M2: server → client   server_X25519(32) + AES-CTR(Ed25519 签名)(64) = 96B
 *     M2: client → server   [0x00][0,0,0] + AES-CTR(签名)(64) = 68B
 *     server 解密验签后回空 200
 *   握手状态必须按"连接"记（UxPlay 同款）：iOS 每条新连接都会重新做
 *   pair-verify 握手，跨连接复用状态会把新连接的 M1 错当"已完成"。
 *
 *   关键前提：本设备必须持有稳定的 Ed25519 身份，公钥需同时出现在
 *   mDNS TXT 的 pk= 和 /info plist 的 pk=<data>，否则 iOS 反复
 *   /info → /pair-setup → 断连，永远不会进入投屏。
 *
 *   身份持久化：load_or_create_identity(path) 从 32 字节种子文件恢复
 *   密钥对（不存在则生成并写盘），保证重启后公钥不变。
 */
#ifndef AIRPLAY2_AIRPLAY_PAIRING_H
#define AIRPLAY2_AIRPLAY_PAIRING_H

#include <cstdint>
#include <functional>   // std::function（PinCb 回调类型）
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../crypto/curve25519.h"   // crypto::Ed25519Key

namespace airplay2 {

class AirPlayPairing {
public:
    AirPlayPairing() = default;

    /// Set the PIN code clients must enter (4 digits, empty = no auth)
    void set_pin(const std::string& pin) {
        std::lock_guard<std::mutex> lk(mu_);
        pin_ = pin;
    }
    std::string pin() const {
        std::lock_guard<std::mutex> lk(mu_); return pin_;
    }

    /*!
     * @brief 加载（或创建并写盘）持久 Ed25519 身份
     *
     * 文件格式：恰好 32 字节随机种子（可直接 cp 备份）。
     * 若 path 为空，则只生成内存中的临时身份（每次进程启动都换新，不推荐）。
     *
     * @param path 种子文件路径；文件不存在时自动创建目录与文件
     */
    void load_or_create_identity(const std::string& path);

    /// 是否已具备 Ed25519 身份（pk 可供 /info 与 mDNS 使用）
    bool identity_ready() const {
        std::lock_guard<std::mutex> lk(mu_);
        return have_identity_ && identity_.pk.size() == crypto::kEd25519KeySize;
    }

    /// 32 字节原始 Ed25519 公钥（空 vector 表示无身份）
    std::vector<uint8_t> public_key() const {
        std::lock_guard<std::mutex> lk(mu_);
        return identity_.pk;
    }

    /// base64(RFC4648) 公钥，直接用于 mDNS TXT 与 /info plist 的 <data>
    std::string public_key_b64() const;

    /// Is this client IP currently paired / whitelisted?
    bool is_paired(const std::string& client_ip) const;

    /// Mark a client as paired (after successful PIN verification)
    void mark_paired(const std::string& client_ip);

    /// Revoke a client pairing (e.g., reset button)
    void unpair(const std::string& client_ip);

    /*!
     * @brief 处理 /pair-setup（legacy Ed25519 M1→M2）
     *
     * 无 PIN 且已生成身份时：把 body 前 32 字节视为客户端公钥，返回
     * 本机 32 字节公钥（M2），并把该连接记为"setup 完成"（之后同连接的
     * pair-verify 直接空 200）。
     * 设了 PIN：维持旧的占位 PIN 流程（返回非空表示"需要 PIN"）。
     *
     * @param conn_id    连接 ID（配对握手状态按连接记，UxPlay 同款语义：
     *                   每条新连接都要重新做 pair-verify 握手，不能跨连接复用）
     * @param client_ip  客户端 IP（配对白名单按 IP 记）
     * @param body       请求体（legacy M1 应为 32 字节公钥）
     * @param len        body 长度
     * @param needs_pin_response 输出：true 表示要客户端输入 PIN
     * @return 响应体；空 vector 表示"未处理，调用方应走其它配对路径"
     */
    std::vector<uint8_t> handle_pair_setup(uint64_t conn_id, const std::string& client_ip,
                                           const uint8_t* body, size_t len,
                                           bool& needs_pin_response);
    /*!
     * @brief 处理 /pair-verify（legacy ECDH+Ed25519 握手）
     *
     * 无 PIN 时的完整字节流（UxPlay/shairplay 同款）：
     *   M1: client → server   [0x01][0,0,0] + client_X25519(32) + client_Ed25519(32) = 68B
     *   M2: server → client   server_X25519(32) + AES-CTR(Ed25519 签名)(64) = 96B
     *   签名内容：server_ecdh_pk || client_ecdh_pk（Ed25519 长期私钥签）
     *   M2: client → server   [0x00][0,0,0] + AES-CTR(签名)(64) = 68B
     *   server 解密验签，msg = client_ecdh_pk || server_ecdh_pk（注意顺序相反）
     *   AES-CTR 密钥：SHA512("Pair-Verify-AES-Key"||ecdh_secret) 前 16B，
     *   IV：SHA512("Pair-Verify-AES-IV"||ecdh_secret) 前 16B；
     *   解客户端签名前要先消耗一档 keystream（fake round），因为双方同一
     *   密钥流：我们响应 M2 已用掉 0..63 字节。
     *
     * 同连接若 pair-setup 已完成（setup_done），直接回空 200（UxPlay no-PIN 行为）。
     *
     * @param conn_id    连接 ID（握手状态按连接记，每条连接独立）
     * @param client_ip  客户端 IP（验签通过后写入配对白名单）
     * @param pin_matched_out 输出：校验是否通过（无 PIN 时视为 true）
     */
    std::vector<uint8_t> handle_pair_verify(uint64_t conn_id, const std::string& client_ip,
                                            const uint8_t* body, size_t len,
                                            bool& pin_matched_out);

    /// 连接销毁时清理其配对握手状态（防止 map 无限增长）
    void clear_verify_state(uint64_t conn_id);

    /// 取某连接的 X25519 共享密钥（pair-verify 成功后才有，32 字节）。
    /// AirPlay 2 音频密钥 = SHA256(fairplay_aeskey || ecdh_secret) 前 16B，
    /// 视频密钥派生也依赖它；无状态时返回空 vector。
    std::vector<uint8_t> ecdh_secret(uint64_t conn_id) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = verify_states_.find(conn_id);
        if (it == verify_states_.end()) return {};
        return it->second.ecdh_secret;
    }

    /// PIN request callback: return true to accept this client/pin combo
    using PinCb = std::function<bool(const std::string& client_ip, const std::string& pin)>;
    void set_pin_callback(PinCb cb) {
        std::lock_guard<std::mutex> lk(mu_);
        pin_cb_ = std::move(cb);
    }

private:
    mutable std::mutex mu_;
    std::string pin_;
    PinCb pin_cb_;
    std::map<std::string, bool> paired_ips_;

    // 长期 Ed25519 身份（sk/pk/prefix 由 ed25519_keygen 生成）
    bool have_identity_ = false;
    crypto::Ed25519Key identity_;

    /*!
     * @brief pair-verify 的每连接握手状态（无 PIN 时用）
     *
     * 状态机：stage=0 未开始 → M1 握手 → stage=1 → M2 验签 → stage=2 完成。
     * setup_done 表示该连接已完成 pair-setup（UxPlay 语义：之后 pair-verify
     * 直接空 200）。
     */
    struct VerifyState {
        std::vector<uint8_t> ecdh_ours;    ///< 本端临时 X25519 公钥(32)
        std::vector<uint8_t> ecdh_theirs;  ///< 对端临时 X25519 公钥(32)
        std::vector<uint8_t> ed_theirs;    ///< 对端长期 Ed25519 公钥(32)
        std::vector<uint8_t> ecdh_secret;  ///< X25519 共享密钥(32)
        int stage = 0;                     ///< 0=无 1=已握手 2=已验签
        bool setup_done = false;           ///< pair-setup 是否已完成
    };
    // 按连接 ID 记状态：iOS 每条连接都重新做 pair-verify 握手，跨连接复用
    // 会把新连接的 M1 错当成"已完成"而回空 200，导致 iOS 不进入 ANNOUNCE。
    std::map<uint64_t, VerifyState> verify_states_;
};

} // namespace airplay2

#endif // AIRPLAY2_AIRPLAY_PAIRING_H
