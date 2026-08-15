# airplay2lib — 公共 API 参考

目录：`include/airplay2/*.h` 是对外稳定 API。其他 `src/*` 文件请勿直接 include。

## 1. 快速开始（基本用法）

```cpp
#include <airplay2/airplay2.h>
#include <airplay2/airplay_server.h>
#include <airplay2/audio_renderer.h>
#include <cstdio>

using namespace airplay2;

// 第一步：自定义 IAudioRenderer 子类实现音频输出
class StdoutRenderer : public IAudioRenderer {
public:
    void on_config(const AudioConfig& cfg) override {
        std::printf("Audio config: %u Hz, %u ch, fmt=%u\n",
                    cfg.sample_rate, cfg.channels, (unsigned)cfg.format);
    }
    void on_play()  override { std::puts("▶ PLAY"); }
    void on_pause() override { std::puts("⏸ PAUSE"); }
    void on_stop()  override { std::puts("⏹ STOP"); }
    void on_flush() override { /* flush local audio buffer */ }
    void set_volume(float v) override { std::printf("volume = %.3f\n", v); }
    void on_pcm(const void* pcm, size_t bytes, uint64_t userdata) override {
        // pcm: 按 AudioConfig format (默认 PCM16LE) 排列的交错帧
        // bytes: 字节数 (samples * channels * bytes_per_sample)
        // userdata: 单调钟微秒时间戳 (platform::time_now_us 风格)
        (void)pcm; (void)bytes; (void)userdata;
    }
};

int main() {
    // 全局初始化（平台 socket 库：Winsock 启动等；必须先于 Server 构造）
    AirPlayServer::global_init();

    ServerConfig cfg;
    cfg.device.name = "airplay2lib Speaker";
    cfg.device.features = 0x5A7FFFF7;
    cfg.device.control_port = 7000;
    cfg.device.manufacturer = "MyCompany";
    cfg.control_port = 7000;
    cfg.rtp_port_min = 5000;
    cfg.rtp_port_max = 5020;
    cfg.max_sessions = 4;
    cfg.buffer_ms = 2000;

    StdoutRenderer renderer;

    auto server = AirPlayServer::create(cfg, &renderer);
    if (!server) return 1;

    ServerCallbacks cb;
    cb.on_session_started = [](AirPlaySession& s) {
        std::printf("Session %lu from %s (%s)\n",
                    s.id(), s.client_address().c_str(), s.client_name().c_str());
    };
    cb.on_session_disconnected = [](AirPlaySession& s) {
        std::printf("Session %lu ended\n", s.id());
    };
    cb.on_log = [](int level, const char* msg) {
        (void)level; std::fprintf(stderr, "[ap2] %s\n", msg);
    };
    // 如果你要响应 AirPlay 2 action / event / metadata，也可以在这里设回调：
    // cb.on_action = [](auto conn_id, const auto& dict, auto raw, auto n){ ... return vector<uint8_t>{}; };
    server->set_callbacks(cb);

    if (!server->start()) return 2;

    std::getchar();  // 阻塞直到回车

    server->stop();
    AirPlayServer::global_cleanup();
    return 0;
}
```

完整可运行版本见 `examples/basic_server.cpp`。

## 2. `airplay_config.h` — 枚举 / 数据结构

### 2.1 `AudioFormat` 枚举

| 值 | 含义 |
|----|------|
| `PCM16LE`（默认）| 16-bit little-endian，所有渲染器必须支持 |
| `PCM24LE` | 24-bit 3 字节紧排 |
| `PCM32LE` | 32-bit 位宽 |
| `PCM_FLOAT` | 32-bit float [-1, 1] |
| `ALAC` / `AAC_LC` / `AAC_ELD` / `OPUS` | 编码格式，仅在 `audio_config()` 报告"源格式"时出现；`IAudioRenderer::on_pcm` 一律收到解码后的 PCM |

### 2.2 `AudioConfig`

| 字段 | 类型 | 说明 |
|------|------|------|
| `sample_rate` | uint32_t | 采样率 (Hz) |
| `channels` | uint8_t | 声道数 |
| `format` | AudioFormat | 采样格式 |
| `frame_size` | uint16_t | 单帧样本数（0 = 发送端决定） |
| `bitrate` | uint32_t | 编码建议码率（0 = 自动） |

### 2.3 `DeviceInfo`（mDNS 宣告用）

关键字段：

- `name`：UI 显示名
- `device_id`：形如 `aa:bb:cc:dd:ee:ff`，留空时库会用本地 MAC 生成
- `model`：空时默认 `AudioAccessory1,2`
- `features`：默认 `0x5A7FFFF7`（音频全覆盖 + 支持 AirPlay 2）
- `port` / `control_port`：RTSP 端口（通常 7000）
- `requires_encryption`：true 时需要 PIN / FairPlay（当前 FairPlay 未实现，需要 PIN 请填 `pin_code`）
- `pin_code`：非空时每次配对要输入这个 4 位数字

### 2.4 `ServerConfig`

- `device`：上面的设备信息
- `audio`：期望的**输出** PCM 格式（库内部会做位宽转换）
- `bind_address`：默认 `0.0.0.0`；`::` 表示 IPv6 any
- `control_port`：RTSP/HTTP 监听端口，默认 7000
- `rtp_port_min / rtp_port_max`：每会话要占 3 个连续 UDP 端口；默认 21 个端口 → 支持 7 个并发会话
- `max_sessions`：会话上限（超过直接回 503）
- `buffer_ms`：抖动 + 渲染合计目标延迟，默认 2s（蓝牙/视频同步建议 3~5s）
- `enable_logging` + `log_level`：0 error ~ 4 trace

### 2.5 `SessionStats`（会话只读快照）

| 字段 | 含义 |
|------|------|
| `packets_received` | RTP data + RTCP 总包数 |
| `packets_lost` | seq gap 推断的丢包 |
| `bytes_received` | 音频负载字节 |
| `current_latency_ms` | 当前抖动缓冲深度换算的延迟 |
| `jitter_ms` | 到达间隔抖动（≈ RTP jitter / 44.1k） |
| `audio_frames_decoded` | 送入解码器的帧数（含 ALAC / PCM） |
| `session_duration_sec` | 会话存活秒数（PLAYING + PAUSED） |
| `client_ip` / `client_name` | 客户端来源信息 |

### 2.6 `Status` 错误码

`OK=0` 负值是错；配合 `ServerCallbacks::on_error(const char* message, Status s)`
一起诊断比单看错误码更高效。

## 3. `airplay_server.h` — `AirPlayServer`

### 3.1 生命周期

```
AirPlayServer::global_init()
  ▼
auto srv = AirPlayServer::create(cfg, renderer)
  ▼
srv->set_callbacks(cb)
  ▼
srv->start()  → 返回 false：端口被占/权限不足等
  ▼
（运行中...）
  ▼
srv->stop()
  ▼
AirPlayServer::global_cleanup()
```

### 3.2 关键方法

- `create(ServerConfig, IAudioRenderer*)`：`std::unique_ptr<AirPlayServer>`
- `set_callbacks(ServerCallbacks cb)`：启动前调；回调对象被深拷贝
- `start() / stop()`：启动/停止 RTSP server + mDNS 宣告 + 会话回收线程
- `sessions()`：当前所有活跃会话（vector<AirPlaySession*>&），只读
- `config() / set_config(new)`：后者需要 `stop()` 后再调用

### 3.3 `ServerCallbacks`（可选回调，都有默认空实现）

| 回调 | 触发时机 |
|------|----------|
| `on_session_started(AirPlaySession&)` | ANNOUNCE 成功后，会话进入 SETUP 态 |
| `on_session_ready(AirPlaySession&)` | SETUP OK，进入 READY，等待 RECORD |
| `on_session_playing(AirPlaySession&)` | RECORD 后 |
| `on_session_paused(AirPlaySession&)` | PAUSE 后 |
| `on_session_disconnected(AirPlaySession&)` | TEARDOWN / 断线 |
| `on_error(Status, const char* msg)` | 任何严重错误 |
| `on_log(int level, const char* msg)` | 内部日志输出 |
| `on_volume_changed(AirPlaySession&, float)` | SET_PARAMETER volume 或 /action volume |
| `on_metadata(AirPlaySession&, artist, title, album)` | 收到 /metadata 时（简易回调） |
| `on_action / on_rate / on_feedback / on_event / on_metadata` | 对应 RTSP 路由（原始 `PlistValue` 版本，功能最全） |
| `on_pin(client_ip, pin_digits)` | PIN 配对时，返回 true=接受；默认接受空/正确 |

## 4. `airplay_session.h` — `AirPlaySession`

会话对外句柄，**由 server 管理生命周期**，外部不要 `delete`。

| 方法 / 字段 | 说明 |
|-------------|------|
| `uint64_t id()` | 同进程单调递增，永不复用 |
| `std::string client_address()` | `"ip:port"` 字符串 |
| `std::string client_name()` | RTSP User-Agent |
| `State state()` | 会话状态机：`IDLE→CONNECTED→PAIRING→SETUP→READY→PLAYING⇄PAUSED→CLOSED`，任何态可 → `ERROR→CLOSED` |
| `SessionStats stats()` | 快照，统计字段 |
| `AudioConfig audio_config()` | 当前源端格式（SDP 原始描述） |
| `void disconnect()` | 主动踢客户端（幂等） |

## 5. `audio_renderer.h` — `IAudioRenderer`

音频输出抽象。**所有回调都在内部线程**（`ap2-playback`）同步调用，实现里
不要阻塞太久（>10ms），否则会爆 AudioBuffer、发生播放断续。

| 回调 | 说明 |
|------|------|
| `on_config(AudioConfig)` | 每次 ANNOUNCE 完成后触发，至少一次 |
| `on_play / on_pause / on_stop` | 状态切换通知 |
| `on_flush` | FLUSH / SEARCH 操作，清掉你本地音频设备里还没播的样本 |
| `set_volume(float v)` | v ∈ [0, 1]，线性增益 |
| `on_pcm(const void* pcm, size_t bytes, uint64_t timestamp_us)` | 20ms 粒度喂数据；`timestamp_us` 是这块音频应该"开声"的单调钟微秒（可直接喂系统 ALSA/CoreAudio 的时间戳 API） |

## 6. `airplay2.h` — 版本与辅助

- `const char* airplay2_version()`：语义化版本字符串
- `void AirPlayServer::global_init() / global_cleanup()`：跨进程全局平台初始化

## 7. 线程安全

- `AirPlayServer::create / start / stop / get_session`：**同一对象请串行调用**
  （典型都在应用主线程）。
- `AirPlaySession::stats / id / client_* / audio_config`：任意线程读，快照。
- `disconnect()`：任意线程、幂等，内部自己加锁。
- `IAudioRenderer` 回调：全部在 `ap2-playback` 单线程里串行触发，不会并发。
- `RtpReceiver`、`AesCtr`、`RtcpHandler`、`TimingHandler`：自身不暴露给
  public API，保证只在单条 `ap2-rtp` 线程访问。

## 8. 常见 FAQ

**Q：为什么连不上 / iPhone 搜不到？**
A：检查 3 件事：
- mDNS：同一子网下，接收器和 iPhone 是否同一个 WiFi；`mdns_publisher.cpp`
  默认发 224.0.0.251:5353 组播，很多 AP 默认打开"组播隔离"会导致看不到。
- 防火墙：UDP 5353、TCP 7000、UDP 5000-5020（默认端口池）要放行。
- 设备名：AirPlay 对设备名的某些 emoji/UTF-8 字符支持不好，先试 ASCII 短名。

**Q：支持视频 / 镜像吗？**
A：当前只实现音频接收器。视频路径需要 H.264/H.265 解码器和视频渲染，
以及 AirPlay Mirroring 特定的 `/play` + NAL stream 处理，不在当前范围。

**Q：支持多房间（group playback）吗？**
A：库层已经把 `/action: groupJoin / setOutputs / removeOutput` 路由和 plist
  解析暴露给 `on_action` 回调，具体多房间需要 UI 层做协调
  （主端选扬声器 → 计算 offset/delay → 分发音频）。

**Q：FairPlay 加密何时支持？**
A：FairPlay Streaming 需要苹果 MFi 项目授权 + FairPlay SDK 二进制库，
  所以无法在开源库里包含。如果您已经获得授权，从 `airplay_pairing.cpp`
  骨架对接 `on_pair_setup / on_pair_verify` 回调，即可完成接入。
