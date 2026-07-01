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
# 推流走 mjpeg_http_sink.cc (兼容 mjpg_streamer 的 output_http.so)
# 使用 NPU 零拷贝 (rknpu2/yolov8_zero_copy.cc + ZERO_COPY 宏),
# letterbox 直接写入 rknn_create_mem 出来的 DMA 内存, 省一次 1.2MB memcpy
add_executable(${PROJECT_NAME}_stream
    main_stream.cc
    postprocess.cc
    v4l2_capture.cc
    mjpeg_http_sink.cc
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