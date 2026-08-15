# airplay2lib — 架构设计文档

airplay2lib 是一个零第三方依赖（标准 C++17 + 平台 Socket/Thread 抽象）、跨平台的
AirPlay 2 音频接收端 C++ 库。设计目标：从 iPhone / iPad / iTunes / HomePod
发送端接收 RTSP 握手 + RTP 音频流，解码后交付用户自定义的音频渲染器。

## 1. 分层架构（由上到下）

```
┌───────────────────────────────────────────────────────┐
│           Public API（include/airplay2/*.h）           │
│  AirPlayServer / AirPlaySession / ServerConfig /      │
│  IAudioRenderer / Status / SessionStats               │
├───────────────────────────────────────────────────────┤
│           Core（src/core/*）                          │
│  ServerImpl · SessionImpl · AirPlayPairing            │
├───────────────────────────────────────────────────────┤
│           Network & Control（src/net/*, src/mdns/*）  │
│  HttpServer · RtspServer · RtpReceiver · RTCP · Timing│
│  MDNS Publisher / Browser                             │
├───────────────────────────────────────────────────────┤
│           Codec & Crypto（src/codec, src/crypto）     │
│  AlacDecoder · AudioBuffer · AES-128-CTR              │
├───────────────────────────────────────────────────────┤
│           Utilities（src/util/*）                     │
│  Apple Plist（XML + bplist）解析/序列化               │
├───────────────────────────────────────────────────────┤
│           Platform Abstraction（src/platform/*）      │
│  Socket · Thread · Time · Log（POSIX/Win32 条件编译） │
└───────────────────────────────────────────────────────┘
```

- **Public API**：唯一稳定对外层。内部类型（`SessionImpl` / `RtpReceiver` 等）
  一律不进入 `include/` 目录，避免 ABI/头文件泄露。
- **Core**：会话状态机（CONNECTED→SETUP→READY→PLAYING⇄PAUSED→CLOSED）、
  会话池管理、ANNOUNCE/SETUP/RECORD/PAUSE/TEARDOWN 串起音频管线。
- **Net & mDNS**：RTSP over HTTP（端口 7000 默认）、三 UDP 端口
  （data/ctrl/timing，连续三元组）、AirPlay 私有的 timing 协议同步、
  RFC 3550 RTCP SR/RR 反馈、mDNS（Bonjour/Avahi/内置 UDP 三后端）设备发现。
- **Codec/Crypto**：自实现 ALAC 解码器（按 SDP fmtp 的 magic cookie
  配置） + 自实现 AES-128-CTR（处理 AirPlay 加密音频）。
- **Util**：Apple Plist（XML 和 binary 两格式）解析器，给
  `/action /event /metadata /info /pair-*` 这些端点用。
- **Platform**：套接字、线程、睡眠、wallclock、日志。条件编译覆盖
  `AP2_WINDOWS / AP2_MACOS / AP2_IOS / AP2_ANDROID / AP2_LINUX` 五种
  平台；`cmake/Platform.cmake` 自动识别。

## 2. 会话 + 音频管线

一次完整 AirPlay 2 音频播放的数据流：

```
iPhone (Sender)
  │
  ├─ mDNS  ──► MDNS Publisher  ◄──► DeviceInfo（features / deviceid / port）
  │
  ├─ RTSP ANNOUNCE (SDP)  ──► parse_sdp() ──► SessionImpl::configure_audio()
  │                                     │
  │                                     ├── codec mode / sample rate / channel
  │                                     ├── AES key / iv ──► RtpReceiver::set_decryption_params
  │                                     └── ALAC configure (ALACMagicCookie fmtp)
  │
  ├─ RTSP SETUP  ──► RtpReceiver::open(min,max,ports) 绑定 3 连续 UDP
  │                ──► set_remote_address(client_ip, [X,X+1,X+2])
  │                ◄── server_port=P-P+1
  │
  ├─ RTSP RECORD ──► SessionImpl::start_streaming() 启动 2 个线程
  │                   ├─ ap2-rtp 线程 (select 3 UDP)
  │                   └─ ap2-playback 线程 (拉 PCM → 渲染)
  │
  ├─ UDP data port: RTP audio packets
  │      ▼
  │   RtpReceiver
  │     ├─ RTP header 解析 (cc / ext 跳过)
  │     ├─ AES-128-CTR process(payload) ← (密钥来自 ANNOUNCE)
  │     ├─ Jitter buffer (按 seq 重排序, 128 包深度)
  │     ├─ emit_ready() → on_rtp_packet()
  │     ▼
  │   AlacDecoder.decode_frame() → PCM bytes
  │     ▼
  │   AudioBuffer.write_bytes() → 内部 SPSC ring buffer
  │     ▼
  │   SessionImpl::playback_worker()
  │     └── IAudioRenderer::on_pcm()  → 用户实现（CoreAudio / ALSA / OpenSL / …）
  │
  ├─ UDP ctrl port: RTCP
  │      ▼
  │   RtcpHandler.handle_packet() 解析 Sender Report (NTP + RTP ts 锚点)
  │   RtpReceiver::maybe_send_rr() 每 5s 构造 RR → sendto(ctrl peer)
  │
  └─ UDP timing port: AirPlay timing request
         ▼
      TimingHandler.handle_packet() → 构造 timing response → sendto(timing peer)
```

## 3. 线程模型

| 线程 | 生命周期 | 主要职责 | 互斥点 |
|------|----------|----------|--------|
| `ap2-http-*`（多条）| TCP 连接期间 | 解析 HTTP/RTSP 请求、路由分发、写响应 | 只写各自 session 的 session 级字段（state_ 用 atomic） |
| `ap2-rtp` | RECORD → TEARDOWN | select(data/ctrl/timing 3 UDP)，收包 → 解密 → jbuffer → 回调 session | `jbuf_mu_`：与外部 `flush()` 互斥 |
| `ap2-playback` | 同上 | 定期从 ring buffer 拉 PCM 喂 `IAudioRenderer` | `AudioBuffer` 内部原子写指针，无锁 SPSC |
| `ap2-mdns` | server.start→stop | 周期发 mDNS announcement / 内置 responder 组播收发 | 独立对象，无共享 |

设计原则：**关键数据单写者**。音频、RTCP、Timing 全部在 `ap2-rtp` 单线程中完成，
不需要锁；只有"外部 flush / 统计快照"两个跨线程入口需要最小粒度锁。

## 4. 内存与性能

- 音频路径零分配热路径：`RtpAudioPacket.payload` 一次 `assign` + `vector` 预分配
  （RTP MTU ≈ 1500 B）。
- AES-128 单块 `aes128_encrypt_block` 查表实现；CTR 流式，每包前对齐
  16 字节。性能 ≈ 50 MB/s（-O2，现代 CPU），足够 44100Hz 立体声 ALAC
  （约 200 KB/s）。
- 抖动缓冲 128 包 ≈ 1 秒（44100/4096 samples ≈ 11 FPS），WiFi 环境完全够用。
- AudioBuffer：16384 帧 ring buffer（约 370 ms @ 44.1k），足够支撑
  `playback_worker` 50ms 粒度的拉取。
- ALAC 解码器：每帧独立解码，无跨帧状态（除了当前配置的 magic cookie）。

## 5. 跨平台策略

- `cmake/Platform.cmake`：在 configure 阶段检测平台，定义
  `AP2_<PLATFORM>=1`；CMake targets 不出现 `if(APPLE)` 这种直接判断，
  统一走宏。
- `platform_socket.cpp` / `platform_thread.cpp` / `platform_time.cpp`：
  同一个 API，不同平台条件编译实现（Winsock2 vs BSD Socket / pthreads vs
  Win32 threads / mach_absolute_time vs clock_gettime vs
  QueryPerformanceCounter）。
- Android 使用 NDK toolchain；iOS 用 `Toolchain-iOS.cmake`；Linux aarch64
  用 `Toolchain-Linux-aarch64.cmake`（cross-compile）；Windows 用 VS 2019+。
- mDNS 后端：macOS/iOS 用系统 DNSSD；Linux 可选 Avahi；三平台都可用
  内置零依赖 UDP multicast 实现（fallback），避免 CI 环境缺依赖时链接失败。

## 6. 扩展点

- 新增 AAC 解码：实现 `AacDecoder`，在 `SessionImpl::configure_audio` 里
  `lower_mode.find("mpeg4-generic")` 分支初始化它，`on_rtp_packet` 中
  并行 ALAC 处理。
- 新增视频：新建 `VideoPipeline` 与 `H264Decoder`，复制音频
  "RTP→抖动缓冲→解码→渲染"模式；mDNS DeviceInfo::supports_video 打开。
- FairPlay 配对：`airplay_pairing.cpp` 目前是骨架，想启用 FairPlay
  加密鉴权，接 `RtspHandlers::on_pair_setup / on_pair_verify`，需要
  苹果 MFi 证书和 FairPlay Streaming SDK。
