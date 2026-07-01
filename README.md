# EdgeVision-RK3588

基于 **Rockchip RK3588 NPU** 的边缘视觉目标检测全栈工程，覆盖 **模型训练 → 量化转换 → 边缘部署 → 多路推理** 完整 pipeline。

项目以 YOLOv8 为核心检测网络，针对宿舍场景自建数据集完成训练，并通过 QAT 量化感知训练、RKNN 工具链转换、C++/Python 双端推理优化，在 RK3588 边缘设备上实现高效能、低延迟的实时目标检测。

---

## 目录

- [项目概览](#项目概览)
- [整体架构](#整体架构)
- [目录结构](#目录结构)
- [模块详解](#模块详解)
  - [1. 训练 — YOLOv8 模型训练与 QAT](#1-训练--yolov8-模型训练与-qat)
  - [2. 转换 — ONNX → RKNN 模型转换](#2-转换--onnx--rknn-模型转换)
  - [3. 部署 — RK3588 边缘推理](#3-部署--rk3588-边缘推理)
- [快速开始](#快速开始)
- [性能参考](#性能参考)
- [硬件环境](#硬件环境)
- [依赖与版本](#依赖与版本)
- [许可证](#许可证)

---

## 项目概览

| 维度 | 说明 |
|------|------|
| **检测网络** | YOLOv8n/s/m（支持 detect / segment / pose / obb 多任务） |
| **训练框架** | Ultralytics YOLOv8 + PyTorch |
| **量化方案** | QAT 量化感知训练（PyTorch FakeQuantize）+ RKNN Post-Training Quantization |
| **目标平台** | Rockchip RK3588 / RK3588S（6 TOPS NPU，支持 INT8/FP16） |
| **推理语言** | C++（高性能单路/推流）、Python（多线程异步池） |
| **典型应用** | 宿舍场景人员/物品检测、边缘端实时视频分析 |

---

## 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                      EdgeVision-RK3588                       │
├─────────────────┬─────────────────┬─────────────────────────┤
│   1. 训练        │   2. 转换        │   3. 部署 (RK3588)      │
│  (PC/服务器)     │  (PC/服务器)     │   (边缘设备 Linux)       │
├─────────────────┼─────────────────┼─────────────────────────┤
│ • YOLOv8 训练   │ • ONNX 导出     │ • C++ 推理 (RKNN Model  │
│ • QAT 伪量化    │ • RKNN 转换     │   Zoo)                  │
│   微调训练      │ • INT8 量化校准 │ • Python 多线程异步池    │
│ • 精度对比验证  │                 │ • RGA 硬件预处理        │
│ • 导出 ONNX     │                 │ • Zero-Copy 零拷贝      │
│                 │                 │ • MJPEG HTTP 推流       │
└─────────────────┴─────────────────┴─────────────────────────┘
```

**流程说明**：
1. **训练**：在 PC/服务器上使用自定义数据集训练 YOLOv8 模型；可选执行 QAT 量化感知训练，降低后续 INT8 量化精度损失。
2. **转换**：将训练好的 `.pt` 模型导出为 ONNX，再通过 RKNN-Toolkit2 转换为 `.rknn` 格式，并完成 INT8 量化校准。
3. **部署**：将 `.rknn` 模型部署到 RK3588 设备，通过 C++ 或 Python 推理引擎调用 NPU 进行加速推理。

---

## 目录结构

```
EdgeVision-RK3588/
├── 训练/
│   └── ultralytics_yolov8-main/          # YOLOv8 训练与导出工程
│       ├── train.py                       # 基础训练入口
│       ├── QAT.py                         # 量化感知训练 (QAT)
│       ├── QAT_Test.py                    # QAT 精度对比测试
│       ├── Convert.py                     # 导出 ONNX / RKNN
│       ├── RKOPT_README.zh-CN.md          # RK NPU 适配导出说明
│       └── datasets/shushe.yaml           # 宿舍场景数据集配置
│
├── 转换/
│   └── convert2rknn/
│       ├── convert.py                     # ONNX → RKNN 转换脚本
│       ├── generate_paths_txt.py          # 生成量化校准图片路径
│       ├── best.rknn                      # 转换后的 RKNN 模型
│       └── shushe/images/                 # 量化校准数据集
│
└── 部署/
    └── RK3588/
        ├── c_tuili/                       # C++ 推理模块 (RKNN Model Zoo)
        │   ├── examples/yolov8/cpp/       # YOLOv8 C++ 推理源码
        │   ├── build-linux.sh             # Linux 交叉编译脚本
        │   └── ...                        # RGA / OpenCV / RKNN Runtime
        ├── python_tuili/                  # Python 多线程异步推理
        │   ├── main.py                    # Python 推理主程序
        │   ├── rknnpool.py                # RKNN 线程池管理
        │   └── func.py                    # 自定义推理后处理
        └── moniter.sh                     # NPU / CPU 实时监控脚本
```

---

## 模块详解

### 1. 训练 — YOLOv8 模型训练与 QAT

**路径**: `训练/ultralytics_yolov8-main/`

本模块基于 [Ultralytics YOLOv8](https://github.com/ultralytics/ultralytics) 修改，针对 RK NPU 做了输出结构适配（移除对量化不友好的后处理节点，将 DFL 移至外部 CPU 处理）。

#### 1.1 基础训练

```bash
cd 训练/ultralytics_yolov8-main
python train.py
```

- 默认加载 `yolov8n.pt` 预训练权重
- 数据集配置：`datasets/shushe.yaml`（宿舍场景）
- 训练 50 epoch，自动评估验证集

#### 1.2 量化感知训练 (QAT)

为降低 RKNN INT8 量化带来的精度损失，提供两阶段 QAT 训练：

```bash
python QAT.py
```

**训练阶段**：
1. **浮点预热训练**（10 epochs）：在浮点模式下微调模型，保存预热权重。
2. **伪量化 QAT 训练**（50 epochs）：插入 `QuantStub` / `DeQuantStub` / `FakeQuantize` 节点，在模拟 INT8 环境下继续训练，使权重适应量化分布。
3. **保存模型**：剥离量化节点后保存为 `best_float32_fakequant_final.pt`，可直接用于导出 ONNX。

**精度验证**：

```bash
python QAT_Test.py
```

对比原始 float32 模型与 QAT 微调后模型的 mAP50 / mAP50-95，验证 QAT 有效性。

#### 1.3 导出 ONNX / RKNN

```bash
python Convert.py
```

- 自动融合 Conv+BN 层
- 导出 `format="rknn"`，固定输入尺寸 `640×640`，静态 batch=1

更多导出细节请参考：`训练/ultralytics_yolov8-main/RKOPT_README.zh-CN.md`

---

### 2. 转换 — ONNX → RKNN 模型转换

**路径**: `转换/convert2rknn/`

使用 **RKNN-Toolkit2** 将 ONNX 模型转换为 RK3588 NPU 可执行的 `.rknn` 格式，并支持 INT8 量化校准。

#### 2.1 生成量化校准数据集路径

```bash
python generate_paths_txt.py
```

- 遍历 `shushe/images/train/` 下的所有图片，生成绝对路径列表 `paths.txt`
- 该文件作为 RKNN 量化校准的 dataset 输入

#### 2.2 执行转换

```bash
python convert.py <onnx_model> <platform> [dtype] [output_path]

# 示例：RK3588 平台，INT8 量化
python convert.py best.onnx rk3588 i8 ./best.rknn
```

| 参数 | 说明 |
|------|------|
| `onnx_model` | 输入 ONNX 模型路径 |
| `platform` | 目标平台：`rk3588` / `rk3568` / `rk3576` 等 |
| `dtype` | `i8` / `u8` / `fp`，默认 `i8`（INT8 量化） |
| `output_path` | 输出 `.rknn` 路径，默认 `./best.rknn` |

转换完成后，将生成的 `.rknn` 模型拷贝至 RK3588 设备即可部署。

---

### 3. 部署 — RK3588 边缘推理

**路径**: `部署/RK3588/`

提供 **C++** 与 **Python** 两套推理方案，覆盖从单张图片到实时视频流的全场景需求。

#### 3.1 C++ 推理 (`c_tuili`)

基于 [RKNN Model Zoo](https://github.com/airockchip/rknn_model_zoo) 构建，具备极致性能与硬件亲和性。

| 可执行文件 | 功能 |
|-----------|------|
| `rknn_yolov8_demo` | 单张图片推理 |
| `rknn_yolov8_demo_video` | 视频文件推理 |
| `rknn_yolov8_demo_stream` | V4L2 摄像头实时推理 + MJPEG HTTP 推流 |
| `rknn_yolov8_demo_zero_copy` | Zero-Copy 零拷贝优化推理（减少内存拷贝开销） |

**编译**：

```bash
cd 部署/RK3588/c_tuili
./build-linux.sh -t rk3588 -a aarch64 -d yolov8
```

**关键技术**：
- **RKNN Runtime (`librknnrt.so`)**：直接调用 NPU 进行模型推理
- **RGA (Rockchip Graphics Accelerator)**：硬件级图像 resize / format 转换，释放 CPU 压力
- **OpenCV 3.4.5**：图像编解码与检测框可视化
- **MJPEG HTTP Server**：内置轻量 HTTP 服务，浏览器可直接查看实时推理画面
- **Zero-Copy**：通过 DRM / DMA 缓冲区实现 NPU 与图像预处理零拷贝交互

详细编译与运行说明请参考：
- [`部署/RK3588/c_tuili/README_CN.md`](部署/RK3588/c_tuili/README_CN.md)
- [`部署/RK3588/c_tuili/examples/yolov8/README.md`](部署/RK3588/c_tuili/examples/yolov8/README.md)

#### 3.2 Python 推理 (`python_tuili`)

采用 **多线程异步池** 设计，提升 RK3588 NPU 利用率与推理帧率。

```bash
cd 部署/RK3588/python_tuili
python main.py
```

**核心设计**：
- `rknnpool.py`：管理多个 RKNN 实例的线程池，异步调度 `put()` / `get()`
- `func.py`：自定义推理后处理函数（NMS、画框等）
- `main.py`：主循环，支持视频文件 / USB 摄像头输入

**可调参数**：
- `modelPath`：RKNN 模型路径
- `cap`：输入源（视频文件路径或摄像头索引 `0`）
- `TPEs`：线程数，建议 3，可根据负载调整

#### 3.3 性能监控

```bash
cd 部署/RK3588
./moniter.sh
```

实时循环打印 NPU 负载与 CPU 使用率，便于调优与稳定性观察。

---

## 快速开始

### 环境准备

| 阶段 | 环境 | 关键依赖 |
|------|------|---------|
| 训练 | PC / 服务器（Windows/Linux） | Python 3.8+, PyTorch, Ultralytics |
| 转换 | PC / 服务器（Linux 推荐） | RKNN-Toolkit2 >= 2.3.0 |
| 部署 | RK3588 开发板（Linux aarch64） | RKNN Runtime >= 2.0.0, OpenCV 3.4.5, librga |

### 端到端流程

```bash
# ===== 1. 训练（PC）=====
cd 训练/ultralytics_yolov8-main
python train.py              # 基础训练
# 或
python QAT.py                # QAT 量化感知训练（推荐）

# ===== 2. 导出 ONNX（PC）=====
python Convert.py            # 生成 best.onnx

# ===== 3. 转换为 RKNN（PC，需 Linux 环境或 Docker）=====
cd ../../转换/convert2rknn
python convert.py best.onnx rk3588 i8 best.rknn

# ===== 4. 部署到 RK3588（开发板）=====
# 将 best.rknn 拷贝至设备
scp best.rknn root@rk3588:/userdata/

# C++ 推理
cd /userdata/c_tuili
./build-linux.sh -t rk3588 -a aarch64 -d yolov8
./rknn_yolov8_demo model/best.rknn model/bus.jpg

# 或 Python 推理
cd /userdata/python_tuili
python main.py
```

---

## 性能参考

以下数据为 RK3588 平台 NPU 单核 INT8 推理性能（来自 RKNN Model Zoo 官方基准，不含前后处理）：

| 模型 | 输入尺寸 | 精度 | RK3588 FPS |
|------|---------|------|-----------|
| YOLOv8n | 640×640 | INT8 | ~73.5 |
| YOLOv8s | 640×640 | INT8 | ~38.0 |
| YOLOv8m | 640×640 | INT8 | ~16.2 |

> 实际端到端帧率受图像预处理、后处理 NMS、输入输出方式影响，Python 多线程方案在摄像头场景下通常可达 25~45 FPS（YOLOv8n）。

---

## 硬件环境

- **芯片**: Rockchip RK3588 / RK3588S
- **NPU**: 6 TOPS 算力，三核架构，支持 INT8 / FP16
- **CPU**: 四核 Cortex-A76 + 四核 Cortex-A55
- **系统**: Linux（aarch64）
- **推荐外设**: USB 摄像头、MIPI CSI 摄像头、千兆网口（HTTP 推流）

---

## 依赖与版本

| 组件 | 版本 | 说明 |
|------|------|------|
| Ultralytics | YOLOv8 主线 | 训练框架 |
| PyTorch | >= 1.12 | 训练与 QAT |
| RKNN-Toolkit2 | >= 2.3.0 | 模型转换（PC 端） |
| RKNN Runtime | >= 2.0.0 | 推理库（设备端） |
| OpenCV | 3.4.5 | 图像处理（C++） |
| librga | Rockchip 官方 | 硬件图像加速 |
| CMake | >= 3.10 | C++ 编译 |
| Python | 3.8+ | Python 推理 |

---

## 许可证

本项目各子模块遵循其原始开源许可证：

- **训练模块**: [AGPL-3.0](https://github.com/ultralytics/ultralytics/blob/main/LICENSE)（Ultralytics YOLOv8）
- **C++ 推理模块 (RKNN Model Zoo)**: [Apache License 2.0](部署/RK3588/c_tuili/LICENSE)
- **Python 多线程推理**: 遵循各自原始仓库协议

---

## 相关资源

### 官方文档

- [Ultralytics YOLOv8 官方文档](https://docs.ultralytics.com/)
- [Rockchip NPU 开发指南](https://github.com/airockchip/rknn-toolkit2/tree/master/doc)

### 工具下载

| 工具 | 版本 | 下载链接 |
|------|------|---------|
| **RKNN-Toolkit2** | 2.3.0 | [GitHub Releases](https://github.com/airockchip/rknn-toolkit2/releases/tag/v2.3.0) / [PyPI](https://pypi.org/project/rknn-toolkit2/2.3.0/) |
| **RKNN Model Zoo** | 2.3.0 | [GitHub Releases](https://github.com/airockchip/rknn_model_zoo/releases/tag/v2.3.0) / [仓库首页](https://github.com/airockchip/rknn_model_zoo/tree/v2.3.0) |

> **说明**：RKNN-Toolkit2 用于在 PC 端完成模型转换与量化；RKNN Model Zoo 提供各平台的 C++/Python 推理示例与基准测试数据。建议两者版本保持一致。
