# 打包运行时库实现快速移植 - 实施计划

## Context

当前 install 目录推送到新板子后无法直接运行，存在以下问题：

1. **CMakeLists.txt install(DIRECTORY) 把整个 /usr/lib 目录树都拷进去了** — install/lib 下有 100+ 个无关子目录（gtk-3.0, gstreamer-1.0, webkit2gtk 等），但这些只是空壳或残留
2. **缺少传递依赖** — librtmp 需要 libgnutls 等库，libopencv_core 需要 libz/libgomp，但都没打包
3. **stream demo 链接了 OpenCV highgui/videoio** — 带来 GTK3 + GStreamer + FFmpeg 等 100+ 个 .so 的巨型依赖链，在无头板上根本不用
4. **mediamtx 未打包** — RTMP→WebRTC 转换必需
5. **存在垃圾文件** — .a 文件(11MB)、android/ 目录、libssl3.so 等无用文件

## 成功标准

1. 清空 install 目录 → 重新 cmake install → deploy_libs.sh → 推到另一块 RK3588 板子 → 直接运行所有 demo 无报错
2. install/lib 中无无关子目录、无 .a 文件
3. stream demo 在 HEADLESS 模式下依赖链 ≤ 25 个 .so（vs 当前 100+）
4. mediamtx 在 install 目录中可用

## 实施步骤

### 第 1 步：添加 HEADLESS 编译选项，精简 stream 依赖

**修改文件**: `examples/yolov8/cpp/CMakeLists.txt`, `examples/yolov8/cpp/main_stream.cc`

**CMakeLists.txt 变更**:
- 添加 `option(HEADLESS "Disable OpenCV highgui/videoio for headless streaming (reduces runtime deps)" ON)` 默认开启
- HEADLESS=ON 时：stream target 只链接 `libopencv_core.so` + `libopencv_imgproc.so`（不再链接 videoio/highgui/imgcodecs）
- HEADLESS=OFF 时：保持现有行为（完整 OpenCV 链接）

**main_stream.cc 变更**:
- `#ifdef HEADLESS` 包裹 `#include <opencv2/highgui.hpp>` 和 `cv::VideoWriter` 相关代码
- HEADLESS 模式下：无 imshow/waitKey/VideoWriter，仅保留推流 + 检测功能
- 非 HEADLESS 模式下：保持原有行为不变

**效果**: 依赖链从 100+ 个 .so 降到 ~20 个，包体大幅缩减

### 第 2 步：修复 CMakeLists.txt install 规则，消除垃圾文件

**修改文件**: `examples/yolov8/cpp/CMakeLists.txt`

**核心变更**: 将 `install(DIRECTORY ${SYS_LIB_DIR}/ ...)` 替换为 `install(FILES ...)` 精确安装单个 .so 文件

替换前（有 bug — 拷贝整个系统 lib 目录树）:
```cmake
install(DIRECTORY ${MPP_LIB_DIR}/
    DESTINATION lib FILES_MATCHING PATTERN "librockchip_mpp*")
```

替换后（只安装指定文件）:
```cmake
# 使用 install(CODE ...) 在安装时复制指定 .so 及其符号链接
install(CODE "
    file(GLOB mpp_libs \"${MPP_LIB_DIR}/librockchip_mpp*\")
    file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/lib\" TYPE SHARED_LIBRARY FILES \${mpp_libs})
")
```

或者更简单：改用 `install(FILES ...)` 逐个指定 .so 文件路径。

对于 OpenCV 安装规则（3rdparty/CMakeLists.txt），保持 `install(DIRECTORY ...)` 不变（因为它只拷贝 OpenCV 自己的 .so，源目录是干净的），但需要排除 .a 文件和不必要的子目录(pkgconfig/)。

**额外清理**:
- 移除 install/lib 中的 .a 文件过滤（CMake install 时不要安装 .a）
- 移除 libssl3.so 等冗余文件
- 只安装 OpenCV 核心 5 个模块的 .so（core, imgproc, imgcodecs, videoio, highgui），排除 dnn/ml/stitching 等未使用模块

### 第 3 步：创建 deploy_libs.sh 自动收集传递依赖

**新建文件**: `deploy_libs.sh`（项目根目录）

功能：
1. 查找 install 目录下所有可执行文件
2. 对每个执行 `LD_LIBRARY_PATH=lib ldd`
3. 找出不在 install/lib 中的 .so（排除 linux-vdso, ld-linux 等虚拟库）
4. 复制 .so + 追踪符号链接到 install/lib
5. 跳过系统基础库（libc, libm, libpthread, libdl, librt, libgcc_s, libstdc++）— 这些在目标板上通常已有
6. 复制 mediamtx 到 install 目录
7. 清理 install/lib 中的 .a 文件和无关子目录

**注意**: 只收集非系统库（即 /usr/lib 下除了基础 C/C++ 运行时之外的库）。系统基础库（glibc, libstdc++）不打包，因为它们必须与目标板的内核/glibc 版本匹配。

### 第 4 步：打包 mediamtx

**修改文件**: `examples/yolov8/cpp/CMakeLists.txt`

在 HAVE_MPP_RTMP 块中添加：
```cmake
# 安装 mediamtx (RTMP→WebRTC 流媒体服务器, 静态链接 Go 二进制)
if (EXISTS ${CMAKE_SOURCE_DIR}/mediamtx)
    install(PROGRAMS ${CMAKE_SOURCE_DIR}/mediamtx DESTINATION .)
endif()
```

### 第 5 步：精简 OpenCV 安装范围

**修改文件**: `3rdparty/CMakeLists.txt`

当前 `install(DIRECTORY ... PATTERN "libopencv_*")` 会安装所有 OpenCV 模块（包括 dnn, ml, stitching 等未使用的）。改为只安装实际使用的模块：

```cmake
# 只安装 YOLOv8 demo 实际使用的 OpenCV 模块
set(OPENCV_INSTALL_LIBS
    ${OPENCV_LIB_DIR}/libopencv_core.so*
    ${OPENCV_LIB_DIR}/libopencv_imgproc.so*
    ${OPENCV_LIB_DIR}/libopencv_imgcodecs.so*
    ${OPENCV_LIB_DIR}/libopencv_videoio.so*
    ${OPENCV_LIB_DIR}/libopencv_highgui.so*
)
install(FILES ${OPENCV_INSTALL_LIBS} DESTINATION lib)
```

注意：`install(FILES` 不支持 glob 模式，需要用 `install(DIRECTORY` 但指定排除列表，或用 `file(GLOB ...)` + `install(FILES ...)` 的方式。

## 修改文件汇总

| 文件 | 修改类型 | 说明 |
|------|----------|------|
| examples/yolov8/cpp/CMakeLists.txt | 修改 | HEADLESS 选项 + install 规则修复 + mediamtx |
| examples/yolov8/cpp/main_stream.cc | 修改 | #ifdef HEADLESS 条件编译 |
| 3rdparty/CMakeLists.txt | 修改 | 精简 OpenCV 安装范围 |
| deploy_libs.sh | 新建 | 自动收集传递依赖脚本 |

## 验证方法

1. 清空 install 目录: `rm -rf install/rk3588_linux_aarch64`
2. 重新构建并安装: `./build-linux.sh -t rk3588 -a aarch64 -j4 && cd build_* && cmake --install .`
3. 运行 deploy_libs.sh: `./deploy_libs.sh install/rk3588_linux_aarch64/rknn_yolov8_demo`
4. 检查 install/lib 内容: 应无 .a 文件、无无关子目录、只有必要的 .so
5. 检查依赖: `LD_LIBRARY_PATH=lib ldd ./rknn_yolov8_demo_stream` 应全部找到
6. 运行测试: MJPEG 和 RTMP 模式都能正常工作
7. 检查 mediamtx: `ls -la mediamtx` 应存在
