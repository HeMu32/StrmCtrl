# Copilot 指南 — StrmCtrl

快速目标：帮助 AI 代码代理立刻上手本仓库的结构、常用工作流、关键约定和修改点。

- 项目类型：C++17 + CMake（Windows / MinGW-w64 优先）。主目录 CMake 文件：`CMakeLists.txt`。
- 主要组件：
  - `include/strmctrl/` + `src/`：核心库，提供主端/从端与 RTP 视频能力。
  - `demo/`：示例程序（保留 `demo_master`/`demo_slave` 作为 IXWebSocket 验证；新增 `stream_master`/`stream_slave` 展示视频推流）。
    - 关键文件：`demo/stream_master.cpp`、`demo/stream_slave.cpp`、`demo/CMakeLists.txt`。
  - `3rdparty/IXWebSocket`：已作为 git submodule 引入，提供 WebSocket 实现（不要修改子模块源，除非明确提交子模块更新）。

重要架构/模式（必读）
- WebSocket 通信使用 IXWebSocket（API 位于 `3rdparty/IXWebSocket/ixwebsocket`）。
- `demo/commons.h` 存放共享运行时常量（如 HOST/PORT）——所有 demo 均引用此文件。
- 在 Windows 上需调用 `ix::initNetSystem()` / `ix::uninitNetSystem()`（见两个 demo）。
- `WebSocketServer::setOnConnectionCallback` 的第一个参数为 `std::weak_ptr<ix::WebSocket>`；在回调中请先 `lock()` 再使用（见 `demo/master.cpp`）。
- 消息处理回调接收 `ix::WebSocketMessagePtr`：检查 `msg->type`（Message/Open/Close/Error）并使用 `msg->str` / `msg->errorInfo`。
- **视频数据流（stream_slave）**：目前的 `stream_slave` 是**Headless（无头）**的。
  - 收到 RTP 包 -> `StrmCtrl` 内部解码 -> 调用 `MessageCallbacks::on_frame`。
  - `on_frame` 回调接收 `const strmctrl::VideoFrame&`（包含 YUV 数据指针）。
  - 当前 demo 仅在控制台打印帧信息（宽/高/PTS），**不进行图形渲染**。
  - 若需渲染，需引入 SDL2/Qt/OpenCV 等外部库并在回调中处理。

构建与运行（开发者常用命令）
- 初始化仓库与子模块：
  - git clone ...; git submodule update --init --recursive
- 用 MinGW 生成并构建（Windows 示例）：
  - cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
  - cmake --build build --config Release -j 4
  - *注：若 CMake 自动找到 FFmpeg，则会构建 `stream_master`/`stream_slave`；否则仅构建基础 demo。*
- 运行 demo：
  - WebSocket 验证：`build/demo/demo_master.exe` & `build/demo/demo_slave.exe`
  - 视频推流（需 FFmpeg）：`build/demo/stream_master.exe <ip> <file>` & `build/demo/stream_slave.exe`

补充 — 指定 FFmpeg 路径（若自动查找失败）
- 本仓库可使用仓库内捆绑的 FFmpeg（位于 `3rdparty/ffmpeg`）。若存在该目录，CMake 会自动优先使用它。
- 在 MinGW/PowerShell 中构建（Windows 示例）：
- 配置：
    ```
    # If the repository includes a bundled FFmpeg under 3rdparty/ffmpeg,
    # CMake will now automatically use it. You only need to pass
    # -DFFMPEG_ROOT when you want to override or specify a different
    # FFmpeg installation.
    cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
    ```
    (PowerShell example)
    ```
    cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
    ```
  - 构建：
    ```
    cmake --build build --config Release -j 4
    ```
- 运行 demo 前需确保 FFmpeg 的运行时 DLL 可被加载（将 `3rdparty/ffmpeg/bin` 下的 `.dll` 复制到 `build/demo/`，或把该目录加入 PATH）。
- 运行 streaming demo：
  - Master (推流)：`build/demo/stream_master.exe <peer_ip> <input_file>`
  - Slave  (拉流)：`build/demo/stream_slave.exe <master_ip>`
- 若 CMake 找不到 FFmpeg，使用 `-DFFMPEG_ROOT=...` 指定路径，或设置 `FFMPEG_INCLUDE_DIR` / `FFMPEG_LIB_DIR` 环境变量。
 - 若 CMake 找不到 FFmpeg 或你想使用其他位置的 FFmpeg，请传 `-DFFMPEG_ROOT=...` 指定路径，或设置 `FFMPEG_INCLUDE_DIR` / `FFMPEG_LIB_DIR` 环境变量。
  - 注意：如果未找到 FFmpeg 的运行时库（libavcodec/libavformat/libavutil），CMake 将跳过 `strmctrl` 库和依赖于它的 `stream_master`/`stream_slave` demo；基础的 `demo_master`/`demo_slave`（IXWebSocket 验证）仍然会被构建。

编辑器 / IDE 设置（解决头文件被标红）
- 本仓库已开启 `compile_commands.json` 导出（顶层 CMake 已设置 `CMAKE_EXPORT_COMPILE_COMMANDS ON`），并在构建时自动复制到项目根目录。
- 如果 IDE 报头文件找不到（如 `#include <ixwebsocket/IXWebSocket.h>` 被标红）：
  - 先运行： `cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug` （或使用 CMake Tools 的 configure）。
  - 确认 `build/compile_commands.json` 存在（编辑器会用它来解析包含路径）。
  - VS Code 已包含 `.vscode/c_cpp_properties.json` 与 `.vscode/settings.json`，它们指向 `build/compile_commands.json` 并把 `3rdparty/IXWebSocket/ixwebsocket` 加入 include 路径。
  - 若仍显示错误：重新运行 CMake configure（或在 VS Code 中运行 CMake: Configure），然后重启语言服务（重载窗口或重启 clangd/C++ 扩展）。


项目约定与小细节（供 AI 遵循）
- 代码标准：C++17（见顶层 CMake 设置）。
- 新的二进制/库目标应添加到对应目录下的 `CMakeLists.txt`（例如 `demo/CMakeLists.txt` 已示范 `add_executable` + `target_link_libraries(ixwebsocket)`）。
- 不要将构建产物或第三方二进制提交（`.gitignore` 已配置）。
- 修改子模块时：在子模块仓库中提交更改，然后在主仓库更新子模块引用并提交 `.gitmodules`/子模块条目。

哪里开始改动（高价值修改点）
- 扩展 demo：修改 `demo/master.cpp` / `demo/slave.cpp` 或在 `demo/CMakeLists.txt` 添加新 target。
- 添加协议功能（例如二进制传输、心跳/认证）：观察并复用 IXWebSocket 回调模式（`setOnConnectionCallback` / `setOnMessageCallback`）。
- 若要新增外部库：在顶层 `CMakeLists.txt` 使用 `find_package` 或 `add_subdirectory(3rdparty/...)`（参照已存在的 IXWebSocket 引入方式）。

检查点（AI 在修改/PR 前应验证）
- 本地能通过 CMake 构建（见上面命令）。
- 不破坏子模块引用（`git submodule status`）。
- 保持 `commons.h` 的接口不破坏（它是多个 demo 的共享配置点）。

补充说明
- 本仓库已集成 FFmpeg 动态链接（MinGW-w64）。FFmpeg 需为 shared build，并通过 `FFMPEG_ROOT`/`FFMPEG_INCLUDE_DIR`/`FFMPEG_LIB_DIR` 指定路径（或设置同名环境变量）。
- demo 运行时需保证 FFmpeg DLL 可被加载（例如将 DLL 放入可执行文件同目录，或在 PATH 中）。
- 没有自动测试框架；新增功能请同时添加简单运行说明到 `README.md` 或 `demo/` 下的说明文件。

FFmpeg — 运行时合规检测（建议）
- 如果你的程序以动态方式依赖系统/用户安装的 FFmpeg，建议启动时检测目标 ffmpeg 的编译选项并拒绝或降级运行以避免意外依赖 GPL/nonfree 特性。
- 可用判断代码（C/API）示例：
  ```cpp
  #include <libavcodec/avcodec.h>
  bool ffmpeg_has_gpl_or_nonfree()
  {
      const char* cfg = avcodec_configuration();
      return strstr(cfg, "--enable-gpl") || strstr(cfg, "--enable-nonfree");
  }
  ```
  在程序启动时调用并在检测到不合规构建时打印明确错误信息、退出或降级到安全功能集。

若内容有遗漏或需把 AI 行为限制得更严格（例如禁止改动子模块、或要求强制 CI 检查），告诉我我会迭代此文件。
