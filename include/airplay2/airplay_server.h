/*!
 * @file airplay_server.h
 * @brief AirPlay 2 接收器（服务端）对外主接口
 *
 * 本文件定义了 airplay2lib 最重要的用户态对象 AirPlayServer。
 * 典型使用流程：
 *   1. 调用 AirPlayServer::global_init() 初始化平台子系统（Winsock 等）
 *   2. 填 ServerConfig / DeviceInfo / AudioConfig，设置好 IAudioRenderer
 *   3. 构造 AirPlayServer，可选注册 ServerCallbacks 回调
 *   4. start()，此时库会在本地网络通过 mDNS 宣告自身，并在 control_port
 *      上监听 RTSP/HTTP 请求，同时为每个连接建立 AirPlaySession
 *   5. 收到 on_started 后，即可在 iPhone / Mac / iTunes 上看到本设备并投音
 *   6. 进程退出前 stop() → global_cleanup()
 */
#ifndef AIRPLAY2_AIRPLAY_SERVER_H
#define AIRPLAY2_AIRPLAY_SERVER_H

#include "airplay_config.h"
#include "airplay_session.h"
#include "audio_renderer.h"
#include "video_renderer.h"
#include <functional>
#include <memory>
#include <vector>

namespace airplay2 {

// 前置声明：真实实现放在 src/core/airplay_server_impl.h，避免
// 把内部实现头暴露到公共 include 路径上。
class ServerImpl;

/*!
 * @brief 服务端状态回调集合。
 *
 * 所有回调都会在"服务器内部工作线程"上被调用，实现端需要自行保证
 * 回调中对 UI / 播放设备的访问线程安全。回调中请勿做阻塞操作
 * （> 数毫秒），否则会卡住 RTSP 控制通道或音频线程。
 */
struct ServerCallbacks {
    /// 服务器绑定完成 + mDNS 宣告已发出后调用。
    /// 之后发送端即可在列表里看到设备。
    std::function<void()> on_started;

    /// stop() 返回前调用一次，表示已经完成端口关闭 + mDNS goodbye 包。
    std::function<void()> on_stopped;

    /// 新客户端 TCP 连入并完成 session_id 分配时调用。
    /// 此时 session 处于 CONNECTED 状态，尚未通过配对或 ANNOUNCE。
    std::function<void(AirPlaySession& session)> on_session_connected;

    /// 会话断开（含 TEARDOWN、TCP 断线、server.stop() 批量踢线）。
    /// session_id 此后无效；如需持久化统计请在 stats() 里读取。
    std::function<void(uint64_t session_id)> on_session_disconnected;

    /// 要求输入配对 PIN。仅当 DeviceInfo::pin_code 非空时会触发。
    /// @param client_address 对端 IP:port 字符串，便于显示在 UI 上
    /// @param pin            客户端输入的 4 位数字 PIN
    /// @return 返回 true 表示接受；返回 false 立即拒绝该配对请求。
    std::function<bool(const std::string& client_address, const std::string& pin)> on_pin_request;

    /// 不可恢复的运行时错误（绑定失败、mDNS 宣告失败、解码严重错误等）。
    /// 注意：网络瞬断 / 单包丢包不会触发这里，只影响 SessionStats。
    std::function<void(Status code, const std::string& message)> on_error;

    /// 日志回调，可选。若不设置，默认按平台输出 stderr / Android logcat /
    /// macOS os_log；设置后所有日志改走该回调，级别 0..4 对应
    /// error / warn / info / debug / trace。
    std::function<void(int level, const std::string& message)> on_log;
};

/*!
 * @brief AirPlay 2 服务端主类
 *
 * 内部组合了 4 个协作模块：
 *   - MdnsPublisher：在 224.0.0.251:5353 上宣告 _airplay._tcp / _raop._tcp
 *   - RtspServer   ：在 control_port 上跑 HTTP/RTSP 路由，处理 ANNOUNCE/SETUP/RECORD 等
 *   - RtpReceiver  ：为每个会话分配 RTP/RTCP/Timing 3 个 UDP 端口 + 抖动缓冲
 *   - ALAC Decoder + AudioBuffer：把收到的 RTP 包解成 PCM 并交给 IAudioRenderer
 *
 * @note 线程模型：1 个 RTSP 监听线程 + 每会话 1 条控制连接（接受短连接复用）+
 *       每会话 1 条 RTP 收包线程 + 1 条播放渲染线程；所有线程通过
 *       platform::Thread / 原子 / 互斥锁管理，外部仅需保证 AirPlayServer
 *       的 start()/stop() 不被并发调用即可。
 */
class AirPlayServer {
public:
    /*!
     * @brief 构造服务端对象（不启动任何网络端口或线程，仅初始化配置）
     * @param config         服务端配置（device、audio、端口范围等），内部会拷贝
     * @param callbacks      状态回调，可空；可后续通过 set_callbacks() 替换
     * @param audio_renderer 音频渲染回调；为空时相当于"静音模式"
     * @param video_renderer 视频渲染回调；为空时视频帧被丢弃（仍可投音）
     */
    explicit AirPlayServer(const ServerConfig& config,
                           ServerCallbacks callbacks = {},
                           IAudioRenderer* audio_renderer = nullptr,
                           IVideoRenderer* video_renderer = nullptr);
    ~AirPlayServer();

    AirPlayServer(const AirPlayServer&) = delete;
    AirPlayServer& operator=(const AirPlayServer&) = delete;

    /*!
     * @brief 启动服务：绑定 control_port、mDNS 宣告、启动工作线程池
     * @return Status::OK 表示成功；
     *         ERROR_BIND_FAILED 表示端口被占用；
     *         ERROR_MDNS 表示 mDNS 宣告线程无法创建 socket；
     *         ERROR_ALREADY_RUNNING 表示重复调用 start()。
     */
    Status start();

    /*!
     * @brief 同步停止：发送 mDNS goodbye → 关闭 RTSP/RTP 端口 →
     *        等待所有会话 TEARDOWN → 回收线程。通常 < 1s 返回。
     *        可重复调用（幂等）。
     */
    void stop();

    /// 当前是否处于 start() 之后 / stop() 之前
    bool is_running() const;

    /// 当前所有活跃 session_id 列表（快照）
    std::vector<uint64_t> active_session_ids() const;

    /// 按 session_id 拿到会话指针；找不到返回 nullptr。
    /// @warning 指针仅在"本会话断开之前"有效；若你在另一个线程使用，
    ///          请持有由你自己保证的引用计数，或干脆在 on_session_disconnected
    ///          时立即丢弃。
    AirPlaySession* get_session(uint64_t session_id);

    /// 读出启动时传入的配置（只读；如需修改，stop 后新建 AirPlayServer）
    const ServerConfig& config() const;

    /// 运行中替换音频渲染器；传 nullptr 停止向外喂音频。
    /// 线程安全（内部有一次轻量锁），但最好在没有 PLAYING 会话时切换。
    void set_audio_renderer(IAudioRenderer* renderer);

    /// 运行中替换视频渲染器；传 nullptr 停止向外喂视频帧。
    void set_video_renderer(IVideoRenderer* renderer);

    /// 运行中替换回调；不替换的字段需自行保留旧值
    void set_callbacks(ServerCallbacks callbacks);

    /*!
     * @brief 全局库初始化：做平台相关的 one-shot 准备
     *
     * Windows：调用 WSAStartup() 初始化 Winsock 2.2；
     * POSIX：检查 SO_REUSEPORT 等 socket option 可用性；
     * Android：调用 `__android_log_print` 的输出注册；
     * macOS/iOS：无副作用，但仍要求先调以兼容未来扩展。
     *
     * 建议在 main() 第一行调用，且与 global_cleanup() 配对。
     *
     * @return ERROR_NETWORK 表示平台初始化失败（例如 Winsock DLL 缺失）。
     */
    static Status global_init();

    /// 全局清理：对应 global_init() 的资源释放。退出主函数前调用一次。
    static void global_cleanup();

private:
    // 不透明实现：避免把内部类型泄漏到公共 API，从而允许 ABI 兼容升级。
    std::unique_ptr<ServerImpl> impl_;
};

} // namespace airplay2

#endif // AIRPLAY2_AIRPLAY_SERVER_H
