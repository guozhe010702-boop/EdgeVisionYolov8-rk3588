// Copyright (c) 2023 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*
================================================================================
【文件总说明】
文件：main_stream.cc
功能：YOLOv8 实时摄像头检测演示程序入口
用途：通过 V4L2 直接采集摄像头画面，逐帧推理检测，支持 MJPEG/RTMP 推流和视频录制

【整体工作流程】
1. main() 解析命令行参数：模型路径 + 摄像头设备 + 分辨率/帧率 + 推流参数
2. 注册 SIGINT/SIGTERM 信号处理器，实现优雅退出
3. 根据 push_arg 创建推流对象：
   - 纯数字端口 → MjpegHttpSink（MJPEG-over-HTTP 推流）
   - rtmp:// URL → RtmpSink（H.264 硬编码 + RTMP 推流）
4. init_post_process() + init_yolov8_model()：初始化后处理和 RKNN 模型
5. run_stream_detect() 核心循环：
   5.1 V4L2Capture 直接采集 RGB 帧（跳过 BGR→RGB 转换）
   5.2 封装为 image_buffer_t，RV1106 平台拷贝到 CMA 内存
   5.3 inference_yolov8_model()：推理 + 后处理
   5.4 在 RGB 图上画检测框和标签
   5.5 预览窗口 imshow + VideoWriter 录制（HEADLESS 模式下均禁用）
   5.6 异步推流：submit() 一次 Mat swap 即返回，编码/发送在独立线程
   5.7 V4L2 连续失败超过 2s 才退出，短暂 USB 抖动不中断
6. 检测到信号/RTMP 断开/用户按 q 后退出，释放推流对象和模型资源

关键技术点：
- V4L2 直出 RGB，跳过 BGR→RGB 转换开销
- 预分配 cv::Mat 缓冲帧外复用，避免每帧 allocate/deallocate
- 异步 producer-consumer 推流模型，检测线程不阻塞
- HEADLESS 模式去除 OpenCV highgui/videoio 依赖，减少部署体积
================================================================================
*/

/*-------------------------------------------
                Includes
-------------------------------------------*/
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <atomic>
#include <chrono>

#include <opencv2/imgproc.hpp>
#include <opencv2/core.hpp>
#ifndef HEADLESS
#include <opencv2/highgui.hpp>
#include <opencv2/videoio.hpp>
#endif

#include "yolov8.h"
#include "image_utils.h"
#include "file_utils.h"
#include "image_drawing.h"
#include "v4l2_capture.h"
#include "mjpeg_http_sink.h"
#ifdef HAVE_MPP_RTMP
#include "rtmp_sink.h"
#endif

// RTMP 推流能力: 编译期决定
#ifdef HAVE_MPP_RTMP
#define RTMP_AVAILABLE 1
#else
#define RTMP_AVAILABLE 0
#endif

#if defined(RV1106_1103)
#include "dma_alloc.hpp"
#endif

// Ctrl+C / kill 触发的优雅退出标志 (async-signal-safe, 仅置位)
static std::atomic<int> g_stop_requested{0};

/**
 * @brief SIGINT/SIGTERM 信号处理函数，仅置位原子标志，不执行任何不安全操作
 * @param signo 信号编号（未使用）
 */
static void on_sigint(int signo)
{
    (void)signo;
    g_stop_requested.store(1);
}

/**
 * @brief 实时摄像头检测核心循环，逐帧采集→推理→绘制→推流
 * @param rknn_app_ctx RKNN 应用上下文
 * @param source 摄像头设备路径（如 /dev/video0）或序号（如 "0"）
 * @param req_w 请求分辨率宽度，0 表示使用默认值
 * @param req_h 请求分辨率高度，0 表示使用默认值
 * @param req_fps 请求帧率，0 表示使用默认值
 * @param save_path 视频录制输出路径，NULL 或空串表示不录制
 * @param show_window 是否开启预览窗口
 * @param mjpeg_sink MJPEG 推流对象指针，NULL 表示不推 MJPEG
 * @param rtmp_sink_raw RTMP 推流对象指针，NULL 表示不推 RTMP
 * @return 0成功，-1失败
 */
static int run_stream_detect(rknn_app_context_t *rknn_app_ctx,
                             const char *source,
                             int req_w, int req_h, int req_fps,
                             const char *save_path,
                             int show_window,
                             MjpegHttpSink *mjpeg_sink,
                             void *rtmp_sink_raw,
                             rknn_app_context_t *aux_ctx,
                             int auxiliary_class)
{
#ifdef HAVE_MPP_RTMP
    RtmpSink *rtmp_sink = static_cast<RtmpSink *>(rtmp_sink_raw);
#else
    (void)rtmp_sink_raw;
#endif
    V4L2Capture cap;
    if (cap.open(source, req_w, req_h, req_fps) != 0)
    {
        return -1;
    }

    int frame_w = cap.width();
    int frame_h = cap.height();
    double fps = cap.fps();
    if (fps <= 0)
        fps = 30.0; // RKISP 等设备可能不支持帧率查询, 默认 30

#ifndef HEADLESS
    cv::VideoWriter writer;
    int save_enabled = (save_path != NULL && save_path[0] != '\0');
    if (save_enabled)
    {
        writer.open(save_path,
                    cv::VideoWriter::fourcc('m', 'p', '4', 'v'),
                    fps,
                    cv::Size(frame_w, frame_h));
        if (!writer.isOpened())
        {
            printf("open output video fail! path=%s (continue without recording)\n", save_path);
            save_enabled = 0; // 关掉录制, 继续检测 + 推流
        }
        else
        {
            printf("save stream to: %s\n", save_path);
        }
    }

    if (show_window)
    {
        cv::namedWindow("yolov8_stream", cv::WINDOW_AUTOSIZE);
    }
#else
    // HEADLESS 模式: 无 VideoWriter, 无预览窗口
    int save_enabled = 0;
    if (save_path != NULL && save_path[0] != '\0')
    {
        printf("HEADLESS mode: video recording disabled (no OpenCV videoio)\n");
    }
    show_window = 0;
#endif

    // ==================== 优化: 预分配复用 Mat ====================
    // 整段循环共用 3 个 Mat, 避免每帧 new/delete / 重新分配
    cv::Mat rgb_frame; // V4L2 直出 RGB (TJPF_RGB), 同时也是绘制画布
    cv::Mat bgr_for_save;
#ifndef HEADLESS
    cv::Mat bgr_for_show;
#endif
    if (save_enabled || show_window)
    {
        bgr_for_save.create(frame_h, frame_w, CV_8UC3);
    }
#ifndef HEADLESS
    if (show_window)
    {
        bgr_for_show.create(frame_h, frame_w, CV_8UC3);
    }
#endif

    int frame_index = 0;
    auto metric_time = std::chrono::steady_clock::now();
    int metric_frames = 0;
    double metric_detect_ms = 0;
    int ret = 0;
    bool loop = true;
    int v4l2_fail_streak = 0;                  // 连续 V4L2 失败计数, 短暂抖动不退出
    const int v4l2_retry_us = 33000;           // 重试间隔 ~33ms
    const int v4l2_max_fail_time_us = 2000000; // 连续失败 2 秒才真退出

    while (loop)
    {
        // SIGINT / SIGTERM: 跳出循环, 让 main() 走 push_sink->stop() 清理
        if (g_stop_requested.load())
        {
            printf("signal received, stopping stream...\n");
            break;
        }

        // RTMP 发送失败 (连接断开): 自动退出, 不浪费 CPU
#ifdef HAVE_MPP_RTMP
        if (rtmp_sink != nullptr && rtmp_sink->sendFailed())
        {
            printf("RTMP connection lost, stopping stream...\n");
            break;
        }
#endif

        if (cap.read(rgb_frame) != 0 || rgb_frame.empty())
        {
            // V4L2 偶发抽风 (驱动插入非 JPEG 状态字节, USB 抖动等);
            // 短暂 sleep 后重试, 连续失败 2s 才真退出 (不依赖帧率)
            v4l2_fail_streak++;
            if (v4l2_fail_streak * v4l2_retry_us >= v4l2_max_fail_time_us)
            {
                printf("read frame fail %d times (%dms), give up.\n",
                       v4l2_fail_streak, v4l2_fail_streak * v4l2_retry_us / 1000);
                break;
            }
            printf("[v4l2] read fail (%d), retry...\n", v4l2_fail_streak);
            usleep(v4l2_retry_us);
            continue;
        }
        v4l2_fail_streak = 0;

        // ==================== 优化: 省 BGR2RGB ====================
        // 旧流程: cv::cvtColor(bgr_frame, rgb_mat, COLOR_BGR2RGB) 一次整帧转换
        // 新流程: V4L2 解码已直出 RGB, 跳过

        image_buffer_t src_image;
        memset(&src_image, 0, sizeof(image_buffer_t));
        src_image.width = rgb_frame.cols;
        src_image.height = rgb_frame.rows;
        src_image.width_stride = rgb_frame.cols;
        src_image.height_stride = rgb_frame.rows;
        src_image.format = IMAGE_FORMAT_RGB888;
        src_image.size = (int)(rgb_frame.total() * rgb_frame.elemSize());
        src_image.virt_addr = rgb_frame.data;
        src_image.fd = -1;

#if defined(RV1106_1103)
        rknn_app_ctx->img_dma_buf.size = src_image.size;
        int dma_ret = dma_buf_alloc(RV1106_CMA_HEAP_PATH, src_image.size,
                                    &rknn_app_ctx->img_dma_buf.dma_buf_fd,
                                    (void **)&(rknn_app_ctx->img_dma_buf.dma_buf_virt_addr));
        if (dma_ret != 0)
        {
            printf("dma_buf_alloc fail! ret=%d\n", dma_ret);
            ret = -1;
            break;
        }
        memcpy(rknn_app_ctx->img_dma_buf.dma_buf_virt_addr, src_image.virt_addr, src_image.size);
        dma_sync_cpu_to_device(rknn_app_ctx->img_dma_buf.dma_buf_fd);
        src_image.virt_addr = (unsigned char *)rknn_app_ctx->img_dma_buf.dma_buf_virt_addr;
        src_image.fd = rknn_app_ctx->img_dma_buf.dma_buf_fd;
#endif

        object_detect_result_list od_results;
        object_detect_result_list aux_results = {};
        auto detect_started = std::chrono::steady_clock::now();
        int inf_ret = inference_yolov8_model(rknn_app_ctx, &src_image, &od_results);
        // Both contexts see the same unpainted original frame.
        if (inf_ret == 0 && aux_ctx)
            inf_ret = inference_yolov8_model(aux_ctx, &src_image, &aux_results);
        metric_detect_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - detect_started).count();
#if defined(RV1106_1103)
        dma_buf_free(rknn_app_ctx->img_dma_buf.size,
                     &rknn_app_ctx->img_dma_buf.dma_buf_fd,
                     rknn_app_ctx->img_dma_buf.dma_buf_virt_addr);
#endif
        if (inf_ret != 0)
        {
            printf("inference_yolov8_model fail! frame=%d ret=%d\n", frame_index, inf_ret);
            ret = -1;
            break;
        }

        char text[256];
        for (int i = 0; i < od_results.count; i++)
        {
            object_detect_result *det = &(od_results.results[i]);
            int x1 = det->box.left;
            int y1 = det->box.top;
            int x2 = det->box.right;
            int y2 = det->box.bottom;

            if (x1 < 0)
                x1 = 0;
            if (y1 < 0)
                y1 = 0;
            if (x2 > src_image.width)
                x2 = src_image.width;
            if (y2 > src_image.height)
                y2 = src_image.height;

            draw_rectangle(&src_image, x1, y1, x2 - x1, y2 - y1, COLOR_BLUE, 3);
            snprintf(text, sizeof(text), "%s %.1f%%", model_class_name(rknn_app_ctx, det->cls_id), det->prop * 100);
            draw_text(&src_image, text, x1, (y1 - 20 > 0 ? y1 - 20 : 0), COLOR_RED, 10);
        }
        int auxiliary_objects=0;
        for (int i=0; aux_ctx && i<aux_results.count; ++i) {
            object_detect_result *det=&aux_results.results[i];
            if (det->cls_id != auxiliary_class) continue;
            ++auxiliary_objects;
            int x1=det->box.left<0 ? 0 : det->box.left;
            int y1=det->box.top<0 ? 0 : det->box.top;
            int x2=det->box.right>src_image.width ? src_image.width : det->box.right;
            int y2=det->box.bottom>src_image.height ? src_image.height : det->box.bottom;
            draw_rectangle(&src_image, x1, y1, x2-x1, y2-y1, COLOR_GREEN, 3);
            snprintf(text, sizeof(text), "%s %.1f%%", model_class_name(aux_ctx, det->cls_id), det->prop*100);
            draw_text(&src_image, text, x1, y1>20 ? y1-20 : 0, COLOR_GREEN, 10);
        }

        if (frame_index % (fps > 0 ? (int)fps : 30) == 0)
        {
            printf("frame %d done, %d objects detected\n", frame_index, od_results.count+auxiliary_objects);
        }

        // 显示 + 保存: RGB→BGR 只做一次, 两处复用 (HEADLESS 模式下 show_window/save_enabled 均为 0, 此块不执行)
#ifndef HEADLESS
        if (show_window || (save_enabled && writer.isOpened()))
        {
            cv::cvtColor(rgb_frame, bgr_for_save, cv::COLOR_RGB2BGR);

            if (show_window)
            {
                bgr_for_show = bgr_for_save; // 浅拷贝 (共享数据, 零开销)
                cv::imshow("yolov8_stream", bgr_for_show);
                int key = cv::waitKey(1);
                if (key == 'q' || key == 'Q' || key == 27 /* ESC */)
                {
                    printf("user quit\n");
                    loop = false;
                }
            }

            if (save_enabled && writer.isOpened())
            {
                writer.write(bgr_for_save);
            }
        }
#endif

        // ==================== 优化: 异步推流 ====================
        // submit() 只 swap 一次 Mat 句柄就返回, 编码发送在独立 sender 线程
        //     submit 走 RGB, 跳过保存时的那次 BGR2RGB 转换
        if (mjpeg_sink != nullptr)
        {
            mjpeg_sink->submit(rgb_frame, 80);
        }
#ifdef HAVE_MPP_RTMP
        if (rtmp_sink != nullptr)
        {
            rtmp_sink->submit(rgb_frame);
        }
#endif

        frame_index++;
        metric_frames++;
        auto metric_now = std::chrono::steady_clock::now();
        double metric_seconds = std::chrono::duration<double>(metric_now - metric_time).count();
        if (metric_seconds >= 1.0) {
            printf("METRIC {\"frames\":%d,\"interval_s\":%.6f,\"fps\":%.3f,\"detect_ms\":%.3f,\"objects\":%d,\"primary_objects\":%d,\"helmet_objects\":%d}\n",
                frame_index, metric_seconds, metric_frames / metric_seconds,
                metric_detect_ms / metric_frames, od_results.count+auxiliary_objects, od_results.count, auxiliary_objects);
            printf("OBJECTS {\"frame\":%d,\"primary\":[", frame_index);
            for (int j=0; j<od_results.count; ++j)
                printf("%s{\"class_id\":%d,\"score\":%.4f}", j ? "," : "", od_results.results[j].cls_id, od_results.results[j].prop);
            printf("],\"auxiliary\":[");
            int printed=0;
            for (int j=0; aux_ctx && j<aux_results.count; ++j) {
                if (aux_results.results[j].cls_id!=auxiliary_class) continue;
                printf("%s{\"class_id\":%d,\"score\":%.4f}", printed++ ? "," : "", aux_results.results[j].cls_id, aux_results.results[j].prop);
            }
            printf("]}\n");
            fflush(stdout);
            metric_frames = 0;
            metric_detect_ms = 0;
            metric_time = metric_now;
        }
    }

    printf("stream done, total frames=%d\n", frame_index);
#ifndef HEADLESS
    if (show_window)
        cv::destroyAllWindows();
    if (save_enabled && writer.isOpened())
        writer.release();
#endif
    return ret;
}

/**
 * @brief 实时摄像头检测程序入口，解析参数、初始化模型和推流、运行检测循环
 * @param argc 参数个数，至少为3
 * @param argv 命令行参数：argv[1]=模型路径, argv[2]=摄像头设备, argv[3-8]可选
 * @return 0成功，-1失败
 */
int main(int argc, char **argv)
{
    cv::setNumThreads(2);
    if (argc < 3)
    {
        printf("%s <model_path> <camera_id|device_path> [width] [height] [fps] [save_path] [push_port|rtmp_url]\n", argv[0]);
        printf("  model_path    : RKNN 模型文件路径\n");
        printf("  camera_id     : 摄像头序号, 例如 0; 或设备路径 /dev/video0\n");
        printf("  width/height  : 可选, 请求的分辨率\n");
        printf("  fps           : 可选, 请求的帧率\n");
        printf("  save_path     : 可选, 录制输出到该 mp4 文件; 不填则不保存\n");
        printf("  push_port     : 可选, MJPEG-over-HTTP 推流端口; 0 表示不推 (默认)\n");
        printf("                 输出格式与 mjpg_streamer 的 output_http.so 兼容\n");
        printf("  rtmp_url      : 或以 rtmp:// 开头的 RTMP 推流地址\n");
        printf("                 搭配 mediamtx, 浏览器通过 WebRTC 观看, 码率仅 ~1Mbps\n");
        printf("示例:\n");
        printf("  %s model/best.rknn /dev/video0 0 0 0 \"\" 8080\n", argv[0]);
        printf("  %s model/best.rknn /dev/video0 0 0 0 \"\" rtmp://127.0.0.1:1935/live/stream\n", argv[0]);
        printf("按 'q' 或 ESC 退出预览\n");
        return -1;
    }

    const char *model_path = argv[1];
    const char *source = argv[2];

    // 参数校验: width/height/fps 必须为非负整数
    int req_w = 0, req_h = 0, req_fps = 0;
    if (argc >= 4)
    {
        req_w = atoi(argv[3]);
        if (req_w < 0)
            req_w = 0;
    }
    if (argc >= 5)
    {
        req_h = atoi(argv[4]);
        if (req_h < 0)
            req_h = 0;
    }
    if (argc >= 6)
    {
        req_fps = atoi(argv[5]);
        if (req_fps < 0)
            req_fps = 0;
    }
    const char *save_path = (argc >= 7) ? argv[6] : NULL;
    const char *push_arg = (argc >= 8) ? argv[7] : NULL;

    // HEADLESS builds never create a local preview window.
#ifdef HEADLESS
    int show_window = 0;
    printf("HEADLESS build: use the HTTP browser preview; local HDMI window is disabled.\n");
#else
    // 预览窗口: 只要有 DISPLAY 环境变量就打开; 在没有显示的板子上保持关闭
    int show_window = (getenv("DISPLAY") != NULL) ? 1 : 0;
    if (show_window)
    {
        printf("preview enabled (DISPLAY=%s)\n", getenv("DISPLAY"));
    }
    else
    {
        printf("preview disabled (no DISPLAY)\n");
    }
#endif

    // Ctrl+C / kill 走优雅退出, 让 push_sink->stop() 能清理监听 socket
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // 不要 SA_RESTART, 让 accept() 立即被 EINTR 打断
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    // SIGPIPE 写到已关闭的 socket 时默认终止进程, 这里忽略避免杀进程
    signal(SIGPIPE, SIG_IGN);

    // 推流: 根据 push_arg 判断是 MJPEG (纯数字端口) 还是 RTMP (rtmp:// URL)
    MjpegHttpSink *mjpeg_sink = nullptr;
#ifdef HAVE_MPP_RTMP
    RtmpSink *rtmp_sink = nullptr;
#else
    void *rtmp_sink = nullptr;
#endif

    if (push_arg != NULL && push_arg[0] != '\0')
    {
        if (strncmp(push_arg, "rtmp://", 7) == 0)
        {
#ifdef HAVE_MPP_RTMP
            // RTMP 推流模式 (H.264 + mediamtx/WebRTC)
            rtmp_sink = new RtmpSink();
            if (rtmp_sink->open(push_arg,
                                req_w > 0 ? req_w : 640,
                                req_h > 0 ? req_h : 480,
                                req_fps > 0 ? req_fps : 30) != 0)
            {
                printf("RTMP open failed: %s\n", push_arg);
                delete rtmp_sink;
                rtmp_sink = nullptr;
            }
#else
            printf("RTMP streaming not available (compiled without MPP/librtmp support)\n");
            printf("Install librockchip-mpp-dev and librtmp-dev, then recompile.\n");
#endif
        }
        else
        {
            // MJPEG-over-HTTP 推流模式 (传统)
            int push_port = atoi(push_arg);
            if (push_port > 0)
            {
                mjpeg_sink = new MjpegHttpSink();
                if (mjpeg_sink->start(push_port) != 0)
                {
                    printf("start MJPEG push server on port %d failed\n", push_port);
                    delete mjpeg_sink;
                    mjpeg_sink = nullptr;
                }
            }
        }
    }

    rknn_app_context_t rknn_app_ctx;
    memset(&rknn_app_ctx, 0, sizeof(rknn_app_context_t));
    rknn_app_context_t auxiliary_ctx = {};
    const char *aux_model=getenv("EDGEVISION_AUX_MODEL");
    bool use_auxiliary=aux_model && *aux_model;
    int auxiliary_class=0;

    int ret = init_yolov8_model(model_path, &rknn_app_ctx);
    if (ret==0)
        ret=load_model_labels(&rknn_app_ctx, getenv("EDGEVISION_LABELS") ? getenv("EDGEVISION_LABELS") : LABEL_NALE_TXT_PATH);
    if (ret==0) ret=configure_model_execution(&rknn_app_ctx, getenv("EDGEVISION_CONFIDENCE"));
    if (ret==0 && use_auxiliary) {
        const char *aux_labels=getenv("EDGEVISION_AUX_LABELS");
        if (!aux_labels || !*aux_labels) ret=-1;
        if (ret==0) ret=init_yolov8_model(aux_model, &auxiliary_ctx);
        if (ret==0) ret=load_model_labels(&auxiliary_ctx, aux_labels);
        if (ret==0) ret=configure_model_execution(&auxiliary_ctx, getenv("EDGEVISION_AUX_CONFIDENCE") ? getenv("EDGEVISION_AUX_CONFIDENCE") : "0.5");
        const char *class_option=getenv("EDGEVISION_AUX_CLASS");
        if (class_option && *class_option) {
            char *end=nullptr;
            auxiliary_class=(int)strtol(class_option, &end, 10);
            if (!end || *end) ret=-1;
        }
        if (auxiliary_class<0 || auxiliary_class>=auxiliary_ctx.model_classes) ret=-1;
    }
    if (ret != 0)
    {
        printf("init_yolov8_model fail! ret=%d model_path=%s\n", ret, model_path);
        release_yolov8_model(&auxiliary_ctx);
        release_yolov8_model(&rknn_app_ctx);
        if (mjpeg_sink)
        {
            mjpeg_sink->stop();
            delete mjpeg_sink;
        }
#ifdef HAVE_MPP_RTMP
        if (rtmp_sink)
        {
            rtmp_sink->close();
            delete rtmp_sink;
        }
#endif
        return -1;
    }

    ret = run_stream_detect(&rknn_app_ctx, source, req_w, req_h, req_fps,
                            save_path, show_window, mjpeg_sink, rtmp_sink,
                            use_auxiliary ? &auxiliary_ctx : nullptr, auxiliary_class);

    release_yolov8_model(&auxiliary_ctx);
    release_yolov8_model(&rknn_app_ctx);

    if (mjpeg_sink)
    {
        mjpeg_sink->stop();
        delete mjpeg_sink;
    }
#ifdef HAVE_MPP_RTMP
    if (rtmp_sink)
    {
        rtmp_sink->close();
        delete rtmp_sink;
    }
#endif
    return ret;
}
