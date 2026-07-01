from ultralytics import YOLO
import torch
from torch.quantization import (
    QuantStub, DeQuantStub, prepare_qat, convert, QuantWrapper,
    FakeQuantize, MinMaxObserver, MovingAverageMinMaxObserver, QConfig
)
from torch.quantization.fuse_modules import fuse_modules
from ultralytics.nn.modules.block import C2f, SPPF
from ultralytics.nn.modules.head import Detect
import warnings
from multiprocessing import freeze_support
import os
import copy

# 忽略无关警告，提升运行稳定性
warnings.filterwarnings('ignore')
# 修复多进程启动问题（Windows必加）
try:
    torch.multiprocessing.set_start_method('spawn', force=True)
except RuntimeError:
    pass


class YOLOv8QAT(torch.nn.Module):
    def __init__(self, yolo_model, quant_enabled=True):
        super().__init__()
        self.model = yolo_model.model  # 封装YOLO底层模型
        self.quant_enabled = quant_enabled
        
        # 全局量化/反量化节点（PyTorch QAT必需）
        self.quant = QuantStub()    
        self.dequant = DeQuantStub()
        
        # 兼容配置
        self.yaml = yolo_model.model.yaml
        self.names = yolo_model.names if hasattr(yolo_model, 'names') else yolo_model.model.names
        
        # 1. 融合Conv-BN层
        self._fuse_modules()
        
        if self.quant_enabled:
            # 2. 量化配置
            self.qconfig = self._get_fake_quant_qconfig()
            # 3. 分配量化配置（适配PyTorch 2.x）
            self._set_module_qconfig()
            # 4. 包装核心模块
            self._apply_module_quantization()
            # 5. 准备QAT训练
            prepare_qat(self, inplace=True)

    def _get_fake_quant_qconfig(self):
        """伪量化配置（适配PyTorch 2.x）"""
        weight_qconfig = FakeQuantize.with_args(
            observer=MinMaxObserver,
            quant_min=-128,
            quant_max=127,
            dtype=torch.qint8,
            qscheme=torch.per_tensor_symmetric,
            reduce_range=False
        )
        act_qconfig = FakeQuantize.with_args(
            observer=MovingAverageMinMaxObserver,
            quant_min=0,
            quant_max=255,
            dtype=torch.quint8,
            qscheme=torch.per_tensor_affine,
            reduce_range=False
        )
        return QConfig(weight=weight_qconfig, activation=act_qconfig)

    def _fuse_modules(self):
        """融合Conv-BN层"""
        for name, module in self.model.named_modules():
            if isinstance(module, (C2f, SPPF)):
                for sub_name, sub_mod in module.named_modules():
                    if isinstance(sub_mod, torch.nn.Conv2d) and hasattr(sub_mod, 'bn'):
                        try:
                            fuse_modules(module, [f"{sub_name}", f"{sub_name}.bn"], inplace=True)
                        except:
                            continue
            elif isinstance(module, torch.nn.Sequential):
                try:
                    fuse_modules(module, [["0", "1"]], inplace=True)
                except:
                    continue

    def _apply_module_quantization(self):
        """仅包装核心模块，避免序列化冲突"""
        for name, module in self.model.named_children():
            if isinstance(module, (C2f, SPPF)):
                setattr(self.model, name, QuantWrapper(module))
            elif isinstance(module, Detect):
                module.qconfig = None
                continue

    def _set_module_qconfig(self):
        """逐个模块分配qconfig"""
        for name, module in self.named_modules():
            if isinstance(module, Detect):
                module.qconfig = None
            elif not isinstance(module, (QuantStub, DeQuantStub)):
                module.qconfig = self.qconfig
        # 量化节点自身赋值qconfig
        self.quant.qconfig = self.qconfig
        self.dequant.qconfig = self.qconfig

    def forward(self, x):
        """前向传播（兼容量化/非量化）"""
        if self.quant_enabled:
            x = self.quant(x)
        outputs = self.model(x)
        if self.quant_enabled:
            if isinstance(outputs, (list, tuple)):
                outputs = [self.dequant(out) for out in outputs]
            else:
                outputs = self.dequant(outputs)
        return outputs

    def get_float_weights(self):
        """提取纯浮点权重（剥离量化节点），用于保存"""
        # 深拷贝模型，避免影响训练中的模型
        float_model = copy.deepcopy(self.model)
        # 移除QuantWrapper包装，恢复原始模块
        for name, module in float_model.named_children():
            if isinstance(module, QuantWrapper):
                setattr(float_model, name, module.module)  # 取出包装内的原始模块
        return float_model.state_dict()


if __name__ == '__main__':
    freeze_support()  # Windows多进程必需
    
    # ========== 1. 路径配置 ==========
    data_yaml_path = r"C:/Users/beast/Desktop/ultralytics_yolov8-main/datasets/shushe.yaml"
    custom_model_path = "runs/detect/train4/weights/best.pt"
    if not os.path.exists(data_yaml_path):
        raise FileNotFoundError(f"数据集配置不存在：{data_yaml_path}")
    if not os.path.exists(custom_model_path):
        raise FileNotFoundError(f"预训练模型不存在：{custom_model_path}")

    # ========== 2. 初始化基础模型 ==========
    base_model = YOLO(custom_model_path)

    # ========== 3. 阶段1：浮点预热训练 ==========
    print("===== 阶段1：浮点预热训练 =====")
    warmup_config = {
        'data': data_yaml_path,
        'epochs': 10,
        'batch': 4,
        'lr0': 0.002,
        'lrf': 0.1,
        'imgsz': (640,640),
        'device': 0 if torch.cuda.is_available() else 'cpu',
        'workers': 0,
        'amp': False,
        'cache': False,
        'project': 'runs/qat',
        'name': 'best_qat_warmup',
        'pretrained': True,
        'warmup_epochs': 0,
        'weight_decay': 0.0001,
        'cos_lr': True,
        'mosaic': 1.0,
    }
    warmup_results = base_model.train(**warmup_config)
    torch.save(base_model.model.state_dict(), 'shushe_warmup_weights.pt')
    print("✅ 浮点预热训练完成")

    # ========== 4. 阶段2：伪量化QAT训练 ==========
    print("\n===== 阶段2：伪量化QAT训练 =====")
    # 创建QAT模型
    qat_model_train = YOLOv8QAT(base_model, quant_enabled=True)
    # 加载预热权重
    qat_model_train.load_state_dict(torch.load('shushe_warmup_weights.pt'), strict=False)
    # 替换底层模型
    base_model.model = qat_model_train
    
    qat_config = {
        'data': data_yaml_path,
        'epochs': 50,
        'batch': 16,
        'lr0': 0.0001,
        'lrf': 0.01,
        'imgsz': (640,640),
        'device': 0 if torch.cuda.is_available() else 'cpu',
        'workers': 0,
        'amp': False,
        'cache': False,
        'project': 'runs/qat',
        'name': 'best_qat_fakequant',
        'pretrained': False,
        'warmup_epochs': 0,
        'weight_decay': 0.0001,
        'cos_lr': True,
        'mosaic': 1.0,
    }
    qat_results = base_model.train(**qat_config)

    # ========== 5. 保存模型（避免序列化量化节点） ==========
    print("\n===== 保存QAT模型 =====")
    # 步骤1：提取纯浮点权重（剥离QuantWrapper/ FakeQuantize）
    float_weights = qat_model_train.get_float_weights()
    torch.save(float_weights, 'shushe_qat_float_weights.pt')
    
    # 步骤2：重新初始化原始YOLO模型，加载QAT浮点权重
    final_yolo_model = YOLO(custom_model_path)  # 重新初始化纯浮点YOLO模型
    final_yolo_model.model.load_state_dict(float_weights, strict=False)
    
    # 步骤3：保存YOLO格式模型（无量化节点，可正常序列化）
    final_yolo_model.save('shushe_float32_fakequant_final.pt')
    
    # 可选：保存含量化节点的权重（仅用于推理，不用于YOLO save）
    torch.save(qat_model_train.state_dict(), 'best_qat_with_quant_nodes.pth')
    
    print("\n✅ QAT训练+保存全部完成！")
    print(f"📌 YOLO格式QAT模型（可正常测试）：best_float32_fakequant_final.pt")
    print(f"📌 纯浮点权重：best_qat_float_weights.pt")



    