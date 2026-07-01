cmake_minimum_required(VERSION 3.15)

project(rknn_model_zoo_3rdparty_libs)

if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    set (TARGET_LIB_ARCH ${CMAKE_ANDROID_ARCH_ABI})
else()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set (TARGET_LIB_ARCH aarch64)
    else()
        set (TARGET_LIB_ARCH armhf)
    endif()
    if (CMAKE_C_COMPILER MATCHES "uclibc")
        set (TARGET_LIB_ARCH ${TARGET_LIB_ARCH}_uclibc)
    endif()
endif()

# stb image
set(STB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/stb_image)
set(STB_INCLUDES ${STB_PATH} PARENT_SCOPE)

# jpeg turbo
set(JPEG_PATH ${CMAKE_CURRENT_SOURCE_DIR}/jpeg_turbo)
set(LIBJPEG ${JPEG_PATH}/${CMAKE_SYSTEM_NAME}/${TARGET_LIB_ARCH}/libturbojpeg.a PARENT_SCOPE)
set(LIBJPEG_INCLUDES ${JPEG_PATH}/include PARENT_SCOPE)

# rknn runtime
# for rknpu2
if (TARGET_SOC STREQUAL "rk3588" OR TARGET_SOC STREQUAL "rk3576" OR TARGET_SOC STREQUAL "rk356x" OR TARGET_SOC STREQUAL "rv1106" OR TARGET_SOC STREQUAL "rv1103")
    set(RKNN_PATH ${CMAKE_CURRENT_SOURCE_DIR}/rknpu2)
    if (TARGET_SOC STREQUAL "rk3588" OR TARGET_SOC STREQUAL "rk356x" OR TARGET_SOC STREQUAL "rk3576" )
        set(LIBRKNNRT ${RKNN_PATH}/${CMAKE_SYSTEM_NAME}/${TARGET_LIB_ARCH}/librknnrt.so)
    endif()
    if (TARGET_SOC STREQUAL "rv1106" OR TARGET_SOC STREQUAL "rv1103")
        set(LIBRKNNRT ${RKNN_PATH}/${CMAKE_SYSTEM_NAME}/armhf-uclibc/librknnmrt.so)
    endif()
    set(LIBRKNNRT_INCLUDES ${RKNN_PATH}/include PARENT_SCOPE)
endif()

# for rknpu1
if(TARGET_SOC STREQUAL "rk1808" OR TARGET_SOC STREQUAL "rv1109" OR TARGET_SOC STREQUAL "rv1126")
    set(RKNN_PATH ${CMAKE_CURRENT_SOURCE_DIR}/rknpu1)

    set(LIBRKNNRT ${RKNN_PATH}/${CMAKE_SYSTEM_NAME}/${TARGET_LIB_ARCH}/librknn_api.so)

    set(LIBRKNNRT_INCLUDES ${RKNN_PATH}/include PARENT_SCOPE)
endif()
install(PROGRAMS ${LIBRKNNRT} DESTINATION lib)
set(LIBRKNNRT ${LIBRKNNRT} PARENT_SCOPE)

# rga
set(RGA_PATH ${CMAKE_CURRENT_SOURCE_DIR}/librga)
set(LIBRGA ${RGA_PATH}/${CMAKE_SYSTEM_NAME}/${TARGET_LIB_ARCH}/librga.a PARENT_SCOPE)
set(LIBRGA_INCLUDES ${RGA_PATH}/include PARENT_SCOPE)
install(PROGRAMS ${RGA_PATH}/${CMAKE_SYSTEM_NAME}/${TARGET_LIB_ARCH}/librga.so DESTINATION lib)

# opencv (3rdparty/opencv-3.4.5/install)
# 与 librknnrt / librga 一起 install 到 lib/, 这样整个 install 目录可以直接打包发板
set(OPENCV_PATH ${CMAKE_CURRENT_SOURCE_DIR}/opencv-3.4.5/install)
if (EXISTS ${OPENCV_PATH}/lib)
    set(OPENCV_LIB_DIR ${OPENCV_PATH}/lib)
else()
    set(OPENCV_LIB_DIR ${OPENCV_PATH}/share/OpenCV/3rdparty/lib)
endif()
set(OPENCV_INCLUDE_DIR ${OPENCV_PATH}/include)
set(OPENCV_LIBS
    ${OPENCV_LIB_DIR}/libopencv_core.so
    ${OPENCV_LIB_DIR}/libopencv_imgproc.so
    ${OPENCV_LIB_DIR}/libopencv_imgcodecs.so
    ${OPENCV_LIB_DIR}/libopencv_videoio.so
    ${OPENCV_LIB_DIR}/libopencv_highgui.so
)
set(OPENCV_LIB_DIR      ${OPENCV_LIB_DIR}      PARENT_SCOPE)
set(OPENCV_INCLUDE_DIR  ${OPENCV_INCLUDE_DIR}  PARENT_SCOPE)
set(OPENCV_LIBS         ${OPENCV_LIBS}         PARENT_SCOPE)
# 用 DIRECTORY 复制整个目录, 保留 .so -> .so.3.4 -> .so.3.4.5 三级软链,
# 避免只复制裸 .so 名称导致断链
install(DIRECTORY ${OPENCV_LIB_DIR}/
    DESTINATION lib
    FILES_MATCHING PATTERN "libopencv_*"
)

# timer
set(TIMER_PATH ${CMAKE_CURRENT_SOURCE_DIR}/timer)
set(LIBTIMER_INCLUDES ${TIMER_PATH} PARENT_SCOPE)

# libsndfile
set(LIBSNDFILE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/libsndfile)
set(LIBSNDFILE_INCLUDES ${LIBSNDFILE_PATH}/include PARENT_SCOPE)
set(LIBSNDFILE ${LIBSNDFILE_PATH}/${CMAKE_SYSTEM_NAME}/${TARGET_LIB_ARCH}/libsndfile.a PARENT_SCOPE)

# fftw
set(LIBFFTW_PATH ${CMAKE_CURRENT_SOURCE_DIR}/fftw)
set(LIBFFTW_INCLUDES ${LIBFFTW_PATH}/include PARENT_SCOPE)
set(LIBFFTW ${LIBFFTW_PATH}/${CMAKE_SYSTEM_NAME}/${TARGET_LIB_ARCH}/libfftw3f.a PARENT_SCOPE)

# kaldi_native_fbank
set(LIBKALDI_NATIVE_FBANK_PATH ${CMAKE_CURRENT_SOURCE_DIR}/kaldi_native_fbank)
set(LIBKALDI_NATIVE_FBANK_INCLUDES ${LIBKALDI_NATIVE_FBANK_PATH}/include PARENT_SCOPE)
set(LIBKALDI_NATIVE_FBANK ${LIBKALDI_NATIVE_FBANK_PATH}/${CMAKE_SYSTEM_NAME}/${TARGET_LIB_ARCH}/libkaldi-native-fbank-core.a PARENT_SCOPE)