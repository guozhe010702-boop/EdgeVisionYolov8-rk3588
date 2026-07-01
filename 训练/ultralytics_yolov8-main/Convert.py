from ultralytics import YOLO

if __name__ == '__main__':  # 核心：主程序入口包裹
    
    # 加载模型
    model = YOLO("runs/detect/train4/weights/best.pt")


    #融合Conv+BN层（训练后执行，不影响训练精度）
    model.fuse()
    # 导出RKNN（指定640×640输入+简化ONNX）
    model.export(
        format="rknn",          # 目标格式RKNN
        imgsz=(640, 640),      # 输入尺寸：height=640, width=640 → 640×640
        simplify=True,         # 简化ONNX模型
        dynamic=False,         # 静态尺寸（RKNN必需）
        batch=1                # 固定批次为1
    )

