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

#include <opencv2/videoio.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/core.hpp>

#include "yolov8.h"
#include "image_utils.h"
#include "file_utils.h"
#include "image_drawing.h"

#if defined(RV1106_1103)
    #include "dma_alloc.hpp"
#endif

/*-------------------------------------------
        离线视频检测主函数
        用法: ./rknn_yolov8_video_demo <model_path> <input_video> [output_video]
-------------------------------------------*/
static int run_video_detect(rknn_app_context_t *rknn_app_ctx,
                            const char *input_video,
                            const char *output_video)
{
    cv::VideoCapture cap(input_video);
    if (!cap.isOpened())
    {
        printf("open input video fail! path=%s\n", input_video);
        return -1;
    }

    int frame_w   = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
    int frame_h   = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    double fps    = cap.get(cv::CAP_PROP_FPS);
    int frame_num = (int)cap.get(cv::CAP_PROP_FRAME_COUNT);
    if (fps <= 0) fps = 25.0;
    printf("input video: %s, %dx%d, fps=%.2f, frames=%d\n",
           input_video, frame_w, frame_h, fps, frame_num);

    cv::VideoWriter writer;
    writer.open(output_video,
                cv::VideoWriter::fourcc('m', 'p', '4', 'v'),
                fps,
                cv::Size(frame_w, frame_h));
    if (!writer.isOpened())
    {
        printf("open output video fail! path=%s\n", output_video);
        cap.release();
        return -1;
    }

    cv::Mat bgr_frame;
    cv::Mat rgb_mat;
    int frame_index = 0;
    int ret = 0;

    while (true)
    {
        cap >> bgr_frame;
        if (bgr_frame.empty())
        {
            break;
        }

        cv::cvtColor(bgr_frame, rgb_mat, cv::COLOR_BGR2RGB);

        image_buffer_t src_image;
        memset(&src_image, 0, sizeof(image_buffer_t));
        src_image.width         = rgb_mat.cols;
        src_image.height        = rgb_mat.rows;
        src_image.width_stride  = rgb_mat.cols;
        src_image.height_stride = rgb_mat.rows;
        src_image.format        = IMAGE_FORMAT_RGB888;
        src_image.size          = (int)(rgb_mat.total() * rgb_mat.elemSize());
        src_image.virt_addr     = rgb_mat.data;
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

        if (frame_index % 10 == 0)
        {
            printf("frame %d done, %d objects detected\n", frame_index, od_results.count);
        }

        cv::Mat out_bgr(src_image.height, src_image.width, CV_8UC3, src_image.virt_addr);
        cv::Mat bgr_out;
        cv::cvtColor(out_bgr, bgr_out, cv::COLOR_RGB2BGR);
        writer.write(bgr_out);

        frame_index++;
    }

    printf("process done, total frames=%d, output=%s\n", frame_index, output_video);
    writer.release();
    cap.release();
    return ret;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        printf("%s <model_path> <input_video> [output_video]\n", argv[0]);
        printf("  model_path   : RKNN 模型文件路径\n");
        printf("  input_video  : 输入离线视频文件路径 (例如 test.mp4)\n");
        printf("  output_video : 输出视频文件路径, 可选, 默认 out_video.mp4\n");
        return -1;
    }

    const char *model_path   = argv[1];
    const char *input_video  = argv[2];
    const char *output_video = (argc >= 4) ? argv[3] : "out_video.mp4";

    rknn_app_context_t rknn_app_ctx;
    memset(&rknn_app_ctx, 0, sizeof(rknn_app_context_t));

    init_post_process();

    int ret = init_yolov8_model(model_path, &rknn_app_ctx);
    if (ret != 0)
    {
        printf("init_yolov8_model fail! ret=%d model_path=%s\n", ret, model_path);
        deinit_post_process();
        return -1;
    }

    ret = run_video_detect(&rknn_app_ctx, input_video, output_video);

    deinit_post_process();
    release_yolov8_model(&rknn_app_ctx);
    return ret;
}
