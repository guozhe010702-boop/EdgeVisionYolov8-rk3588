from ultralytics import YOLO

# Windows 下 multiprocessing 用 spawn 启动子进程，
# 必须把训练逻辑放在 if __name__ == "__main__": 之下，否则会报 Popen 错误
if __name__ == "__main__":
    # 加载模型
    model = YOLO("yolov8n.pt")  # 加载预训练模型（建议用于训练）

    # 使用模型
    model.train(data="datasets/shushe.yaml", epochs=50)  # 训练模型
    metrics = model.val()  # 在验证集上评估模型性能
