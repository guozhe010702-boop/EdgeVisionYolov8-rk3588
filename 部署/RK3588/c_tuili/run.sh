#!/bin/bash
# run.sh — YOLOv8 demo 启动器
# 设置 LD_LIBRARY_PATH 确保所有 .so 依赖从本地 lib/ 目录加载
# 用法: ./run.sh <demo_executable> [args...]
#   例如: ./run.sh ./rknn_yolov8_demo_stream model/best.rknn /dev/video0 0 0 0 "" 8080

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="${SCRIPT_DIR}/lib:${LD_LIBRARY_PATH:-}"

if [ $# -eq 0 ]; then
    echo "Usage: $0 <executable> [args...]"
    echo ""
    echo "Available executables:"
    for exe in "${SCRIPT_DIR}"/rknn_yolov8_demo*; do
        [ -x "$exe" ] && echo "  $(basename "$exe")"
    done
    echo ""
    echo "Example:"
    echo "  $0 ./rknn_yolov8_demo_stream model/best.rknn /dev/video0 0 0 0 \"\" 8080"
    exit 1
fi

exec "${SCRIPT_DIR}/$@"
