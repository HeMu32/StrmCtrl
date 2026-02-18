# StrmCtrl — RTP video + text messaging toolkit

Minimal C++17 toolkit for LAN text messaging (IXWebSocket) and RTP video streaming
with FFmpeg (shared build). See `demo/` for examples and `.github/copilot-instructions.md`
for contributor guidance.

Build (Windows / MinGW example):

  cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
  cmake --build build -j 4

FFmpeg (shared build):
  cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
        -DFFMPEG_ROOT=C:/ffmpeg

Editor (VS Code): run CMake configure once; the workspace includes `.vscode/` settings
that point the language server to `build/compile_commands.json`.

Run (IXWebSocket quick check):
  build/demo/demo_master.exe
  build/demo/demo_slave.exe

Optional (FFmpeg / streaming demos):
- The core `strmctrl` library and `stream_master`/`stream_slave` demos require FFmpeg
  shared libraries to be available at CMake configure time. You can provide a
  prebuilt FFmpeg by setting `-DFFMPEG_ROOT=path/to/ffmpeg` (CMake will look for
  headers and libs under that location). If FFmpeg is not found, the project
  will still build the basic WebSocket demos (`demo_master`/`demo_slave`) but
  will skip `strmctrl` and the streaming demos.

Example (use bundled prebuilt FFmpeg):

  cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DFFMPEG_ROOT="${PWD}/3rdparty/ffmpeg"

If you want `strmctrl` enabled, ensure `ffmpeg` provides both headers and
libraries (libavcodec/libavformat/libavutil) under the `FFMPEG_ROOT` path.

Run (streaming demo):
  build/demo/stream_master.exe <peer_ip> <input_file>
  build/demo/stream_slave.exe <master_ip>
