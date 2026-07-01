# EdgeVision-RK3588 YOLOv8 训练与量化仓库

本仓库是 [Ultralytics YOLOv8](https://github.com/ultralytics/ultralytics) 的定制版本，专为 **RK3588（RKNPU）边缘部署** 进行优化。在完整保留原仓库训练、验证、预测能力的基础上，增加了模型导出适配与量化感知训练（QAT）能力，实现从训练到 RKNN 部署的完整闭环。

> 原始仓库来源：[ultralytics/ultralytics](https://github.com/ultralytics/ultralytics)（基于 commit `0b0bc56` 修改验证）

---

## 核心特性

- **RKNPU 模型适配**：修改模型输出结构，移除对量化不友好的后处理节点，DFL 结构外移至 CPU 后处理，提升 NPU 推理性能
- **完整训练 pipeline**：支持检测、分割、姿态、旋转框（OBB）等任务的训练与验证
- **量化感知训练（QAT）**：基于 PyTorch 伪量化的两阶段 QAT 训练，降低 INT8 量化精度损失
- **一键导出 RKNN**：融合 Conv+BN，导出静态尺寸、简化后的 RKNN 模型

---

## 文件说明

| 文件 | 说明 |
|------|------|
| `train.py` | 标准 YOLOv8 训练入口，加载预训练模型并在自定义数据集上训练 |
| `Convert.py` | 将训练好的 `.pt` 模型导出为 **RKNN** 格式（640×640 静态输入，简化 ONNX 中间过程） |
| `QAT.py` | 量化感知训练主脚本：阶段一浮点预热 → 阶段二伪量化微调 → 保存可序列化的浮点权重 |
| `QAT_Test.py` | QAT 效果验证脚本：检测量化节点、对比原始模型与 QAT 微调后模型的 mAP 精度 |
| `RKOPT_README.zh-CN.md` | RKNPU 模型导出的详细说明（输出结构改动、ONNX 导出步骤、RKNN 转换指引） |

---

## 快速开始

### 1. 环境准备

- Python >= 3.8
- PyTorch >= 1.8
- 安装依赖：`pip install -r requirements.txt`
- RKNN 导出需安装 `ultralytics[rknn]` 相关依赖

### 2. 训练模型

```python
python train.py
```

默认加载 `yolov8n.pt`，在 `datasets/shushe.yaml` 数据集上训练 50 个 epoch。根据需要修改脚本中的模型路径与数据配置。

### 3. 量化感知训练（QAT）

```python
python QAT.py
```

流程：
1. **浮点预热训练**（10 epoch）：稳定特征提取层
2. **伪量化 QAT 训练**（50 epoch）：在卷积层插入 `FakeQuantize` 节点，模拟 INT8 量化误差并微调
3. **保存模型**：剥离量化节点，输出可正常序列化的 `*_float32_fakequant_final.pt`

### 4. 验证 QAT 效果

```python
python QAT_Test.py
```

自动对比原始 float32 模型与 QAT 微调后模型的 precision / recall / mAP50 / mAP50-95。

### 5. 导出 RKNN 模型

```python
python Convert.py
```

或按 `RKOPT_README.zh-CN.md` 中的说明，使用 `exporter.py` 导出 ONNX 后转 RKNN。

---

## 模型适配改动（相对于原生 YOLOv8）

为兼容 RKNPU 的量化与推理特性，本仓库在**导出阶段**对模型结构做了以下调整（**无需重新训练**）：

1. **移除后处理结构**：后处理对量化不友好，移至模型外部由 CPU 执行
2. **DFL 外移**：DFL（Distribution Focal Loss）结构在 NPU 上性能不佳，移至外部后处理阶段
3. **新增置信度总和分支**：辅助后处理阶段加速阈值筛选

> 对应的后处理代码可在 [RKNN_Model_Zoo](https://github.com/airockchip/rknn_model_zoo) 中获取。

---

## 支持任务

- [x] 目标检测（Detect）
- [x] 实例分割（Segment）
- [x] 姿态估计（Pose）
- [x] 旋转框检测（OBB）

---

## 参考资料

- [Ultralytics YOLOv8 官方文档](https://docs.ultralytics.com/)
- [RKNN_Model_Zoo](https://github.com/airockchip/rknn_model_zoo)
- [RKOPT_README.zh-CN.md](./RKOPT_README.zh-CN.md) —— RKNPU 导出详细说明

---

## 许可证

本项目继承自 Ultralytics 的 [AGPL-3.0](https://github.com/ultralytics/ultralytics/blob/main/LICENSE) 开源许可证。若用于商业产品，请联系 Ultralytics 获取企业授权。
