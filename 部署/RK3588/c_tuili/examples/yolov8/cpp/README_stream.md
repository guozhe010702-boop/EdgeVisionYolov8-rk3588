# YOLOv8 实时目标检测 — 部署与使用指南

RKNN YOLOv8 Demo 提供摄像头实时目标检测功能，支持 MJPEG-HTTP 和 RTMP-H.264 两种推流模式，适配 USB 和 MIPI 摄像头。

## 目录结构

```
rknn_yolov8_demo/
├── rknn_yolov8_demo              # 图片检测 (单帧)
├── rknn_yolov8_demo_zero_copy    # 图片检测 (NPU 零拷贝)
├── rknn_yolov8_demo_video        # 离线视频检测
├── rknn_yolov8_demo_stream       # 实时摄像头检测 + 推流 (主要使用)
├── run.sh                        # 启动脚本 (自动设置 LD_LIBRARY_PATH)
├── mediamtx                      # RTMP→WebRTC 流媒体服务器 (静态链接)
├── mediamtx.yml                  # mediamtx 配置文件 (RTMP 端口 1936)
├── deploy_libs.sh                # 依赖收集脚本 (部署到新板子时使用)
├── lib/                          # 运行时共享库 (~30 个 .so)
└── model/
    ├── best.rknn                 # RKNN 模型文件
    ├── bus.jpg                   # 测试图片
    └── coco_80_labels_list.txt   # 类别标签
```

## 快速开始

### 1. 编译 (在开发机上)

```bash
cd rknn_model_zoo-2.3.0

# 编译 (HEADLESS 模式默认开启, 去除 OpenCV highgui/videoio 依赖)
./build-linux.sh

# 收集运行时依赖到 install 目录
./deploy_libs.sh

# 确认 mediamtx.yml 已复制到 install 目录
cp mediamtx.yml install/rk3588_linux_aarch64/rknn_yolov8_demo/
```

### 2. 部署到目标板

将整个 `install/rk3588_linux_aarch64/rknn_yolov8_demo/` 目录推送到目标板：

```bash
# 方式一: scp
scp -r install/rk3588_linux_aarch64/rknn_yolov8_demo/ user@target-board:~/rknn_yolov8_demo/

# 方式二: 打包后传输
tar czf rknn_yolov8_demo.tar.gz -C install/rk3588_linux_aarch64 rknn_yolov8_demo/
scp rknn_yolov8_demo.tar.gz user@target-board:~/
```

> **注意**: 系统基础库 (glibc, libstdc++, libm 等) 不打包，目标板需有兼容版本。

### 3. 在目标板上运行

## 摄像头设备识别

不同类型的摄像头对应不同的 V4L2 设备节点，运行前请确认设备路径：

```bash
# 列出所有 V4L2 视频设备及其关联
v4l2-ctl --list-devices
```

常见设备映射（Orange Pi 5 Max 为例）：

| 设备路径 | 类型 | 格式 | 说明 |
|---------|------|------|------|
| `/dev/video20` | USB (Orbbec Astra Pro) | MJPG / YUYV | 推荐用于推流 |
| `/dev/video-camera0` → `/dev/video11` | MIPI (OV13855) | NV12 | 需要 opi5max-cam0 overlay |
| `/dev/video0` | rkcif (Bayer raw) | — | 不支持直接采集，请勿使用 |

> **提示**: USB 摄像头的设备号可能因 USB 端口变化而改变，建议用 `v4l2-ctl --list-devices` 确认后再运行。

## 推流模式

### 模式一：MJPEG over HTTP

无需 mediamtx，直接在浏览器中查看 MJPEG 流。

```bash
# USB 摄像头
./run.sh ./rknn_yolov8_demo_stream model/best.rknn /dev/video20 0 0 0 "" 8080

# MIPI 摄像头
./run.sh ./rknn_yolov8_demo_stream model/best.rknn /dev/video-camera0 0 0 0 "" 8080
```

浏览器访问：
- 局域网: `http://<板子IP>:8080`
- 本机: `http://localhost:8080`

### 模式二：RTMP H.264 + WebRTC

需要 **两个终端** 分别运行 mediamtx 和推流程序。

**终端 1 — 启动 mediamtx**（RTMP→WebRTC 转发）：

```bash
./mediamtx mediamtx.yml
```

**终端 2 — 启动推流**：

```bash
# USB 摄像头
./run.sh ./rknn_yolov8_demo_stream model/best.rknn /dev/video20 0 0 0 "" rtmp://127.0.0.1:1936/live/stream

# MIPI 摄像头
./run.sh ./rknn_yolov8_demo_stream model/best.rknn /dev/video-camera0 0 0 0 "" rtmp://127.0.0.1:1936/live/stream
```

浏览器访问 WebRTC 播放页：
- 局域网: `http://<板子IP>:8889/live/stream`
- Tailscale: `http://<Tailscale IP>:8889/live/stream`

也可通过 HLS 播放（延迟较高但兼容性好）：
- `http://<板子IP>:8888/live/stream`

## 命令行参数

```
./run.sh ./rknn_yolov8_demo_stream <model_path> <camera> [width] [height] [fps] [save_path] [push_addr]

参数说明:
  model_path    RKNN 模型文件路径 (如 model/best.rknn)
  camera        摄像头设备路径 (如 /dev/video20) 或数字 ID (如 0)
  width         请求宽度 (0 = 使用摄像头默认)
  height        请求高度 (0 = 使用摄像头默认)
  fps           请求帧率 (0 = 使用摄像头默认)
  save_path     录制保存路径 (HEADLESS 模式下无效, 传 "" 跳过)
  push_addr     推流地址: 端口号 → MJPEG 模式, rtmp:// → RTMP 模式
```

## run.sh 启动脚本

`run.sh` 会自动设置 `LD_LIBRARY_PATH`，确保程序优先从本地 `lib/` 目录加载共享库。
**必须使用 `run.sh` 启动**，直接运行可执行文件会因 OpenCV .so 的残留构建路径 RUNPATH 而失败。

```bash
# 正确 ✓
./run.sh ./rknn_yolov8_demo_stream model/best.rknn /dev/video20 0 0 0 "" 8080

# 错误 ✗ (会报 libopencv_core.so: cannot open shared object file)
./rknn_yolov8_demo_stream model/best.rknn /dev/video20 0 0 0 "" 8080
```

## mediamtx 配置

`mediamtx.yml` 已预设以下关键配置：

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| RTMP 监听 | `:1936` | 端口 1935 常被系统占用，使用 1936 替代 |
| WebRTC 监听 | `:8889` (HTTP) / `:8189` (ICE/UDP) | 浏览器 WebRTC 播放 |
| HLS 监听 | `:8888` | HLS 兼容播放 |
| RTSP 监听 | `:8554` | RTSP 协议播放 |

如需修改配置，编辑 `mediamtx.yml` 后重启 mediamtx 即可。

## 部署到新板子

当需要将程序部署到另一块开发板时：

1. **编译并打包**（在开发机上）：

```bash
./build-linux.sh
./deploy_libs.sh
cp mediamtx.yml install/rk3588_linux_aarch64/rknn_yolov8_demo/
```

2. **传输到目标板**：

```bash
scp -r install/rk3588_linux_aarch64/rknn_yolov8_demo/ user@target:~/demo/
```

3. **在目标板上运行**：

```bash
cd ~/demo
# 先启动 mediamtx (RTMP 模式需要)
./mediamtx mediamtx.yml &

# 确认摄像头设备
v4l2-ctl --list-devices

# 启动推流
./run.sh ./rknn_yolov8_demo_stream model/best.rknn /dev/videoX 0 0 0 "" rtmp://127.0.0.1:1936/live/stream
```

> **目标板要求**: 需有兼容的 glibc 版本。系统基础库 (libc, libm, libpthread, libdl, librt, libgcc_s, libstdc++) 不在打包范围内。

## 常见问题

### Q: 浏览器显示"连接被该站点断开"

**原因**: mediamtx 未启动。RTMP 推流需要 mediamtx 做 RTMP→WebRTC 转换。

**解决**:
```bash
./mediamtx mediamtx.yml
```

### Q: 运行报 `[v4l2] open /dev/video0 failed: No such device`

**原因**: `/dev/video0` 是 rkcif raw Bayer 设备，不支持直接采集。USB 摄像头设备号可能已变化。

**解决**: 用 `v4l2-ctl --list-devices` 确认实际设备号（如 `/dev/video20`）。

### Q: MIPI 摄像头 STREAMON 报 `Operation not permitted`

**原因**: MIPI OV13855 需要 `opi5max-cam0` 设备树 overlay。错误的 overlay 会导致 I2C/ISP 配置不匹配。

**解决**:
1. 检查 `/boot/orangepiEnv.txt` 中 overlays 是否包含 `opi5max-cam0`（不要用 `ov13855-c1`）
2. 重启后确认 `/dev/video-camera0` 存在且指向 `video11`

### Q: 直接运行可执行文件报 `.so: cannot open shared object file`

**原因**: OpenCV .so 文件有残留的构建路径 RUNPATH，直接运行找不到库。

**解决**: 使用 `./run.sh` 启动，它会设置 `LD_LIBRARY_PATH` 覆盖 RUNPATH。

### Q: RGA 日志输出 `COLORFILL fail: Invalid argument`

**原因**: RGA 硬件在 rgb888 格式 (3 字节/像素) 下不支持 colorfill 操作。这是 letterbox 填充的已知行为，程序会自动 CPU fallback (memset)，不影响检测和推流。

**影响**: 无。CPU 填充仅在 letterbox 边距区域执行，性能影响可忽略。

### Q: RTMP 推流显示 `HandShake: client signature does not match`

**原因**: librtmp 与 mediamtx 的 RTMP 握手实现差异，属于非致命警告。

**影响**: 不影响推流功能，可以忽略。

## 技术架构

```
┌─────────────┐     MJPG/YUYV/NV12      ┌──────────────────┐
│  V4L2 摄像头  │ ──────────────────────→ │ V4L2Capture      │
│  /dev/videoX │    (直接 ioctl, 无 libv4l2)│ (libturbojpeg 解码)│
└─────────────┘                          └────────┬─────────┘
                                                  │ RGB 帧
                                                  ▼
                                         ┌──────────────────┐
                                         │ RKNN NPU 推理     │
                                         │ (零拷贝 letterbox) │
                                         └────────┬─────────┘
                                                  │ 检测结果
                                                  ▼
                                         ┌──────────────────┐
                              ┌──────────│ 绘制检测结果       │
                              │          └──────────────────┘
                              │
                    ┌─────────┴──────────┐
                    ▼                    ▼
          ┌─────────────────┐  ┌─────────────────┐
          │ MJPEG-HTTP 推流  │  │ RTMP H.264 推流  │
          │ (tjCompress2)   │  │ (MPP 硬编码)     │
          │ 端口 8080       │  │ RGA RGB→NV12    │
          └────────┬────────┘  └────────┬────────┘
                   │                    │
                   ▼                    ▼
          ┌─────────────────┐  ┌─────────────────┐
          │ 浏览器直接查看    │  │ mediamtx        │
          │ http://IP:8080  │  │ RTMP :1936      │
          └─────────────────┘  │ WebRTC :8889    │
                               │ HLS :8888       │
                               └─────────────────┘
```

## 编译选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `HEADLESS` | ON | 去除 OpenCV highgui/videoio 依赖，适用无头板部署 |
| `HAVE_MPP_RTMP` | 自动检测 | 检测到 MPP+librtmp 时自动启用 RTMP 推流功能 |

关闭 HEADLESS 模式（恢复窗口预览和视频录制）：
```bash
# 修改 build-linux.sh 中的 cmake 参数，添加:
-DHEADLESS=OFF
```
