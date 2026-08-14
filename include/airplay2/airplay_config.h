/*!
 * @file airplay_config.h
 * @brief AirPlay 2 服务端所有"配置类 / 枚举 / 状态码"
 *
 * 本文件是公共 API 的基础，所有其它公共头（airplay_server.h /
 * airplay_session.h / audio_renderer.h）都会 include 它。
 * 设计目的：把"纯数据类型"和"行为类"分文件放，降低循环依赖概率，
 * 也让 CI 单测更容易独立 include。
 */
#ifndef AIRPLAY2_AIRPLAY_CONFIG_H
#define AIRPLAY2_AIRPLAY_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

namespace airplay2 {

/*!
 * @brief 解码器输出 / 渲染器输入的音频采样格式
 *
 * 对 AirPlay 发送端而言：其通过 RTSP ANNOUNCE 里 SDP 告知格式（
 * 一般是 44100Hz 16-bit ALAC 或 48000Hz AAC-ELD），库内部会统一解码
 * 成 IAudioRenderer::configure() 中设置的格式。
 */
enum class AudioFormat : uint8_t {
    PCM16LE   = 0, ///< 16-bit signed little-endian PCM（默认；所有渲染器都必须支持）
    PCM24LE   = 1, ///< 24-bit signed little-endian，按 3 字节紧排
    PCM32LE   = 2, ///< 32-bit signed little-endian，便于 DSP 处理
    PCM_FLOAT = 3, ///< 32-bit 浮点数（[-1.0, 1.0]），现代音频框架常用
    ALAC      = 4, ///< Apple Lossless：仅当要透传时使用，库默认不解码后输出它
    AAC_LC    = 5, ///< AAC Low Complexity（AirPlay 1 常见）
    AAC_ELD   = 6, ///< AAC Enhanced Low Delay（AirPlay 2 新发送端、HomePod 用）
    OPUS      = 7  ///< 预留；目前没有 AirPlay 源会用 Opus
};

/*!
 * @brief 一条音频会话的格式参数
 *
 * 有两个位置会出现 AudioConfig：
 *   1. ServerConfig::audio — 表示"期望渲染器用什么 PCM 格式"，
 *      若 SDP 协商得到的源格式与之不同，内部会做重采样 / 位宽转换（
 *      目前只做位宽转换，重采样需要后续接 libsamplerate）。
 *   2. AirPlaySession::audio_config() — 表示"当前会话发送端真正送出的"
 *      原始格式，用于诊断与回调。
 */
struct AudioConfig {
    uint32_t    sample_rate = 44100;   ///< 采样率 (Hz)，AirPlay 常见 44100 / 48000
    uint8_t     channels    = 2;       ///< 声道数：1 单声道 2 立体声；>2 未来扩展
    AudioFormat format      = AudioFormat::PCM16LE; ///< 采样格式
    uint16_t    frame_size  = 0;       ///< 单帧样本数（每个通道）；0 表示由发送端 SDP 指定
    uint32_t    bitrate     = 0;       ///< 对编码格式（AAC/ALAC）建议码率，0 表示自动
};

/*!
 * @brief AirPlay 设备在 mDNS 上宣告的信息
 *
 * 这些字段会写入 Bonjour TXT record。发送端（iPhone/iTunes）会根据
 * TXT 中 features / model / pk 等决定 UI 展示图标以及是否启用加密。
 * 参考：非官方 AirPlay2 逆向文档中 feature flags 的 bit 定义，
 * 默认 features = 0x5A7FFFF7 基本覆盖所有音频功能。
 */
struct DeviceInfo {
    std::string name;        ///< 显示名（UI 上看到的音箱名）
    std::string device_id;   ///< 唯一 ID，形如 "aa:bb:cc:dd:ee:ff"，留空时库会用本地 MAC 生成
    std::string model;       ///< 模型 ID，例如 "AppleTV3,2" / "AudioAccessory1,2"；为空取 "AudioAccessory1,2"
    std::string manufacturer = "airplay2lib"; ///< 制造商名（在一些扫描工具里显示）
    std::string serial_number;              ///< 序列号；未开启 FairPlay 时可留空
    uint16_t    port     = 7000;            ///< AirPlay RTSP 控制端口（默认 7000；改了要同时改 ServerConfig）
    // 为什么是 32 位？：按 AirPlay 协议 feature 位已经超过 uint16
    // （bit24=has video、bit31=requires auth 等），必须用 uint32。
    uint32_t    features = 0x5A7FFFF7;      ///< Feature flags 位图
    uint8_t     protocol_version = 1;       ///< AirPlay 协议版本：1 = AP1/AP2 兼容
    bool        supports_audio = true;      ///< 宣告支持音频（关闭后仅能做屏幕镜像接收器）
    bool        supports_video = false;     ///< 宣告支持视频（当前库未实现视频解码，建议 false）
    bool        supports_photo = false;     ///< 宣告支持照片投射（目前未实现）
    bool        requires_encryption = false;///< 是否要求 FairPlay 加密（需要 MFi 证书，当前留 false）
    std::string pin_code;                   ///< 4 位数字 PIN；非空时每次配对要输入
};

/*!
 * @brief AirPlayServer 的运行时配置
 *
 * 传给 AirPlayServer 构造函数后被深拷贝；启动后想修改需要 stop +
 * 重新构造新 server（避免并发修改端口带来的 bug）。
 */
struct ServerConfig {
    DeviceInfo device;                      ///< 设备信息（用于 mDNS 宣告）
    AudioConfig  audio;                     ///< 期望的音频输出格式
    std::string  bind_address = "0.0.0.0";  ///< 控制端口绑定地址；"::" 表示 IPv6 any
    uint16_t     control_port = 7000;       ///< HTTP/RTSP 控制监听端口（默认 7000）
    // RTP 端口范围：每会话需要占用 3 个连续 UDP 端口（data/rtcp/timing）。
    // 默认 21 个端口可以支撑 7 个并发会话；按需放大。
    uint16_t     rtp_port_min = 5000;       ///< RTP/UDP 端口池下界（含）
    uint16_t     rtp_port_max = 5020;       ///< RTP/UDP 端口池上界（含）
    bool         publish_mdns = true;       ///< 是否自动 mDNS 宣告；关了就只能手动用 IP:port 连接
    size_t       max_sessions = 8;          ///< 最大并发会话数；超了会拒绝 ANNOUNCE
    // 缓冲大小：2s 是常见经验值；若需要跟视频同步或蓝牙播放，建议升到 3~5s
    uint32_t     buffer_ms  = 2000;         ///< 抖动缓冲 + 渲染缓冲合计目标延迟（毫秒）
    bool         enable_logging = true;     ///< 是否启用内部日志；配合 ServerCallbacks::on_log 使用
    int          log_level = 2;             ///< 0=error 1=warn 2=info 3=debug 4=trace
};

/*!
 * @brief 单个会话的运行统计（只读快照）
 *
 * 所有字段在服务端运行中由原子递增或在持锁下更新，外部读取的值
 * 是"读取瞬间"的近似值，不保证同一秒内所有字段完全一致，但足以
 * 用于 UI 仪表盘或健康检查。
 */
struct SessionStats {
    uint64_t    packets_received  = 0;     ///< 收包总数（RTP data + RTCP）
    uint64_t    packets_lost      = 0;     ///< 根据 RTP sequence 间隙估算的丢包
    uint64_t    bytes_received    = 0;     ///< 音频负载字节数（不含 RTP header）
    uint32_t    current_latency_ms = 0;    ///< 当前抖动缓冲深度换算出的延迟
    uint32_t    jitter_ms         = 0;     ///< 近 1 秒到达间隔抖动
    uint64_t    audio_frames_decoded = 0;  ///< 已成功送入解码器的帧
    double      session_duration_sec = 0.0;///< 会话存活时长（秒），PLAYING + PAUSED 都计时
    std::string client_ip;                 ///< 客户端来源 IP
    std::string client_name;               ///< 客户端 User-Agent 或 DACP 名
};

/*!
 * @brief 库通用错误码
 *
 * 采用"负值表示错误，0 为 OK"的常见 C/C++ 风格，便于上层 if (status) 判定。
 * 带消息版本请配合 ServerCallbacks::on_error 使用；单独返回码只做分类。
 */
enum class Status : int {
    OK                     = 0,    ///< 成功
    ERROR_INVALID_ARGUMENT = -1,   ///< 参数非法（例如端口 0、空名称）
    ERROR_NOT_INITIALIZED  = -2,   ///< 未调用 AirPlayServer::global_init()
    ERROR_ALREADY_RUNNING  = -3,   ///< 重复 start()
    ERROR_NETWORK          = -4,   ///< 通用网络错误（socket 创建失败等）
    ERROR_BIND_FAILED      = -5,   ///< 端口被占用或权限不够（<1024 在 Linux 需要 CAP_NET_BIND）
    ERROR_MDNS             = -6,   ///< mDNS 宣告 / 加入组播失败
    ERROR_SESSION_LIMIT    = -7,   ///< 超过 ServerConfig::max_sessions
    ERROR_AUTH_FAILED      = -8,   ///< PIN 配对失败或 FairPlay 校验失败
    ERROR_CODEC            = -9,   ///< 解码错误（坏帧 / 不支持的格式）
    ERROR_IO               = -10,  ///< 本地 I/O 错误（渲染器写失败等）
    ERROR_OUT_OF_MEMORY    = -11,  ///< 分配失败
    ERROR_UNSUPPORTED      = -12,  ///< 请求的特性当前库未实现
    ERROR_TIMEOUT          = -13,  ///< 对端超过阈值不响应
    ERROR_UNKNOWN          = -99   ///< 未分类错误（配合 on_error 的 message 一起看）
};

} // namespace airplay2

#endif // AIRPLAY2_AIRPLAY_CONFIG_H
