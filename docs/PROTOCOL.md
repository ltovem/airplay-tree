# AirPlay 2 协议说明（面向库使用者）

AirPlay 并没有公开官方规范。这里的描述基于社区逆向工程（参考
[`openairplay`](https://github.com/openairplay)、`Shairport Sync`、
`RAOP` 文档、[`libplist`](https://github.com/libimobiledevice/libplist)）
以及我们库中 `src/net/rtsp_server.cpp` 已实现的部分。

## 1. 连接建立（三步握手）

AirPlay 2 接收器默认在 **TCP 7000** 上监听 RTSP/HTTP 混合协议，并通过
**mDNS（Bonjour）** 在 `_airplay._tcp.local.` 宣告自己。宣告的 TXT record
包含：

| 字段 | 含义 | 我们默认值 |
|------|------|------------|
| `deviceid` | 设备 MAC，配对时作为唯一标识 | `DeviceInfo::device_id` |
| `features` | 32-bit 位图，能力声明 | `0x5A7FFFF7`（音频全覆盖） |
| `model` | 产品模型，决定 UI 图标 | `AudioAccessory1,2` |
| `srcvers` | "AirTunes" 版本号 | `605.30.1` |
| `vv` | AirPlay 大版本 | `2` |
| `pw` | 是否需要 PIN | 0/1（`DeviceInfo::requires_encryption`） |
| `pk` | 是否需要 FairPlay 公钥配对 | 0（当前未启用） |

客户端拿到 TXT 后向 `deviceid` 宣告的 TCP 端口发起 RTSP/HTTP。

## 2. RTSP 方法与端点

AirPlay 在 RTSP 之上同时用了类似 HTTP 的 URL 路由机制：
- 经典 `ANNOUNCE / SETUP / RECORD / PAUSE / TEARDOWN / FLUSH / OPTIONS /
  GET_PARAMETER / SET_PARAMETER` — 这些方法的 URI 通常留空或 `rtsp://.../`
- 新 AirPlay 2 端点用 HTTP 方法 + 明确路径：`GET /info`、
  `POST /pair-setup /pair-verify /action /feedback /event /metadata`、
  `PUT /rate /metadata`

下表列出 **本库已经实现** 的端点（在 `RtspServer::install_routes()`）：

| 方法 | 路径 | 作用 | 响应类型 | 上层回调 |
|------|------|------|----------|----------|
| GET | `/info` | 设备能力（plist XML） | text/x-apple-plist+xml | `on_info` |
| POST | `/pair-setup` | PIN 配对第一步 | application/octet-stream | `on_pair_setup` |
| POST | `/pair-verify` | PIN 配对第二步 | application/octet-stream | `on_pair_verify` |
| OPTIONS | `*` `/` | 返回支持的方法集合 | - | 内置 |
| ANNOUNCE | `` | 发送 SDP，描述音频格式/编码 | - | `on_announce` |
| SETUP | `` | 协商 UDP 端口三元组 | Transport / Session | `on_setup` |
| RECORD | `` | 开始播放 → 启动 RTP + 解码 | Session / Audio-Latency | `on_record` |
| PAUSE | `` | 暂停，保留 RTP 端口和缓冲 | Session | `on_pause` |
| FLUSH | `` | 清空抖动/解码/渲染缓冲 | Session | `on_teardown(flush=true)` |
| TEARDOWN | `` | 结束会话，归还端口 | - | `on_teardown(flush=false)` |
| GET_PARAMETER | `` | 读取音量等参数 | text/parameters | `on_get_param` |
| SET_PARAMETER | `` | 设置音量等参数 | - | `on_set_param` |
| **POST** | **`/action`** | AirPlay 2 播放控制（plist） | binary plist | `on_action` |
| **PUT** | **`/rate`** | 播放速度/暂停恢复 | - | `on_rate` |
| **POST** | **`/feedback`** | 发送端网络反馈 | binary plist | `on_feedback` |
| **POST** | **`/event`** | 发送端事件通知 | - | `on_event` |
| **PUT/POST** | **`/metadata`** | 正在播放的曲目/封面信息 | - | `on_metadata` |

未实现（通常 iPhone 不会要求，或可通过 `on_unknown` 自定义）：

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/play` | 老 AirTunes 的 play（替代 RECORD）；现代客户端不会发 |
| POST | `/scrub` | 播放进度跳转（iPhone 一般通过 /action + param 传） |
| POST | `/setProperty` `/getProperty` | AirDisplay 视频相关属性 |
| GET | `/photo` | 照片投射协议 |

### 2.1 `/action` 里常见 command 值（binary plist dict）

`category = "playback"` 时：

| `command` | 含义 | `params` |
|-----------|------|----------|
| `play` | 开始播放 | `{ "rate": 1.0, "startTime": double_ns }` |
| `pause` | 暂停 | `{}` |
| `seekToPlaybackTime` | 拖动进度 | `{ "playbackTime": double_sec }` |
| `volume` | 音量变更 | `{ "volume": 0.0~1.0 或 -144.0~0.0 dB }` |
| `setVolume` | 同上（某些发送端变体） | `{ "volume": float }` |

`category = "group"` 时（多房间）：

| `command` | 含义 |
|-----------|------|
| `setOutputs` | 调整组里设备和相对延迟/音量 |
| `removeOutput` | 从组里移除一个设备 |
| `groupJoin` | 加入已存在的组 |

本库把 `category/command/params` 已经完整反序列化成 `util::PlistValue`，
传给 `on_action(conn_id, dict, raw_body, raw_len)`，可以直接用 `dict.get_string("command")`
取值。

### 2.2 SDP（ANNOUNCE body）关键字段

`parse_sdp()` 会提取：

- `o=- <session_id> ...`：会话 id
- `m=audio <port_min-port_max> RTP/AVP <pt>`：客户端期望的 payload type
- `c=IN IP4 <ip>`：发送端 RTP 源 IP
- `a=rtpmap:<pt> AppleLossless/44100/2` —— 也支持 `L16`、`AAC-eld`
- `a=fmtp:<pt> 0 16 4096 10 14 2 255 0 0 44100` —— ALAC MagicCookie
- `a=aeskey:<32 字节 hex>` 和 `a=aesiv:<32 字节 hex>` —— AES-128 密钥
- `a=es-parameters:aeskey=...;aesiv=...` —— 某些旧发送端的变体，会自动识别
- `a=ts-clk:<NTP>` —— RTP 时间戳锚点（可选）

## 3. RTP / RTCP / Timing 三 UDP 端口

SETUP 响应 `server_port=P-Q` 实际表示三**连续**端口：

| 本地端口 | 作用 | 发送端端口（约定） | 包类型 |
|----------|------|--------------------|--------|
| P | audio data | client_port[0] = X | RFC 3550 RTP（ALAC/AAC/PCM 负载） |
| P+1 | RTCP 控制 | X+1 | SR（sender report）→ 我们；我们定期回 RR |
| P+2 | AirPlay timing | X+2 | sender → request，我们即时回 response |

### 3.1 RTP 包格式（audio data）

- V=2, P=0, X=0 或 1（extension 存在时会跳过）
- Marker bit：一帧的最后一个 RTP 包（我们未使用，因为 ALAC 帧都是单包）
- Payload type：动态，通常 96=ALAC、97=AAC、98=L16
- Sequence：16 位 wrap-around，用于抖动缓冲重排序
- Timestamp：采样时钟（44100Hz 或 48000Hz），同步到渲染时钟的依据
- SSRC：32 位会话 id（同一个流恒定，我们用来校验是不是同一条发送端）

若启用 AES，payload 是密文：`CTR_decrypt(payload, key, iv+counter)`。
计数器按"累计加密字节 ÷ 16"递增，包与包之间不 reset——所以本库每会话
保留一个 `AesCtr` 实例，**flush 时才 reset counter**。

### 3.2 RTCP（control port）

- 收：`PT=200 Sender Report`，我们提取 `ntp_msw / ntp_lsw / rtp_ts / ssrc`
  存在 `RtcpSrInfo`，可供外部做音视频唇音对齐。
- 发：`PT=201 Receiver Report`，内含：
  - `cumulative packets lost`（来自 `RtpReceiver::stats_.lost`，通过 seq gap 统计）
  - `extended highest seq`（当前 buffer 最大 seq）
  - `interarrival jitter`（RFC 3550 A.8 平滑公式，近似值）
  - `LSR / DLSR`：从最近一次 SR 提取，供发送端估算 RTT
  - 发送间隔 ≈ 5 秒；未收 SR 之前不发

### 3.3 AirPlay 私有 Timing（timing port）

包格式：

```
 0        1        2        3
|0x80| PT=0x53 |   length=4  |   (header 4B)
|      sender SSRC (4B)       |
|      originator ts1 (4B compact NTP) |
|      [resp only] ts2 (4B)  |
|      [resp only] ts3 (4B)  |
```

- `compact NTP`：高 16 位 = NTP 秒的低 16 位；低 16 位 = NTP 小数的高 16 位
- 接收端收到 request，立即回 `PT=0x54` 的 response，填 ts2=ts3=now。
- 发送端用 `(ts3-ts1) - (ts3-ts2)` 估算 RTT，用来做多房间设备的时钟同步。
- 不回 response 会让发送端判定"远端卡住"，然后断开。

## 4. 错误处理约定

- 控制面：所有 RTSP 端点在 handler 失败时都走 HTTP/RTSP 标准错误码。
  - `400` 参数错（parse SDP 失败 / AES 长度错）
  - `401` 未授权（PIN/FairPlay 校验失败）
  - `404` 端点不存在（default handler）
  - `415` 不支持媒体格式（codec_mode 未知）
  - `503` 会话数超过 `max_sessions`
- 数据面：RTP 丢包（>128 包缓冲判为 loss）不影响会话，记录到
  `stats_.lost` 并在 RR 中上报。ALAC 单个坏帧直接丢，不会崩溃。
