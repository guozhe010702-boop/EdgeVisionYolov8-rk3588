# EdgeVision-RK3588 部署工程

本项目为基于 **RK3588 NPU** 的边缘视觉目标检测部署方案，提供 C++ 与 Python 双端推理能力，支持图片、视频、摄像头及 HTTP 推流等多种输入输出方式。

---

## 目录结构

```
.
├── c_tuili/           # C++ 推理模块（基于 RKNN Model Zoo）
├── python_tuili/      # Python 推理模块（多线程异步推理）
└── moniter.sh         # NPU/CPU 实时监控脚本
```

---

## 模块说明

### 1. c_tuili — C++ 推理

基于 [RKNN Model Zoo](https://github.com/airockchip/rknn_model_zoo) 构建，提供 YOLOv8 目标检测的 C/C++ 推理实现，支持以下运行模式：

| 可执行文件 | 说明 |
|-----------|------|
| `rknn_yolov8_demo` | 单张图片推理 |
| `rknn_yolov8_demo_video` | 视频文件推理 |
| `rknn_yolov8_demo_stream` | V4L2 摄像头实时推理 + MJPEG HTTP 推流 |
| `rknn_yolov8_demo_zero_copy` | Zero-Copy 零拷贝优化推理 |

**关键技术特性：**
- 使用 **RKNN Runtime (librknnrt.so)** 调用 NPU 进行模型推理
- 集成 **RGA (Rockchip Graphics Accelerator)** 进行高效图像前处理
- 支持 **OpenCV 3.4.5** 进行图像编解码与后处理可视化
- 内置 MJPEG HTTP Server，可直接通过浏览器查看推理画面
- 支持 `RK3588` / `RK3588S` 平台 Linux aarch64 部署

**编译方式：**

```bash
cd c_tuili
./build-linux.sh -t rk3588 -a aarch64 -d yolov8
```

详细编译与运行说明请参见 [c_tuili/README_CN.md](c_tuili/README_CN.md) 及 [c_tuili/examples/yolov8/README.md](c_tuili/examples/yolov8/README.md)。

---

### 2. python_tuili — Python 推理

使用多线程异步操作 RKNN 模型，提升 RK3588 NPU 利用率与推理帧率。

**关键特性：**
- 多线程池 (`rknnpool.py`) 异步调度推理任务
- 支持自定义推理函数 (`func.py`)
- 适用于视频流或摄像头实时场景

**快速运行：**

```bash
cd python_tuili
python main.py
```

详细说明请参见 [python_tuili/README.md](python_tuili/README.md)。

---

## 硬件平台

- **芯片**: Rockchip RK3588 / RK3588S
- **NPU**: 6 TOPS 算力，支持 INT8/FP16 推理
- **系统**: Linux (aarch64)

---

## 模型

本项目使用 **YOLOv8** 目标检测模型，经 RKNN 工具链转换为 `.rknn` 格式后部署至 NPU 运行。

模型文件位置：
- `c_tuili/examples/yolov8/model/best.rknn`
- `python_tuili/rknnModel/yolov8n_rknn.rknn`

---

## 辅助工具

### moniter.sh — 性能监控

实时查看 NPU 负载与 CPU 使用率：

```bash
./moniter.sh
```

---

## 依赖环境

| 组件 | 版本/说明 |
|------|----------|
| RKNN Runtime | >= 2.0.0 |
| OpenCV | 3.4.5 |
| librga | Rockchip 官方版本 |
| CMake | >= 3.10 |
| Python | 3.8+ (python_tuili) |
| 交叉编译工具链 | aarch64-linux-gnu |

---

## 许可证

各子模块遵循其原始开源许可证，详见：
- [c_tuili/LICENSE](c_tuili/LICENSE)
- [python_tuili/LICENSE](python_tuili/LICENSE)
