# RKNN YOLOv8 多模式检测 Demo

基于 Rockchip [RKNN Model Zoo 2.3.0](https://github.com/airockchip/rknn_model_zoo) 的 YOLOv8
目标检测示例。在原版图片检测的基础上，新增了**离线视频检测**和**实时摄像头检测**两个模式，
实时模式还支持把带检测框的画面以 MJPEG-over-HTTP 推流到浏览器，输出格式与
[mjpg_streamer](https://github.com/jacksonliam/mjpg-streamer) 的 `output_http.so` 完全兼容。

## 目录

- [支持的功能](#支持的功能)
- [目录结构](#目录结构)
- [编译](#编译)
- [部署与可移植性](#部署与可移植性)
- [Demo 用法](#demo-用法)
- [实时推流（mjpeg）](#实时推流mjpeg)
- [修改点（相对上游）](#修改点相对上游)
- [常见问题](#常见问题)
export LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH
## 支持的功能

| 模式 | 可执行文件 | 输入 | 输出 |
| --- | --- | --- | --- |
| 图片检测 | `rknn_yolov8_demo` | 单张图片 | 标注后的图片 |
| 零拷贝图片检测 | `rknn_yolov8_demo_zero_copy` | 单张图片 | 标注后的图片（零拷贝优化） |
| **离线视频检测** | `rknn_yolov8_demo_video` | 视频文件 | 标注后的视频 |
| **实时摄像头检测** | `rknn_yolov8_demo_stream` | UVC 摄像头 | OpenCV 预览窗口 + 可选本地录像 + **MJPEG HTTP 推流** |

实时检测模式额外提供：

- 直接 V4L2 ioctl 采集，**绕开 OpenCV videoio 与 libv4l2 兼容层**（解决 Orbbec Astra Pro
  等 UVC 摄像头在某些内核下 `invalid fd` / `VIDIOCSPICT` 报错问题）。
- MJPG 用 `3rdparty/jpeg_turbo` 静态链接的 libturbojpeg 解码，YUYV 手动转 BGR。
- MJPEG 推流也用 libturbojpeg 编码（`tjCompress2`），**避免 OpenCV 3.4 `cv::imencode`
  在 ARM 平台偶发 segfault**。
- 内置 HTTP 服务器，浏览器直接打开 `http://<board-ip>:8080/` 即可看到带检测框的实时画面。

## 目录结构

```
.
├── build-linux.sh              # 顶层编译脚本
├── 3rdparty/                   # 预编译的第三方库
│   ├── opencv-3.4.5/           # OpenCV 源码（仅 Linux 编译时用）
│   ├── jpeg_turbo/             # libturbojpeg 预编译 .a
│   ├── rknpu1/  rknpu2/        # librknnrt.so
│   ├── librga/                 # Rockchip RGA（图像 resize / letterbox）
│   └── ...
├── examples/yolov8/
│   ├── cpp/
│   │   ├── main.cc             # 图片检测入口
│   │   ├── main_video.cc       # 离线视频检测入口
│   │   ├── main_stream.cc      # 实时摄像头 + 推流入口
│   │   ├── v4l2_capture.{h,cc} # 直接 V4L2 采集 + MJPG/YUYV 解码
│   │   ├── mjpeg_http_sink.{h,cc} # MJPEG-over-HTTP 推流
│   │   ├── rknpu1/yolov8.cc    # RK1808 / RV1109 / RV1126 实现
│   │   ├── rknpu2/yolov8.cc    # RK3562 / RK3566 / RK3568 / RK3588 / RK3576 实现
│   │   └── rknpu2/yolov8_rv1106_1103.cc # RV1106 / RV1103 (CMA dma_buf)
│   └── model/                  # best.rknn, coco_80_labels_list.txt
├── utils/                      # imagedrawing / imageutils / fileutils
└── install/                    # make install 输出（已配置 RPATH = $ORIGIN/../lib）
    └── rk3588_linux_aarch64/rknn_yolov8_demo/
        ├── rknn_yolov8_demo{, _zero_copy, _video, _stream}
        ├── model/
        └── lib/                # 所有运行时 .so 集中放这里
```

## 编译

需要先安装目标平台的交叉编译器（开发板上本机编译则用系统默认 gcc 即可），然后：

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

交叉编译时通过 `GCC_COMPILER` 环境变量指定工具链前缀，例如：

```bash
export GCC_COMPILER=aarch64-linux-gnu
./build-linux.sh -t rk3588 -a aarch64 -d yolov8
```

输出位置：`install/<soc>_linux_<arch>/rknn_yolov8_demo/`。

> ⚠️ `build-linux.sh` 开头已加 `set +H`，避免 `echo` 字符串里的 `!` 触发 bash 历史扩展。

## 部署与可移植性

**所有运行时库（OpenCV / librknnrt / RGA）都会在 `make install` 时拷贝到
`install/.../rknn_yolov8_demo/lib/`。** `CMakeLists.txt` 把可执行文件的 RPATH
设置为 `$ORIGIN/../lib`，因此只要把整个 `rknn_yolov8_demo/` 目录推到任意板子上，
设置 `LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH` 即可直接运行，不需要再额外装系统库。

部署示例（开发机 → 板子）：

```bash
rsync -avz install/rk3588_linux_aarch64/rknn_yolov8_demo/ \
         orangepi@<board-ip>:~/rknn_yolov8_demo/

# 在板子上
cd ~/rknn_yolov8_demo
export LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH
./rknn_yolov8_demo model/bus.jpg
```

> ⚠️ `libopencv_*.so` 用 `install(DIRECTORY)` + `FILES_MATCHING` 拷贝，
> 不能用 `install(PROGRAMS)`，否则软链断裂、目标板找不到版本号文件。

## Demo 用法

### 1. 图片检测

```bash
./rknn_yolov8_demo model/best.rknn model/bus.jpg
```

### 2. 零拷贝图片检测（仅 rknpu2，且不支持 rv1106/1103）

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
                          [width] [height] [fps] [save_path] [push_port]
```

- `camera_id`：整数（如 `0`）或设备路径（如 `/dev/video0`）。
- `width/height/fps`：请求的分辨率与帧率，传 `0` 用默认 640x480@30。
- `save_path`：可选，把带检测框的实时画面录成 mp4。
- `push_port`：可选，MJPEG HTTP 推流端口；`0` 表示不推流。
- 按 `q` 或 `ESC` 退出预览（无 DISPLAY 时不显示窗口）。

```bash
# 1) 仅预览
./rknn_yolov8_demo_stream model/best.rknn /dev/video0

# 2) 预览 + 录像
./rknn_yolov8_demo_stream model/best.rknn /dev/video0 1280 720 30 out.mp4

# 3) 预览 + 录像 + HTTP 推流
./rknn_yolov8_demo_stream model/best.rknn /dev/video0 0 0 0 "" 8080
```

## 实时推流（mjpeg）

开启 `push_port` 后程序内置一个 HTTP 服务器，输出格式与
[mjpg_streamer](https://github.com/jacksonliam/mjpg-streamer) 的 `output_http.so`
完全一致（`multipart/x-mixed-replace; boundary=mjpegboundary`），可与
`mjpg_streamer` 的网页前端 / VLC / OpenCV `VideoCapture("http://.../")` 互通。

```bash
# 板子上
./rknn_yolov8_demo_stream model/best.rknn /dev/video0 0 0 0 "" 8080
# 终端打印
#   [http] MJPEG push server listening on 0.0.0.0:8080
#   [http] open http://<board-ip>:8080/ in browser to view

# PC 浏览器
http://10.121.175.13:8080/
```

实现细节：

- 采集：自定义 [v4l2_capture.cc](examples/yolov8/cpp/v4l2_capture.cc) 走 V4L2 ioctl，
  不依赖 `libv4l2`，优先尝试 `MJPG`，失败回退 `YUYV`。
- 解码 MJPG：3rdparty `libturbojpeg.a`（静态链接，无需额外 .so）。
- 推流编码：mjpeg_http_sink 中 `tjCompress2(BGR → JPEG)`，与解码共用同一份 turbojpeg。
- 慢客户端：单次 `send` 用 `MSG_DONTWAIT`，send buffer 满 → 主动断开，不再占用主线程。

## 修改点（相对上游）

| 文件 | 改动 |
| --- | --- |
| `build-linux.sh` | 顶部加 `set +H`，修复 bash 历史扩展导致 echo 颜色码 `\e[0m` 报错 |
| `examples/yolov8/cpp/CMakeLists.txt` | 新增 `_video` 和 `_stream` 两个可执行目标；`_stream` 静态链接 `libturbojpeg.a` |
| `examples/yolov8/cpp/main_video.cc` | 新增：视频文件逐帧推理 + 写视频 |
| `examples/yolov8/cpp/main_stream.cc` | 新增：实时摄像头 + 预览 + 录像 + MJPEG 推流 |
| `examples/yolov8/cpp/v4l2_capture.{h,cc}` | 新增：直接 V4L2 采集（绕开 libv4l2）+ libturbojpeg 解码 |
| `examples/yolov8/cpp/mjpeg_http_sink.{h,cc}` | 新增：MJPEG-over-HTTP 推流（兼容 mjpg_streamer） |
| `examples/yolov8/cpp/rknpu2/yolov8.cc` | 修复 RKNN 输入张量维度解析：NHWC 用 `dims[3]` 取 channel（修复 `rknn_inputs_set, param input size(409600) < model input size(1228800)`） |
| `3rdparty/CMakeLists.txt` | `install(DIRECTORY ... FILES_MATCHING)` 拷贝 OpenCV .so，保留版本号软链 |

## 常见问题

**Q：浏览器连上去后没有画面，只有 `client X disconnected (slow or closed)`？**
A：PC 防火墙可能挡住了端口；或在板子上 `iptables -I INPUT -p tcp --dport 8080 -j ACCEPT`。
也可在 PC 用 VLC 打开 `http://<board-ip>:8080/` 验证。

**Q：OpenCV 报 `libopencv_core.so: cannot open shared object file`？**
A：检查 `install/.../lib/` 目录下软链是否完整（`ls -l lib/`），需要
`libopencv_core.so -> libopencv_core.so.3.4 -> libopencv_core.so.3.4.5`。
如果只剩孤文件，重新跑 `make install`（`install(DIRECTORY ... FILES_MATCHING)` 会重建软链）。

**Q：UVC 摄像头报 `invalid fd` / `VIDIOCSPICT` 失败？**
A：用本项目自带的 `rknn_yolov8_demo_stream` 即可，**不要**回退到 OpenCV `cv::VideoCapture`。
我们已经绕开 libv4l2 兼容层。

**Q：`rknn_inputs_set, param input size(409600) < model input size(1228800)`？**
A：模型是 3 通道 NHWC 输入，但 `dims[0]=1`、`dims[1]=H`、`dims[2]=W`、`dims[3]=C`。
本项目 `rknpu2/yolov8.cc` 已修复用 `dims[3]` 取 channel。如果换模型后仍报，请确认 RKNN
是 NHWC 还是 NCHW（看打印 `fmt=NHWC/NCHW`）。

**Q：换了开发板运行不起来？**
A：把整个 `rknn_yolov8_demo/` 目录拷过去，并在目标板上 `export
LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH` 后再运行。`lib/` 目录集中了所有 OpenCV /
librknnrt / RGA .so。
