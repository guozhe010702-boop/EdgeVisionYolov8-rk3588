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

file(GLOB SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)

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
add_executable(${PROJECT_NAME}_stream
    main_stream.cc
    postprocess.cc
    v4l2_capture.cc
    mjpeg_http_sink.cc
    rtmp_sink.cc
    rknpu2/yolov8_zero_copy.cc
)

target_compile_definitions(${PROJECT_NAME}_stream PRIVATE ZERO_COPY)

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
    ${OPENCV_LIBS}
    ${LIBRKNNRT}
    dl
)

# MPP 硬件编码 (H.264) + librtmp 推流 (Linux + 有 MPP 库即可, 不限架构)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # 按架构和发行版搜索 MPP 库
    find_library(MPP_LIBRARY
        NAMES rockchip_mpp mpp
        PATHS /usr/lib/aarch64-linux-gnu
              /usr/lib/arm-linux-gnueabihf
              /usr/lib/arm-linux-gnueabi
              /usr/lib
              /usr/local/lib
    )
    # 按架构和发行版搜索 librtmp
    find_library(RTMP_LIBRARY
        NAMES rtmp
        PATHS /usr/lib/aarch64-linux-gnu
              /usr/lib/arm-linux-gnueabihf
              /usr/lib/arm-linux-gnueabi
              /usr/lib
              /usr/local/lib
    )
    # 搜索 MPP 头文件路径
    find_path(MPP_INCLUDE_DIR
        NAMES rk_mpi.h
        PATHS /usr/include/rockchip
              /usr/local/include/rockchip
    )

    if (MPP_LIBRARY AND RTMP_LIBRARY)
        message(STATUS "Linking MPP: ${MPP_LIBRARY}")
        message(STATUS "Linking RTMP: ${RTMP_LIBRARY}")
        target_link_libraries(${PROJECT_NAME}_stream ${MPP_LIBRARY} ${RTMP_LIBRARY} ssl crypto)
        if (MPP_INCLUDE_DIR)
            target_include_directories(${PROJECT_NAME}_stream PRIVATE ${MPP_INCLUDE_DIR})
        else()
            target_include_directories(${PROJECT_NAME}_stream PRIVATE /usr/include/rockchip)
        endif()

        # 将 MPP/RTMP/SSL 运行时库打包到 install/lib, 方便整个 install 目录直接拷贝到新板子
        # 使用 install(DIRECTORY) 保留软链接, 避免断链
        get_filename_component(MPP_LIB_DIR ${MPP_LIBRARY} DIRECTORY)
        get_filename_component(RTMP_LIB_DIR ${RTMP_LIBRARY} DIRECTORY)

        # 搜索 libssl.so 和 libcrypto.so (librtmp 依赖)
        find_library(SSL_LIBRARY
            NAMES ssl ssl3
            PATHS ${MPP_LIB_DIR} /usr/lib/aarch64-linux-gnu /usr/lib/arm-linux-gnueabihf /usr/lib
        )
        find_library(CRYPTO_LIBRARY
            NAMES crypto crypto3
            PATHS ${MPP_LIB_DIR} /usr/lib/aarch64-linux-gnu /usr/lib/arm-linux-gnueabihf /usr/lib
        )

        # 安装 MPP .so (带版本号的, 如 librockchip_mpp.so.1)
        install(DIRECTORY ${MPP_LIB_DIR}/
            DESTINATION lib
            FILES_MATCHING PATTERN "librockchip_mpp*"
        )
        # 安装 librtmp .so
        install(DIRECTORY ${RTMP_LIB_DIR}/
            DESTINATION lib
            FILES_MATCHING PATTERN "librtmp*"
        )
        # 安装 libssl/libcrypto .so (如果找到了)
        if (SSL_LIBRARY)
            get_filename_component(SSL_LIB_DIR ${SSL_LIBRARY} DIRECTORY)
            install(DIRECTORY ${SSL_LIB_DIR}/
                DESTINATION lib
                FILES_MATCHING PATTERN "libssl*"
            )
        endif()
        if (CRYPTO_LIBRARY)
            get_filename_component(CRYPTO_LIB_DIR ${CRYPTO_LIBRARY} DIRECTORY)
            install(DIRECTORY ${CRYPTO_LIB_DIR}/
                DESTINATION lib
                FILES_MATCHING PATTERN "libcrypto*"
            )
        endif()
    else()
        message(WARNING "MPP or RTMP library not found, RTMP streaming will not be available")
        message(STATUS "  MPP_LIBRARY: ${MPP_LIBRARY}")
        message(STATUS "  RTMP_LIBRARY: ${RTMP_LIBRARY}")
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
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../model/bus.jpg DESTINATION model)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../model/coco_80_labels_list.txt DESTINATION model)
file(GLOB RKNN_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../model/*.rknn")
install(FILES ${RKNN_FILES} DESTINATION model)