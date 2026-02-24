````instructions
# Copilot Instructions — StrmCtrl

## Project Overview

C++17 static library (`strmctrl`) + demo programs.  
Provides **bidirectional text messaging** (WebSocket/IXWebSocket) and **one-way RTP audio+video streaming** (FFmpeg) between a **Master** node (server / sender) and one or more **Slave** nodes (client / receiver) on a LAN.  
Build system: CMake + MinGW-w64 on Windows.

---

## Code Formatting

- The project follows **Visual Studio / Microsoft coding style**. Use spaces (typically
  4 per indentation level) rather than tabs, keep opening braces on the same line as the
  statement, and match the surrounding formatting in existing source files. Running a
  configured `clang-format`/`astyle` with an MSVC profile is encouraged if available.


---

## Repository Layout

```
strmctrl/                    # Core library  (BUILD TARGET: strmctrl, STATIC)
  core/
    Message.h                #   TextMessage struct + factory
    VideoFrame.h             #   RAII AVFrame* wrapper (move-only, .clone())
    AudioFrame.h             #   RAII AVFrame* wrapper for audio (move-only, .clone())
    Callbacks.h              #   MessageCallback / VideoFrameCallback / AudioFrameCallback / ConnectionCallback
  codec/
    CodecConfig.h            #   Video encoder params; makeOpenH264() factory
    AudioConfig.h            #   Audio encoder params; makeAAC() factory
    VideoEncoder.h/.cpp      #   FFmpeg encode + swscale; PacketCallback per encoded pkt
    VideoDecoder.h/.cpp      #   FFmpeg decode; openWithParameters(AVCodecParameters*)
    AudioEncoder.h/.cpp      #   FFmpeg AAC encode; AVAudioFifo + SwrContext inside
    AudioDecoder.h/.cpp      #   FFmpeg audio decode; openWithParameters(AVCodecParameters*)
  transport/
    SignalingChannel.h/.cpp  #   WebSocket server (Master) or client (Slave); MSG:/SDP:/READY dispatch
    RtpSender.h/.cpp         #   Multi-stream avformat rtp:// output; addStream()/sendPacket()/generateSdp()
    RtpReceiver.h/.cpp       #   avformat sdp:// input; worker thread; VideoDecoder+AudioDecoder inside
  Master.h/.cpp              #   Facade: SignalingChannel(server) + VideoEncoder + AudioEncoder + RtpSender
  Slave.h/.cpp               #   Facade: SignalingChannel(client) + RtpReceiver
  CMakeLists.txt             #   Builds strmctrl static lib

demo/
  master.cpp / slave.cpp                   # Legacy IXWebSocket text-only smoke-test
  commons.h                                # Shared HOST/PORT constants for legacy demos
  stream_demo/stream_master.cpp            # Full demo: file decode -> encode (A+V) -> RTP + WS text
  stream_demo/stream_slave.cpp             # Full demo: RTP receive/decode (A+V) + WS text consumer

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

> **Note**: When running both Master and Slave on the same machine, FFmpeg's RTP sender
> automatically uses local ports offset by +100 (configurable via `RtpSender::setLocalPortBase()`)
> to avoid binding conflicts with the Slave's receive sockets.

---

## Architecture & Data Flow

```
Master                                      Slave
  SignalingChannel (WS server :11451)  <-->  SignalingChannel (WS client)
    MSG:<text>   ── text messages ──>
    SDP:REQUEST  <── auto on connect ──
    SDP:<sdp>    ── SDP offer ────────>    RtpReceiver.openWithSdp()
                                                bind UDP :11452 (video)
                                                bind UDP :11456 (audio)
                                                avformat_find_stream_info
             <── READY ────────────────    sendReady()
  rtp_ready_ = true
  VideoEncoder + AudioEncoder + RtpSender push frames ─> RtpReceiver worker thread
                                            VideoDecoder + AudioDecoder (auto from SDP)
```

SDP negotiation and the **READY handshake** are fully transparent to callers — handled inside `SignalingChannel` + `Master`/`Slave`.

---

## Connection & Startup Sequence (Critical)

The full handshake must complete before any RTP packets are sent. **Never skip or reorder these steps.**

```
1. Slave calls connect()
      └─ SignalingChannel (WS client) starts, connects to Master :11451

2. On WS Open → Slave sends "SDP:REQUEST"

3. Master receives "SDP:REQUEST" in `onSdpRequest()`
      └─ strips `:port` suffix from slave info → pure IP stored as `current_slave_ip_`
      └─ RtpSender::addStream(slave_ip, rtp_port,   video_codec_ctx)  ← video stream
      └─ RtpSender::addStream(slave_ip, rtp_port+4, audio_codec_ctx)  ← audio stream
      └─ RtpSender::setLocalPortBase(rtp_port+100)                    ← avoids localhost bind clash
      └─ RtpSender::open()
      └─ bindEncoderToSender()                          ← wires PacketCallbacks
      └─ generateSdp() + signaling_->sendSdp(sdp)

4. Slave receives "SDP:<sdp>"
      └─ RtpReceiver::openWithSdp(sdp)
            ├─ writes SDP to %TEMP%\strmctrl_recv.sdp   (binary, CRLF normalized)
            ├─ avformat_open_input  with fmt="sdp",
            │      protocol_whitelist="file,crypto,data,rtp,udp",
            │      analyzeduration=500000, probesize=1048576
            ├─ avformat_find_stream_info  (warning on "unspecified size" is OK —
            │      H.264 decoder self-configures from in-band SPS NAL units)
            └─ VideoDecoder::openWithParameters(codecpar)
            └─ AudioDecoder::openWithParameters(codecpar)  ← if audio stream present
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
master.setAudioConfig(strmctrl::AudioConfig::makeAAC(48000, 2, 128000));
master.setMessageCallback([](const strmctrl::TextMessage& m){ /* handle incoming text */ });
master.setConnectionCallback([](bool connected, const std::string& info){ });
master.setSignalingPort(11451);
master.setRtpPort(11452);
master.start();
master.pushVideoFrame(avframe_ptr);  // call per decoded source frame
master.pushAudioFrame(audio_frame);  // call per decoded audio frame
master.sendMessage("hello");
master.stop();

// ---- Slave side ----
strmctrl::Slave slave;
slave.setVideoFrameCallback([](const strmctrl::VideoFrame& f){
    auto copy = f.clone();  // MUST clone if frame is needed beyond callback scope
    // hand copy to consumer thread / queue
});
slave.setAudioFrameCallback([](const strmctrl::AudioFrame& f){
    auto copy = f.clone();  // MUST clone if frame is needed beyond callback scope
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
| `AudioFrameCallback` | RtpReceiver worker thread |
| `ConnectionCallback` | IXWebSocket internal dispatch thread |

- All callbacks are **blocking** — the firing thread is stalled until the callback returns.
- **Do not** call heavy processing, blocking I/O, or long loops directly in callbacks.
- Use a producer-consumer queue (see `stream_slave.cpp` for reference pattern).
- `VideoFrame` / `AudioFrame` lifetime is **callback-scoped** — call `.clone()` to get an independent copy.

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
| 11452 | RTP Video | UDP — Master sends, Slave binds to receive |
| 11453 | RTCP Video | UDP — auto-used by FFmpeg alongside video RTP |
| 11456 | RTP Audio | UDP — `rtp_port + 2`; Master sends, Slave binds |
| 11457 | RTCP Audio | UDP — auto-used by FFmpeg alongside audio RTP |

**Avoid UDP 5004** on Windows — `wmpnetwk.exe` (Windows Media Player Network Sharing) routinely occupies it, causing `bind failed: WSAEACCES (-10013)` on the Slave side.

**Localhost bind-collision avoidance**: When Master and Slave run on the same machine, the RTP sender sets a local port base of `rtp_port + 100` via `RtpSender::setLocalPortBase()`. This makes the sender bind its outgoing UDP sockets to ports `11552/11553` (video) and `11554/11555` (audio), well away from the receiver's ports.

**`current_slave_ip_` in Master**: The connection info string from IXWebSocket is `"ip:port"` (e.g. `"127.0.0.1:52390"`). `Master::onSlaveConnected()` always strips the `:port` suffix before storing the IP, supporting both IPv4 and IPv6 (`[::1]:port`) formats.

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
| `analyzeduration` | `500000` (0.5 s) | Gives `avformat_find_stream_info` time to receive real RTP packets; 500ms is sufficient for H.264 SPS/PPS NAL detection |
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

*(Currently no known critical issues. The previous `stream_master` looping instability, `libopenh264` encoding failures, localhost RTP bind-collision (WSAEADDRINUSE) on multi-stream setups, and FFmpeg blocking hangs during rapid reconnects have all been resolved.)*

---

## C++ Threading & FFmpeg Robustness Best Practices (Lessons Learned)

When making further modifications to `strmctrl`, strictly adhere to these robustness rules discovered during stress testing:

### 1. FFmpeg Network Blocking & Timeout (`interrupt_callback`)
- **Issue**: `avformat_open_input` and `avformat_find_stream_info` can block indefinitely when opening SDP/RTP streams if no UDP packets arrive (e.g., rapid connection teardowns before the sender pushes data).
- **Rule**: Never rely solely on AVOptions like `timeout` or `rw_timeout` for RTP/UDP. Always implement a custom `interrupt_callback` on the `AVFormatContext` before calling open/find functions. 
- **Implementation**: Provide a time-based or atomic-flag-based callback that returns `1` to force FFmpeg to abort the blocking I/O operation. Remember to clear (`nullptr`) the callback and delete its opaque context payload once the stream is successfully probed and before entering the main reading loop.

### 2. Thread Synchronization During Rapid Reconnects
- **Issue**: Async callbacks (like `onSdpReceived`) often spawn detached or managed background threads (`init_thread_`) to handle blocking FFmpeg initializations. If a teardown (`disconnect()`) is called concurrently, multiple threads might attempt to `join()` the same `std::thread` handle, causing `std::system_error` (Resource deadlock would occur) or application crashes.
- **Rule**: All modifications to `std::thread` members must be protected by a dedicated `std::mutex` (e.g., `init_mutex_`).
- **Implementation**: Never `join()` a thread while holding the mutex. Instead, move the thread handle out of the shared scope, release the lock, and then join:
  ```cpp
  std::thread old_thread;
  {
      std::lock_guard<std::mutex> lock(init_mutex_);
      if (init_thread_.joinable()) {
          old_thread = std::move(init_thread_);
      }
  }
  if (old_thread.joinable()) {
      old_thread.join();
  }
  ```

### 3. Connection Lifecycles & Data Races
- **Issue**: Callbacks triggered by third-party libraries (IXWebSocket) run on internal dispatch threads.
- **Rule**: When destroying or resetting the signaling channel, explicitly clear its callbacks (e.g., `signaling_->setConnectionCallback(nullptr);`) *before* calling `stop()` or `reset()`. This prevents reentrant or concurrent `onDisconnected` events from firing while the parent object is already partially destructed.

---

## Testing Framework & Best Practices (CTest & MiniTest)

We use CMake's CTest alongside a custom lightweight testing header (`test/MiniTest.h`) to avoid heavy third-party dependencies like GTest. 

### Writing Tests
- All test files reside in the `test/` directory.
- Define test cases as standard `void` functions (e.g., `void test_MyFeature();`) or using the `TEST(Suite, Name)` macro if included.
- **Assertions**: `MiniTest.h` provides macros that throw `std::runtime_error` on failure. These include:
  - `ASSERT_TRUE(condition)` / `EXPECT_TRUE(condition)`
  - `ASSERT_FALSE(condition)` / `EXPECT_FALSE(condition)`
  - `ASSERT_EQ(val1, val2)` / `EXPECT_EQ(val1, val2)`
- Because exceptions are used for assertions, wrapping assertions within separate threads (e.g., inside an IXWebSocket callback or FFmpeg decode thread) requires care. **Exceptions thrown in background threads will not automatically fail the test in the main thread and will terminate the program abruptly.**

### Asynchronous & Multithreaded Testing (The "Wait-and-Poll" Pattern)
Because network and AV streams are asynchronous, follow these conventions when verifying state:
1. **Use Atomic Flags**: Use `std::atomic<bool>` or thread-safe structs to capture async conditions inside callbacks.
2. **Do Not Block Forever**: Always use finite polling loops with `std::this_thread::sleep_for()` to wait for success. Never use `while(!flag) {}` without a timeout.
3. **Assert at the End**: Let the main thread poll until a timeout or success is achieved, then call `ASSERT_TRUE(flag)` in the *main thread*.
4. **Cleanup Handlers**: Before a test function exits (or when a test fixture destructs), be sure to unregister callbacks (e.g., `slave_->setVideoFrameCallback(nullptr)`) to prevent dangling references if the underlying network connection is slow to close.

**Example Asynchronous Test Pattern**:
```cpp
void test_AsyncMessage() {
    StrmCtrlTestSuite fixture;
    std::atomic<bool> msg_received = false;
    
    // Set callback (runs on a background thread)
    fixture.slave_->setMessageCallback([&](const TextMessage& msg) {
        if (msg.content == "Hello") msg_received = true;
    });

    fixture.master_->start();
    fixture.slave_->connect("127.0.0.1", fixture.signaling_port_, fixture.rtp_port_);
    
    // Trigger action
    fixture.master_->sendMessage("Hello");

    // Finite wait-and-poll loop
    for (int i = 0; i < 50; ++i) { // Max 5000ms wait
        if (msg_received) break;
        std::this_thread::sleep_for(100ms);
    }
    
    // Main thread assertion
    ASSERT_TRUE(msg_received);
    fixture.slave_->setMessageCallback(nullptr); // Cleanup
}
```

### Setup & Teardown Assumptions
- **Ports**: Avoid hardcoding port `11451`/`11452` in the test suite as rapid successive test runs might encounter `WSAEADDRINUSE` if the OS hasn't reclaimed the port (`TIME_WAIT`). Instead, use `test::PortAllocator::allocate()` from `test/TestUtils.h` which returns unique, incrementing port numbers for each test fixture.
- **Fixture Class**: Inherit or instantiate `StrmCtrlTestSuite` (defined in `StrmCtrlTest.cpp`) inside your test function to automatically get allocated isolated ports, initialized `master_`/`slave_` pointers, and safe teardown via its destructor (`slave_->disconnect()`, `master_->stop()`).

### Adding Tests to the Runner
New tests must be explicitly added to the `void run_all_tests()` block at the bottom of `test/StrmCtrlTest.cpp`:
```cpp
try { 
    test_MyFeature(); 
    std::cout << "[       OK ] MyFeature\n" << std::flush; 
    num_tests_passed++; 
} catch (const std::exception& e) { 
    std::cout << "[  FAILED  ] MyFeature: " << e.what() << "\n" << std::flush; 
    num_tests_failed++; 
}
```
If you add robustness tests that push edge boundaries, categorize them under the `// 5. Robustness Tests` section.
```
