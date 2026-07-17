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

#include <opencv2/imgproc.hpp>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>

#include "yolov8.h"
#include "image_utils.h"
#include "file_utils.h"
#include "image_drawing.h"
#include "v4l2_capture.h"
#include "mjpeg_http_sink.h"
#include "rtmp_sink.h"

#if defined(RV1106_1103)
    #include "dma_alloc.hpp"
#endif

// Ctrl+C / kill 触发的优雅退出标志 (async-signal-safe, 仅置位)
static std::atomic<int> g_stop_requested{0};
static void on_sigint(int signo)
{
    (void)signo;
    g_stop_requested.store(1);
}

/*-------------------------------------------
        实时视频检测 (摄像头)
        用法: ./rknn_yolov8_demo_stream <model_path> <camera_id|device_path>
                                    [width] [height] [fps] [save_path] [push_port|rtmp_url]
        示例: ./rknn_yolov8_demo_stream model/best.rknn 0
              ./rknn_yolov8_demo_stream model/best.rknn /dev/video0 1280 720 30 out.mp4
              ./rknn_yolov8_demo_stream model/best.rknn /dev/video0 0 0 0 "" 8080
              ./rknn_yolov8_demo_stream model/best.rknn /dev/video0 0 0 0 "" rtmp://127.0.0.1:1935/live/stream
        按 'q' 退出预览
-------------------------------------------*/
static int run_stream_detect(rknn_app_context_t *rknn_app_ctx,
                             const char *source,
                             int req_w, int req_h, int req_fps,
                             const char *save_path,
                             int show_window,
                             MjpegHttpSink *mjpeg_sink,
                             RtmpSink *rtmp_sink)
{
    V4L2Capture cap;
    if (cap.open(source, req_w, req_h, req_fps) != 0)
    {
        return -1;
    }

    int frame_w   = cap.width();
    int frame_h   = cap.height();
    double fps    = cap.fps();
    if (fps <= 0) fps = 25.0;

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
            save_enabled = 0;  // 关掉录制, 继续检测 + 推流
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

    // ==================== 优化: 预分配复用 Mat ====================
    // 整段循环共用 3 个 Mat, 避免每帧 new/delete / 重新分配
    cv::Mat rgb_frame;  // V4L2 直出 RGB (TJPF_RGB), 同时也是绘制画布
    cv::Mat bgr_for_save, bgr_for_show;
    if (save_enabled || show_window)
    {
        bgr_for_save.create(frame_h, frame_w, CV_8UC3);
    }
    if (show_window)
    {
        bgr_for_show.create(frame_h, frame_w, CV_8UC3);
    }

    int frame_index = 0;
    int ret = 0;
    bool loop = true;
    int  v4l2_fail_streak = 0;  // 连续 V4L2 失败计数, 短暂抖动不退出

    while (loop)
    {
        // SIGINT / SIGTERM: 跳出循环, 让 main() 走 push_sink->stop() 清理
        if (g_stop_requested.load())
        {
            printf("signal received, stopping stream...\n");
            break;
        }

        // RTMP 发送失败 (连接断开): 自动退出, 不浪费 CPU
        if (rtmp_sink != nullptr && rtmp_sink->sendFailed())
        {
            printf("RTMP connection lost, stopping stream...\n");
            break;
        }

        if (cap.read(rgb_frame) != 0 || rgb_frame.empty())
        {
            // V4L2 偶发抽风 (驱动插入非 JPEG 状态字节, USB 抖动等);
            // 短暂 sleep 后重试, 连续失败 1s 才真退出
            v4l2_fail_streak++;
            if (v4l2_fail_streak >= 30)  // ~1s @30fps
            {
                printf("read frame fail %d times in a row, give up.\n", v4l2_fail_streak);
                break;
            }
            printf("[v4l2] read fail (%d), retry...\n", v4l2_fail_streak);
            usleep(33000);
            continue;
        }
        v4l2_fail_streak = 0;

        // ==================== 优化: 省 BGR2RGB ====================
        // 旧流程: cv::cvtColor(bgr_frame, rgb_mat, COLOR_BGR2RGB) 一次整帧转换
        // 新流程: V4L2 解码已直出 RGB, 跳过

        image_buffer_t src_image;
        memset(&src_image, 0, sizeof(image_buffer_t));
        src_image.width         = rgb_frame.cols;
        src_image.height        = rgb_frame.rows;
        src_image.width_stride  = rgb_frame.cols;
        src_image.height_stride = rgb_frame.rows;
        src_image.format        = IMAGE_FORMAT_RGB888;
        src_image.size          = (int)(rgb_frame.total() * rgb_frame.elemSize());
        src_image.virt_addr     = rgb_frame.data;
        src_image.fd            = -1;

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
        src_image.fd        = rknn_app_ctx->img_dma_buf.dma_buf_fd;
#endif

        object_detect_result_list od_results;
        int inf_ret = inference_yolov8_model(rknn_app_ctx, &src_image, &od_results);
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

            if (x1 < 0) x1 = 0;
            if (y1 < 0) y1 = 0;
            if (x2 > src_image.width)  x2 = src_image.width;
            if (y2 > src_image.height) y2 = src_image.height;

            draw_rectangle(&src_image, x1, y1, x2 - x1, y2 - y1, COLOR_BLUE, 3);
            sprintf(text, "%s %.1f%%", coco_cls_to_name(det->cls_id), det->prop * 100);
            draw_text(&src_image, text, x1, (y1 - 20 > 0 ? y1 - 20 : 0), COLOR_RED, 10);
        }

        if (frame_index % 30 == 0)
        {
            printf("frame %d done, %d objects detected\n", frame_index, od_results.count);
        }

        // 显示 (RGB -> BGR 给 highgui 看)
        if (show_window)
        {
            cv::cvtColor(rgb_frame, bgr_for_show, cv::COLOR_RGB2BGR);
            cv::imshow("yolov8_stream", bgr_for_show);
            int key = cv::waitKey(1);
            if (key == 'q' || key == 'Q' || key == 27 /* ESC */)
            {
                printf("user quit\n");
                loop = false;
            }
        }

        // 保存到文件 (MP4 需要 BGR, 复用 bgr_for_save)
        if (save_enabled && writer.isOpened())
        {
            cv::cvtColor(rgb_frame, bgr_for_save, cv::COLOR_RGB2BGR);
            writer.write(bgr_for_save);
        }

        // ==================== 优化: 异步推流 ====================
        // submit() 只 swap 一次 Mat 句柄就返回, 编码发送在独立 sender 线程
        //     submit 走 RGB, 跳过保存时的那次 BGR2RGB 转换
        if (mjpeg_sink != nullptr)
        {
            mjpeg_sink->submit(rgb_frame, 80);
        }
        if (rtmp_sink != nullptr)
        {
            rtmp_sink->submit(rgb_frame);
        }

        frame_index++;
    }

    printf("stream done, total frames=%d\n", frame_index);
    if (show_window) cv::destroyAllWindows();
    if (save_enabled && writer.isOpened()) writer.release();
    return ret;
}

int main(int argc, char **argv)
{
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
    const char *source     = argv[2];
    int req_w  = (argc >= 4) ? atoi(argv[3]) : 0;
    int req_h  = (argc >= 5) ? atoi(argv[4]) : 0;
    int req_fps= (argc >= 6) ? atoi(argv[5]) : 0;
    const char *save_path = (argc >= 7) ? argv[6] : NULL;
    const char *push_arg  = (argc >= 8) ? argv[7] : NULL;

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

    // Ctrl+C / kill 走优雅退出, 让 push_sink->stop() 能清理监听 socket
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // 不要 SA_RESTART, 让 accept() 立即被 EINTR 打断
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    // SIGPIPE 写到已关闭的 socket 时默认终止进程, 这里忽略避免杀进程
    signal(SIGPIPE, SIG_IGN);

    // 推流: 根据 push_arg 判断是 MJPEG (纯数字端口) 还是 RTMP (rtmp:// URL)
    MjpegHttpSink *mjpeg_sink = nullptr;
    RtmpSink      *rtmp_sink  = nullptr;

    if (push_arg != NULL && push_arg[0] != '\0')
    {
        if (strncmp(push_arg, "rtmp://", 7) == 0)
        {
            // RTMP 推流模式 (H.264 + mediamtx/WebRTC)
            rtmp_sink = new RtmpSink();
            // 先打开摄像头获取实际分辨率和帧率, 再初始化编码器
            // 这里先用请求参数, 后面 cap.open 后会拿到实际值
            if (rtmp_sink->open(push_arg,
                                req_w > 0 ? req_w : 640,
                                req_h > 0 ? req_h : 480,
                                req_fps > 0 ? req_fps : 30) != 0)
            {
                printf("RTMP open failed: %s\n", push_arg);
                delete rtmp_sink;
                rtmp_sink = nullptr;
            }
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

    init_post_process();

    int ret = init_yolov8_model(model_path, &rknn_app_ctx);
    if (ret != 0)
    {
        printf("init_yolov8_model fail! ret=%d model_path=%s\n", ret, model_path);
        deinit_post_process();
        if (mjpeg_sink) { mjpeg_sink->stop(); delete mjpeg_sink; }
        if (rtmp_sink)  { rtmp_sink->close(); delete rtmp_sink; }
        return -1;
    }

    ret = run_stream_detect(&rknn_app_ctx, source, req_w, req_h, req_fps,
                            save_path, show_window, mjpeg_sink, rtmp_sink);

    deinit_post_process();
    release_yolov8_model(&rknn_app_ctx);

    if (mjpeg_sink)
    {
        mjpeg_sink->stop();
        delete mjpeg_sink;
    }
    if (rtmp_sink)
    {
        rtmp_sink->close();
        delete rtmp_sink;
    }
    return ret;
}
