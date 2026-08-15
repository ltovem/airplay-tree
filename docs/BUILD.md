# airplay2lib 构建指南

本文档覆盖 airplay2lib 在**所有支持平台**的构建方法：macOS、Windows、Linux、iOS、Android、Linux aarch64 交叉编译，以及 Sanitizer 质量构建。所有命令均在 CI（`.github/workflows/ci.yml`）中实测通过。

## 1. 前置依赖

| 平台 | 依赖 | 说明 |
|------|------|------|
| 通用 | CMake ≥ 3.15、支持 C++17 的编译器 | 必备 |
| macOS | Xcode 命令行工具（`xcode-select --install`） | 含 Clang |
| Windows | Visual Studio（MSVC）+ CMake | 生成器自动选最新 VS |
| Linux | gcc/g++、cmake、ninja-build | Avahi 可选（见 §4.3） |
| iOS | macOS + Xcode | 交叉编译在 mac 上进行 |
| Android | Android NDK r25c+、CMake | 交叉编译可在 Linux 上 |
| Linux aarch64 | `gcc-aarch64-linux-gnu` / `g++-aarch64-linux-gnu` | 交叉编译 |

## 2. CMake 常用选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `CMAKE_BUILD_TYPE` | Release | `Release` / `Debug` |
| `AIRPLAY2_BUILD_EXAMPLES` | ON | 构建示例程序 |
| `AIRPLAY2_BUILD_TESTS` | ON | 构建单元测试（iOS/Android/交叉编译自动关闭） |
| `AIRPLAY2_USE_AVAHI` | ON | Linux 用 Avahi mDNS；库缺失时自动回退内置 UDP mDNS |
| `CMAKE_TOOLCHAIN_FILE` | - | 交叉编译工具链（cmake/ 目录下） |
| `CMAKE_INSTALL_PREFIX` | - | 安装路径 |

## 3. macOS 原生构建

### 3.1 命令行构建（Release / Debug）

```bash
# Release（默认，能正常投屏）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAIRPLAY2_BUILD_EXAMPLES=ON
cmake --build build --target airplay2 airplay2_mac_mirror -j 8

# Debug（用于断点调试；内存错误检测更严格）
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DAIRPLAY2_BUILD_EXAMPLES=ON
cmake --build build-debug --target airplay2 airplay2_mac_mirror -j 8
```

产物：
- 库：`build/libairplay2.a`
- 镜像接收 demo（.app 双击可运行）：`build/examples/airplay2_mac_mirror.app`

### 3.2 生成 Xcode 工程（build-xcode）

推荐使用仓库内置脚本（会重新 configure + 构建 Debug）：

```bash
./scripts/gen_xcode.sh                 # 默认：生成工程 + 构建 Debug
./scripts/gen_xcode.sh --config Release # 生成 + 构建 Release
./scripts/gen_xcode.sh --no-build      # 只生成工程，不构建
```

等价的手工命令：

```bash
cmake -S . -B build-xcode -G Xcode --fresh \
    -DCMAKE_BUILD_TYPE=Release \
    -DAIRPLAY2_BUILD_EXAMPLES=ON \
    -DAIRPLAY2_BUILD_TESTS=ON \
    -DAIRPLAY2_USE_AVAHI=ON

# 命令行构建（任选配置）
xcodebuild -project build-xcode/airplay2lib.xcodeproj \
    -scheme airplay2_mac_mirror -configuration Debug build
xcodebuild -project build-xcode/airplay2lib.xcodeproj \
    -scheme airplay2_mac_mirror -configuration Release build
```

> ⚠️ **重要**：CMakeLists.txt 增删源文件后，Xcode 工程不会自动更新，必须重新跑上面的
> configure（或 `gen_xcode.sh`）。直接改 `.xcodeproj` 会导致构建的是旧文件列表。
>
> ⚠️ **Xcode 点 Run 的配置**：scheme 的 LaunchAction 已设为 `Release`（与 cmake
> 构建行为一致）。若手工生成过工程，注意 Debug 配置下 malloc 严格检查会暴露
> Release 掩盖的内存错误，表现为"cmake 能投屏、Xcode 崩"。

打开 Xcode 调试：

```bash
open build-xcode/airplay2lib.xcodeproj
```

scheme 选 `airplay2_mac_mirror`，目标 `My Mac`，⌘R 运行。

## 4. Linux（GCC）

### 4.1 原生构建（x86_64）

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build \
    libavahi-client-dev libavahi-common-dev   # Avahi 可选，见 4.3

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAIRPLAY2_BUILD_EXAMPLES=ON
cmake --build build --parallel 3
```

### 4.2 不用 Avahi（内置 UDP mDNS）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DAIRPLAY2_USE_AVAHI=0 -DAIRPLAY2_BUILD_EXAMPLES=ON
cmake --build build --parallel 3
```

### 4.3 Avahi 回退说明

`AIRPLAY2_USE_AVAHI=ON` 但系统缺 libavahi-client 时，构建**不会失败**——
CMake 找不到库就打印警告并回退到内置 UDP mDNS responder（零依赖）。

## 5. Windows（MSVC x64）

```powershell
# 在 VS 开发者命令行或任意 shell 中（CMake 自动选最新 VS 生成器）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -A x64 -DAIRPLAY2_BUILD_EXAMPLES=ON
cmake --build build --config Release --parallel 3
```

> 不要硬编码 `-G "Visual Studio 17 2022"`；`-A x64` 会让 CMake 自动选择已安装的
> 最新 VS 生成器。

## 6. iOS 交叉编译（在 macOS 上进行）

真机 arm64：

```bash
cmake -S . -B build-ios \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-iOS.cmake \
    -DIOS_PLATFORM=OS \
    -DCMAKE_SYSTEM_PROCESSOR=arm64 \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DIPHONEOS_DEPLOYMENT_TARGET=12.0 \
    -DAIRPLAY2_BUILD_EXAMPLES=OFF
cmake --build build-ios --config Release --parallel 3
```

模拟器（x86_64 或 arm64，`IOS_PLATFORM=SIMULATOR`）：

```bash
cmake -S . -B build-ios \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-iOS.cmake \
    -DIOS_PLATFORM=SIMULATOR \
    -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DIPHONEOS_DEPLOYMENT_TARGET=12.0 \
    -DAIRPLAY2_BUILD_EXAMPLES=OFF
cmake --build build-ios --config Release --parallel 3
```

## 7. Android NDK 交叉编译

需要 Android NDK（r25c 已验证）。NDK 路径按以下优先级自动探测：
`ANDROID_NDK_LATEST_HOME` → `ANDROID_NDK_HOME` → `ANDROID_HOME/ndk-bundle`，
也可显式传 `-DANDROID_NDK=...`。

```bash
cmake -S . -B build-android \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-Android.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-24 \
    -DANDROID_NDK="${ANDROID_NDK}" \
    -DAIRPLAY2_BUILD_EXAMPLES=OFF
cmake --build build-android --config Release --parallel 3
```

`ANDROID_ABI` 可选：`armeabi-v7a` / `arm64-v8a` / `x86_64`。

## 8. Linux aarch64 交叉编译

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

cmake -S . -B build-arm64 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-Linux-aarch64.cmake
cmake --build build-arm64 --config Release --parallel 3

# 验证 ELF 架构
file build-arm64/libairplay2.a
```

## 9. Sanitizer 构建（内存 / UB 检查，Linux）

```bash
cmake -S . -B build-san \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined" \
    -DAIRPLAY2_BUILD_EXAMPLES=ON
cmake --build build-san --parallel 3
```

## 10. 单元测试（桌面三平台）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAIRPLAY2_BUILD_EXAMPLES=ON
cmake --build build --parallel 3
cd build && ctest --output-on-failure -j 2
```

iOS / Android / 交叉编译自动跳过测试（宿主不能运行目标二进制）。

## 11. 常见问题

| 现象 | 原因 | 解决 |
|------|------|------|
| cmake 能投屏、Xcode 点 Run 崩 | Xcode scheme 用了 Debug 配置 | scheme LaunchAction 改 `Release`，或 `gen_xcode.sh` 重建 |
| 改了 CMakeLists 但 Xcode 没生效 | `.xcodeproj` 未重新生成 | 重跑 `./scripts/gen_xcode.sh` |
| 启动报"端口被占用" | 上一个实例未退出 | 先退出旧实例再启动 |
| Linux 缺 Avahi 编译告警 | 系统未装 libavahi-client | 忽略（自动回退内置 mDNS）或 `apt install libavahi-client-dev` |
| Windows 找不到 VS | 未装 VS / 用错了生成器 | 只传 `-A x64`，让 CMake 自动选生成器 |

## 12. 构建产物速查

| 平台 | 库产物 | demo/示例 |
|------|--------|-----------|
| macOS | `build/libairplay2.a` | `build/examples/airplay2_mac_mirror.app` |
| Windows | `build/Release/airplay2.lib` | `build/examples/Release/*.exe` |
| Linux | `build/libairplay2.a` | `build/examples/airplay2_basic_server` |
| iOS | `build-ios/libairplay2.a` | - |
| Android | `build-android/libairplay2.a` | - |
| Linux aarch64 | `build-arm64/libairplay2.a` | - |
