#!/usr/bin/env bash
set -euo pipefail

# EdgeVision RK3588 C++ stream demo, configured for the EVM3588 IMX415 node.
# Override any setting for a one-off run, for example:
# EDGEVISION_PORT=8090 EDGEVISION_FPS=25 ./start_imx415_mjpeg.sh

APP_DIR="${EDGEVISION_APP_DIR:-/userdata/edgevision-yolov8/ema-runtime}"
DEVICE="${EDGEVISION_CAMERA:-/dev/video44}"
WIDTH="${EDGEVISION_WIDTH:-1920}"
HEIGHT="${EDGEVISION_HEIGHT:-1080}"
FPS="${EDGEVISION_FPS:-30}"
PORT="${EDGEVISION_PORT:-8080}"
MODEL="${EDGEVISION_MODEL:-${APP_DIR}/model/best.rknn}"
PROGRAM="${APP_DIR}/rknn_yolov8_demo_stream"

if [[ ! -x "${PROGRAM}" ]]; then
    echo "Missing executable: ${PROGRAM}" >&2
    exit 1
fi
if [[ ! -r "${MODEL}" ]]; then
    echo "Missing RKNN model: ${MODEL}" >&2
    exit 1
fi
if [[ ! -e "${DEVICE}" ]]; then
    echo "Camera device does not exist: ${DEVICE}" >&2
    exit 1
fi
if [[ -r "${APP_DIR}/SHA256SUMS" ]]; then
    (cd "${APP_DIR}" && sha256sum --quiet -c SHA256SUMS)
fi

export LD_LIBRARY_PATH="${APP_DIR}/lib"
export OMP_NUM_THREADS=2
echo "EdgeVision YOLOv8: ${DEVICE} ${WIDTH}x${HEIGHT}@${FPS}, MJPEG port ${PORT}"
echo "Open http://<board-ip>:${PORT}/ in a browser on the same LAN."
echo "Press Ctrl+C to stop this process."

cd "${APP_DIR}"
exec stdbuf -oL -eL "${PROGRAM}" "${MODEL}" "${DEVICE}" "${WIDTH}" "${HEIGHT}" "${FPS}" "" "${PORT}"
