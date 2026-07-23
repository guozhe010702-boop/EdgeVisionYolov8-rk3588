cmake_minimum_required(VERSION 3.10)

project(rknn_yolov8_demo)

if (ENABLE_ASAN)
	message(STATUS "BUILD WITH ADDRESS SANITIZER")
	set (CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
	set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
	set (CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
endif ()

set(rknpu_yolov8_file rknpu2/yolov8.cc)

if (TARGET_SOC STREQUAL "rv1106" OR TARGET_SOC STREQUAL "rv1103")
    add_definitions(-DRV1106_1103)
    set(rknpu_yolov8_file rknpu2/yolov8_rv1106_1103.cc)
    #dma
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../3rdparty/allocator/dma)
endif()

if(TARGET_SOC STREQUAL "rk1808" OR TARGET_SOC STREQUAL "rv1109" OR TARGET_SOC STREQUAL "rv1126")
    add_definitions(-DRKNPU1)
    set(rknpu_yolov8_file rknpu1/yolov8.cc)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../3rdparty/ 3rdparty.out)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../utils/ utils.out)

set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")

# opencv 路径与 install 已在 ../../../3rdparty/CMakeLists.txt 中处理
# 这里只需要拿到 lib 列表和 include 路径供 target 使用
set(OPENCV_INCLUDE_DIR ${OPENCV_INCLUDE_DIR})

add_executable(${PROJECT_NAME}
    main.cc
    postprocess.cc
    ${rknpu_yolov8_file}
)

target_link_libraries(${PROJECT_NAME}
    imageutils
    fileutils
    imagedrawing    
    ${LIBRKNNRT}
    dl
)

if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(${PROJECT_NAME}
    log
)
endif()

message(STATUS "!!!!!!!!!!!CMAKE_SYSTEM_NAME: ${CMAKE_SYSTEM_NAME}")
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} Threads::Threads)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBRKNNRT_INCLUDES}
)


# Currently zero copy only supports rknpu2, v1103/rv1103b/rv1106 supports zero copy by default
if (NOT (TARGET_SOC STREQUAL "rv1106" OR TARGET_SOC STREQUAL "rv1103" OR TARGET_SOC STREQUAL "rk1808" 
    OR TARGET_SOC STREQUAL "rv1109" OR TARGET_SOC STREQUAL "rv1126" OR TARGET_SOC STREQUAL "rv1103b"))
    add_executable(${PROJECT_NAME}_zero_copy
        main.cc
        postprocess.cc
        rknpu2/yolov8_zero_copy.cc
    )

    target_compile_definitions(${PROJECT_NAME}_zero_copy PRIVATE ZERO_COPY)

    target_link_libraries(${PROJECT_NAME}_zero_copy
        imageutils
        fileutils
        imagedrawing    
        ${LIBRKNNRT}
        dl
    )

    if (CMAKE_SYSTEM_NAME STREQUAL "Android")
        target_link_libraries(${PROJECT_NAME}_zero_copy
        log
    )
    endif()

    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(THREADS_PREFER_PTHREAD_FLAG ON)
        find_package(Threads REQUIRED)
        target_link_libraries(${PROJECT_NAME}_zero_copy Threads::Threads)
    endif()

    target_include_directories(${PROJECT_NAME}_zero_copy PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${LIBRKNNRT_INCLUDES}
    )
    install(TARGETS ${PROJECT_NAME}_zero_copy DESTINATION .)
endif()

# 离线视频检测 Demo (使用 3rdparty 中的 OpenCV 解码视频)
add_executable(${PROJECT_NAME}_video
    main_video.cc
    postprocess.cc
    ${rknpu_yolov8_file}
)

target_link_libraries(${PROJECT_NAME}_video
    imageutils
    fileutils
    imagedrawing
    ${OPENCV_LIBS}
    ${LIBRKNNRT}
    dl
)

if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(${PROJECT_NAME}_video log)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME}_video Threads::Threads)
endif()

target_include_directories(${PROJECT_NAME}_video PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBRKNNRT_INCLUDES}
    ${OPENCV_INCLUDE_DIR}
)
install(TARGETS ${PROJECT_NAME}_video DESTINATION .)

# 实时视频检测 Demo (摄像头)
# 自带 V4L2 采集 (v4l2_capture.cc), 不依赖系统 libv4l2, MJPG 解码走 3rdparty/jpeg_turbo
# 推流支持两种模式:
#   1. MJPEG-over-HTTP (mjpeg_http_sink.cc, 兼容 mjpg_streamer)
#   2. RTMP H.264 (rtmp_sink.cc, MPP 硬编码 + librtmp, 搭配 mediamtx WebRTC)
# 使用 NPU 零拷贝 (rknpu2/yolov8_zero_copy.cc + ZERO_COPY 宏),
# letterbox 直接写入 rknn_create_mem 出来的 DMA 内存, 省一次 1.2MB memcpy
#
# MPP/RTMP 条件编译: 找到 MPP+RTMP 时启用 RTMP 推流, 否则只编译 MJPEG 推流
set(STREAM_SRCS
    main_stream.cc
    postprocess.cc
    v4l2_capture.cc
    mjpeg_http_sink.cc
    rknpu2/yolov8_zero_copy.cc
)

# 检测 MPP + RTMP 是否可用 (Linux 平台, 按架构和发行版搜索)
set(HAVE_MPP_RTMP OFF)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_library(MPP_LIBRARY
        NAMES rockchip_mpp mpp
        PATHS /usr/lib/aarch64-linux-gnu
              /usr/lib/arm-linux-gnueabihf
              /usr/lib/arm-linux-gnueabi
              /usr/lib
              /usr/local/lib
              ${CMAKE_SYSROOT}/usr/lib
    )
    find_library(RTMP_LIBRARY
        NAMES rtmp
        PATHS /usr/lib/aarch64-linux-gnu
              /usr/lib/arm-linux-gnueabihf
              /usr/lib/arm-linux-gnueabi
              /usr/lib
              /usr/local/lib
              ${CMAKE_SYSROOT}/usr/lib
    )
    find_path(MPP_INCLUDE_DIR
        NAMES rk_mpi.h
        PATHS /usr/include/rockchip
              /usr/local/include/rockchip
              ${CMAKE_SYSROOT}/usr/include/rockchip
    )
    if (MPP_LIBRARY AND RTMP_LIBRARY)
        set(HAVE_MPP_RTMP ON)
    endif()
endif()

# RTMP 推流功能: 仅在 MPP+RTMP 都可用时编入
if (HAVE_MPP_RTMP)
    list(APPEND STREAM_SRCS rtmp_sink.cc)
    message(STATUS "RTMP streaming: ENABLED (MPP=${MPP_LIBRARY}, RTMP=${RTMP_LIBRARY})")
else()
    message(WARNING "MPP or RTMP not found, RTMP streaming disabled (MJPEG still available)")
    message(STATUS "  MPP_LIBRARY: ${MPP_LIBRARY}")
    message(STATUS "  RTMP_LIBRARY: ${RTMP_LIBRARY}")
endif()

add_executable(${PROJECT_NAME}_stream ${STREAM_SRCS})

# HEADLESS 模式: 去除 OpenCV highgui/videoio 依赖, 适用无头板部署
# 去除 imshow/waitKey/VideoWriter, 依赖链从 100+ 降到 ~20 个 .so
option(HEADLESS "Disable OpenCV highgui/videoio for headless streaming (reduces runtime deps)" ON)
if (HEADLESS)
    target_compile_definitions(${PROJECT_NAME}_stream PRIVATE HEADLESS)
    message(STATUS "HEADLESS mode: ON (no OpenCV highgui/videoio, minimal deps)")
else()
    message(STATUS "HEADLESS mode: OFF (full OpenCV with highgui/videoio)")
endif()

target_compile_definitions(${PROJECT_NAME}_stream PRIVATE ZERO_COPY)
if (HAVE_MPP_RTMP)
    target_compile_definitions(${PROJECT_NAME}_stream PRIVATE HAVE_MPP_RTMP)
endif()

# 定位 libturbojpeg.a (只在 linux aarch64/armhf/armhf_uclibc/x64 平台有预编译)
set(JPEG_TURBO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../3rdparty/jpeg_turbo)
set(JPEG_TURBO_LIB "")
# 优先按 TARGET_SOC 选 (避免 rv1106 用到 armhf 那份不兼容的 .a)
if (TARGET_SOC STREQUAL "rv1106" OR TARGET_SOC STREQUAL "rv1103")
    set(JPEG_TURBO_LIB ${JPEG_TURBO_DIR}/Linux/armhf_uclibc/libturbojpeg.a)
elseif (CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    set(JPEG_TURBO_LIB ${JPEG_TURBO_DIR}/Linux/aarch64/libturbojpeg.a)
elseif (CMAKE_SYSTEM_PROCESSOR STREQUAL "arm" OR CMAKE_SYSTEM_PROCESSOR MATCHES "armhf")
    set(JPEG_TURBO_LIB ${JPEG_TURBO_DIR}/Linux/armhf/libturbojpeg.a)
elseif (CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "AMD64")
    set(JPEG_TURBO_LIB ${JPEG_TURBO_DIR}/Linux/x64/libturbojpeg.a)
endif()

target_include_directories(${PROJECT_NAME}_stream PRIVATE
    ${JPEG_TURBO_DIR}/include
)

target_link_libraries(${PROJECT_NAME}_stream
    imageutils
    fileutils
    imagedrawing
    ${LIBRKNNRT}
    dl
)

# HEADLESS 模式: 只链接 OpenCV core+imgproc (无 highgui/videoio 的 GTK3/GStreamer/FFmpeg 依赖链)
if (HEADLESS)
    target_link_libraries(${PROJECT_NAME}_stream
        ${OPENCV_LIB_DIR}/libopencv_core.so
        ${OPENCV_LIB_DIR}/libopencv_imgproc.so
    )
else()
    target_link_libraries(${PROJECT_NAME}_stream ${OPENCV_LIBS})
endif()

# MPP/RTMP 链接 + 运行时库打包 (仅 HAVE_MPP_RTMP 时)
if (HAVE_MPP_RTMP)
    target_link_libraries(${PROJECT_NAME}_stream ${MPP_LIBRARY} ${RTMP_LIBRARY} ssl crypto)
    if (MPP_INCLUDE_DIR)
        target_include_directories(${PROJECT_NAME}_stream PRIVATE ${MPP_INCLUDE_DIR})
    else()
        target_include_directories(${PROJECT_NAME}_stream PRIVATE /usr/include/rockchip)
    endif()
    # RGA: RGB→NV12 硬件加速 (rtmp_sink.cc 中使用, MPP DMA buffer 做 imcvtcolor)
    target_include_directories(${PROJECT_NAME}_stream PRIVATE ${LIBRGA_INCLUDES})
    target_link_libraries(${PROJECT_NAME}_stream ${LIBRGA})

    # 将 MPP/RTMP/SSL/RGA 运行时库打包到 install/lib
    # 使用 install(CODE) + file(GLOB) 精确安装 .so 文件 (含符号链接), 避免 install(DIRECTORY) 把整个系统 lib 目录树拷贝进来
    get_filename_component(MPP_LIB_DIR ${MPP_LIBRARY} DIRECTORY)
    get_filename_component(RTMP_LIB_DIR ${RTMP_LIBRARY} DIRECTORY)

    find_library(SSL_LIBRARY
        NAMES ssl ssl3
        PATHS ${MPP_LIB_DIR} /usr/lib/aarch64-linux-gnu /usr/lib/arm-linux-gnueabihf /usr/lib
    )
    find_library(CRYPTO_LIBRARY
        NAMES crypto crypto3
        PATHS ${MPP_LIB_DIR} /usr/lib/aarch64-linux-gnu /usr/lib/arm-linux-gnueabihf /usr/lib
    )

    # MPP .so (librockchip_mpp.so → .so.1 → .so.0)
    install(CODE "
        file(GLOB _mpp_so \"${MPP_LIB_DIR}/librockchip_mpp.so*\")
        file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/lib\" TYPE SHARED_LIBRARY FILES \${_mpp_so})
    ")
    # RTMP .so (librtmp.so → .so.1)
    install(CODE "
        file(GLOB _rtmp_so \"${RTMP_LIB_DIR}/librtmp.so*\")
        file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/lib\" TYPE SHARED_LIBRARY FILES \${_rtmp_so})
    ")
    # SSL .so
    if (SSL_LIBRARY)
        get_filename_component(SSL_LIB_DIR ${SSL_LIBRARY} DIRECTORY)
        install(CODE "
            file(GLOB _ssl_so \"${SSL_LIB_DIR}/libssl.so*\")
            file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/lib\" TYPE SHARED_LIBRARY FILES \${_ssl_so})
        ")
    endif()
    # Crypto .so
    if (CRYPTO_LIBRARY)
        get_filename_component(CRYPTO_LIB_DIR ${CRYPTO_LIBRARY} DIRECTORY)
        install(CODE "
            file(GLOB _crypto_so \"${CRYPTO_LIB_DIR}/libcrypto.so*\")
            file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/lib\" TYPE SHARED_LIBRARY FILES \${_crypto_so})
        ")
    endif()
    # RGA 运行时 .so
    get_filename_component(RGA_LIB_DIR ${LIBRGA} DIRECTORY)
    install(CODE "
        file(GLOB _rga_so \"${RGA_LIB_DIR}/librga.so*\")
        file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/lib\" TYPE SHARED_LIBRARY FILES \${_rga_so})
    ")

    # mediamtx (RTMP→WebRTC 流媒体服务器, 静态链接 Go 二进制)
    # CMAKE_SOURCE_DIR 是 examples/yolov8/cpp, 所以向上 3 级找项目根
    get_filename_component(PROJ_ROOT "${CMAKE_SOURCE_DIR}/../../.." ABSOLUTE)
    if (EXISTS ${PROJ_ROOT}/mediamtx)
        install(PROGRAMS ${PROJ_ROOT}/mediamtx DESTINATION .)
        message(STATUS "mediamtx: will be installed from ${PROJ_ROOT}/mediamtx")
    else()
        message(STATUS "mediamtx: not found at ${PROJ_ROOT}/mediamtx (skip)")
    endif()
endif()

if (JPEG_TURBO_LIB AND EXISTS ${JPEG_TURBO_LIB})
    message(STATUS "Linking libturbojpeg: ${JPEG_TURBO_LIB}")
    target_link_libraries(${PROJECT_NAME}_stream ${JPEG_TURBO_LIB})
else()
    message(WARNING "libturbojpeg.a not found for ${CMAKE_SYSTEM_PROCESSOR}, YUYV fallback only")
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(${PROJECT_NAME}_stream log)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME}_stream Threads::Threads)
endif()

target_include_directories(${PROJECT_NAME}_stream PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBRKNNRT_INCLUDES}
    ${OPENCV_INCLUDE_DIR}
)
install(TARGETS ${PROJECT_NAME}_stream DESTINATION .)

install(TARGETS ${PROJECT_NAME} DESTINATION .)

# 安装启动脚本 (设置 LD_LIBRARY_PATH, 解决 OpenCV .so 残留构建路径 RUNPATH 的问题)
get_filename_component(PROJ_ROOT2 "${CMAKE_SOURCE_DIR}/../../.." ABSOLUTE)
if (EXISTS ${PROJ_ROOT2}/run.sh)
    install(PROGRAMS ${PROJ_ROOT2}/run.sh DESTINATION .)
endif()

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../model/bus.jpg DESTINATION model)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../model/coco_80_labels_list.txt DESTINATION model)
file(GLOB RKNN_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../model/*.rknn")
install(FILES ${RKNN_FILES} DESTINATION model)