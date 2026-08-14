/*!
 * @file airplay_session.h
 * @brief 单个 AirPlay 客户端连接的对外句柄
 *
 * AirPlaySession 的生命周期由 AirPlayServer 管理：
 *   - CONNECTED  ← TCP accept 后
 *   - PAIRING    ← pair-setup / pair-verify 过程中
 *   - SETUP      ← ANNOUNCE 成功、等待 SETUP 分配端口
 *   - READY      ← SETUP 完成、等待 RECORD 开始流
 *   - PLAYING    ← RECORD 成功且 RTP 流正在接收
 *   - PAUSED     ← 客户端发 PAUSE（AirPlayVideo / AirTunes 都走这条）
 *   - CLOSED     ← TEARDOWN 或断线
 *   - ERROR      ← 任意严重错误
 *
 * 不能直接 new / delete AirPlaySession，只能通过 AirPlayServer 的回调
 * 或 get_session() 拿到其指针，并且不要跨 session 生命周期保存它。
 */
#ifndef AIRPLAY2_AIRPLAY_SESSION_H
#define AIRPLAY2_AIRPLAY_SESSION_H

#include "airplay_config.h"
#include <cstdint>
#include <string>
#include <memory>

namespace airplay2 {

class SessionImpl;   // 内部实现（src/core/airplay_session_impl.h）
class ServerImpl;    // 只有 ServerImpl 能构造 AirPlaySession

/*!
 * @brief 单个会话（客户端 ↔ 本设备 一对一）
 *
 * 对外暴露只读访问：id / 客户端信息 / state / stats / audio_config / disconnect
 * 所有真实实现放在 SessionImpl（避免对外暴露 RtpReceiver 等内部类型）。
 */
class AirPlaySession {
public:
    ~AirPlaySession();

    AirPlaySession(const AirPlaySession&) = delete;
    AirPlaySession& operator=(const AirPlaySession&) = delete;

    /// 单调递增 ID，同进程内永不复用。
    uint64_t id() const;

    /// 客户端 socket 的 IP:port（字符串形式，便于打印）
    std::string client_address() const;

    /// 客户端 User-Agent（RTSP header 中获取）；未识别时返回空字符串
    std::string client_name() const;

    /*!
     * @brief 会话状态机（参考类头描述）
     *
     * 状态迁移路径：
     *   IDLE → CONNECTED → PAIRING → SETUP → READY → PLAYING ⇄ PAUSED → CLOSED
     *   任何状态都可能 → ERROR → CLOSED
     */
    enum class State : uint8_t {
        IDLE = 0,      ///< 初始态（对外不应该看到）
        CONNECTED,     ///< TCP accept 完成，等待 ANNOUNCE 或 pair-*
        PAIRING,       ///< 正在执行 PIN / FairPlay 配对
        SETUP,         ///< ANNOUNCE 完成并解析 SDP，等待 SETUP 分配 RTP 端口
        READY,         ///< SETUP OK，RECORD 未到
        PLAYING,       ///< RECORD OK，正在收 RTP 并发给解码器
        PAUSED,        ///< PAUSE 收到，保留 RTP 端口 + 抖动缓冲内容
        CLOSED,        ///< 会话结束（TEARDOWN / 断线 / server.stop）
        ERROR          ///< 发生致命错误（随后会自动进入 CLOSED）
    };

    /// 当前状态（快照）
    State state() const;

    /// 运行统计（快照）；PLAYING 状态下字段变化最快
    SessionStats stats() const;

    /// 本会话 SDP 解析出的"源端"音频配置；若要改"播放端"格式请改 ServerConfig
    AudioConfig audio_config() const;

    /*!
     * @brief 主动断开该会话。
     *
     * 同步返回时：
     *   - 会向客户端发 RTSP 200 OK（可选）然后关闭控制 socket
     *   - RTP 端口归还给端口池
     *   - on_session_disconnected 回调被触发
     *
     * 多次调用安全（幂等）。
     */
    void disconnect();

private:
    // 只有 ServerImpl 能构造我们；这样 SessionImpl 的所有权完全在 server 侧，
    // 外部不能跨进程边界 delete 造成不匹配析构。
    friend class ServerImpl;
    explicit AirPlaySession(SessionImpl* impl);

    SessionImpl* impl_      = nullptr; ///< 真实实现（ServerImpl 拥有其生命周期）
    bool         owns_impl_ = false;   ///< 预留；当前 server::sessions_ 统一持有
};

} // namespace airplay2

#endif // AIRPLAY2_AIRPLAY_SESSION_H
