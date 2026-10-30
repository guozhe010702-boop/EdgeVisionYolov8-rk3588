#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INSTALL="${EDGEVISION_INSTALL_PREFIX:-$ROOT/ema-runtime}"
BUILD_DIR="${EDGEVISION_BUILD_DIR:-$ROOT/build-native}"
case "$INSTALL/" in "$ROOT/"*) ;; *) echo 'Install must stay inside this project' >&2; exit 1;; esac
python3 - "$INSTALL/rknn_yolov8_demo_stream" <<'PY'
import os,sys
from pathlib import Path
expected=str(Path(sys.argv[1]).resolve())
for p in Path('/proc').iterdir():
    if not p.name.isdigit(): continue
    try: target=os.readlink(str(p/'exe')).split(' (deleted)',1)[0]
    except OSError: continue
    if target==expected: raise SystemExit('Stop the target runtime before rebuilding it.')
PY
test "$(uname -m)" = aarch64
mkdir -p "$ROOT/build-deps/packages" "$ROOT/build-deps/root"
cd "$ROOT/build-deps/packages"
# Download/extract only: never install packages or replace system libraries.
apt-get download 'libopencv-core4.2=4.2.0+dfsg-5' 'libopencv-core-dev=4.2.0+dfsg-5' \
    'libopencv-imgproc4.2=4.2.0+dfsg-5' 'libopencv-imgproc-dev=4.2.0+dfsg-5' \
    'libturbojpeg=2.0.3-0ubuntu1.20.04.3' 'libturbojpeg0-dev=2.0.3-0ubuntu1.20.04.3'
sha256sum ./*.deb > "$ROOT/build-deps/SHA256SUMS"
for package in ./*.deb; do dpkg-deb -x "$package" "$ROOT/build-deps/root"; done
cmake -S "$ROOT/ema3588" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$INSTALL"
cmake --build "$BUILD_DIR" --parallel 4
cmake --install "$BUILD_DIR"
cp "$ROOT/ema3588/start_imx415_mjpeg.sh" "$ROOT/start_imx415_mjpeg.sh"
chmod +x "$ROOT/start_imx415_mjpeg.sh"
for pattern in 'libopencv_core.so*' 'libopencv_imgproc.so*' 'libturbojpeg.so*'; do
    find "$ROOT/build-deps/root/usr/lib/aarch64-linux-gnu" -maxdepth 1 -name "$pattern" \
        -exec cp -a '{}' "$INSTALL/lib/" \;
done
mkdir -p "$INSTALL/licenses"
cp -a "$ROOT/upstream/licenses/." "$INSTALL/licenses/"
cp -a "$ROOT/build-deps/root/usr/share/doc/." "$INSTALL/licenses/ubuntu-packages/"
if [[ -d "$ROOT/model-bundle" ]]; then
    mkdir -p "$INSTALL/models"
    cp -a "$ROOT/model-bundle/." "$INSTALL/models/"
fi
python3 - "$ROOT" "$INSTALL" <<'PY'
import json,platform,subprocess,sys
from pathlib import Path
root=Path(sys.argv[1])
info=json.loads((root/'source-manifest.json').read_text())
info.update(system=platform.platform(), compiler=subprocess.check_output(['g++','--version'],text=True).splitlines()[0],
            libc=platform.libc_ver(), parallel_jobs=4,
            packages_sha256=(root/'build-deps/SHA256SUMS').read_text(),
            cmake_options=['CMAKE_BUILD_TYPE=Release','HEADLESS','ZERO_COPY','OpenCV 4.2'])
(Path(sys.argv[2])/'build-info.json').write_text(json.dumps(info,indent=2))
PY
cd "$INSTALL"
sha256sum rknn_yolov8_demo_stream rknn_yolov8_demo rknn_yolov8_demo_zero_copy model/best.rknn build-info.json lib/*.so* > SHA256SUMS
if [[ -d models ]]; then find models -type f -print0 | sort -z | xargs -0 sha256sum >> SHA256SUMS; fi
ldd ./rknn_yolov8_demo_stream
echo "Native build ready: $INSTALL"
