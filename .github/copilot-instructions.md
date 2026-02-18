# Copilot 指南 — StrmCtrl

快速目标：帮助 AI 代码代理立刻上手本仓库的结构、常用工作流、关键约定和修改点。

- 项目类型：C++17 + CMake（Windows / MinGW-w64 优先）。主目录 CMake 文件：`CMakeLists.txt`。
- 主要组件：
  - `demo/`：示例程序（`demo_master` = WebSocket 服务器，`demo_slave` = WebSocket 客户端）。
    - 关键文件：`demo/master.cpp`、`demo/slave.cpp`、`demo/commons.h`、`demo/CMakeLists.txt`。
  - `3rdparty/IXWebSocket`：已作为 git submodule 引入，提供 WebSocket 实现（不要修改子模块源，除非明确提交子模块更新）。

重要架构/模式（必读）
- WebSocket 通信使用 IXWebSocket（API 位于 `3rdparty/IXWebSocket/ixwebsocket`）。
- `demo/commons.h` 存放共享运行时常量（如 HOST/PORT）——所有 demo 均引用此文件。
- 在 Windows 上需调用 `ix::initNetSystem()` / `ix::uninitNetSystem()`（见两个 demo）。
- `WebSocketServer::setOnConnectionCallback` 的第一个参数为 `std::weak_ptr<ix::WebSocket>`；在回调中请先 `lock()` 再使用（见 `demo/master.cpp`）。
- 消息处理回调接收 `ix::WebSocketMessagePtr`：检查 `msg->type`（Message/Open/Close/Error）并使用 `msg->str` / `msg->errorInfo`。

构建与运行（开发者常用命令）
- 初始化仓库与子模块：
  - git clone ...; git submodule update --init --recursive
- 用 MinGW 生成并构建（Windows 示例）：
  - cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
  - cmake --build build --config Release -j 4
- 运行 demo：在两个终端分别运行 `build/demo/demo_master.exe`、`build/demo/demo_slave.exe`。在控制台输入文本并回车发送；输入 `quit` 停止。

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
- 本仓库当前没有 FFmpeg / 音视频解码集成（若添加，请注意许可和动态链接策略）。
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