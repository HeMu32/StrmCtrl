# StrmCtrl — minimal WebSocket demo + workspace helper files

Short demo project that uses IXWebSocket (submodule in 3rdparty/) and demonstrates
a master (server) / slave (client) pair. See `demo/` for examples and `.github/copilot-instructions.md`
for contributor guidance.

Build (Windows / MinGW example):

  cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
  cmake --build build -j 4

Editor (VS Code): run CMake configure once; the workspace includes `.vscode/` settings
that point the language server to `build/compile_commands.json`.

Run:
  build/demo/demo_master.exe
  build/demo/demo_slave.exe

The `stream_demo` applications also illustrate the new extensible messaging: type `gimb <text>` in either master or slave console to send a custom-prefixed `gimb:` frame, which the receiver prints.

