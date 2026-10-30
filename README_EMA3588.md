# EVM3588 室内目标检测：部署与运行

更新日期：2026-10-30。独立目录为 `edgevision -yolov8/`；板端为 `/userdata/edgevision-yolov8/`。不替换 av_dev 视频工程、系统 FFmpeg 或内核。

## 当前能做什么

原仓库随附模型只有 Purple、Yellow 两类，不能靠修改标签变成人、水杯或椅子。本次新增两个公开预训练模型，完成 PT → Rockchip 九输出 ONNX → INT8 RKNN 转换，未进行重新训练。

| 模式 | 模型与检测范围 | 本板实测有效输出 |
| --- | --- | --- |
| `indoor` | YOLOv8s COCO80：人、水杯、椅子、瓶子等 | 约 20.4 fps，短测 |
| `indoor-helmet` | COCO80 加独立 YOLOv8n 安全帽模型 | 约 13.5–13.6 fps，当前默认 |
| `legacy` | 原 Purple / Yellow 模型 | 仅保留历史对照 |

两个模型在同一张未画框的原始图像上依次运行，再合并绘制。COCO 蓝框、安全帽绿框；安全帽模型包含 Hardhat、NO-Hardhat，实时显示仅使用 Hardhat。无帽类别不等于经过人员关联验证的违规判定。

当前 IMX415 输入 1920×1080 NV12，模型输入 640×640；请求帧率 30 不代表传感器实际帧率，检测输出按实测报告。HDMI 和电脑订阅同一份已画框 MJPEG，不会分别打开摄像头。

**已确认人员实拍识别；水杯、椅子、安全帽目前完成公开图片测试，尚未完成三类物体的现场验收。** 上一次长时间运行曾发生 RKNN 提交失败，稳定性仍需继续排查，不能视作长期运行验收通过。详细数值见 [实测记录](VALIDATION_EMA3588.md)。

## 开始、查看与停止

以下代码块均在**开发板 Linux 终端**执行，例如提示符 `root@ema:~#`；不要粘到 Windows PowerShell。

启动当前双模型检测：

```bash
python3 /userdata/edgevision-yolov8/ema3588/board_control.py start --take-camera --profile indoor-helmet
```

`python3` 执行板端控制程序；`start` 启动；`--profile indoor-helmet` 选择室内加安全帽；`--take-camera` 只允许通过已知 UAV 服务接口交接摄像头，不杀死其它占用进程。已运行同一模式时返回现有 PID。

打开板端 HDMI：

```bash
python3 /userdata/edgevision-yolov8/ema3588/open_hdmi_preview.py
```

该脚本在 ema 用户的 X11 桌面启动 ffplay，窗口放在 HDMI 显示区域，避免落到另一个 DSI 桌面。它只读取 HTTP 检测流，不再次采集摄像头。

电脑浏览器访问 [实时检测画面](http://192.168.2.221:8080/)。这是 MJPEG 视频入口，不是 UAV 项目 8088 端口的管理网页。若板子 IP 改变，应替换地址。

电脑也可以在 **Windows PowerShell** 执行：

```powershell
ffplay -fflags nobuffer -flags low_delay -framedrop -x 1280 -y 720 "http://192.168.2.221:8080/"
```

`ffplay` 播放检测画面；`nobuffer`、`low_delay` 尽量减少缓存；`-framedrop` 允许播放器追赶；`-x/-y` 设窗口为 1280×720，保留原始检测图像内容。按 `q` 只关闭电脑播放器，不停止板端检测。

查询板端状态：

```bash
python3 /userdata/edgevision-yolov8/ema3588/board_control.py status
```

`status` 返回进程身份、摄像头占用者、模式、日志、阶段和错误。`last_metric` 是最后一次日志统计；进程停止时 `metrics_stale=true`。运行时缺少逐条时间戳，`metrics_stale=null` 表示需要多次查询帧计数才能确认持续出帧，不代表已证明新鲜。

采集 60 秒 CPU、内存、NPU 和检测性能：

```bash
python3 /userdata/edgevision-yolov8/ema3588/board_control.py sample --seconds 60
```

`sample` 每秒读取一次资源及检测日志；`--seconds 60` 指持续时长，允许 1–1800 秒。CSV 和 JSON 写入板端 `local/telemetry-日期时间.*`。进程 CPU 以一个逻辑核为 100%，整机 CPU 已归一化；RSS 是检测进程内存，物理内存百分比是全板。

切换到更快的通用室内模式：

```bash
python3 /userdata/edgevision-yolov8/ema3588/board_control.py apply-profile --profile indoor
```

`apply-profile` 先校验文件，再受控重启检测及已有 HDMI 预览；失败会尝试恢复之前的运行模式。`indoor` 不运行安全帽模型。要切回双模型，把最后一个参数改为 `indoor-helmet`。

停止本项目并恢复之前由控制器暂停的 UAV 摄像头任务：

```bash
python3 /userdata/edgevision-yolov8/ema3588/open_hdmi_preview.py --stop
python3 /userdata/edgevision-yolov8/ema3588/board_control.py stop --restore-uav
```

第一条 `--stop` 仅关闭本项目的 HDMI 播放器。第二条 `stop` 结束检测，`--restore-uav` 在确认摄像头已释放且设置未被修改后恢复原 UAV 任务；不带该参数只停止 EdgeVision。

## 程序位置与职责

| 文件或目录 | 作用 |
| --- | --- |
| `models.lock.json` | 模型下载来源、类别、PT/ONNX/RKNN SHA-256、转换及验证口径 |
| `部署/RK3588/c_tuili/examples/yolov8/cpp/main_stream.cc` | 采集、主辅模型推理、合并画框、MJPEG、METRIC/OBJECTS 日志 |
| 同目录 `v4l2_capture.cc/.h` | V4L2 mmap 采集，校验 NV12/NV12M、stride、有效长度、偏移及归还缓冲 |
| 同目录 `postprocess.cc/.h` | DFL 解码、阈值过滤、NMS、按实际输出数量加载类别，独立上下文标签 |
| 同目录 `rknpu2/yolov8_zero_copy.cc` | 实时程序的 RKNN 内存与推理接口，检查输出结构和核心掩码 |
| 同目录 `rknpu2/yolov8.cc` | 标准 RKNN 输入输出路径，用于图片检测及对照 |
| 同目录 `main.cc` | 单张图片推理、结构化结果和输出图像；分别编译标准/零拷贝版本 |
| 同目录 `mjpeg_http_sink.cc` | HTTP MJPEG 发送，处理分段请求和断开客户端 |
| `部署/RK3588/ema3588/board_control.py` | 摄像头交接、启停、模式切换与失败恢复、状态和性能采样 |
| 同目录 `model_profiles.json` | 三种模式的模型、阈值及 NPU mask，当前 COCO .35、安全帽 .50 |
| 同目录 `open_hdmi_preview.py`、`x11_preview.py` | ema 桌面播放器启停与 HDMI 窗口定位 |
| 同目录 `fetch_indoor_assets.py` | 下载公开权重与校准/验证样本，并核对锁定来源 |
| 同目录 `convert_indoor_models.py` | 容器内 CPU 导出 ONNX、量化 RKNN、生成参考预测与清单 |
| 同目录 `validate_indoor_models.py` | 板端标准/零拷贝、ONNX 参考和标注对照；不是训练脚本 |
| 同目录 `test_model_contracts.py`、`test_controller_status.py` | 错误类别/阈值拒绝，以及退出错误、日志缺失等回归 |
| 同目录 `CMakeLists.txt`、`build_native.sh` | 在板上原生编译三个可执行程序，私有 OpenCV4.2/库，不安装系统包 |
| 同目录 `prepare_source.py`、`deploy_ema3588.ps1` | Windows 打包源码/模型，再通过指定 ADB 设备部署并编译 |
| 同目录 `activate_indoor_candidate.py` | 候选运行目录切换、完整旧目录备份与失败回滚 |
| `转换/convert2rknn/yolov8.py` | 上游转换参考入口；当前新增模型使用上述独立转换脚本，不直接覆盖旧模型 |
| `.cache/` | 本地权重、ONNX、公共样本、私人实拍、验证中间文件；不入 Git |
| `releases/` | 本地 ARM64 便携运行包；不入 Git |

板端 `ema-runtime/` 放三个可执行程序、私有 `lib/`、`models/coco80/`、`models/helmet/`、旧 `model/`、许可、构建信息和 SHA256SUMS；`ema3588/` 放控制脚本；`local/` 放会话、日志和采样。它们是部署产物，不应复制成另一份本地源码工程。

## 模型与构建说明

通用权重来自 [Ultralytics 官方发布](https://github.com/ultralytics/assets/releases/tag/v8.3.0)，安全帽权重来自 [keremberke 模型卡](https://huggingface.co/keremberke/yolov8n-hard-hat-detection)。COCO80 没有安全帽，因此采用第二模型，未把其它类别重命名成安全帽。类别和输出契约参考 [Rockchip Model Zoo](https://github.com/airockchip/rknn_model_zoo/blob/main/examples/yolov8/README.md)。第三方权重、数据和代码各自许可须分别遵守，不将它们统一宣称为同一许可。

转换环境为已准备的 `uav-yolo11-converter:2.3.2` Docker 镜像，RKNN Toolkit2 2.3.2，Rockchip exporter 固定提交见锁定文件。板端 runtime 2.3.0、RKNPU 驱动 0.8.2，已跑通当前两个 INT8 模型；没有升级内核或系统 libc。使用核心掩码 7 不代表三核负载必定均衡，实测仍以 Core0 为主。

完整离线运行包在 `releases/edgevision-ema3588-ubuntu20.04-arm64-20261030.tar.gz`，含模型与私有库，不含私人画面、PT/ONNX、数据集、日志或访问凭据。首次复制到另一台板之前，需要重新核对系统、NPU 驱动、摄像头和图形桌面；不承诺任意固件兼容。

源码重建前必须停止目标运行目录中的程序。`build_native.sh` 会拒绝覆盖正在运行的同名二进制；Windows 部署脚本必须显式指定设备序列号，并核对实际设备树。ADB 中 Nexus4 字样来自厂商通用属性，不能据此判定实际硬件。
