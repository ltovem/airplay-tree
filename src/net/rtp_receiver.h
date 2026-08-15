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
#include "../crypto/aes_ctr.h"
#include "../crypto/aes_cbc.h"
#include "rtcp.h"
#include "timing.h"
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

    /// 启动接收线程（open 之后才能调）；重复调用安全。
    /// 若 open() 尚未执行（AP2 里带流的 SETUP 可能晚于 RECORD 到达），
    /// 会延迟到 open() 绑定端口成功后自动拉起线程。
    bool start();

    /// 停止并关闭三个 UDP socket；会把端口立即还给系统
    void stop();

    /// 丢弃所有缓冲（例如 FLUSH / seek 时，避免旧时间戳被吐出来）
    /// 同时会重置 AES-CTR 计数器到初始 IV（ANNOUNCE 给的），避免
    /// FLUSH 之后计数器错位造成后续包解出全噪声。
    void flush();

    /*!
     * @brief 设置 AES-128-CTR 解密参数（ANNOUNCE SDP 解析结果）
     *
     * AirPlay 2 发送端如果在 SDP 里放了 a=aeskey / a=aesiv，
     * 后续所有 RTP audio payload 都会以 AES-128-CTR 模式加密。
     * 调用方把 hex 字符串原样传入（如 "5b...32 字节 hex"），
     * 内部会 hex 解码后 set_key。未调用或 hex 长度非法时，
     * RTP 负载原样交付（等同于不加密的路径）。
     *
     * @return true = 解码成功；false = hex 长度/字符不合法，不生效
     */
    bool set_decryption_params(const std::string& aes_key_hex,
                               const std::string& aes_iv_hex);

    /*!
     * @brief 设置 AES-128-CBC 解密参数（AirPlay 2 SETUP bplist 的 ekey/eiv）
     *
     * AP2 音频 RTP：12 字节 RTP 头明文，payload 从第 0 字节起 AES-CBC
     * 加密，IV = SETUP bplist 的 eiv（固定，非每包随机）。解密在
     * receiver 线程按包进行（每包重新 init IV，因为 CBC 每包独立）。
     *
     * @param key 16 字节 AES 密钥（已含 ecdh_secret 哈希）
     * @param iv  16 字节 IV（SETUP bplist 的 eiv）
     */
    void set_cbc_decryption(const uint8_t* key, const uint8_t* iv);

    /*!
     * @brief 告诉 RtpReceiver 发送端的 UDP 地址，用于回 RR / timing response
     *
     * SETUP 请求里 Transport header 带 client_port=X-Y，
     * AirPlay 约定：
     *   - remote data      = X   （接收端不会往 data 发包）
     *   - remote control   = X+1 （接收端需要往这里发 RR）
     *   - remote timing    = X+2 （接收端需要往这里发 timing response）
     *
     * 三端口连续的约定也和本地一致，所以 session 传 [X, X+1, X+2] 进来。
     * start() 之前调用，保证收到首个 RTCP SR 就能即时回 RR。
     */
    void set_remote_address(const std::string& client_ip, int remote_ports[3]);

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

    /// 是否已收到 RTCP SR（判断 NTP 锚点拿到了没）
    bool has_rtcp_sr() const { return rtcp_.has_sr(); }

    /// 最近一次 RTCP SR（用于外部打印日志或音视频同步）
    const RtcpSrInfo& last_rtcp_sr() const { return rtcp_.last_sr(); }

private:
    // ---- 线程主循环 -----------------------------------------------------------
    //   select(data_sock_ | ctrl_sock_ | timing_sock_, 100ms timeout)
    //   - 可读 → 按 socket 分发处理：
    //       * data   → RTP 解码 → 抖动缓冲 → emit_ready
    //       * ctrl   → RTCP::handle_packet（解析 SR） + 定期用它的地址回 RR
    //       * timing → Timing::handle_packet → 非空就用 sendto 回
    //   - 超时 → 检查 jbuffer_ 是否"缺包太久"，必要时推进丢包计数；
    //            同时检查"距离上次发 RR 是否超过 5 秒"，超过就回一个。
    void receiver_worker();

    // ---- 抖动缓冲弹出 ---------------------------------------------------------
    //   从 jbuffer_.begin() 起按 seq 连续弹出，每次弹出会让 next_expected_seq_++，
    //   并触发 packet_cb_ 回调。遇到缺口立即停止，等待后续包或超时判丢。
    void emit_ready();

    // ---- 按需发 RTCP RR -------------------------------------------------------
    //   每 5 秒或 RR 间隔到期时构造 RR 包并通过 ctrl_sock_ 发往 sender_ip_+ctrl_port_
    //   参数来源：rtcp_.last_sr() 的 sender_ssrc，jitter/lost/highest_seq 来自 stats_
    void maybe_send_rr();

    // ---- socket / thread ------------------------------------------------------
    platform::Socket data_sock_;    ///< port1: RTP audio data（UDP）
    platform::Socket ctrl_sock_;    ///< port2: RTCP SR/RR（UDP）
    platform::Socket timing_sock_;  ///< port3: AirPlay timing（UDP）
    platform::Thread worker_;       ///< 接收线程
    std::atomic<bool> running_{false};
    // start() 在端口绑定前被调用时置位；open() 绑定成功后会自动补启动。
    // 只在 RTSP 连接线程（SETUP/RECORD/TEARDOWN handler）里读写，无需加锁。
    bool start_deferred_ = false;
    AudioPacketCb packet_cb_;       ///< 有序输出回调（无锁，只在 worker 访问）

    // ---- 远端地址（发 RR / timing response 用）--------------------------------
    //   ANNOUNCE 拿到的 source_ip 或 SETUP client_ip。
    //   control / timing 端口来自 SETUP client_port 的第 2/3 个。
    std::string         sender_ip_;
    int                 remote_ctrl_port_   = 0;
    int                 remote_timing_port_ = 0;
    // 远端"最后一次向我们发 ctrl / timing 包"的 peer，作为备用地址：
    // 某些客户端的真实发送端口与 SETUP 声明不一致（NAT/端口漂移），
    // 这里用最近一次 recvfrom 的 from 地址优先。
    platform::SocketAddr ctrl_peer_;
    platform::SocketAddr timing_peer_;

    // ---- 协议处理子模块（都只在 worker 线程使用，无锁）------------------------
    RtcpHandler          rtcp_;        ///< 解析 SR + 构造 RR
    TimingHandler        timing_;      ///< 解析 timing request + 构造 response
    crypto::AesCtr       aes_;         ///< AES-128-CTR 解密（逐包 process，AP1 SDP 路径）
    crypto::AesCbc      cbc_;          ///< AES-128-CBC 解密（AP2 SETUP ekey/eiv 路径）
    uint8_t             cbc_iv_[16];   ///< CBC IV（SETUP bplist 的 eiv）
    bool                cbc_ready_ = false; ///< 是否启用 CBC 解密
    // RTP jitter 估算（RFC 3550 A.8）：给 RR 里的 jitter 字段，
    // 不用很精确，AirPlay 发送端只用它做大致拥塞判断。
    uint32_t             jitter_est_ = 0;
    int64_t              last_arrival_ts_ = 0; ///< 上一包到达时的 RTP 时间戳
    uint64_t             last_arrival_us_ = 0; ///< 上一包到达时的本地微秒单调钟
    uint64_t             last_rr_send_us_ = 0; ///< 上一次发 RR 的时刻（wallclock_us）
    uint32_t             our_ssrc_ = 0x41503200;  ///< "AP2\0"

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
