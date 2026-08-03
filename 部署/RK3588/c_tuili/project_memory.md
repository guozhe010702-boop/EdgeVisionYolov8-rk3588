## Hard Constraints
- All runtime libraries must be placed in the install/lib directory to ensure portability across development boards
- Video processing functionality must use libraries from the 3rdparty folder
- All detection features must maintain compatibility with RV1106/RV1103 platforms using dma_buf_alloc for CMA memory
- rknn_run debug printing must be throttled to once per second to reduce syscall overhead
- H.264 streaming mode must use RK3588 MPP hardware encoder with 640x480@30fps, 1Mbps CBR, High Profile, and GOP=30
- mediamtx must be deployed as a static binary on target boards for RTMP/WebRTC protocol conversion
- RTMP推流地址必须使用rtmp://127.0.0.1:1936/live/stream（端口1935被其他进程占用无法释放，使用1936替代）

## Engineering Conventions
- New detection modes are implemented in separate main files (main_video.cc for offline, main_stream.cc for real-time)
- Executables for different detection modes follow the naming pattern: rknn_yolov8_demo_[mode]
- OpenCV runtime libraries are installed via 3rdparty/CMakeLists.txt using install(DIRECTORY) to preserve soft links and versioned files
- YOLOv8 demo currently provides 4 modes in install/<soc>_linux_<arch>/rknn_yolov8_demo/: rknn_yolov8_demo (image), rknn_yolov8_demo_zero_copy (zero-copy image), rknn_yolov8_demo_video (offline video), rknn_yolov8_demo_stream (real-time camera)
- MJPEG HTTP streaming must use asynchronous producer-consumer model with separate detection and sender threads to prevent slow clients from blocking the detection loop
- JPEG encoding must use TJFLAG_NOREALLOC with preallocated buffers (size W*H*2) to eliminate per-frame memory allocations
- HTTP frame transmission must use writev() to combine multipart header, JPEG data, and boundary into a single system call
- V4L2 capture must decode MJPG directly to RGB format (TJPF_RGB) to avoid redundant BGR↔RGB conversions
- cv::Mat buffers for frame processing (bgr_for_save, bgr_for_show) must be preallocated outside the main loop and reused to reduce construction/destruction overhead
- H.264 RTMP streaming must be implemented in separate rtmp_sink.h/cc files, maintaining compatibility with existing MJPEG mode via command-line parameter
- rgb_to_nv12 conversion must only memset padding regions when src_w != enc_w or src_h != enc_h to avoid unnecessary memory operations
- NV12 conversion should write directly to MPP DMA memory (mpp_buffer_get_ptr(enc_buf_)) without intermediate heap buffers to eliminate redundant memcpy
- libturbojpeg decoding must write directly to preallocated cv::Mat buffers (out.data) instead of using .clone() to avoid deep copies
- V4L2Capture must support both Single-planar (V4L2_CAP_VIDEO_CAPTURE) and Multiplanar (V4L2_CAP_VIDEO_CAPTURE_MPLANAR) devices, with priority for MIPI cameras
- YUV→RGB conversion (NV12/YUYV/UYVY) must use OpenCV cv::cvtColor / cvtColorTwoPlane instead of hand-written loops — OpenCV has internal NEON/SSE optimization, more portable and faster
- RGB→NV12 conversion in RTMP path must try RGA hardware acceleration (imcvtcolor) first, with CPU fallback (rgb_to_nv12). RGA MPP DMA buffer handle is pre-imported in init_mpp and cached
- V4L2 capture wait must use poll() instead of select() — lighter weight for single-fd scenarios
- V4L2Capture must check if output Mat dimensions match before calling create() to avoid unnecessary allocations
- When both save and show features are enabled, RGB→BGR conversion must be performed once and the resulting Mat reused for both operations

## Lessons Learned
- OpenCV .so files from 3rdparty must be deployed to the lib directory on target boards for proper execution
- Using install(PROGRAMS) for OpenCV .so files results in broken soft links on target boards; install(DIRECTORY) with FILES_MATCHING is required instead
- OpenCV CAP_V4L2 backend uses libv4l2 compatibility layer (v4l1 emulation) which may cause UVC camera incompatibility (invalid fd errors)
- OpenCV GStreamer backend may SIGBUS (bus error / core dump) on boards with GStreamer ABI mismatch
- **Real-time camera capture: bypass OpenCV videoio entirely** — use the custom V4L2Capture class in examples/yolov8/cpp/v4l2_capture.{h,cc} which does direct V4L2 ioctl (no libv4l2) and uses 3rdparty/jpeg_turbo libturbojpeg.a for MJPG decoding; YUYV converted to BGR manually
- yolov8.cc (rknpu2/) has TWO pre-existing API/format bugs: (1) calls rknn_init with 4 args but the header declares 5 — must pass NULL as 5th arg; (2) NHWC dim interpretation is wrong (uses dims[0] for channel, should be dims[3]). Symptom: "rknn_inputs_set, param input size(409600) < model input size(1228800)" for a 3-channel 640x640 model. The fix must apply BOTH the rknn_init NULL and the dims[3] for channel. dump_tensor_attr prints dims in reverse order (dims[3], dims[2], dims[1], dims[0]), so a logged `dims=[3, 640, 640, 1]` actually means the array is [1, 640, 640, 3]
- build-linux.sh: must add `set +H` at the top to prevent bash history expansion from breaking echo strings containing "!" in \e[0m color escapes
- UVC cameras (e.g. Orbbec Astra Pro) typically only support YUYV and MJPG formats; requesting RGB/BGR via OpenCV triggers VIDIOCSPICT (v4l1) failures
- libturbojpeg.a prebuilt paths: 3rdparty/jpeg_turbo/Linux/{aarch64,armhf,armhf_uclibc,x64}/libturbojpeg.a — link statically so no extra .so deploy needed
- Linking order matters for libturbojpeg.a static link: place it AFTER other libs in target_link_libraries, otherwise libturbojpeg symbols may not resolve
- mjpeg_http_sink.cc: Each client connection must be handled in a separate detached thread to prevent accept() blocking; use active_clients_ atomic counter and condition variable to ensure proper cleanup during shutdown
- mjpeg_http_sink.cc: Set SO_RCVTIMEO=2s on client sockets to prevent recv() from blocking indefinitely
- mjpeg_http_sink.cc: In client_thread_fn, extract self and fd from the argument struct before deleting it to avoid use-after-free errors when accessing a->self after deletion
- mjpeg_http_sink.cc handle_client: check `stop_flag_` under client_mutex_ before adding fd to client_fds_; if stop is in progress, shutdown+close the new fd directly. Prevents fd leak when stop() runs between HTTP-header send and the client list insert
- mjpeg_http_sink.cc: cv::imencode from OpenCV 3.4 may segfault on ARM with statically-linked libjpeg; replace it with libturbojpeg's tjCompress2 (use TJPF_BGR, TJSAMP_420, TJFLAG_FASTDCT, quality=80) and tjFree the returned buffer each frame
- mjpeg_http_sink.cc async push: use cv::Mat swap (std::swap not cv::Mat::swap — OpenCV 3.x has no member swap) for zero-copy producer/consumer between detection thread and sender thread. Detection thread calls submit(rgb) which std::swap(rgb, pending_frame_); sender loop std::swap(pending_frame_, local) and takes ownership. Both swaps are O(1) shallow pointer swaps, no pixel copy
- mjpeg_http_sink.cc: Short write or EAGAIN on writev → treat the whole frame as "slow client", skip rather than retry
- yolov8_zero_copy.cc: setup rknn_create_mem + rknn_set_io_mem (UINT8) for input, point dst_img.fd / dst_img.virt_addr at app_ctx->input_mems[0], then convert_image_with_letterbox writes directly to NPU DMA memory. No rknn_inputs_set needed. For outputs, allocate output_mems via rknn_create_mem, then post-process reads from app_ctx->output_mems[i]->virt_addr (handle NC1HWC2 → NCHW conversion in software)
- main_stream.cc: install SIGINT/SIGTERM handler that only sets an atomic<int> flag; main loop checks the flag at top of each frame to break out cleanly so push_sink->stop() can run. Use sigaction() with sa_flags=0 (no SA_RESTART) so accept() returns EINTR immediately, and ignore SIGPIPE to prevent process kill on broken pipe
- v4l2_capture.cc: after VIDIOC_S_FMT, verify the returned `fmt.fmt.pix.pixelformat` actually matches the requested pixfmt. Drivers can silently fall back to a different format (especially with libv4l2); trusting the original request leads to wrong decode path (YUYV→MJPG mismatch)
- v4l2_capture.cc: use `bytesperline` from VIDIOC_S_FMT for YUYV row stride, not `width_*2`. Drivers may insert padding, so stride > width*2. Also handle odd widths: YUYV macro-pixels are 2 pixels; the last single pixel should be processed separately reusing the previous U/V. Add a length check `(stride*height) > len` to bail on truncated frames
- CMakeLists.txt libturbojpeg selection: detect via TARGET_SOC first (rv1106/rv1103 → armhf_uclibc), then fall back to CMAKE_SYSTEM_PROCESSOR (aarch64/armhf/x86_64/AMD64). The original code only had aarch64 and armhf, which would silently fall through to YUYV-only on rv1106 (an incompatible armhf binary) and x64 host builds
- V4L2 camera capture: Implement a failure streak counter (30 consecutive failures ≈1s) to handle temporary USB camera glitches without terminating the stream
- RGA hardware letterbox padding may fail with "invalid parameter" when using rgb888 format (3 bytes/pixel); code has CPU fallback via memset in image_utils.c:634-641
- MPP frame buffer management: Preallocate enc_frame_ and enc_buf_ during init_mpp, reuse them across frames, and only call mpp_frame_deinit and mpp_buffer_put during deinit_mpp. Do not call mpp_frame_deinit per frame as it releases buffer references while the encoder is still using them, causing double free
- RTMP sender thread: Do not call close() within the sender thread to avoid self-join deadlock
- SPS/PPS transmission: Do not send SPS/PPS in init_mpp as RTMP connection is not yet established (causes failure); send SPS/PPS after open() connection during first encoding
- MPP SPS/PPS API: MPP_ENC_GET_EXTRA_INFO is deprecated (in rk_mpi_cmd.h); must use MPP_ENC_GET_HDR_SYNC instead. The returned MppPacket is internally managed by MPP — do NOT call mpp_packet_deinit on it, or it will free internal MPP memory and cause "double free or corruption" crash
- MPP encode_get_packet returns a packet from MPP's internal pool — must call mpp_packet_deinit to return it to the pool after reading the data
- MPP fps config keys: use rc:fps_in_num + rc:fps_in_denorm / rc:fps_out_num + rc:fps_out_denorm (NOT rc:fps_in / rc:fps_out which don't exist in the API)
- MPP codec:type should NOT be set in enc_cfg (mpp_enc_cfg_set_s32) — it's already specified in mpp_init(); setting it again is redundant
- MPP SPS/PPS获取: MPP_ENC_GET_HDR_SYNC 可能返回 NULL (MPP_OK 但空指针); 必须设置 MPP_ENC_HEADER_MODE_EACH_IDR 模式, 让 SPS/PPS 嵌入每个 IDR 帧的码流, 然后从码流中解析获取
- RTMP AVC sequence header: 必须将 SPS+PPS 合并为单个 AVCDecoderConfigurationRecord 发送, 格式为 5B RTMP头 + 1B version + 3B profile/level + 1B 0xFF + 1B 0xE1 + 2B SPS长度 + SPS数据 + 1B numPPS + 2B PPS长度 + PPS数据。不能分别发送 SPS 和 PPS 作为独立的 NALU 消息
- RTMP SendPacket 内存安全: RTMPPacket_Alloc 分配 nSize+RTMP_MAX_HEADER_SIZE 字节, m_body 指向偏移后位置。使用 malloc 会导致堆缓冲区下溢。发送后需将 m_vecChannelsOut[channel]->m_body 置 NULL 防止 RTMP_Close 时 double-free
- MIPI OV13855 camera requires using 'opi5max-cam0' overlay (NOT 'ov13855-c1') on Orange Pi 5 Max. The c1 overlay has 3 bugs: (1) wrong I2C bus (i2c7 instead of i2c6), (2) wrong sensor node name (ov13855-1@36 instead of ov13855-0@36 on i2c6), (3) wrong ISP virtual channel (rkisp0_vir1 instead of rkisp0_vir2). This causes "modalias failure" in dmesg and sensor driver never binds
- MIPI OV13855 on Orange Pi 5 Max camera0: sensor on i2c6 (fec80000), pipeline: dphy0 → mipi2_csi2 → rkcif_mipi_lvds2 → rkisp0_vir2 → mainpath (/dev/video11 or /dev/video-camera0)
- MIPI camera uses RKISP mainpath (/dev/video11) for NV12 output, NOT /dev/video0 (rkcif raw Bayer output). /dev/video-camera0 is a udev symlink to video11
- RKISP doesn't support VIDIOC_S_PARM (fps setting), fps shows as 0. OV13855 outputs 4224x3136@30fps from sensor, ISP downscale to 640x480 yields ~15fps
- V4L2Capture open() format priority: MJPG → NV12 → YUYV → UYVY. USB cameras hit MJPG, MIPI RKISP hits NV12. Multi-planar detected by V4L2_CAP_VIDEO_CAPTURE_MPLANE capability flag
- cv::cvtColorTwoPlane(y_mat, uv_mat, dst, COLOR_YUV2RGB_NV12) for NV12→RGB: y_mat is CV_8UC1(height, width, y_data, y_stride), uv_mat is CV_8UC2(height/2, width/2, uv_data, uv_stride). Supports stride padding via Mat step parameter
- cv::cvtColor(input, dst, COLOR_YUV2RGB_YUY2/UYVY) for YUYV/UYVY→RGB: input is CV_8UC2(height, width, data, bytesperline). Stride check (stride*height > len) still needed before conversion
- RGA imcvtcolor for RGB→NV12: importbuffer_virtualaddr for RGB source, importbuffer_fd for MPP DMA destination. Cache MPP buffer handle (rga_dst_handle_) across frames. Release in deinit_mpp. RGA also handles scaling if src != enc dimensions
- CMakeLists.txt: RGA library (${LIBRGA}) and include dir (${LIBRGA_INCLUDES}) are set by 3rdparty/CMakeLists.txt. Add to stream target under HAVE_MPP_RTMP block for rtmp_sink.cc RGB→NV12 acceleration
## Deploy & Portability
- HEADLESS mode (default ON for stream target): removes OpenCV highgui/videoio dependencies (imshow/waitKey/VideoWriter), reducing NEEDED libs from 12+ (with 100+ transitive) to 10 (with ~20 transitive). CMake option: -DHEADLESS=OFF to restore
- install(DIRECTORY ${SYSTEM_LIB_DIR}/ ...) must NOT be used — it copies the entire /usr/lib directory tree even with FILES_MATCHING. Use install(CODE "file(GLOB ...) file(INSTALL ...)") instead for precise .so file installation
- deploy_libs.sh: post-install script that uses ldd to collect all transitive dependencies from system paths into install/lib. Skips system base libs (glibc, libstdc++, etc.)
- OpenCV .so files have hardcoded RUNPATH pointing to the build machine path (/home/orangepi/Desktop/opencv-3.4.5/install/lib). Use run.sh wrapper with LD_LIBRARY_PATH to override at runtime
- mediamtx is a statically-linked Go binary (~40MB), installed via CMake install(PROGRAMS) from project root
- Only OpenCV modules core/imgproc/imgcodecs/videoio/highgui are installed (excluded: dnn/ml/stitching/superres/videostab/objdetect/flann/photo/shape/features2d/calib3d)
- System base libs (libc/libm/libpthread/libdl/librt/libgcc_s/libstdc++) are NOT packaged — target board must have compatible glibc version

## USB Camera Device Node (2026-07-22)
- USB camera device numbers are NOT fixed — /dev/video0 may be rkcif (not a USB camera at all)
- On current board: Orbbec Astra Pro HD Camera → /dev/video20, /dev/video21 (confirmed by v4l2-ctl --list-devices)
- /dev/video0 shows "No such device" from v4l2-ctl — it's an rkcif device that's not properly initialized
- Always use `v4l2-ctl --list-devices` to find the correct device node before running
- Device numbers can change after USB replug or reboot

## RTMP/WebRTC Viewing Failure Root Cause (2026-07-22)
- User reported "连接被该站点断开" when viewing RTMP via browser — TWO root causes:
  1. mediamtx was not running (main cause) — without mediamtx, no RTMP→WebRTC conversion
  2. USB camera device node was wrong (/dev/video0 → should be /dev/video20)
- mediamtx must be started BEFORE the stream program, with mediamtx.yml in the same directory
- mediamtx does NOT support --rtmp-addr flag; must use config file (mediamtx.yml)
- mediamtx.yml must be copied to the install directory alongside the mediamtx binary
- Verify mediamtx is running: `ss -tlnp | grep 1936`

## HEADLESS Mode Runtime (2026-07-22)
- HEADLESS mode stream target only links libopencv_core.so + libopencv_imgproc.so
- RTMP push code (rtmp_sink->submit) is NOT inside any #ifdef, works identically in both modes
- HEADLESS disables: imshow/waitKey/namedWindow/destroyAllWindows (highgui), VideoWriter (videoio), video recording
- New board apt deps for HEADLESS: only libgomp1 zlib1g (vs 10+ packages for non-HEADLESS)
- run.sh sets LD_LIBRARY_PATH to override OpenCV .so embedded RUNPATH

## Current Board IP Addresses
- LAN (wlan0): 10.33.189.13
- Tailscale: 100.90.126.70
- WebRTC viewing: http://10.33.189.13:8889/live/stream or http://100.90.126.70:8889/live/stream
