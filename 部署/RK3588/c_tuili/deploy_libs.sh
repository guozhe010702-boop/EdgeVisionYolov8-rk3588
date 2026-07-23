#!/bin/bash
# deploy_libs.sh — 自动收集运行时依赖, 打包到 install 目录
# 用法: ./deploy_libs.sh [install_demo_dir]
#   install_demo_dir 默认为 install/rk3588_linux_aarch64/rknn_yolov8_demo
#
# 功能:
#   1. 扫描所有可执行文件的 ldd 依赖
#   2. 复制非系统库 (lib, .so + 符号链接) 到 install_demo_dir/lib/
#   3. 清理 .a 文件和无关子目录
#   4. 验证所有依赖是否可解析
#
# 注意: 系统基础库 (glibc, libstdc++, libm 等) 不打包,
#       目标板需有兼容版本的 glibc / libstdc++

set -e

# ==================== 配置 ====================
# 不打包的系统基础库 (目标板通常已有, 且 glibc 版本必须匹配内核)
SKIP_LIBS=(
    linux-vdso
    ld-linux
    libc.so
    libm.so
    libpthread.so
    libdl.so
    librt.so
    libgcc_s.so
    libstdc++.so
    libresolv.so
    libnss_dns.so
    libnss_files.so
    libnss_compat.so
)

# ==================== 参数 ====================
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ $# -ge 1 ]; then
    DEMO_DIR="$1"
else
    # 自动检测: 查找 install 目录下的 rknn_yolov8_demo
    DEMO_DIR=$(find "${SCRIPT_DIR}/install" -name "rknn_yolov8_demo_stream" -type f -executable 2>/dev/null | head -1 | xargs dirname 2>/dev/null || true)
    if [ -z "${DEMO_DIR}" ]; then
        echo "ERROR: Cannot find install directory. Usage: $0 <install_demo_dir>"
        echo "  Example: $0 install/rk3588_linux_aarch64/rknn_yolov8_demo"
        exit 1
    fi
fi

if [ ! -d "${DEMO_DIR}" ]; then
    echo "ERROR: Directory not found: ${DEMO_DIR}"
    exit 1
fi

LIB_DIR="${DEMO_DIR}/lib"
echo "=== deploy_libs.sh ==="
echo "Target: ${DEMO_DIR}"
echo ""

# ==================== Step 1: 清理旧垃圾 ====================
echo "[1/5] Cleaning up old junk files..."
# 删除 .a 文件 (静态库, 运行时不需要)
find "${LIB_DIR}" -name "*.a" -type f -delete 2>/dev/null || true
# 删除 libssl3.so (旧版本残留)
rm -f "${LIB_DIR}/libssl3.so" 2>/dev/null || true
# 删除 android/ 子目录 (Android 平台库, Linux 板子不需要)
rm -rf "${LIB_DIR}/android" 2>/dev/null || true
# 删除空子目录 (install(DIRECTORY) 可能遗留的空壳)
find "${LIB_DIR}" -mindepth 1 -maxdepth 1 -type d -empty -delete 2>/dev/null || true
# 删除已知的无关子目录
for _junk_dir in pkgconfig cmake perl perl5 perl-base icu sasl2 engines-3; do
    rm -rf "${LIB_DIR}/${_junk_dir}" 2>/dev/null || true
done
echo "  Cleaned .a files, android/, pkgconfig/, empty dirs"

# ==================== Step 2: 扫描可执行文件依赖 ====================
echo "[2/5] Scanning executable dependencies..."
NEEDED_LIBS=()

for exe in "${DEMO_DIR}"/*; do
    [ -f "${exe}" ] || continue
    [ -x "${exe}" ] || continue
    # 跳过 .so 文件和非 ELF 文件
    file "${exe}" | grep -q "ELF" || continue

    echo "  Scanning: $(basename "${exe}")"
    while IFS= read -r line; do
        # ldd 输出格式: "libfoo.so.1 => /usr/lib/libfoo.so.1 (0x...)"
        lib_path=$(echo "${line}" | sed -n 's/.*=> \([^ ]*\).*/\1/p')
        [ -z "${lib_path}" ] && continue

        # 跳过已在 install/lib 中的库
        lib_name=$(basename "${lib_path}")
        [ -f "${LIB_DIR}/${lib_name}" ] && continue

        # 跳过系统基础库
        skip=0
        for skip_pat in "${SKIP_LIBS[@]}"; do
            if [[ "${lib_name}" == ${skip_pat}* ]]; then
                skip=1
                break
            fi
        done
        [ ${skip} -eq 1 ] && continue

        # 去重
        if ! echo "${NEEDED_LIBS[@]}" | grep -qw "${lib_path}"; then
            NEEDED_LIBS+=("${lib_path}")
        fi
    done < <(LD_LIBRARY_PATH="${LIB_DIR}" ldd "${exe}" 2>/dev/null || true)
done

echo "  Found ${#NEEDED_LIBS[@]} missing libraries"

# ==================== Step 3: 复制依赖库 ====================
echo "[3/5] Copying ${#NEEDED_LIBS[@]} libraries to ${LIB_DIR}..."
copied=0
for lib_path in "${NEEDED_LIBS[@]}"; do
    if [ ! -f "${lib_path}" ]; then
        echo "  WARNING: ${lib_path} not found, skipping"
        continue
    fi

    lib_name=$(basename "${lib_path}")

    # 如果是符号链接, 复制链接 + 目标
    if [ -L "${lib_path}" ]; then
        # 追踪到真实文件
        real_path=$(readlink -f "${lib_path}")
        real_name=$(basename "${real_path}")

        # 先复制真实文件
        if [ ! -f "${LIB_DIR}/${real_name}" ]; then
            cp -f "${real_path}" "${LIB_DIR}/${real_name}"
            echo "  + ${real_name}"
            copied=$((copied + 1))
        fi

        # 创建相对符号链接
        if [ "${lib_name}" != "${real_name}" ] && [ ! -e "${LIB_DIR}/${lib_name}" ]; then
            ln -sf "${real_name}" "${LIB_DIR}/${lib_name}"
            echo "  + ${lib_name} -> ${real_name}"
        fi
    else
        # 普通文件直接复制
        cp -f "${lib_path}" "${LIB_DIR}/${lib_name}"
        echo "  + ${lib_name}"
        copied=$((copied + 1))
    fi
done
echo "  Copied ${copied} new libraries"

# ==================== Step 4: 复制 mediamtx (如果还没打包) ====================
echo "[4/5] Checking mediamtx..."
if [ ! -f "${DEMO_DIR}/mediamtx" ]; then
    for search_path in "${SCRIPT_DIR}/mediamtx" /usr/local/bin/mediamtx; do
        if [ -f "${search_path}" ]; then
            cp -f "${search_path}" "${DEMO_DIR}/mediamtx"
            chmod +x "${DEMO_DIR}/mediamtx"
            echo "  + mediamtx ($(du -sh "${search_path}" | cut -f1))"
            break
        fi
    done
else
    echo "  mediamtx already present"
fi

# ==================== Step 5: 验证 ====================
echo "[5/5] Verifying all dependencies..."
all_ok=true
missing_count=0
for exe in "${DEMO_DIR}"/*; do
    [ -f "${exe}" ] || continue
    [ -x "${exe}" ] || continue
    file "${exe}" | grep -q "ELF" || continue

    missing=$(LD_LIBRARY_PATH="${LIB_DIR}" ldd "${exe}" 2>/dev/null | grep "not found" || true)
    if [ -n "${missing}" ]; then
        echo "  WARNING: $(basename "${exe}") has unresolved deps:"
        echo "${missing}" | sed 's/^/    /'
        missing_count=$((missing_count + $(echo "${missing}" | wc -l)))
        all_ok=false
    fi
done

echo ""
echo "=== Summary ==="
so_count=$(find "${LIB_DIR}" -maxdepth 1 -name "*.so*" -not -type d | wc -l)
dir_size=$(du -sh "${DEMO_DIR}" | cut -f1)
echo "  .so files in lib/: ${so_count}"
echo "  Total package size: ${dir_size}"
echo "  Missing deps: ${missing_count}"

if ${all_ok}; then
    echo "  Status: ALL OK ✓"
else
    echo "  Status: SOME DEPS MISSING (see warnings above)"
    echo "  Note: System base libs (glibc, libstdc++) are not packaged by design."
    echo "  Ensure the target board has a compatible glibc/libstdc++ version."
fi
