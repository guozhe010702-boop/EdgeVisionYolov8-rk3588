



from ultralytics import YOLO
import torch
import numpy as np
from multiprocessing import freeze_support
# 导入你的QAT模型类（必须和训练代码中的YOLOv8QAT一致）
from QAT import YOLOv8QAT  
if __name__ == '__main__':
    freeze_support()
    # -------------------------- 1. 路径配置（替换为你的实际路径） --------------------------
    original_model_path = "best.pt"  # 原始未量化模型
    qat_float_model_path = "best_float32_fakequant_final.pt"  # QAT微调后的浮点模型
    qat_quant_nodes_weight_path = "best_qat_with_quant_nodes.pth"  # 含量化节点的权重文件
    data_config = r"C:\Users\beast\Downloads\ultralytics_yolov8-main\ultralytics\cfg\datasets\shushe.yaml"

    # -------------------------- 2. 可选：检测量化节点（加载含量化节点的权重） --------------------------
    def check_quant_nodes_from_weight():
        """从含量化节点的权重文件检测量化节点"""
        print("===== 检测量化节点（从QAT原始权重） =====")
        # 初始化基础YOLO模型
        base_model = YOLO(original_model_path)
        # 创建QAT模型实例（启用量化，生成量化节点）
        qat_model = YOLOv8QAT(base_model, quant_enabled=True)
        try:
            # 加载含量化节点的权重
            qat_model.load_state_dict(torch.load(qat_quant_nodes_weight_path), strict=False)
            # 检测量化节点
            quant_node_types = ["FakeQuantize", "QuantStub", "DeQuantStub", "QuantWrapper"]
            quant_nodes = []
            for name, module in qat_model.named_modules():
                module_name = module.__class__.__name__
                if any(qt in module_name for qt in quant_node_types):
                    quant_nodes.append({"name": name, "type": module_name})
            if quant_nodes:
                print(f"✅ 检测到{len(quant_nodes)}个量化节点（前5个）：")
                for i, node in enumerate(quant_nodes[:5]):
                    print(f"  [{i+1}] 节点名：{node['name']} | 类型：{node['type']}")
            else:
                print("❌ 仍未检测到量化节点（QAT训练过程可能未生成节点）")
        except FileNotFoundError:
            print(f"⚠️  未找到含量化节点的权重文件：{qat_quant_nodes_weight_path}")
        except Exception as e:
            print(f"❌ 检测量化节点出错：{str(e)}")

    # 执行量化节点检测（可选）
    check_quant_nodes_from_weight()

    # -------------------------- 3. 核心：精度对比（验证QAT是否有效） --------------------------
    def evaluate_model(model, data_path, batch=4, device="cuda:0"):
        """评估模型性能（适配新版ultralytics）"""
        try:
            results = model.val(
                data=data_path,
                batch=batch,
                device=device,
                verbose=False,
                plots=False,
                save_json=False
            )
            metrics = {
                "precision": results.box.mp,
                "recall": results.box.mr,
                "mAP50": results.box.map50,
                "mAP50-95": results.box.map
            }
            return metrics
        except Exception as e:
            print(f"❌ 模型评估出错：{str(e)}")
            return None

    # 加载模型
    print("\n===== 加载模型并对比精度 =====")
    device = "cuda:0" if torch.cuda.is_available() else "cpu"
    original_model = YOLO(original_model_path)
    qat_float_model = YOLO(qat_float_model_path)

    # 评估模型
    original_metrics = evaluate_model(original_model, data_config, device=device)
    qat_metrics = evaluate_model(qat_float_model, data_config, device=device)

    # 打印对比结果
    if original_metrics and qat_metrics:
        print("\n| 模型类型                | 精度(P) | 召回率(R) | mAP50  | mAP50-95 |")
        print("|-------------------------|---------|-----------|--------|----------|")
        print(f"| 原始float32模型         | {original_metrics['precision']:.4f} | {original_metrics['recall']:.4f} | {original_metrics['mAP50']:.4f} | {original_metrics['mAP50-95']:.4f} |")
        print(f"| QAT微调后float32模型    | {qat_metrics['precision']:.4f} | {qat_metrics['recall']:.4f} | {qat_metrics['mAP50']:.4f} | {qat_metrics['mAP50-95']:.4f} |")

        # 计算精度变化（QAT的核心是后续量化后损失小，此处先看微调后精度）
        delta_mAP50 = qat_metrics['mAP50'] - original_metrics['mAP50']
        delta_mAP50_95 = qat_metrics['mAP50-95'] - original_metrics['mAP50-95']
        print(f"\n📊 QAT微调后精度变化：")
        print(f"   mAP50变化：{delta_mAP50:.4f} (正数=提升，负数=轻微损失)")
        print(f"   mAP50-95变化：{delta_mAP50_95:.4f}")

        # 判定QAT是否有效（核心：QAT模型后续量化为int8后，精度损失远小于直接后量化）
        if abs(delta_mAP50) <= 0.03 and abs(delta_mAP50_95) <= 0.03:
            print("\n✅ QAT训练有效！微调后浮点精度无显著损失（后续int8量化会受益）。")
        else:
            print("\n⚠️  QAT微调后精度损失略大，可调整训练轮数/学习率优化。")

