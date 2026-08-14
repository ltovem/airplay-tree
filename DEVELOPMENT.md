# airplay2lib · 开发协作约定（DEVELOPMENT）

> **给 AI / 贡献者都适用的一份项目内"规矩"。**
> 你是项目维护者时，只要在这里追加段落，下次打开会话把链接发给 AI 即可；AI 会按下列约定执行。

---

## 1. 需求 & 要求如何告知（两种方式都行）

### 方式 A · 直接在对话里说（最快，推荐日常使用）
直接在当前会话里用自然语言描述，比如：

- 功能类：`加个 X 功能，输入 A，输出 B，在平台 Y 上可用`
- 修改类：`把 Z 的默认值改成 W，并给个 setter`
- 修复类：`现象 ...，复现步骤 ...，期望 ...`
- 规则类：`以后都要 XXX 风格 / 用 XXX 库 / 不要 XXX`

> ⚠️ 你说的规则类要求，AI 会把它们追加到本文件 §6（自定义规则）并同步到项目记忆中，之后每轮对话都自动遵守。

### 方式 B · 把需求文档 / 设计稿放到仓库里（推荐正式需求）
可以在仓库里放下面这些格式的资料，然后把**文件路径或 URL**直接告诉 AI：

- Markdown / TXT / DOCX：放到 `docs/` 目录，文件名建议带日期或编号，例如 `docs/2026-08-15-fairplay-support.md`
- 接口规范：OpenAPI / RTSP 方法列表，也放到 `docs/`
- 图片 / 截图：可以直接粘贴或放在 `docs/assets/`，AI 能读图识别文本
- 网页 / 协议资料：把 URL 给出来，AI 会直接抓取

> 建议格式：背景（为什么）→ 目标（做什么）→ 验收标准（怎么算做完）→ 非目标（明确不做什么）。

---

## 2. 注释规范 · 核心规则（**"注释尽可能全面"**）

**本项目默认的哲学：源码就是文档，注释不写"代码已经说了的"，但必须写"代码说不清楚的"。**

### 2.1 哪些地方必须写注释

| 位置 | 注释内容要求 | 示例 |
|---|---|---|
| **公共头文件 `include/airplay2/*.h`** | 每个 `class / struct / enum / public 方法` 都要有 Doxygen 风格注释（`/** ... */`），说明：用途、参数含义、返回值语义、生命周期、线程安全级别、空值/越界行为、异常（或"不抛异常"） | 见 `airplay_server.h` |
| **内部头 `src/**/*.h`** | 至少给 `class` / 关键 `struct` / 公开方法写注释；**复杂成员变量要写"何时读、何时写、受哪把锁保护"** | 见 `rtp_receiver.h` `jitter_buffer_` 字段 |
| **每个 .cpp 文件顶部** | 一句话说明本文件做什么 + 关键依赖 / 外部协议引用（AirPlay / RFC 编号等） | |
| **函数** | 公开函数必须 Doxygen；内部 >15 行或逻辑不直观的函数也要写注释，覆盖：输入前提（preconditions）、输出保证（postconditions）、算法思路梗概 | |
| **"魔法数字"** | 必须给出来源或含义，例如 `// RFC 3550 RTP header`、`// AirPlay ANNOUNCE 默认采样率 44100` | |
| **锁 / 原子 / 条件变量** | 写清楚：保护对象、期望持有时长、能否递归、与其他锁的顺序 | |
| **平台分支**（`#if AP2_PLATFORM_WINDOWS` 等） | 在 `#if` 处写一句"为什么这个分支和其它平台不一样" | |
| **TODO / FIXME** | 必须带格式：`// TODO(owner): 简述 + 关联 issue 或原因`；禁止空白 TODO | |

### 2.2 注释风格统一

- 使用 **Doxygen 语法** 写公共 API：`/**` 开头，`@brief / @param / @return / @note / @warning / @sa` 标签
- 内部注释使用 C++ 行注释 `//` 多行或块注释 `/* */`，保持英文术语、中文说明混排可读
- **禁止**：`// 加一`、`// 循环` 这类字面翻译；但要写"为什么加一"、"为什么循环到这里就停"

```cpp
// ✅ 好注释
// 因为 ALAC 块最大 0xFFFF 字节，而 iOS 设备在高码率下会分块发包，
// 这里预留 2x 最大块作为抖动缓冲，避免 burst 丢包。
constexpr size_t kJitterFrames = (65535 / 2048) * 2;

// ❌ 坏注释（和代码说的一样）
buffer_size = 64; // 设置 buffer 为 64
```

### 2.3 代码自查清单（AI 修改后必过）
改完代码时，至少对照以下 5 条：

1. [ ] 新/改的 **public API** 都有 Doxygen 注释？
2. [ ] 新引入的 **锁 / 原子 / 条件变量** 注释了被保护数据？
3. [ ] 任何 `constexpr / const / #define` 中的**魔法数字**有来源？
4. [ ] 新增的**平台条件编译**说明了分支原因？
5. [ ] 算法复杂度不直观的函数，说明了思路或引用了论文/RFC？

---

## 3. 代码风格（补充 §2 之外的约束）

- C++ 标准：**C++17**（CMake 里已锁定，`std::string_view / std::optional / std::variant` 可用，C++20 features 暂时不用）
- 命名：
  - 类/struct/enum：`PascalCase`（如 `AirPlayServer`）
  - 方法/函数：`snake_case`（如 `start_server`）
  - 成员变量：`trailing_underscore_`（如 `port_`）
  - 全局常量 / 枚举值：`kPascalCase`（如 `kDefaultPort`）
  - 宏：全大写下划线，前缀 `AP2_`（如 `AP2_PLATFORM_WINDOWS`）
- **禁止**在头文件 `using namespace std;`；.cpp 里允许小范围用
- 跨平台：不要直接 `#ifdef _WIN32` / `__APPLE__`，统一用项目里的 `AP2_PLATFORM_*` 宏（见 `cmake/Platform.cmake`）

---

## 4. 目录边界

| 目录 | 内容 | 规则 |
|---|---|---|
| `include/airplay2/` | **对外公开头文件** | 只能放 API；禁止包含内部头（`src/**`） |
| `src/platform/` | OS 抽象（socket/thread/time/log） | 只能依赖 C++ 标准 + 系统 SDK；禁止引用 `mdns / net / codec / core` |
| `src/mdns/`、`src/net/`、`src/codec/`、`src/core/` | 协议栈分层 | 依赖方向：`platform → mdns/net/codec → core`，禁止反向依赖 |
| `examples/` | 用户示例 | 只允许 include `include/airplay2/`，不要直接 include `src/**`（如果确实需要，在 CMake 里单独加 `target_include_directories(... PRIVATE src)` 并注释原因） |
| `cmake/` | 工具链 + 平台检测 | CMake 专用，不出现业务代码 |

---

## 5. CI 与质量门禁

- 每次 push / PR 都会跑 `.github/workflows/ci.yml`（桌面 3 项 + iOS 3 项 + Android 3 项 + Linux ARM64 + ASan/UBSan）
- 合并门槛建议：**所有 job 变绿** 再合 main
- 新增文件：
  - 如果是 `.cpp`，必须加入 `CMakeLists.txt` 的 `AIRPLAY2_SOURCES`，否则 Linux/Android 静态库就会缺符号
  - 如果涉及跨平台 API（socket / time / 线程），跑一遍本地构建再 push

---

## 6. 自定义规则（维护者追加区）

> 追加格式：`- [YYYY-MM-DD] 规则描述`，新的在最上面。

- [2026-08-15] **注释尽可能全面**：所有公共 API 必须 Doxygen 化；内部关键类/复杂算法/锁与魔法数字都要有中文或英文注释，解释"为什么"而不是"做了什么"。
- [2026-08-15] 需求提交方式：直接对话即可；对正式需求，推荐放到 `docs/` 目录并把路径告知，含背景/目标/验收/非目标四段。
