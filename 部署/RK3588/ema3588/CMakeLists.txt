cmake_minimum_required(VERSION 3.15)
project(edgevision_ema3588 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")
set(UPSTREAM "${CMAKE_CURRENT_LIST_DIR}/../upstream")
set(PRIVATE_DEPS "${CMAKE_CURRENT_LIST_DIR}/../build-deps/root" CACHE PATH "Extracted Ubuntu packages")
set(PRIVATE_LIB "${PRIVATE_DEPS}/usr/lib/aarch64-linux-gnu")
find_package(Threads REQUIRED)
include_directories("${UPSTREAM}/cpp" "${UPSTREAM}/utils"
    "${UPSTREAM}/rknn/include" "${UPSTREAM}/rga/include"
    "${UPSTREAM}/stb_image" "${PRIVATE_DEPS}/usr/include"
    "${PRIVATE_DEPS}/usr/include/opencv4")
add_library(edge_utils STATIC "${UPSTREAM}/utils/image_utils.c"
    "${UPSTREAM}/utils/file_utils.c" "${UPSTREAM}/utils/image_drawing.c")
target_compile_definitions(edge_utils PRIVATE LIBRGA_IM2D_HANDLE)
target_link_libraries(edge_utils PUBLIC "${UPSTREAM}/rga/librga.so"
    "${PRIVATE_LIB}/libturbojpeg.so" m dl)
add_executable(rknn_yolov8_demo_stream "${UPSTREAM}/cpp/main_stream.cc"
    "${UPSTREAM}/cpp/postprocess.cc" "${UPSTREAM}/cpp/v4l2_capture.cc"
    "${UPSTREAM}/cpp/mjpeg_http_sink.cc" "${UPSTREAM}/cpp/rknpu2/yolov8_zero_copy.cc")
target_compile_definitions(rknn_yolov8_demo_stream PRIVATE HEADLESS ZERO_COPY)
target_link_libraries(rknn_yolov8_demo_stream PRIVATE edge_utils Threads::Threads
    "${UPSTREAM}/rknn/librknnrt.so" "${PRIVATE_LIB}/libopencv_core.so"
    "${PRIVATE_LIB}/libopencv_imgproc.so")
add_executable(rknn_yolov8_demo "${UPSTREAM}/cpp/main.cc"
    "${UPSTREAM}/cpp/postprocess.cc" "${UPSTREAM}/cpp/rknpu2/yolov8.cc")
target_link_libraries(rknn_yolov8_demo PRIVATE edge_utils "${UPSTREAM}/rknn/librknnrt.so")
add_executable(rknn_yolov8_demo_zero_copy "${UPSTREAM}/cpp/main.cc"
    "${UPSTREAM}/cpp/postprocess.cc" "${UPSTREAM}/cpp/rknpu2/yolov8_zero_copy.cc")
target_compile_definitions(rknn_yolov8_demo_zero_copy PRIVATE ZERO_COPY)
target_link_libraries(rknn_yolov8_demo_zero_copy PRIVATE edge_utils "${UPSTREAM}/rknn/librknnrt.so")
set_target_properties(rknn_yolov8_demo_stream rknn_yolov8_demo rknn_yolov8_demo_zero_copy PROPERTIES INSTALL_RPATH "$ORIGIN/lib")
install(TARGETS rknn_yolov8_demo_stream rknn_yolov8_demo rknn_yolov8_demo_zero_copy RUNTIME DESTINATION .)
install(FILES "${UPSTREAM}/rknn/librknnrt.so" "${UPSTREAM}/rga/librga.so" DESTINATION lib)
install(DIRECTORY "${UPSTREAM}/model/" DESTINATION model)
