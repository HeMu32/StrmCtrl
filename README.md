# StrmCtrl — WebSocket-signaled RTP streaming (Master / Slave)

A C++17 streaming framework using **IXWebSocket** for signaling and **FFmpeg** (LGPL, bundled in `3rdparty/ffmpeg`) for encoding/decoding.  Master (server) and Slave (client) negotiate video/audio codec config over WebSocket, then stream RTP packets.

## Features

- WebSocket-based signaling (SDP handshake, bidirectional message exchange, custom prefixes)
- Single-direction RTP video streaming (H.264 via OpenH264)
- Single-direction RTP audio streaming (FFmpeg AAC/MP3 encoding)
- Configurable resolution, bitrate, and frame rate (master-authoritative FPS, negotiated in handshake)
- Custom message passing over the signaling channel
- Disconnection tolerance — single-side failure without notification
- Hierarchical debug / lifecycle logging
- CTest-based test suite

## Project structure

| Path | Description |
|------|-------------|
| `strmctrl/` | Core library: `Master` / `Slave` classes, codec, transport, signaling |
| `demo/` | Demo programs: `master`/`slave`, `decode_test`, `transcode_h264` |
| `demo/stream_demo/` | Audio+video streaming demo (master + slave) |
| `test/` | Unit & regression tests (CTest) |
| `3rdparty/IXWebSocket/` | WebSocket library (git submodule) |
| `3rdparty/ffmpeg/` | Pre-built FFmpeg shared libraries |

## Build (Windows / MinGW)

```sh
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
```

## Run

```sh
# Terminal 1 — start master (signaling on port 11451, RTP on 11452)
build/demo/demo_master.exe

# Terminal 2 — start slave
build/demo/demo_slave.exe

# Stream demo (audio + video)
build/demo/stream_demo/stream_master.exe
build/demo/stream_demo/stream_slave.exe
```

## Run tests

```sh
cd build && ctest --output-on-failure
```

## Editor

VS Code workspace includes `.vscode/` settings that point the language server to `build/compile_commands.json`.  Run CMake configure once first.

See `.github/copilot-instructions.md` for contributor guidance and architecture notes.
