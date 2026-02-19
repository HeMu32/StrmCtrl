# decode_test

`decode_test` 是一个**纯本地 FFmpeg 解码测试**，只验证 demux + decode 的稳定性，不包含 WebSocket / RTP / Master / Slave 任何链路。

## 目标

- 排除网络与推流链路干扰
- 复现/验证文件解码是否稳定
- 支持循环与最大帧数限制，便于快速定位异常

## 用法

```powershell
build\demo\decode_test.exe <video_path> [--loop] [--max-frames N] [--strict] [--check-every N]
```

示例：

```powershell
build\demo\decode_test.exe C:\Videos\sample.mp4
build\demo\decode_test.exe C:\Videos\sample.mp4 --loop
build\demo\decode_test.exe C:\Videos\sample.mp4 --max-frames 300
build\demo\decode_test.exe C:\Videos\sample.mp4 --check-every 10
build\demo\decode_test.exe C:\Videos\sample.mp4 --strict --check-every 1
build\demo\decode_test.exe C:\Videos\sample.mp4 --decoder libopenh264
```

## 输出示例

- 打开信息与 fps
- 每 30 帧打印一次 `frame / pts / size / pix_fmt`
- EOF 时输出总帧数
- 额外输出校验统计（坏帧数量、PTS 逆序次数等）
- 每 30 帧导出一个原始 YUV 文件到 `R:\\`（RAMDISK）

## 注意

- 该 demo 使用 FFmpeg 新式 `send_packet / receive_frame` API
- 若使用 `--loop`，会在 EOF 后重新打开文件
- `--check-every N` 控制每 N 帧做一次校验（默认 1）
- `--strict` 在发现校验失败后立即退出
- 导出的 YUV 文件名格式：`frame_<index>_<width>x<height>_<pix_fmt>.yuv`
- `--decoder <name>` 强制使用指定的 libavcodec 解码器（例如 `libopenh264`）
