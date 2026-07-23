# 可移植性与健壮性增强计划

## Context

用户即将更换开发板，希望代码能在不同 Rockchip 板子间快速移植。当前代码存在以下影响移植的问题：
- CMakeLists.txt 中 MPP/RTMP 找不到时仍编译 rtmp_sink.cc 导致编译失败
- image_utils.c 有 2 个严重 bug（位运算符写错 + 错误路径未 return）会导致 crash
- 默认值分散不一致（fps 默认值 25 vs 30）
- V4L2 失败重试逻辑与实际帧率脱钩
- 运行时缺乏能力检测（能否用 RTMP、是否有 MPP 等）
- image_utils.c 每帧刷 "finish" 日志

**不改动范围**：RKNN/RGA/MPP 的核心推理编码逻辑不变，librtmp 内部结构体访问不变（风险大收益低），DMA heap 路径不变（仅 RV1106 使用）。

---

## 修改计划

### 1. CMakeLists.txt — MPP/RTMP 条件编译

**文件**: `examples/yolov8/cpp/CMakeLists.txt`

**问题**: MPP 或 RTMP 找不到时，`rtmp_sink.cc` 仍被编入 `_stream` 目标，MPP 头文件缺失导致编译失败。

**修改**:
- 添加 `HAVE_MPP_RTMP` 编译定义，仅当 MPP 和 RTMP 都找到时启用
- `rtmp_sink.cc` 仅在 `HAVE_MPP_RTMP` 时加入源文件列表
- 未找到时 rtmp_sink 相关代码通过 `#ifdef HAVE_MPP_RTMP` 跳过（在 main_stream.cc 中）
- 同时扩展 `find_library` 搜索路径，增加 `CMAKE_SYSROOT` 和 `/usr/lib/${CMAKE_SYSTEM_PROCESSOR}` 模式路径

### 2. main_stream.cc — 运行时能力检测 + 默认值统一

**文件**: `examples/yolov8/cpp/main_stream.cc`

**修改**:
- `#include "rtmp_sink.h"` 用 `#ifdef HAVE_MPP_RTMP` 包裹
- RTMP 相关代码块用 `#ifdef HAVE_MPP_RTMP` 包裹，未定义时 rtmp:// 参数给出明确提示
- fps 默认值统一：`fps <= 0` 时使用 V4L2Capture 的实际返回值，不再硬编 25.0
- v4l2_fail_streak 阈值改为基于时间的：`v4l2_fail_streak * retry_interval_us > 2_000_000`（2秒），而非硬编 30
- 命令行参数校验：`atoi` 改用 `strtol` + 错误检测，对非法数字给出提示
- 日志节流 `frame_index % 30` 改为 `frame_index % fps_interval`，与实际帧率挂钩

### 3. image_utils.c — 修复严重 bug + 日志清理

**文件**: `utils/image_utils.c`

**修改**:
- **第 279、281 行**: `|` → `||`，修复位运算符 bug（当前所有非 jpg 扩展名都走 png 路径）
- **第 64-79 行**: `read_file_data` 中 `fopen`/`fseek`/`malloc`/`fread` 失败后加 `return NULL`（当前仅 printf 然后继续，空指针 crash）
- **第 449 行**: `printf("finish\n")` 删除（每帧输出一次，30fps 下日志洪泛）
- **第 762-764 行**: `convert_image_with_letterbox` 的 scale/box printf 删除（同样每帧输出）

### 4. v4l2_capture.cc — 错误处理增强

**文件**: `examples/yolov8/cpp/v4l2_capture.cc`

**修改**:
- `convert_yuyv_to_rgb` / `convert_uyvy_to_rgb` stride 越界时，设置 `out = cv::Mat()` 并 `return`，让调用方能通过 `empty()` 检测异常（当前返回全零 Mat 无法区分）
- `open()` 中 VIDIOC_S_PARM 失败时打印调试日志（方便排查帧率问题）

### 5. rtmp_sink.h — 编译期守卫

**文件**: `examples/yolov8/cpp/rtmp_sink.h`

**修改**:
- 文件顶部加 `#ifdef HAVE_MPP_RTMP` 守卫，未定义时类声明整个跳过
- 或更简单：只在 main_stream.cc 的 include 处加守卫（改动更小）

---

## 不改动的项目（原因说明）

| 项目 | 原因 |
|------|------|
| librtmp 内部结构体访问 | 已在当前平台验证可用，改动风险大收益低 |
| DMA heap 设备路径硬编码 | 仅影响 RV1106/1103，有 `#if defined(RV1106_1103)` 编译守卫 |
| RKNN/RGA/MPP 软件 fallback | 工作量巨大，超出"换开发板快上手"的范围 |
| MJPEG 质量/GOP 等运行时可配 | nice-to-have，非移植阻断问题 |
| cv::namedWindow 无头环境 | OpenCV highgui 已有内部保护，不会 crash |

---

## 验证方式

1. **有 MPP/RTMP 时**：正常编译运行，RTMP 推流功能不变
   ```bash
   ./build-linux.sh -t rk3588 -a aarch64 -d yolov8
   # 运行 MIPI 和 USB 摄像头测试
   ```

2. **无 MPP/RTMP 时**（模拟）：
   ```bash
   # 临时移走 MPP 头文件验证条件编译
   sudo mv /usr/include/rockchip /usr/include/rockchip.bak
   ./build-linux.sh -t rk3588 -a aarch64 -d yolov8
   # 应编译成功，_stream 目标可用 MJPEG 模式，RTMP 模式给出提示
   sudo mv /usr/include/rockchip.bak /usr/include/rockchip
   ```

3. **image_utils bug 修复验证**：保存 .png 文件确认不再走错路径

4. **端到端测试**：
   ```bash
   # MIPI MJPEG
   ./rknn_yolov8_demo_stream model/best.rknn /dev/video-camera0 0 0 0 "" 8080
   # USB RTMP
   ./rknn_yolov8_demo_stream model/best.rknn /dev/video20 0 0 0 "" rtmp://127.0.0.1:1936/live/stream
   ```
