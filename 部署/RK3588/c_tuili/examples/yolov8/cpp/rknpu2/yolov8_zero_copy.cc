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
文件：yolov8_zero_copy.cc（RKNPU2 零拷贝模式）
功能：RK3588 等 RKNPU2 平台的 YOLOv8 模型加载、推理、释放
      使用零拷贝 API（rknn_create_mem + rknn_set_io_mem），输入输出直接在 NPU DMA 内存操作

【整体工作流程】
1. init_yolov8_model()：加载 RKNN 模型文件
   - rknn_init 初始化推理上下文
   - rknn_query 获取 native 输入输出属性（NPU 原生格式，如 NC1HWC2）
   - rknn_create_mem 为输入/输出分配 NPU DMA 内存
   - rknn_set_io_mem 将 DMA 内存绑定到输入/输出槽位
   - 保存 input_native_attrs 和 output_native_attrs（含 NC1HWC2 格式信息）
   - 使用局部变量 + goto cleanup 模式，确保失败时不污染 app_ctx
2. inference_yolov8_model()：单帧推理
   - letterbox 预处理直接写入 NPU DMA 输入内存（零拷贝，无需 rknn_inputs_set）
   - rknn_run 执行推理
   - 逐输出分支：NC1HWC2_i8_to_NCHW_i8 转换为 NCHW 排布，供 post_process 使用
   - post_process 后处理
   - 释放转换后的临时输出缓冲
3. NC1HWC2_i8_to_NCHW_i8()：将 NPU 原生 NC1HWC2 int8 排布转为标准 NCHW int8
4. release_yolov8_model()：释放 DMA 内存、属性数组、销毁 RKNN 上下文

关键技术点：
- 零拷贝：输入直接写入 NPU DMA 内存，省去 rknn_inputs_set 的 memcpy
- 输出排布 NC1HWC2：NPU 内部将通道按 C2=4 分组，软件需转回 NCHW 才能做后处理
- 输入设 UINT8 类型，NPU 自动将归一化+量化融合在硬件内
- rknn_run 打印节流到 1s 一次，减少 syscall 开销
- init_yolov8_model 使用局部变量 + goto cleanup，失败时不泄漏也不污染 app_ctx
- 5 参数 rknn_init（第 5 参数传 NULL）
================================================================================
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>

#include "yolov8.h"
#include "common.h"
#include "file_utils.h"
#include "image_utils.h"

/**
 * @brief 打印 rknn_tensor_attr 结构体的详细信息（含 w_stride、size_with_stride）
 * @param attr 指向待打印的张量属性结构体
 */
static void dump_tensor_attr(rknn_tensor_attr *attr) {
    char dims[128] = {0};
    for (int i = 0; i < attr->n_dims; ++i) {
        int idx = strlen(dims);
        sprintf(&dims[idx], "%d%s", attr->dims[i], (i == attr->n_dims - 1) ? "" : ", ");
    }
    printf("  index=%d, name=%s, n_dims=%d, dims=[%s], n_elems=%d, size=%d, w_stride = %d, size_with_stride = %d, "
           "fmt=%s, type=%s, qnt_type=%s, "
           "zp=%d, scale=%f\n",
           attr->index, attr->name, attr->n_dims, dims, attr->n_elems, attr->size, attr->w_stride, attr->size_with_stride,
           get_format_string(attr->fmt), get_type_string(attr->type), get_qnt_type_string(attr->qnt_type), attr->zp,
           attr->scale);
}

/**
 * @brief 初始化 YOLOv8 RKNN 模型（零拷贝模式）：加载权重、分配 NPU DMA 内存、绑定 IO
 *        使用局部变量 + goto cleanup，确保失败时不污染 app_ctx
 * @param model_path RKNN 模型文件路径
 * @param app_ctx [OUT] RKNN 应用上下文，成功后包含模型句柄、DMA 内存、native 逻辑属性等
 * @return 0成功，-1失败
 */
int init_yolov8_model(const char *model_path, rknn_app_context_t *app_ctx) {
    int ret;
    int model_len = 0;
    char *model = NULL;
    rknn_context ctx = 0;
    // 用 local 变量, 全部成功后再 commit 到 app_ctx, 避免半构造状态被外部看到
    rknn_tensor_attr *input_attrs_heap     = NULL;
    rknn_tensor_attr *output_attrs_heap    = NULL;
    rknn_tensor_attr *input_native_heap   = NULL;
    rknn_tensor_attr *output_native_heap  = NULL;
    // VLA 改用堆, 避免 goto 跨过 VLA 初始化 (C++ 标准禁止)
    rknn_tensor_attr *input_native_attrs  = NULL;
    rknn_tensor_attr *output_native_attrs = NULL;
    rknn_tensor_attr *input_attrs         = NULL;
    rknn_tensor_attr *output_attrs        = NULL;
    bool input_mem_set   = false;
    bool any_output_mem_set = false;

    // Load RKNN Model
    model_len = read_data_from_file(model_path, &model);
    if (model == NULL) {
        printf("load_model fail!\n");
        return -1;
    }

    ret = rknn_init(&ctx, model, model_len, 0, NULL);
    free(model);
    model = NULL;
    if (ret < 0) {
        printf("rknn_init fail! ret=%d\n", ret);
        return -1;
    }

    // Get Model Input Output Number
    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        printf("rknn_query IN_OUT_NUM fail! ret=%d\n", ret);
        goto cleanup;
    }
    printf("model input num: %d, output num: %d\n", io_num.n_input, io_num.n_output);

    // 越界保护: 防止 io_num.n_input / n_output 超过 app_ctx 声明的硬编码容量
    if (io_num.n_input < 1 || io_num.n_input > 1) {
        printf("unsupported n_input=%u (this build supports exactly 1)\n", io_num.n_input);
        goto cleanup;
    }
    if (io_num.n_output < 1 || io_num.n_output > 9) {
        printf("unsupported n_output=%u (this build supports up to 9)\n", io_num.n_output);
        goto cleanup;
    }

    // 分配临时 attr 缓冲区 (堆, VLA 不能跨 goto)
    input_native_attrs  = (rknn_tensor_attr *)calloc(io_num.n_input,  sizeof(rknn_tensor_attr));
    output_native_attrs = (rknn_tensor_attr *)calloc(io_num.n_output, sizeof(rknn_tensor_attr));
    input_attrs         = (rknn_tensor_attr *)calloc(io_num.n_input,  sizeof(rknn_tensor_attr));
    output_attrs        = (rknn_tensor_attr *)calloc(io_num.n_output, sizeof(rknn_tensor_attr));
    if (!input_native_attrs || !output_native_attrs || !input_attrs || !output_attrs) {
        printf("calloc for tensor attrs (temp) failed\n");
        goto cleanup;
    }

    // Get Model Input Info (native, for io_mem binding)
    printf("input tensors:\n");
    for (int i = 0; i < (int)io_num.n_input; i++) {
        input_native_attrs[i].index = (uint32_t)i;
        ret = rknn_query(ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &input_native_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("rknn_query NATIVE_INPUT_ATTR[%d] fail! ret=%d\n", i, ret);
            goto cleanup;
        }
        dump_tensor_attr(&input_native_attrs[i]);
    }

    // default input type is int8 (normalize and quantize need compute in outside)
    // if set uint8, will fuse normalize and quantize to npu
    input_native_attrs[0].type = RKNN_TENSOR_UINT8;
    app_ctx->input_mems[0] = rknn_create_mem(ctx, input_native_attrs[0].size_with_stride);
    if (app_ctx->input_mems[0] == NULL) {
        printf("rknn_create_mem for input failed (size=%u)\n", input_native_attrs[0].size_with_stride);
        goto cleanup;
    }

    ret = rknn_set_io_mem(ctx, app_ctx->input_mems[0], &input_native_attrs[0]);
    if (ret < 0) {
        printf("input_mems rknn_set_io_mem fail! ret=%d\n", ret);
        goto cleanup;
    }
    input_mem_set = true;

    // Get Model Output Info (native, for io_mem binding)
    printf("output tensors:\n");
    for (int i = 0; i < (int)io_num.n_output; i++) {
        output_native_attrs[i].index = (uint32_t)i;
        ret = rknn_query(ctx, RKNN_QUERY_NATIVE_OUTPUT_ATTR, &output_native_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("rknn_query NATIVE_OUTPUT_ATTR[%d] fail! ret=%d\n", i, ret);
            goto cleanup;
        }
        dump_tensor_attr(&output_native_attrs[i]);
    }

    // Set output tensor memory
    for (uint32_t i = 0; i < io_num.n_output; ++i) {
        app_ctx->output_mems[i] = rknn_create_mem(ctx, output_native_attrs[i].size_with_stride);
        if (app_ctx->output_mems[i] == NULL) {
            printf("rknn_create_mem for output[%u] failed (size=%u)\n", i, output_native_attrs[i].size_with_stride);
            goto cleanup;
        }
        ret = rknn_set_io_mem(ctx, app_ctx->output_mems[i], &output_native_attrs[i]);
        if (ret < 0) {
            printf("output_mems[%u] rknn_set_io_mem fail! ret=%d\n", i, ret);
            goto cleanup;
        }
        any_output_mem_set = true;
    }

    if (output_native_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC && output_native_attrs[0].type == RKNN_TENSOR_INT8) {
        app_ctx->is_quant = true;
    } else {
        app_ctx->is_quant = false;
    }

    // Query non-native (logical) input/output attrs
    for (int i = 0; i < (int)io_num.n_input; i++) {
        input_attrs[i].index = (uint32_t)i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &input_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("rknn_query INPUT_ATTR fail! ret=%d\n", ret);
            goto cleanup;
        }
    }

    for (int i = 0; i < (int)io_num.n_output; i++) {
        output_attrs[i].index = (uint32_t)i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &output_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("rknn_query OUTPUT_ATTR[%d] fail! ret=%d\n", i, ret);
            goto cleanup;
        }
    }

    // Allocate heap copies of attrs (4 arrays)
    input_attrs_heap     = (rknn_tensor_attr *)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
    output_attrs_heap    = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
    input_native_heap    = (rknn_tensor_attr *)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
    output_native_heap   = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
    if (!input_attrs_heap || !output_attrs_heap || !input_native_heap || !output_native_heap) {
        printf("malloc for tensor attrs failed\n");
        goto cleanup;
    }
    memcpy(input_attrs_heap,    input_attrs,     io_num.n_input  * sizeof(rknn_tensor_attr));
    memcpy(output_attrs_heap,   output_attrs,    io_num.n_output * sizeof(rknn_tensor_attr));
    memcpy(input_native_heap,   input_native_attrs,  io_num.n_input  * sizeof(rknn_tensor_attr));
    memcpy(output_native_heap,  output_native_attrs, io_num.n_output * sizeof(rknn_tensor_attr));

    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        printf("model is NCHW input fmt\n");
        app_ctx->model_channel = input_attrs[0].dims[1];
        app_ctx->model_height  = input_attrs[0].dims[2];
        app_ctx->model_width   = input_attrs[0].dims[3];
    } else {
        printf("model is NHWC input fmt\n");
        app_ctx->model_height  = input_attrs[0].dims[1];
        app_ctx->model_width   = input_attrs[0].dims[2];
        app_ctx->model_channel = input_attrs[0].dims[3];
    }
    printf("model input height=%d, width=%d, channel=%d\n",
           app_ctx->model_height, app_ctx->model_width, app_ctx->model_channel);

    // ==================== 全部成功, commit 到 app_ctx ====================
    app_ctx->io_num            = io_num;
    app_ctx->input_attrs       = input_attrs_heap;
    app_ctx->output_attrs      = output_attrs_heap;
    app_ctx->input_native_attrs  = input_native_heap;
    app_ctx->output_native_attrs = output_native_heap;
    app_ctx->rknn_ctx          = ctx;
    // 清空 local 指针, 避免 cleanup 重复 free 已经 commit 给 app_ctx 的 heap
    input_attrs_heap = output_attrs_heap = input_native_heap = output_native_heap = NULL;
    // 临时的也置 NULL, cleanup 不再 free
    free(input_native_attrs);   input_native_attrs = NULL;
    free(output_native_attrs);  output_native_attrs = NULL;
    free(input_attrs);          input_attrs = NULL;
    free(output_attrs);         output_attrs = NULL;
    return 0;

cleanup:
    // 任何一步失败, 释放已分配的资源, 不污染 app_ctx
    if (input_attrs_heap)     free(input_attrs_heap);
    if (output_attrs_heap)    free(output_attrs_heap);
    if (input_native_heap)    free(input_native_heap);
    if (output_native_heap)   free(output_native_heap);
    if (input_native_attrs)   free(input_native_attrs);
    if (output_native_attrs)  free(output_native_attrs);
    if (input_attrs)          free(input_attrs);
    if (output_attrs)         free(output_attrs);

    if (ctx != 0) {
        // 先 destroy 已经 set_io_mem 的 mems, 再 destroy ctx
        // 注: input/output mems 绑在 ctx 上, 必须按 rknn_set_io_mem 反序 destroy
        if (input_mem_set && app_ctx->input_mems[0]) {
            rknn_destroy_mem(ctx, app_ctx->input_mems[0]);
            app_ctx->input_mems[0] = NULL;
        }
        if (any_output_mem_set) {
            for (uint32_t i = 0; i < io_num.n_output; ++i) {
                if (app_ctx->output_mems[i]) {
                    rknn_destroy_mem(ctx, app_ctx->output_mems[i]);
                    app_ctx->output_mems[i] = NULL;
                }
            }
        }
        rknn_destroy(ctx);
    }
    return -1;
}

/**
 * @brief 将 NPU 原生 NC1HWC2 int8 排布转换为标准 NCHW int8 排布
 *        NC1HWC2：通道按 C2=4 分组，内存布局为 [N][C1][H][W][C2]
 *        NCHW：标准排布 [N][C][H][W]
 * @param src NC1HWC2 格式源数据指针
 * @param dst [OUT] NCHW 格式目标数据指针
 * @param dims NC1HWC2 维度数组 [N, C1, H, W, C2]
 * @param channel 逻辑通道数（= C1 * C2）
 * @param h 空间高度
 * @param w 空间宽度
 * @param zp 量化零点（未使用，保留参数）
 * @param scale 量化缩放系数（未使用，保留参数）
 * @return 固定返回0
 */
int NC1HWC2_i8_to_NCHW_i8(const int8_t *src, int8_t *dst, int *dims, int channel, int h, int w, int zp, float scale) {
    int batch  = dims[0];
    int C1     = dims[1];
    int C2     = dims[4];
    int hw_src = dims[2] * dims[3];
    int hw_dst = h * w;
    for (int i = 0; i < batch; i++) {
        const int8_t *src_b = src + i * C1 * hw_src * C2;
        int8_t        *dst_b = dst + i * channel * hw_dst;
        for (int c = 0; c < channel; ++c) {
            int           plane  = c / C2;
            const int8_t *src_bc = plane * hw_src * C2 + src_b;
            int           offset = c % C2;
            for (int cur_h = 0; cur_h < h; ++cur_h)
                for (int cur_w = 0; cur_w < w; ++cur_w) {
                    int cur_hw                 = cur_h * w + cur_w;
                    dst_b[c * hw_dst + cur_hw] = src_bc[C2 * cur_hw + offset] ; // int8-->int8
                }
        }
    }

    return 0;
}

/**
 * @brief 释放 YOLOv8 模型资源（零拷贝模式）：释放属性数组、DMA 内存、RKNN 上下文
 *        每个 mem 都尝试 destroy，不因单个失败而跳过后续（防泄漏）
 * @param app_ctx RKNN 应用上下文
 * @return 0成功，-1有任何 rknn_destroy_mem/rknn_destroy 失败
 */
int release_yolov8_model(rknn_app_context_t *app_ctx) {
    int ret;
    int any_fail = 0;

    if (app_ctx->input_attrs != NULL) {
        free(app_ctx->input_attrs);
        app_ctx->input_attrs = NULL;
    }
    if (app_ctx->output_attrs != NULL) {
        free(app_ctx->output_attrs);
        app_ctx->output_attrs = NULL;
    }
    if (app_ctx->input_native_attrs != NULL) {
        free(app_ctx->input_native_attrs);
        app_ctx->input_native_attrs = NULL;
    }
    if (app_ctx->output_native_attrs != NULL) {
        free(app_ctx->output_native_attrs);
        app_ctx->output_native_attrs = NULL;
    }

    // 关键修复: 第一个 destroy 失败不再立刻 return, 否则后续 mems 全部泄漏
    // 每个 mem 都尝试 destroy + 记录失败, 最后统一返回
    for (int i = 0; i < app_ctx->io_num.n_input; i++) {
        if (app_ctx->input_mems[i] != NULL) {
            ret = rknn_destroy_mem(app_ctx->rknn_ctx, app_ctx->input_mems[i]);
            app_ctx->input_mems[i] = NULL;
            if (ret != RKNN_SUCC) {
                printf("rknn_destroy_mem input[%d] fail! ret=%d\n", i, ret);
                any_fail = 1;
            }
        }
    }
    for (int i = 0; i < app_ctx->io_num.n_output; i++) {
        if (app_ctx->output_mems[i] != NULL) {
            ret = rknn_destroy_mem(app_ctx->rknn_ctx, app_ctx->output_mems[i]);
            app_ctx->output_mems[i] = NULL;
            if (ret != RKNN_SUCC) {
                printf("rknn_destroy_mem output[%d] fail! ret=%d\n", i, ret);
                any_fail = 1;
            }
        }
    }
    if (app_ctx->rknn_ctx != 0) {
        ret = rknn_destroy(app_ctx->rknn_ctx);
        if (ret != RKNN_SUCC) {
            printf("rknn_destroy fail! ret=%d\n", ret);
            any_fail = 1;
        }
        app_ctx->rknn_ctx = 0;
    }
    return any_fail ? -1 : 0;
}

/**
 * @brief YOLOv8 单帧推理（零拷贝模式）：letterbox 直写 NPU DMA → rknn_run → NC1HWC2→NCHW → post_process
 * @param app_ctx RKNN 应用上下文
 * @param img 输入图像（RGB888 格式）
 * @param od_results [OUT] 检测结果列表
 * @return 0成功，-1失败
 */
int inference_yolov8_model(rknn_app_context_t *app_ctx, image_buffer_t *img, object_detect_result_list *od_results) {
    int ret;
    image_buffer_t dst_img;
    letterbox_t letter_box;
    const float nms_threshold = NMS_THRESH;      // 默认的NMS阈值
    const float box_conf_threshold = BOX_THRESH; // 默认的置信度阈值
    int bg_color = 114;

    if ((!app_ctx) || !(img) || (!od_results)) {
        return -1;
    }

    memset(od_results, 0x00, sizeof(*od_results));
    memset(&letter_box, 0, sizeof(letterbox_t));
    memset(&dst_img, 0, sizeof(image_buffer_t));

    // Pre Process
    dst_img.width = app_ctx->model_width;
    dst_img.height = app_ctx->model_height;
    dst_img.format = IMAGE_FORMAT_RGB888;
    dst_img.size = get_image_size(&dst_img);
    dst_img.fd = app_ctx->input_mems[0]->fd;
    dst_img.virt_addr = (unsigned char*)app_ctx->input_mems[0]->virt_addr;

    // 修正 (P1-1): rknn_create_mem 成功时 virt_addr 必然非 NULL;
    // fd 可能是任意正整数 (含 0=stdin),不能用 fd==0 判失败
    if (dst_img.virt_addr == NULL) {
        printf("rknn_create_mem for input returned NULL virt_addr\n");
        return -1;
    }

    // letterbox
    ret = convert_image_with_letterbox(img, &dst_img, &letter_box, bg_color);
    if (ret < 0) {
        printf("convert_image_with_letterbox fail! ret=%d\n", ret);
        return -1;
    }

    // Run
    // 调试打印节流到 1s 一次, 30fps 跑下去每秒 30 次 printf 也不便宜
    {
        static double _last_rknn_print_s = 0.0;
        static struct timeval _tv;
        gettimeofday(&_tv, NULL);
        double _now = _tv.tv_sec + _tv.tv_usec / 1e6;
        if (_now - _last_rknn_print_s >= 1.0)
        {
            printf("rknn_run\n");
            _last_rknn_print_s = _now;
        }
    }
    ret = rknn_run(app_ctx->rknn_ctx, nullptr);
    if (ret < 0) {
        printf("rknn_run fail! ret=%d\n", ret);
        return -1;
    }

    //NC1HWC2 to NCHW
    rknn_output outputs[app_ctx->io_num.n_output];
    memset(outputs, 0, sizeof(outputs));
    for (uint32_t i = 0; i < app_ctx->io_num.n_output; i++) {
        int   channel = app_ctx->output_attrs[i].dims[1];
        int   h       = app_ctx->output_attrs[i].n_dims > 2 ? app_ctx->output_attrs[i].dims[2] : 1;
        int   w       = app_ctx->output_attrs[i].n_dims > 3 ? app_ctx->output_attrs[i].dims[3] : 1;
        int   hw      = h * w;
        int   zp      = app_ctx->output_native_attrs[i].zp;
        float scale   = app_ctx->output_native_attrs[i].scale;
        if (app_ctx->is_quant) {
            outputs[i].size = app_ctx->output_native_attrs[i].n_elems * sizeof(int8_t);
            outputs[i].buf = (int8_t *)malloc(outputs[i].size);
            // 修正 (P0-1): malloc 失败直接退出,避免后续 memcpy/NC1HWC2 写 NULL 崩溃
            if (outputs[i].buf == NULL) {
                printf("malloc output buf failed, size=%u (i=%u)\n", outputs[i].size, i);
                ret = -1;
                break;
            }
            if (app_ctx->output_native_attrs[i].fmt == RKNN_TENSOR_NC1HWC2) {
                NC1HWC2_i8_to_NCHW_i8((int8_t *)app_ctx->output_mems[i]->virt_addr, (int8_t *)outputs[i].buf,
                                      (int *)app_ctx->output_native_attrs[i].dims, channel, h, w, zp, scale);
            } else {
                memcpy(outputs[i].buf, app_ctx->output_mems[i]->virt_addr, outputs[i].size);
            }
        } else {
            // 修正 (P0-2): 之前 goto out 跳过释放 + 返回 0 但未做 post_process, 双重 bug
            // 改为 break + ret=-1, 让外层释放循环处理已分配的 buf, 函数返回 -1
            printf("Currently zero copy does not support fp16!\n");
            ret = -1;
            break;
        }
    }

    // Post Process — 仅在所有输出转换成功时执行
    if (ret == 0) {
        post_process(app_ctx, outputs, &letter_box, box_conf_threshold, nms_threshold, od_results);
    }

    for (int i = 0; i < app_ctx->io_num.n_output; i++) {
        free(outputs[i].buf);
    }

    return ret;
}