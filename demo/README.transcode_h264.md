# transcode_h264

`transcode_h264` 是一个**纯本地 H.264 -> H.264 转码 demo**，使用 `libopenh264` 编码输出原始 Annex-B `.h264` 码流文件。

## 用法

```powershell
build\demo\transcode_h264.exe <input_video> <output.h264> [--max-frames N] [--bitrate kbps] [--fps N] [--gop N]
```

示例：

```powershell
build\demo\transcode_h264.exe C:\Videos\sample.mp4 R:\out.h264
build\demo\transcode_h264.exe C:\Videos\sample.mp4 R:\out.h264 --max-frames 300
build\demo\transcode_h264.exe C:\Videos\sample.mp4 R:\out.h264 --bitrate 2000 --fps 30 --gop 60
```

## 输出说明

- 输出是 **裸 H.264 (Annex-B)** 文件，不带容器（可用 ffplay 播放）
- 每 30 帧打印一次进度日志

## 注意事项

- 编码器固定为 `libopenh264`，如果运行时报 “Encoder not found: libopenh264”，说明当前 FFmpeg 未编译此编码器。
- 输入帧格式若非 YUV420P，会自动转换到 YUV420P 后再编码。
