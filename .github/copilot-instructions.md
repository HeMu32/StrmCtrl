````instructions
# Copilot Instructions — StrmCtrl

## Project Overview

C++17 static library (`strmctrl`) + demo programs.  
Provides **bidirectional text messaging** (WebSocket/IXWebSocket) and **one-way RTP video streaming** (FFmpeg) between a **Master** node (server / sender) and one or more **Slave** nodes (client / receiver) on a LAN.  
Build system: CMake + MinGW-w64 on Windows.

---

## Repository Layout

```
strmctrl/                    # Core library  (BUILD TARGET: strmctrl, STATIC)
  core/
    Message.h                #   TextMessage struct + factory
    VideoFrame.h             #   RAII AVFrame* wrapper (move-only, .clone())
    Callbacks.h              #   MessageCallback / VideoFrameCallback / ConnectionCallback
  codec/
    CodecConfig.h            #   Encoder params; makeOpenH264() factory
    VideoEncoder.h/.cpp      #   FFmpeg encode + swscale; PacketCallback per encoded pkt
    VideoDecoder.h/.cpp      #   FFmpeg decode; openWithParameters(AVCodecParameters*)
  transport/
    SignalingChannel.h/.cpp  #   WebSocket server (Master) or client (Slave); MSG:/SDP: dispatch
    RtpSender.h/.cpp         #   avformat rtp:// output; av_sdp_create() SDP generation
    RtpReceiver.h/.cpp       #   avformat sdp:// input; worker thread; VideoDecoder inside
  Master.h/.cpp              #   Facade: SignalingChannel(server) + VideoEncoder + RtpSender
  Slave.h/.cpp               #   Facade: SignalingChannel(client) + RtpReceiver
  CMakeLists.txt             #   Builds strmctrl static lib

demo/
  master.cpp / slave.cpp                   # Legacy IXWebSocket text-only smoke-test
  commons.h                                # Shared HOST/PORT constants for legacy demos
  stream_demo/stream_master.cpp            # Full demo: file decode -> encode -> RTP + WS text
  stream_demo/stream_slave.cpp             # Full demo: RTP receive/decode + WS text consumer

3rdparty/IXWebSocket/    # git submodule — DO NOT modify source directly
3rdparty/ffmpeg/         # LGPL shared build: include/ + lib/ (.dll.a MinGW import libs)
```

---

## Build

```powershell
# Configure (first time or after CMakeLists.txt changes)
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release

# Build everything
cmake --build build -j 4
```

Run the full streaming demo (two terminals):

```powershell
# Terminal 1 — Master (server + sender)
build\demo\stream_master.exe C:\path\to\video.mp4 11451 11452

# Terminal 2 — Slave (client + receiver)
build\demo\stream_slave.exe 127.0.0.1 11451 11452
```

---

## Architecture & Data Flow

```
Master                                      Slave
  SignalingChannel (WS server :11451)  <-->  SignalingChannel (WS client)
    MSG:<text>   ── text messages ──>
    SDP:REQUEST  <── auto on connect ──
    SDP:<sdp>    ── SDP offer ────────>    RtpReceiver.openWithSdp()
                                                bind UDP :11452
                                                avformat_find_stream_info
             <── READY ────────────────    sendReady()
  rtp_ready_ = true
  VideoEncoder + RtpSender push frames ─> RtpReceiver worker thread
                                            VideoDecoder (auto from SDP)
```

SDP negotiation and the **READY handshake** are fully transparent to callers — handled inside `SignalingChannel` + `Master`/`Slave`.

---

## Connection & Startup Sequence (Critical)

The full handshake must complete before any RTP packets are sent. **Never skip or reorder these steps.**

```
1. Slave calls connect()
      └─ SignalingChannel (WS client) starts, connects to Master :11451

2. On WS Open → Slave sends "SDP:REQUEST"

3. Master receives "SDP:REQUEST" in onSlaveConnected()
      └─ RtpSender::open(slave_ip, 11452, codec_ctx)   ← binds UDP send socket
      └─ bindEncoderToSender()                          ← wires PacketCallback
      └─ generateSdp() + signaling_->sendSdp(sdp)

4. Slave receives "SDP:<sdp>"
      └─ RtpReceiver::openWithSdp(sdp)
            ├─ writes SDP to %TEMP%\strmctrl_recv.sdp   (binary, CRLF normalized)
            ├─ avformat_open_input  with fmt="sdp",
            │      protocol_whitelist="file,crypto,data,rtp,udp",
            │      analyzeduration=3000000, probesize=1048576
            ├─ avformat_find_stream_info  (warning on "unspecified size" is OK —
            │      H.264 decoder self-configures from in-band SPS NAL units)
            └─ VideoDecoder::openWithParameters(codecpar)
      └─ RtpReceiver::start()  ← worker thread begins av_read_frame loop
      └─ signaling_->sendReady()  ← sends bare "READY" frame to Master

5. Master receives "READY"
      └─ rtp_ready_ = true  ← pushVideoFrame() now forwards to encoder
```

**Consequence**: `pushVideoFrame()` silently drops frames until `rtp_ready_` is set. This is intentional — calling code should not wait-poll `rtp_ready_`; instead rely on `ConnectionCallback` + the automatic handshake.

---

## Public API Quick-Reference

```cpp
// ---- Master side ----
strmctrl::Master master;
master.setCodecConfig(strmctrl::CodecConfig::makeOpenH264(1280, 720, 30, 2000));
master.setMessageCallback([](const strmctrl::TextMessage& m){ /* handle incoming text */ });
master.setConnectionCallback([](bool connected, const std::string& info){ });
master.setSignalingPort(11451);
master.setRtpPort(11452);
master.start();
master.pushVideoFrame(avframe_ptr);  // call per decoded source frame
master.sendMessage("hello");
master.stop();

// ---- Slave side ----
strmctrl::Slave slave;
slave.setVideoFrameCallback([](const strmctrl::VideoFrame& f){
    auto copy = f.clone();  // MUST clone if frame is needed beyond callback scope
    // hand copy to consumer thread / queue
});
slave.setMessageCallback([](const strmctrl::TextMessage& m){ });
slave.connect("192.168.1.100", 11451, 11452);
// ...
slave.disconnect();
```

---

## Critical Threading Rules

| Callback | Firing Thread |
|---|---|
| `MessageCallback` | IXWebSocket internal dispatch thread |
| `VideoFrameCallback` | RtpReceiver worker thread |
| `ConnectionCallback` | IXWebSocket internal dispatch thread |

- All callbacks are **blocking** — the firing thread is stalled until the callback returns.
- **Do not** call heavy processing, blocking I/O, or long loops directly in callbacks.
- Use a producer-consumer queue (see `stream_slave.cpp` for reference pattern).
- `VideoFrame` lifetime is **callback-scoped** — call `.clone()` to get an independent copy.

---

## Codec Extension

`CodecConfig::codec_name` is passed verbatim to `avcodec_find_encoder_by_name()`.  
No structural changes are required to add hardware encoders:

| Value | Encoder |
|---|---|
| `"libopenh264"` | Default: OpenH264 software H.264 |
| `"h264_qsv"` | Intel QSV H.264 |
| `"h264_nvenc"` | NVIDIA NVENC H.264 |
| `"h264_amf"` | AMD AMF H.264 |
| `"hevc_nvenc"` | NVIDIA NVENC HEVC |

---

## FFmpeg CMake Imported Targets

Defined in root `CMakeLists.txt` as `IMPORTED SHARED` targets:

| Target | Library |
|---|---|
| `ffmpeg::avcodec` | avcodec-62 |
| `ffmpeg::avformat` | avformat-62 |
| `ffmpeg::avutil` | avutil-60 |
| `ffmpeg::swresample` | swresample-6 |
| `ffmpeg::swscale` | swscale-9 |
| `ffmpeg::ffmpeg` | Interface bundle (all of the above) |

New demo executables only need `target_link_libraries(<target> PRIVATE strmctrl)` — FFmpeg propagates transitively via the static library.

**Runtime**: FFmpeg DLLs (`avcodec-62.dll`, `avformat-62.dll`, etc.) must be on `PATH` or placed alongside the executable.

---

## Key Conventions

- All library symbols live in `namespace strmctrl`.
- **Windows WinSock init**: Every executable entry point must call `ix::initNetSystem()` before any network use and `ix::uninitNetSystem()` before exit.
- Adding new `.cpp` files to `strmctrl/`: append them to the `STATIC` source list in `strmctrl/CMakeLists.txt`.
- `WebSocketServer::setOnConnectionCallback` first arg is `std::weak_ptr<ix::WebSocket>` — always `.lock()` before use.
- Message callback receives `ix::WebSocketMessagePtr` — check `msg->type` (Message / Open / Close / Error).
- Public API headers must have Doxygen `@brief` / `@param` / `@return` on every public method.
- Do **not** commit build artifacts or FFmpeg/IXWebSocket binaries (`.gitignore` is configured).
- `demo/commons.h` is the shared HOST/PORT config for **legacy** text-only demos — do not remove.

---

## IXWebSocket Notes

- `WebSocketServer::setOnConnectionCallback(cb)`: `cb` receives `(std::weak_ptr<ix::WebSocket>, std::shared_ptr<ix::ConnectionState>)`.
- Remote IP/port: use `connectionState->getRemoteIp()` and `connectionState->getRemotePort()`.
- Per-connection message callback set via the locked `shared_ptr<ix::WebSocket>`.
- Client: `ix::WebSocket` with `setUrl()` + `setOnMessageCallback()` + `connect()` (non-blocking) or `connectBlocking()`.

---

## Signaling Protocol (Internal)

Messages sent over the WebSocket signaling channel carry a prefix:

| Prefix | Direction | Meaning |
|---|---|---|
| `MSG:` | both | User text message — forwarded to `MessageCallback` |
| `SDP:REQUEST` | Slave → Master | Slave requests SDP offer (sent automatically on WS Open) |
| `SDP:<sdp content>` | Master → Slave | Master delivers SDP offer |
| `READY` | Slave → Master | Slave RtpReceiver is bound and ready; Master sets `rtp_ready_=true` |

**Do not remove or rename these prefixes** — they are parsed by string prefix matching in `SignalingChannel::dispatchRawMessage()`. The `READY` frame is a bare string with no colon; all others use `KEY:value` form.

---

## Port Selection Rules

| Port | Default | Purpose |
|---|---|---|
| 11451 | Signaling | WebSocket (TCP) — Master listens, Slave connects |
| 11452 | RTP | UDP — Master sends, Slave binds to receive |

**Avoid UDP 5004** on Windows — `wmpnetwk.exe` (Windows Media Player Network Sharing) routinely occupies it, causing `bind failed: WSAEACCES (-10013)` on the Slave side.

When choosing alternative ports, ensure both TCP (signaling) and UDP (RTP) on the chosen numbers are free. Check with:
```powershell
netstat -ano | findstr ":<port>"
```

---

## RtpReceiver — FFmpeg Parameter Assertions

These values are **hardcoded in `RtpReceiver::openWithUrl()`** and must not be reduced when modifying that function:

| `av_dict_set` key | Value | Reason |
|---|---|---|
| `protocol_whitelist` | `file,crypto,data,rtp,udp` | The `sdp` demuxer requires explicit protocol allowlist; omitting it causes `avformat_open_input` to fail |
| `analyzeduration` | `3000000` (3 s) | Gives `avformat_find_stream_info` time to receive real RTP packets before timing out |
| `probesize` | `1048576` (1 MB) | Probe buffer; keeps the demuxer from stopping early on sparse RTP streams |

**SDP file path**: always `%TEMP%\strmctrl_recv.sdp`, written in **binary mode** with CRLF-normalized line endings (RFC 4566 §5 requires `\r\n`). Never open it in text mode or the Windows CRT will double-convert the line endings.

**`avformat_find_stream_info` may return a negative value** for live RTP streams (warning: "unspecified size"). This is **not fatal** — the H.264 decoder self-configures from in-band SPS/PPS NAL units once packets arrive. `openWithUrl` logs the warning and continues; it does **not** call `avformat_close_input` on this path.

**`avformat_open_input` for SDP files must specify the `sdp` input format explicitly** via `av_find_input_format("sdp")`. Without it, FFmpeg guesses the format from the file extension, which fails on Windows backslash paths.

---



1. Run CMake configure (see Build section above).
2. `build/compile_commands.json` is auto-copied to project root by CMake.
3. `.vscode/c_cpp_properties.json` references `build/compile_commands.json` and adds `3rdparty/ffmpeg/include` to include paths.
4. If headers still show errors: run **CMake: Configure** from VS Code command palette, then reload the window.

---

## Submodule Management

```powershell
# Initialize after fresh clone
git submodule update --init --recursive

# Check submodule state before committing
git submodule status
```

To update IXWebSocket: make changes inside `3rdparty/IXWebSocket/`, commit there, then update the reference in the parent repo and commit `.gitmodules` + the submodule entry.

---

## FFmpeg Runtime Compliance Check (Recommended)

If using a user-supplied FFmpeg DLL, verify it is LGPL-compatible at startup:

```cpp
#include <libavcodec/avcodec.h>
bool ffmpeg_has_gpl_or_nonfree() {
    const char* cfg = avcodec_configuration();
    return strstr(cfg, "--enable-gpl") || strstr(cfg, "--enable-nonfree");
}
```

Print a clear error and exit (or disable affected features) if the check returns `true`.

---

## Known Issues

### `stream_master` Looping Instability
The `stream_master` demo with `--loop` flag exhibits unstable behavior on some environments.
Symptoms:
- Repeated "Reopen OK" followed by "Resource temporarily unavailable".
- Rapid infinite looping with no frames sent.
- EOF handling logic interactions with `avcodec_receive_frame` / `av_read_frame` are suspect.
- **Status**: Unresolved. Do not assert a specific cause without further evidence.
```
