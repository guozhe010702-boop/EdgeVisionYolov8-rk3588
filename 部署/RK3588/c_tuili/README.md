# RKNN YOLOv8 多模式检测 Demo

基于 Rockchip [RKNN Model Zoo 2.3.0](https://github.com/airockchip/rknn_model_zoo) 的 YOLOv8
目标检测示例。在原版图片检测的基础上，新增了**离线视频检测**和**实时摄像头检测**两个模式，
实时模式支持两种推流方式：

1. **MJPEG-over-HTTP** — 兼容 [mjpg_streamer](https://github.com/jacksonliam/mjpg-streamer)，局域网内浏览器直接观看
2. **H.264 RTMP** — MPP 硬件编码 + librtmp 推流 + mediamtx 转 WebRTC，**码率仅 ~1Mbps**，通过 Tailscale 跨网络也能流畅观看

## 目录

- [支持的功能](#支持的功能)
- [目录结构](#目录结构)
- [依赖](#依赖)
- [编译](#编译)
- [部署与可移植性](#部署与可移植性)
- [Demo 用法](#demo-用法)
- [实时推流 - MJPEG 模式](#实时推流---mjpeg-模式)
- [实时推流 - H.264 RTMP 模式](#实时推流---h264-rtmp-模式)
- [跨网络观看（Tailscale）](#跨网络观看tailscale)
- [修改点（相对上游）](#修改点相对上游)
- [常见问题](#常见问题)

## 支持的功能

| 模式 | 可执行文件 | 输入 | 输出 |
| --- | --- | --- | --- |
| 图片检测 | `rknn_yolov8_demo` | 单张图片 | 标注后的图片 |
| 零拷贝图片检测 | `rknn_yolov8_demo_zero_copy` | 单张图片 | 标注后的图片（零拷贝优化） |
| 离线视频检测 | `rknn_yolov8_demo_video` | 视频文件 | 标注后的视频 |
| 实时摄像头检测 | `rknn_yolov8_demo_stream` | UVC 摄像头 | 预览 + 可选录像 + **MJPEG / RTMP 推流** |

实时检测模式特性：

- 直接 V4L2 ioctl 采集，**绕开 OpenCV videoio 与 libv4l2 兼容层**（解决 Orbbec Astra Pro 等 UVC 摄像头 `invalid fd` / `VIDIOCSPICT` 报错）
- MJPG 用 3rdparty `libturbojpeg.a` 静态链接解码，YUYV 手动转 BGR
- NPU 零拷贝推理（`rknn_create_mem` + `rknn_set_io_mem`），letterbox 直写 DMA 内存，省 1.2MB memcpy
- MJPEG 编码用 `tjCompress2`，避免 OpenCV 3.4 `cv::imencode` 在 ARM 上 segfault
- H.264 编码用 RK3588 MPP 硬件编码器（640x480@30fps, 1Mbps CBR），CPU 占用极低
- RTMP 断连自动退出进程，不浪费 CPU 空转

## 目录结构

```
.
├── build-linux.sh                  # 顶层编译脚本
├── mediamtx                        # 流媒体服务器（RTMP→WebRTC）
├── mediamtx.yml                    # mediamtx 配置
├── 3rdparty/                       # 预编译的第三方库
│   ├── opencv-3.4.5/               # OpenCV 3.4.5（含 install/）
│   ├── jpeg_turbo/                 # libturbojpeg 预编译 .a（aarch64/armhf/x64）
│   ├── rknpu1/  rknpu2/            # librknnrt.so（NPU 运行时）
│   ├── librga/                     # Rockchip RGA（图像 resize / letterbox）
│   └── ...
├── examples/yolov8/
│   ├── cpp/
│   │   ├── main.cc                 # 图片检测入口
│   │   ├── main_video.cc           # 离线视频检测入口
│   │   ├── main_stream.cc          # 实时摄像头 + 推流入口
│   │   ├── v4l2_capture.{h,cc}     # 直接 V4L2 采集 + MJPG/YUYV 解码
│   │   ├── mjpeg_http_sink.{h,cc}  # MJPEG-over-HTTP 推流
│   │   ├── rtmp_sink.{h,cc}        # MPP H.264 编码 + RTMP 推流
│   │   ├── rknpu2/yolov8.cc        # RK3588/3576/356x 实现
│   │   ├── rknpu2/yolov8_zero_copy.cc # 零拷贝推理实现
│   │   └── rknpu2/yolov8_rv1106_1103.cc # RV1106/1103 (CMA dma_buf)
│   └── model/                      # best.rknn, coco_80_labels_list.txt
├── utils/                          # imagedrawing / imageutils / fileutils
└── install/                        # make install 输出（RPATH = $ORIGIN/../lib）
    └── rk3588_linux_aarch64/rknn_yolov8_demo/
        ├── rknn_yolov8_demo{, _zero_copy, _video, _stream}
        ├── model/
        └── lib/                    # 所有运行时 .so（含 MPP/RTMP/SSL）
```

## 依赖

### 编译依赖（开发板上本机编译）

| 库 | 安装方式 | 用途 |
|---|---------|------|
| gcc / g++ / cmake | 系统自带 | 编译工具链 |
| librockchip-mpp-dev | `sudo apt install librockchip-mpp-dev` | MPP 头文件（H.264 编码） |
| librtmp-dev | `sudo apt install librtmp-dev` | RTMP 推流库 |

> 若不装 MPP/RTMP 开发包，仍可编译，只是 RTMP 推流功能不可用（CMake 会输出 WARNING）。

### 运行时依赖

CMakeLists.txt 已将以下库打包到 `install/.../lib/`，拷贝整个目录到新板子即可：

| 库 | 来源 | 说明 |
|---|------|------|
| libopencv_*.so.3.4 | 3rdparty/opencv-3.4.5 | 图像处理 |
| librknnrt.so | 3rdparty/rknpu2 | NPU 推理运行时 |
| librga.so | 3rdparty/librga | RGA 硬件加速 |
| librockchip_mpp.so* | 系统 /usr/lib | MPP H.264 编码（仅 RTMP 模式） |
| librtmp.so* | 系统 /usr/lib | RTMP 推流（仅 RTMP 模式） |
| libssl.so* / libcrypto.so* | 系统 /usr/lib | librtmp 依赖（仅 RTMP 模式） |
| libturbojpeg.a | 3rdparty/jpeg_turbo | 静态链接，无需额外 .so |

**新板子还需安装**（OpenCV 传递依赖，无法打包）：

```bash
sudo apt install libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 \
    libgtk-3-0 libavcodec58 libavformat58 libavutil56 libswscale5 \
    libv4l-0 libdc1394-25 libgomp1 zlib1g
```

### 外部工具（可选）

| 工具 | 用途 | 安装 |
|------|------|------|
| mediamtx | RTMP→WebRTC 流媒体服务器 | 项目根目录已有二进制 |
| Tailscale | 跨网络 VPN 访问 | `curl -fsSL https://tailscale.com/install.sh \| sh` |

## 编译

```bash
cd /home/orangepi/rknn_model_zoo-2.3.0
./build-linux.sh -t rk3588 -a aarch64 -d yolov8
```

参数说明：

- `-t` 目标 SoC：`rk356x` / `rk3588` / `rk3576` / `rv1106` / `rk1808` / `rv1126`
  - `rk356x` 代表 rk3562 / rk3566 / rk3568
  - `rv1106` 代表 rv1103 / rv1106
- `-a` 架构：`aarch64` / `armhf`
- `-d` demo 名，固定为 `yolov8`
- `-b` Release / Debug（可选，默认 Release）
- `-m` 开启 ASan（需要 `-b Debug`）
- `-r` 关闭 RGA（CPU resize 替代，rv1103/rv1106 无效）

交叉编译时通过 `GCC_COMPILER` 环境变量指定工具链前缀：

```bash
export GCC_COMPILER=aarch64-linux-gnu
./build-linux.sh -t rk3588 -a aarch64 -d yolov8
```

输出位置：`install/<soc>_linux_<arch>/rknn_yolov8_demo/`。

## 部署与可移植性

`make install` 会将所有运行时 .so（含 MPP/RTMP/SSL）拷贝到 `install/.../lib/`。
可执行文件 RPATH 设为 `$ORIGIN/../lib`，整个 `rknn_yolov8_demo/` 目录可直接打包发板。

### 同架构板子部署

```bash
# 开发机上打包
tar czf rknn_yolov8_demo.tar.gz -C install/rk3588_linux_aarch64 rknn_yolov8_demo

# 传到新板子
scp rknn_yolov8_demo.tar.gz user@newboard:~/
ssh user@newboard
tar xzf rknn_yolov8_demo.tar.gz
cd rknn_yolov8_demo

# 安装 OpenCV 传递依赖
sudo apt install libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 \
    libgtk-3-0 libavcodec58 libavformat58 libavutil56 libswscale5 \
    libv4l-0 libdc1394-25 libgomp1 zlib1g

# 运行
export LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH
./rknn_yolov8_demo_stream model/best.rknn /dev/video0 0 0 0 "" 8080
```

### 不同架构板子

需要重新编译（3rdparty 中有 aarch64/armhf/armhf_uclibc/x64 的预编译库）：

```bash
# 例如 RK3566 (armhf)
./build-linux.sh -t rk356x -a armhf -d yolov8
```

## Demo 用法

### 1. 图片检测

```bash
./rknn_yolov8_demo model/best.rknn model/bus.jpg
```

### 2. 零拷贝图片检测（仅 rknpu2，不支持 rv1106/1103）

```bash
./rknn_yolov8_demo_zero_copy model/best.rknn model/bus.jpg
```

### 3. 离线视频检测

```bash
./rknn_yolov8_demo_video model/best.rknn input.mp4
```

输出写到 `input.avi`。可选项：`<input.mp4> <output.avi>`。

### 4. 实时摄像头检测

```bash
./rknn_yolov8_demo_stream <model> <camera_id|device_path> \
                          [width] [height] [fps] [save_path] [push_arg]
```

- `camera_id`：整数（如 `0`）或设备路径（如 `/dev/video0`）
- `width/height/fps`：请求的分辨率与帧率，传 `0` 用默认 640x480@30
- `save_path`：可选，把带检测框的画面录成 mp4，传 `""` 不录像
- `push_arg`：推流方式，见下方两种模式

## 实时推流 - MJPEG 模式

`push_arg` 传入端口号即可开启 MJPEG-over-HTTP 推流，输出格式与 mjpg_streamer 的
`output_http.so` 完全兼容（`multipart/x-mixed-replace`）。

```bash
# 启动推流
./rknn_yolov8_demo_stream model/best.rknn /dev/video0 0 0 0 "" 8080

# 局域网浏览器打开
http://<board-ip>:8080/
```

适用场景：同一局域网内观看，码率 3-8Mbps。

## 实时推流 - H.264 RTMP 模式

`push_arg` 传入 `rtmp://` 开头的地址即开启 H.264 RTMP 推流，需要搭配 mediamtx 流媒体服务器。

### 架构

```
摄像头 → V4L2采集 → NPU推理 → MPP H.264编码 → librtmp推流
                                                    ↓
                                              mediamtx (RTMP)
                                                    ↓
                                           WebRTC / HLS 播放
```

### 启动步骤

```bash
# 1. 启动 mediamtx（后台运行）
cd /home/orangepi/rknn_model_zoo-2.3.0
./mediamtx ./mediamtx.yml &

# 2. 启动推流
cd install/rk3588_linux_aarch64/rknn_yolov8_demo
export LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH
./rknn_yolov8_demo_stream model/best.rknn /dev/video0 0 0 0 "" \
    rtmp://127.0.0.1:1936/live/stream
```

### 观看

| 方式 | 地址 | 说明 |
|------|------|------|
| WebRTC | `http://<board-ip>:8889/live/stream` | 超低延迟 (~200ms)，推荐 |
| HLS | `http://<board-ip>:8888/live/stream` | 兼容性好，延迟较高 (~3s) |
| RTSP | `rtsp://<board-ip>:8554/live/stream` | VLC 等播放器 |

> mediamtx.yml 中 RTMP 端口为 1936（非标准），因 1935 被其他进程占用。推流地址需与配置一致。

### 停止

```bash
# 停止推流程序: Ctrl+C 或
pkill rknn_yolov8_demo_stream

# 停止 mediamtx
pkill mediamtx
```

### RTMP 模式参数

| 参数 | 值 | 说明 |
|------|---|------|
| 分辨率 | 640x480 | 与摄像头采集一致（16 对齐） |
| 帧率 | 30fps | 可在代码中调整 |
| 码率 | 1Mbps CBR | 比 MJPEG 省 3-8 倍带宽 |
| 编码 | H.264 High Profile, CABAC | RK3588 VPU 硬件编码 |
| GOP | 30 | 1秒一个关键帧 |

> RTMP 连接断开后程序自动退出（不浪费 CPU 空转）。

## 跨网络观看（Tailscale）

MJPEG 模式码率过高（3-8Mbps），通过 Tailscale 等虚拟网络观看会严重卡顿。
H.264 RTMP 模式码率仅 ~1Mbps，适合跨网络观看。

### 设置步骤

1. 开发板和手机/电脑都安装 [Tailscale](https://tailscale.com) 并登录同一账号
2. 开发板上启动 mediamtx + RTMP 推流（同上）
3. 手机/电脑浏览器打开 `http://<tailscale-ip>:8889/live/stream`

查看开发板 Tailscale IP：

```bash
tailscale ip -4
# 输出类似 100.90.126.70
```

也可使用 Tailscale MagicDNS 域名：`http://<hostname>.<tailnet>.ts.net:8889/live/stream`

> Tailscale IP 是固定的（100.x.x.x），不随局域网 IP 变化。

## 修改点（相对上游）

| 文件 | 改动 |
| --- | --- |
| `build-linux.sh` | 顶部加 `set +H`，修复 bash 历史扩展导致 echo 颜色码 `\e[0m` 报错 |
| `examples/yolov8/cpp/CMakeLists.txt` | 新增 `_video` 和 `_stream` 目标；`_stream` 静态链接 libturbojpeg.a；MPP/RTMP 自动检测链接；运行时库打包到 install/lib |
| `examples/yolov8/cpp/main_video.cc` | 新增：视频文件逐帧推理 + 写视频 |
| `examples/yolov8/cpp/main_stream.cc` | 新增：实时摄像头 + 预览 + 录像 + MJPEG/RTMP 双模式推流 |
| `examples/yolov8/cpp/v4l2_capture.{h,cc}` | 新增：直接 V4L2 采集（绕开 libv4l2）+ libturbojpeg 解码 |
| `examples/yolov8/cpp/mjpeg_http_sink.{h,cc}` | 新增：MJPEG-over-HTTP 推流（兼容 mjpg_streamer） |
| `examples/yolov8/cpp/rtmp_sink.{h,cc}` | 新增：MPP H.264 编码 + RTMP 推流（异步 sender 线程） |
| `examples/yolov8/cpp/rknpu2/yolov8.cc` | 修复 RKNN 输入张量维度解析：NHWC 用 `dims[3]` 取 channel |
| `3rdparty/CMakeLists.txt` | `install(DIRECTORY ... FILES_MATCHING)` 拷贝 OpenCV .so，保留版本号软链 |

## 常见问题

**Q：浏览器连 MJPEG 推流后没有画面？**
A：PC 防火墙可能挡住了端口；或在板子上 `iptables -I INPUT -p tcp --dport 8080 -j ACCEPT`。
也可在 PC 用 VLC 打开 `http://<board-ip>:8080/` 验证。

**Q：OpenCV 报 `libopencv_core.so: cannot open shared object file`？**
A：检查 `lib/` 目录下软链是否完整（`ls -l lib/`），需要
`libopencv_core.so -> libopencv_core.so.3.4 -> libopencv_core.so.3.4.5`。
如果只剩裸文件，重新跑 `make install`。

**Q：UVC 摄像头报 `invalid fd` / `VIDIOCSPICT` 失败？**
A：用本项目自带的 `rknn_yolov8_demo_stream` 即可，已绕开 libv4l2 兼容层。

**Q：`rknn_inputs_set, param input size(409600) < model input size(1228800)`？**
A：模型是 3 通道 NHWC 输入，`dims[3]` 才是 channel。本项目 `rknpu2/yolov8.cc` 已修复。
如果换模型后仍报，请确认 RKNN 是 NHWC 还是 NCHW（看打印 `fmt=NHWC/NCHW`）。

**Q：mediamtx 报 `no tracks found`？**
A：RTMP 推流需要先发送 SPS/PPS（AVC sequence header），否则 mediamtx 无法识别视频轨道。
本项目已通过 `MPP_ENC_SET_HEADER_MODE` + `EACH_IDR` 自动嵌入 SPS/PPS。

**Q：RGA COLORFILL fail 日志刷屏？**
A：RGA 硬件不支持 rgb888 格式的 imfill 操作，代码有 CPU memset 兜底，检测功能正常。
这是日志刷屏问题，不影响功能。

**Q：换了开发板运行不起来？**
A：
1. 拷贝整个 `rknn_yolov8_demo/` 目录到新板子
2. 安装 OpenCV 传递依赖：`sudo apt install libgstreamer1.0-0 ...`（见[依赖](#依赖)）
3. 设置 `export LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH`
4. 如果新板子没有 MPP（非 Rockchip SoC），RTMP 模式不可用，请用 MJPEG 模式

**Q：MJPEG 模式通过 Tailscale 观看很卡？**
A：MJPEG 码率 3-8Mbps，不适合虚拟网络。请使用 H.264 RTMP 模式（~1Mbps），参见
[实时推流 - H.264 RTMP 模式](#实时推流---h264-rtmp-模式)。
