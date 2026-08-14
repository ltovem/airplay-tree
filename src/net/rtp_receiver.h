/*!
 * @file rtp_receiver.h
 * @brief AirPlay 音频 RTP / RTCP / Timing 三端口接收器 + 抖动缓冲
 *
 * AirPlay 协议约定，SETUP 响应里返回 serverPort: "server_port1/server_port2/
 * server_port3"，分别是：
 *   - port1 = audio data （RTP；承载 ALAC / AAC-ELD / PCM 负载）
 *   - port2 = control     （RTCP；SR/RR/NTP 时间戳）
 *   - port3 = timing      （AirPlay 私有 timing 包；维持音频主时钟同步）
 * 三个端口必须是**连续整数**（例如 5000/5001/5002），否则发送端会拒绝。
 *
 * 本类负责：
 *   1. 在 [port_min, port_max] 范围内扫描拿到"三个连续可用 UDP 端口"并绑定；
 *   2. 起一条 receiver 线程，用 select 同时等 data / ctrl / timing；
 *   3. 对 RTP data 做 sequence 排序 + 去重 + 丢包统计 + 有限深度抖动缓冲；
 *   4. 把"按序连续"的音频包通过 AudioPacketCb 回调喂给 session impl → ALAC 解码。
 *
 * 抖动缓冲的策略（简单实用版本）：
 *   - 包按 seq 插入有序 map
 *   - 当 buffer.size() >= jitter_depth_min 或 next_expected_seq_ 已就绪，
 *     就弹出连续前缀直到遇到缺口；这样可以容忍 128 包内的乱序（对于
 *     正常 100 包/秒 ~ 44100Hz / 4096 samples 来说约 1s）
 *   - 若连续 500ms 只收"后序包"但缺失的 seq 始终不到，就把丢失 seq 计为
 *     packets_lost，并推进 next_expected_seq_（避免缓冲永久卡住）。
 */
#ifndef AIRPLAY2_RTP_RECEIVER_H
#define AIRPLAY2_RTP_RECEIVER_H

#include "../platform/platform_socket.h"
#include "../platform/platform_thread.h"
#include <cstdint>
#include <functional>
#include <vector>
#include <map>
#include <mutex>
#include <memory>

namespace airplay2 {
namespace net {

/*!
 * @brief 一帧已经过抖动缓冲排序的 RTP 音频数据包
 *
 * 由 RtpReceiver 传出后，SessionImpl 会根据 pt / format 决定走
 * ALAC 解码还是直接透传 PCM。
 */
struct RtpAudioPacket {
    uint16_t    seq;         ///< RTP sequence（16 位，wrap 时也比较）
    uint32_t    timestamp;   ///< RTP timestamp，采样率时钟（44100 / 48000）
    uint32_t    ssrc;        ///< 本次会话 SSRC，校验用
    uint8_t     pt;          ///< payload type：96=ALAC，97=AAC，98=PCM 动态
    bool        marker;      ///< RTP marker bit（一帧的最后一包）
    std::vector<uint8_t> payload; ///< 纯音频负载（已剥掉 12/16 字节 RTP header）
    uint64_t    recv_us;     ///< 收到本包时的本地单调钟（微秒），用于抖动计算
};

/// 抖动缓冲输出回调；函数对象必须可拷贝，会在 receiver 线程同步调用
using AudioPacketCb = std::function<void(const RtpAudioPacket&)>;

/*!
 * @brief RTP 三端口接收器 + 抖动缓冲 + 重排序 + 统计
 *
 * 典型使用流程：
 *   auto rtp = std::make_unique<RtpReceiver>();
 *   int ports[3];
 *   if (!rtp->open(cfg.rtp_port_min, cfg.rtp_port_max, ports)) return error;
 *   rtp->set_packet_callback([this](auto& pkt){ handle_audio_packet(pkt); });
 *   rtp->start();
 *   // ... SETUP 返回 ports[0]/ports[1]/ports[2] 给客户端
 *   rtp->stop();
 */
class RtpReceiver {
public:
    RtpReceiver();
    ~RtpReceiver();

    /*!
     * @brief 扫描并绑定 3 个连续 UDP 端口
     * @param port_min 端口池下界（含）
     * @param port_max 端口池上界（含）
     * @param[out] ports 成功绑定的 3 个端口，按 data/ctrl/timing 顺序；
     *                  失败时内容未定义
     * @return true 找到并 bind 成功；
     *         false 端口池耗尽（没有 3 个连续空闲）或 socket 创建失败
     */
    bool open(uint16_t port_min, uint16_t port_max, int ports[3]);

    /// 注册音频包回调；建议在 start() 之前或 PLAYING 状态切换之外调用
    void set_packet_callback(AudioPacketCb cb) { packet_cb_ = std::move(cb); }

    /// 启动接收线程（open 之后才能调）；重复调用安全
    bool start();

    /// 停止并关闭三个 UDP socket；会把端口立即还给系统
    void stop();

    /// 丢弃所有缓冲（例如 FLUSH / seek 时，避免旧时间戳被吐出来）
    void flush();

    /*!
     * @brief 运行时统计（由 receiver 线程原子更新，外部读是快照）
     *
     * 这些字段会导出到 SessionStats 中，用于健康面板和丢包告警：
     *   - packets   : RTP data port 收包（去重后的真实包数）
     *   - bytes     : payload 字节数累计
     *   - lost      : 通过 sequence gap 推断的丢失包数
     *   - reordered : 到达顺序与 seq 不一致的包数（>0 说明网络有乱序）
     */
    struct Stats {
        uint64_t packets   = 0;
        uint64_t bytes     = 0;
        uint64_t lost      = 0;
        uint64_t reordered = 0;
    };
    Stats stats() const { return stats_; }

private:
    // ---- 线程主循环 -----------------------------------------------------------
    //   select(data_sock_ | ctrl_sock_ | timing_sock_, 100ms timeout)
    //   - 可读 → 按 socket 分发处理（RTP 解码 / RTCP 丢弃但统计 / Timing 丢弃）
    //   - 超时 → 检查 jbuffer_ 是否"缺包太久"，必要时推进丢包计数
    void receiver_worker();

    // ---- 抖动缓冲弹出 ---------------------------------------------------------
    //   从 jbuffer_.begin() 起按 seq 连续弹出，每次弹出会让 next_expected_seq_++，
    //   并触发 packet_cb_ 回调。遇到缺口立即停止，等待后续包或超时判丢。
    void emit_ready();

    // ---- socket / thread ------------------------------------------------------
    platform::Socket data_sock_;    ///< port1: RTP audio data（UDP）
    platform::Socket ctrl_sock_;    ///< port2: RTCP SR/RR（UDP）
    platform::Socket timing_sock_;  ///< port3: AirPlay timing（UDP）
    platform::Thread worker_;       ///< 接收线程
    std::atomic<bool> running_{false};
    AudioPacketCb packet_cb_;       ///< 有序输出回调（无锁，只在 worker 访问）

    // ---- 抖动缓冲（唯一需要锁的部分）-----------------------------------------
    //   外部 flush() 会从其它线程清 buffer；内部 worker 每次收包 / 超时都要写；
    //   所以用独立的 jbuf_mu_ 保护，而不是和 running_ 绑在一起，减少锁粒度。
    //   其它数据（端口、stats 等）只在 worker 内部或 start/stop 临界区写，无额外锁。
    std::mutex                             jbuf_mu_;
    // 按 RTP sequence（16 位，以 std::map 红黑树自动排序）组织未出队的包
    std::map<uint16_t, RtpAudioPacket>     jbuffer_;
    uint16_t next_expected_seq_ = 0;   ///< 下一个期望 seq；初始值来自首个到达包
    bool     has_started_       = false;///< 是否见过首个包（首次包用于初始化 next_expected_seq_）
    Stats    stats_;                   ///< 统计（只在 worker 写，读不加锁，快照近似）
    // 128 包 ≈ 44100Hz / 4096 samples ≈ 11 FPS → 约 11 秒最大缓存，足以
    // 覆盖 WiFi 常见 burst 延迟；太小会频发丢包误判；太大带来播放延迟。
    size_t   jbuf_max_ = 128;          ///< 抖动缓冲最多包数（超过就强制出队旧的）
};

} // namespace net
} // namespace airplay2

#endif // AIRPLAY2_RTP_RECEIVER_H
