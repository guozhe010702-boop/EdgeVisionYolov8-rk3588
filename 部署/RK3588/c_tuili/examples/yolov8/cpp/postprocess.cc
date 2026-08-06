// Copyright (c) 2021 by Rockchip Electronics Co., Ltd. All Rights Reserved.
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
文件：yolov8.cpp
功能：RK平台(RK3588/RV1106/RV1103等RKNPU) YOLOv8模型后处理代码
支持模型：YOLOv8 detect 检测模型（DFL分布焦点损失输出分支）
支持数据格式：FP32、U8量化、I8量化；区分RKNPU1/RKNPU2、RV1106 NHWC特殊排布

【整体工作流程】
1. init_post_process()：加载类别标签txt文件，初始化全局类别名字符串数组
2. rknn推理完成后调用 post_process() 入口函数
   2.1 遍历3个输出分支（3个不同stride检测头）
   2.2 根据芯片型号、量化类型选择对应process_xxx解析函数
   2.3 解析模型输出张量：反量化(量化模型) → DFL解码得到预测框坐标
   2.4 根据置信度阈值过滤低置信目标，收集候选框、置信度、类别ID
3. 全部检测头解析完成后：
   3.1 按照置信度从高到低排序所有候选框
   3.2 按类别执行NMS非极大值抑制，去除重叠框
4. 坐标映射修正：去除letterbox填充偏移，缩放回原图分辨率
5. 将最终检测结果存入 object_detect_result_list 对外输出
6. 外部接口 coco_cls_to_name() 根据类别ID获取类别名称
7. deinit_post_process() 释放标签内存，资源回收

关键技术点：
- DFL：Distribution Focal Loss，将4个坐标拆解为分布预测，通过softmax加权求和得到最终坐标
- NMS：按类别独立做非极大抑制，使用IOU阈值剔除高度重叠框
- 量化支持：实现u8/i8 affine量化正反转换函数
- 硬件兼容：区分RV1106 NHWC输出排布、RKNPU1/RKNPU2 NCHW张量维度差异
================================================================================
*/

#include "yolov8.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <set>
#include <vector>

static char *labels[OBJ_CLASS_NUM];

/**
 * @brief 将数值限制在 [min, max] 区间内
 * @param val 待裁剪浮点数
 * @param min 最小值
 * @param max 最大值
 * @return 裁剪后数值
 */
inline static int clamp(float val, int min, int max) { return val > min ? (val < max ? val : max) : min; }

/**
 * @brief 加载类别标签文本文件，读取每一行类别名称存入字符串数组
 * @param locationFilename 标签txt文件路径
 * @param label 输出：类别名称指针数组
 * @return 成功读取的类别数量；读取失败返回0
 */
static int loadLabelName(const char *locationFilename, char *label[])
{
    if (locationFilename == NULL || label == NULL || OBJ_CLASS_NUM <= 0)
    {
        printf("Invalid parameter!\n");
        return 0;
    }

    printf("load label %s\n", locationFilename);

    FILE *file = fopen(locationFilename, "r");
    if (file == NULL)
    {
        printf("Open %s fail!\n", locationFilename);
        return 0;
    }

    int i = 0;
    const int max_line = OBJ_CLASS_NUM;
    const size_t MAX_SINGLE_LINE = 512;
    // 预分配行缓冲区，避免循环内频繁realloc
    char *line_buf = (char *)malloc(MAX_SINGLE_LINE);
    if (line_buf == NULL)
    {
        fclose(file);
        printf("malloc line buffer failed\n");
        return 0;
    }

    while (i < max_line)
    {
        int ch;
        int line_idx = 0;

        while ((ch = fgetc(file)) != '\n' && ch != EOF)
        {
            if ((size_t)line_idx >= MAX_SINGLE_LINE - 1)
            {
                while ((fgetc(file)) != '\n' && fgetc(file) != EOF)
                    ;
                break;
            }
            line_buf[line_idx++] = (char)ch;
        }
        line_buf[line_idx] = '\0';

        // EOF并且当前无内容，终止读取
        if (ch == EOF && line_idx == 0)
        {
            break;
        }

        size_t str_len = strlen(line_buf);
        char *buffer = (char *)malloc(str_len + 1);
        if (buffer == NULL)
        {
            printf("malloc label string failed\n");
            break;
        }
        memcpy(buffer, line_buf, str_len + 1);
        label[i++] = buffer;
    }

    free(line_buf);
    fclose(file);

    if (i < max_line)
    {
        for (int k = 0; k < i; k++)
        {
            free(label[k]);
            label[k] = NULL;
        }
        i = 0;
    }

    return i;
}

/**
 * @brief 计算两个矩形框的IOU交并比
 * @param xmin0,ymin0,xmax0,ymax0 框1左上角、右下角坐标
 * @param xmin1,ymin1,xmax1,ymax1 框2左上角、右下角坐标
 * @return IOU值 [0,1]
 */
static float CalculateOverlap(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1, float ymin1, float xmax1, float ymax1)
{
    float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0);
    float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0);
    float i = w * h;
    float u = (xmax0 - xmin0 + 1.0) * (ymax0 - ymin0 + 1.0) + (xmax1 - xmin1 + 1.0) * (ymax1 - ymin1 + 1.0) - i;
    return u <= 0.f ? 0.f : (i / u);
}

/**
 * @brief 单类别NMS非极大值抑制，将重叠度超过阈值的候选框标记为-1剔除
 * @param validCount 候选框总数量
 * @param outputLocations 所有候选框坐标数组 [x,y,w,h]
 * @param classIds 每个候选框对应的类别ID
 * @param order 按置信度排序后的候选框下标数组
 * @param filterId 当前需要执行NMS的目标类别
 * @param threshold IOU阈值，超过阈值则剔除
 * @return 固定返回0
 */
static int nms(int validCount, std::vector<float> &outputLocations, std::vector<int> classIds, std::vector<int> &order, int filterId, float threshold)
{
    for (int i = 0; i < validCount; ++i)
    {
        int n = order[i];
        if (n == -1 || classIds[n] != filterId)
        {
            continue;
        }
        for (int j = i + 1; j < validCount; ++j)
        {
            int m = order[j];
            if (m == -1 || classIds[m] != filterId)
            {
                continue;
            }
            float xmin0 = outputLocations[n * 4 + 0];
            float ymin0 = outputLocations[n * 4 + 1];
            float xmax0 = outputLocations[n * 4 + 0] + outputLocations[n * 4 + 2];
            float ymax0 = outputLocations[n * 4 + 1] + outputLocations[n * 4 + 3];

            float xmin1 = outputLocations[m * 4 + 0];
            float ymin1 = outputLocations[m * 4 + 1];
            float xmax1 = outputLocations[m * 4 + 0] + outputLocations[m * 4 + 2];
            float ymax1 = outputLocations[m * 4 + 1] + outputLocations[m * 4 + 3];

            float iou = CalculateOverlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1);

            if (iou > threshold)
            {
                order[j] = -1;
            }
        }
    }
    return 0;
}

/**
 * @brief 快速排序：根据置信度数组，对下标索引进行逆序排序（从大到小）
 * @param input 置信度数组
 * @param left 排序区间左边界
 * @param right 排序区间右边界
 * @param indices 候选框下标索引数组，随置信度同步交换
 * @return 基准元素位置
 */
static int quick_sort_indice_inverse(std::vector<float> &input, int left, int right, std::vector<int> &indices)
{
    float key;
    int key_index;
    int low = left;
    int high = right;
    if (left < right)
    {
        key_index = indices[left];
        key = input[left];
        while (low < high)
        {
            while (low < high && input[high] <= key)
            {
                high--;
            }
            input[low] = input[high];
            indices[low] = indices[high];
            while (low < high && input[low] >= key)
            {
                low++;
            }
            input[high] = input[low];
            indices[high] = indices[low];
        }
        input[low] = key;
        indices[low] = key_index;
        quick_sort_indice_inverse(input, left, low - 1, indices);
        quick_sort_indice_inverse(input, low + 1, right, indices);
    }
    return low;
}

/**
 * @brief sigmoid激活函数，增加极值截断优化运算
 * @param x 输入浮点数
 * @return sigmoid结果
 */
static float sigmoid(float x) { return 1.0f / (1.0f + expf(-x)); }

/**
 * @brief sigmoid逆函数
 * @param y sigmoid输出值
 * @return 原始输入x
 */
static float unsigmoid(float y) { return -1.0 * logf((1.0 / y) - 1.0); }

/**
 * @brief 将float数值裁剪至[min,max]区间，返回int32
 * @param val 待裁剪浮点数
 * @param min 下限
 * @param max 上限
 * @return 裁剪后整型值
 */
inline static int32_t __clip(float val, float min, float max)
{
    float f = val <= min ? min : (val >= max ? max : val);
    return f;
}

/**
 * @brief FP32浮点量化为int8 affine量化值
 * @param f32 原始浮点值
 * @param zp 量化零点
 * @param scale 量化缩放系数
 * @return int8量化结果
 */
static int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale)
{
    float dst_val = (f32 / scale) + zp;
    int8_t res = (int8_t)__clip(dst_val, -128, 127);
    return res;
}

/**
 * @brief FP32浮点量化为uint8 affine量化值
 * @param f32 原始浮点值
 * @param zp 量化零点
 * @param scale 量化缩放系数
 * @return uint8量化结果
 */
static uint8_t qnt_f32_to_affine_u8(float f32, int32_t zp, float scale)
{
    float dst_val = (f32 / scale) + zp;
    uint8_t res = (uint8_t)__clip(dst_val, 0, 255);
    return res;
}

/**
 * @brief int8量化值反量化恢复为FP32浮点数
 * @param qnt int8量化数据
 * @param zp 量化零点
 * @param scale 量化缩放系数
 * @return 还原浮点数值
 */
static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale) { return ((float)qnt - (float)zp) * scale; }

/**
 * @brief uint8量化值反量化恢复为FP32浮点数
 * @param qnt uint8量化数据
 * @param zp 量化零点
 * @param scale 量化缩放系数
 * @return 还原浮点数值
 */
static float deqnt_affine_u8_to_f32(uint8_t qnt, int32_t zp, float scale) { return ((float)qnt - (float)zp) * scale; }

/**
 * @brief DFL分布焦点损失解码：对分布向量做softmax加权求和，得到最终4个偏移坐标
 * @param tensor DFL原始分布向量
 * @param dfl_len 每个坐标对应的分布长度
 * @param box 输出：解码后的4个坐标偏移 [t0,t1,t2,t3]
 */
static void compute_dfl(float *tensor, int dfl_len, float *box)
{
    for (int b = 0; b < 4; b++)
    {
        float exp_t[dfl_len];
        float exp_sum = 0;
        float acc_sum = 0;
        for (int i = 0; i < dfl_len; i++)
        {
            exp_t[i] = exp(tensor[i + b * dfl_len]);
            exp_sum += exp_t[i];
        }

        for (int i = 0; i < dfl_len; i++)
        {
            acc_sum += exp_t[i] / exp_sum * i;
        }
        box[b] = acc_sum;
    }
}

/**
 * @brief RKNPU U8量化模型单检测头输出解析（NCHW排布，RK3588等）
 * @param box_tensor box分支uint8张量指针
 * @param box_zp box张量零点；box_scale box张量缩放系数
 * @param score_tensor 类别置信度uint8张量；score_zp/score_scale 量化参数
 * @param score_sum_tensor 置信度总和张量，用于快速预过滤；可为nullptr
 * @param score_sum_zp/score_sum_scale sum张量量化参数
 * @param grid_h grid_w 当前检测头网格尺寸
 * @param stride 当前检测头下采样步长
 * @param dfl_len DFL分布长度
 * @param boxes 输出：候选框x,y,w,h集合
 * @param objProbs 输出：候选框置信度
 * @param classId 输出：候选框类别ID
 * @param threshold 置信度过滤阈值
 * @return 该检测头筛选后的有效候选框数量
 */
static int process_u8(uint8_t *box_tensor, int32_t box_zp, float box_scale,
                      uint8_t *score_tensor, int32_t score_zp, float score_scale,
                      uint8_t *score_sum_tensor, int32_t score_sum_zp, float score_sum_scale,
                      int grid_h, int grid_w, int stride, int dfl_len,
                      std::vector<float> &boxes,
                      std::vector<float> &objProbs,
                      std::vector<int> &classId,
                      float threshold)
{
    int validCount = 0;
    int grid_len = grid_h * grid_w;
    uint8_t score_thres_u8 = qnt_f32_to_affine_u8(threshold, score_zp, score_scale);
    uint8_t score_sum_thres_u8 = qnt_f32_to_affine_u8(threshold, score_sum_zp, score_sum_scale);

    for (int i = 0; i < grid_h; i++)
    {
        for (int j = 0; j < grid_w; j++)
        {
            int offset = i * grid_w + j;
            int max_class_id = -1;

            // Use score sum to quickly filter
            if (score_sum_tensor != nullptr)
            {
                if (score_sum_tensor[offset] < score_sum_thres_u8)
                {
                    continue;
                }
            }

            uint8_t max_score = -score_zp;
            for (int c = 0; c < OBJ_CLASS_NUM; c++)
            {
                if ((score_tensor[offset] > score_thres_u8) && (score_tensor[offset] > max_score))
                {
                    max_score = score_tensor[offset];
                    max_class_id = c;
                }
                offset += grid_len;
            }

            // compute box
            if (max_score > score_thres_u8)
            {
                offset = i * grid_w + j;
                float box[4];
                float before_dfl[dfl_len * 4];
                for (int k = 0; k < dfl_len * 4; k++)
                {
                    before_dfl[k] = deqnt_affine_u8_to_f32(box_tensor[offset], box_zp, box_scale);
                    offset += grid_len;
                }
                compute_dfl(before_dfl, dfl_len, box);

                float x1, y1, x2, y2, w, h;
                x1 = (-box[0] + j + 0.5) * stride;
                y1 = (-box[1] + i + 0.5) * stride;
                x2 = (box[2] + j + 0.5) * stride;
                y2 = (box[3] + i + 0.5) * stride;
                w = x2 - x1;
                h = y2 - y1;
                boxes.push_back(x1);
                boxes.push_back(y1);
                boxes.push_back(w);
                boxes.push_back(h);

                objProbs.push_back(deqnt_affine_u8_to_f32(max_score, score_zp, score_scale));
                classId.push_back(max_class_id);
                validCount++;
            }
        }
    }
    return validCount;
}

/**
 * @brief RKNPU I8量化模型单检测头输出解析（NCHW排布，RK3588等RKNPU2）
 * @param box_tensor box分支int8张量指针
 * @param box_zp box张量零点；box_scale box张量缩放系数
 * @param score_tensor 类别置信度int8张量；score_zp/score_scale 量化参数
 * @param score_sum_tensor 置信度总和张量，用于快速预过滤；可为nullptr
 * @param score_sum_zp/score_sum_scale sum张量量化参数
 * @param grid_h grid_w 当前检测头网格尺寸
 * @param stride 当前检测头下采样步长
 * @param dfl_len DFL分布长度
 * @param boxes 输出：候选框x,y,w,h集合
 * @param objProbs 输出：候选框置信度
 * @param classId 输出：候选框类别ID
 * @param threshold 置信度过滤阈值
 * @return 该检测头筛选后的有效候选框数量
 */
static int process_i8(int8_t *box_tensor, int32_t box_zp, float box_scale,
                      int8_t *score_tensor, int32_t score_zp, float score_scale,
                      int8_t *score_sum_tensor, int32_t score_sum_zp, float score_sum_scale,
                      int grid_h, int grid_w, int stride, int dfl_len,
                      std::vector<float> &boxes,
                      std::vector<float> &objProbs,
                      std::vector<int> &classId,
                      float threshold)
{
    int validCount = 0;
    int grid_len = grid_h * grid_w;
    int8_t score_thres_i8 = qnt_f32_to_affine(threshold, score_zp, score_scale);
    int8_t score_sum_thres_i8 = qnt_f32_to_affine(threshold, score_sum_zp, score_sum_scale);

    for (int i = 0; i < grid_h; i++)
    {
        for (int j = 0; j < grid_w; j++)
        {
            int offset = i * grid_w + j;
            int max_class_id = -1;

            // 通过 score sum 起到快速过滤的作用
            if (score_sum_tensor != nullptr)
            {
                if (score_sum_tensor[offset] < score_sum_thres_i8)
                {
                    continue;
                }
            }

            int8_t max_score = -score_zp;
            for (int c = 0; c < OBJ_CLASS_NUM; c++)
            {
                if ((score_tensor[offset] > score_thres_i8) && (score_tensor[offset] > max_score))
                {
                    max_score = score_tensor[offset];
                    max_class_id = c;
                }
                offset += grid_len;
            }

            // compute box
            if (max_score > score_thres_i8)
            {
                offset = i * grid_w + j;
                float box[4];
                float before_dfl[dfl_len * 4];
                for (int k = 0; k < dfl_len * 4; k++)
                {
                    before_dfl[k] = deqnt_affine_to_f32(box_tensor[offset], box_zp, box_scale);
                    offset += grid_len;
                }
                compute_dfl(before_dfl, dfl_len, box);

                float x1, y1, x2, y2, w, h;
                x1 = (-box[0] + j + 0.5) * stride;
                y1 = (-box[1] + i + 0.5) * stride;
                x2 = (box[2] + j + 0.5) * stride;
                y2 = (box[3] + i + 0.5) * stride;
                w = x2 - x1;
                h = y2 - y1;
                boxes.push_back(x1);
                boxes.push_back(y1);
                boxes.push_back(w);
                boxes.push_back(h);

                objProbs.push_back(deqnt_affine_to_f32(max_score, score_zp, score_scale));
                classId.push_back(max_class_id);
                validCount++;
            }
        }
    }
    return validCount;
}

/**
 * @brief FP32浮点模型单检测头输出解析（NCHW排布）
 * @param box_tensor box分支float张量指针
 * @param score_tensor 类别置信度float张量
 * @param score_sum_tensor 置信度总和张量，用于快速预过滤；可为nullptr
 * @param grid_h grid_w 当前检测头网格尺寸
 * @param stride 当前检测头下采样步长
 * @param dfl_len DFL分布长度
 * @param boxes 输出：候选框x,y,w,h集合
 * @param objProbs 输出：候选框置信度
 * @param classId 输出：候选框类别ID
 * @param threshold 置信度过滤阈值
 * @return 该检测头筛选后的有效候选框数量
 */
static int process_fp32(float *box_tensor, float *score_tensor, float *score_sum_tensor,
                        int grid_h, int grid_w, int stride, int dfl_len,
                        std::vector<float> &boxes,
                        std::vector<float> &objProbs,
                        std::vector<int> &classId,
                        float threshold)
{
    int validCount = 0;
    int grid_len = grid_h * grid_w;
    for (int i = 0; i < grid_h; i++)
    {
        for (int j = 0; j < grid_w; j++)
        {
            int offset = i * grid_w + j;
            int max_class_id = -1;

            // 通过 score sum 起到快速过滤的作用
            if (score_sum_tensor != nullptr)
            {
                if (score_sum_tensor[offset] < threshold)
                {
                    continue;
                }
            }

            float max_score = 0;
            for (int c = 0; c < OBJ_CLASS_NUM; c++)
            {
                if ((score_tensor[offset] > threshold) && (score_tensor[offset] > max_score))
                {
                    max_score = score_tensor[offset];
                    max_class_id = c;
                }
                offset += grid_len;
            }

            // compute box
            if (max_score > threshold)
            {
                offset = i * grid_w + j;
                float box[4];
                float before_dfl[dfl_len * 4];
                for (int k = 0; k < dfl_len * 4; k++)
                {
                    before_dfl[k] = box_tensor[offset];
                    offset += grid_len;
                }
                compute_dfl(before_dfl, dfl_len, box);

                float x1, y1, x2, y2, w, h;
                x1 = (-box[0] + j + 0.5) * stride;
                y1 = (-box[1] + i + 0.5) * stride;
                x2 = (box[2] + j + 0.5) * stride;
                y2 = (box[3] + i + 0.5) * stride;
                w = x2 - x1;
                h = y2 - y1;
                boxes.push_back(x1);
                boxes.push_back(y1);
                boxes.push_back(w);
                boxes.push_back(h);

                objProbs.push_back(max_score);
                classId.push_back(max_class_id);
                validCount++;
            }
        }
    }
    return validCount;
}

#if defined(RV1106_1103)
/**
 * @brief RV1106/RV1103专用I8量化解析函数，适配芯片NHWC张量输出排布
 * @param box_tensor box分支int8张量指针
 * @param box_zp box张量零点；box_scale box张量缩放系数
 * @param score_tensor 类别置信度int8张量；score_zp/score_scale 量化参数
 * @param score_sum_tensor 置信度总和张量，用于快速预过滤；可为nullptr
 * @param score_sum_zp/score_sum_scale sum张量量化参数
 * @param grid_h grid_w 当前检测头网格尺寸
 * @param stride 当前检测头下采样步长
 * @param dfl_len DFL分布长度
 * @param boxes 输出：候选框x,y,w,h集合
 * @param objProbs 输出：候选框置信度
 * @param classId 输出：候选框类别ID
 * @param threshold 置信度过滤阈值
 * @return 该检测头筛选后的有效候选框数量
 */
static int process_i8_rv1106(int8_t *box_tensor, int32_t box_zp, float box_scale,
                             int8_t *score_tensor, int32_t score_zp, float score_scale,
                             int8_t *score_sum_tensor, int32_t score_sum_zp, float score_sum_scale,
                             int grid_h, int grid_w, int stride, int dfl_len,
                             std::vector<float> &boxes,
                             std::vector<float> &objProbs,
                             std::vector<int> &classId,
                             float threshold)
{
    int validCount = 0;
    int grid_len = grid_h * grid_w;
    int8_t score_thres_i8 = qnt_f32_to_affine(threshold, score_zp, score_scale);
    int8_t score_sum_thres_i8 = qnt_f32_to_affine(threshold, score_sum_zp, score_sum_scale);

    for (int i = 0; i < grid_h; i++)
    {
        for (int j = 0; j < grid_w; j++)
        {
            int offset = i * grid_w + j;
            int max_class_id = -1;

            // 通过 score sum 起到快速过滤的作用
            if (score_sum_tensor != nullptr)
            {
                // score_sum_tensor [1, 1, 80, 80]
                if (score_sum_tensor[offset] < score_sum_thres_i8)
                {
                    continue;
                }
            }

            int8_t max_score = -score_zp;
            offset = offset * OBJ_CLASS_NUM;
            for (int c = 0; c < OBJ_CLASS_NUM; c++)
            {
                if ((score_tensor[offset + c] > score_thres_i8) && (score_tensor[offset + c] > max_score))
                {
                    max_score = score_tensor[offset + c]; // 80类 [1, 80, 80, 80] 3588NCHW 1106NHWC
                    max_class_id = c;
                }
            }

            // compute box
            if (max_score > score_thres_i8)
            {
                offset = (i * grid_w + j) * 4 * dfl_len;
                float box[4];
                float before_dfl[dfl_len * 4];
                for (int k = 0; k < dfl_len * 4; k++)
                {
                    before_dfl[k] = deqnt_affine_to_f32(box_tensor[offset + k], box_zp, box_scale);
                }
                compute_dfl(before_dfl, dfl_len, box);

                float x1, y1, x2, y2, w, h;
                x1 = (-box[0] + j + 0.5) * stride;
                y1 = (-box[1] + i + 0.5) * stride;
                x2 = (box[2] + j + 0.5) * stride;
                y2 = (box[3] + i + 0.5) * stride;
                w = x2 - x1;
                h = y2 - y1;
                boxes.push_back(x1);
                boxes.push_back(y1);
                boxes.push_back(w);
                boxes.push_back(h);

                objProbs.push_back(deqnt_affine_to_f32(max_score, score_zp, score_scale));
                classId.push_back(max_class_id);
                validCount++;
            }
        }
    }
    printf("validCount=%d\n", validCount);
    printf("grid h-%d, w-%d, stride %d\n", grid_h, grid_w, stride);
    return validCount;
}
#endif

/**
 * @brief YOLOv8后处理入口主函数，rknn推理完成后调用
 * @param app_ctx rknn应用上下文，包含模型宽高、量化信息、输出张量属性
 * @param outputs rknn推理输出张量数组指针（区分RV1106与其他芯片结构体）
 * @param letter_box letterbox填充信息：padding、缩放系数，用于坐标映射回原图
 * @param conf_threshold 置信度阈值
 * @param nms_threshold NMS的IOU阈值
 * @param od_results [OUT] 最终检测结果列表
 * @return 0成功，负数失败
 */
int post_process(rknn_app_context_t *app_ctx, void *outputs, letterbox_t *letter_box, float conf_threshold, float nms_threshold, object_detect_result_list *od_results)
{
#if defined(RV1106_1103)
    rknn_tensor_mem **_outputs = (rknn_tensor_mem **)outputs;
#else
    rknn_output *_outputs = (rknn_output *)outputs;
#endif
    std::vector<float> filterBoxes;
    std::vector<float> objProbs;
    std::vector<int> classId;
    int validCount = 0;
    int stride = 0;
    int grid_h = 0;
    int grid_w = 0;
    int model_in_w = app_ctx->model_width;
    int model_in_h = app_ctx->model_height;

    memset(od_results, 0, sizeof(object_detect_result_list));

    // default 3 branch
#ifdef RKNPU1
    int dfl_len = app_ctx->output_attrs[0].dims[2] / 4;
#else
    int dfl_len = app_ctx->output_attrs[0].dims[1] / 4;
#endif
    int output_per_branch = app_ctx->io_num.n_output / 3;
    for (int i = 0; i < 3; i++)
    {
#if defined(RV1106_1103)
        dfl_len = app_ctx->output_attrs[0].dims[3] / 4;
        void *score_sum = nullptr;
        int32_t score_sum_zp = 0;
        float score_sum_scale = 1.0;
        if (output_per_branch == 3)
        {
            score_sum = _outputs[i * output_per_branch + 2]->virt_addr;
            score_sum_zp = app_ctx->output_attrs[i * output_per_branch + 2].zp;
            score_sum_scale = app_ctx->output_attrs[i * output_per_branch + 2].scale;
        }
        int box_idx = i * output_per_branch;
        int score_idx = i * output_per_branch + 1;
        grid_h = app_ctx->output_attrs[box_idx].dims[1];
        grid_w = app_ctx->output_attrs[box_idx].dims[2];
        stride = model_in_h / grid_h;

        if (app_ctx->is_quant)
        {
            validCount += process_i8_rv1106((int8_t *)_outputs[box_idx]->virt_addr, app_ctx->output_attrs[box_idx].zp, app_ctx->output_attrs[box_idx].scale,
                                            (int8_t *)_outputs[score_idx]->virt_addr, app_ctx->output_attrs[score_idx].zp,
                                            app_ctx->output_attrs[score_idx].scale, (int8_t *)score_sum, score_sum_zp, score_sum_scale,
                                            grid_h, grid_w, stride, dfl_len, filterBoxes, objProbs, classId, conf_threshold);
        }
        else
        {
            printf("RV1106/1103 only support quantization mode\n", LABEL_NALE_TXT_PATH);
            return -1;
        }

#else
        void *score_sum = nullptr;
        int32_t score_sum_zp = 0;
        float score_sum_scale = 1.0;
        if (output_per_branch == 3)
        {
            score_sum = _outputs[i * output_per_branch + 2].buf;
            score_sum_zp = app_ctx->output_attrs[i * output_per_branch + 2].zp;
            score_sum_scale = app_ctx->output_attrs[i * output_per_branch + 2].scale;
        }
        int box_idx = i * output_per_branch;
        int score_idx = i * output_per_branch + 1;

#ifdef RKNPU1
        grid_h = app_ctx->output_attrs[box_idx].dims[1];
        grid_w = app_ctx->output_attrs[box_idx].dims[0];
#else
        grid_h = app_ctx->output_attrs[box_idx].dims[2];
        grid_w = app_ctx->output_attrs[box_idx].dims[3];
#endif
        stride = model_in_h / grid_h;

        if (app_ctx->is_quant)
        {
#ifdef RKNPU1
            validCount += process_u8((uint8_t *)_outputs[box_idx].buf, app_ctx->output_attrs[box_idx].zp, app_ctx->output_attrs[box_idx].scale,
                                     (uint8_t *)_outputs[score_idx].buf, app_ctx->output_attrs[score_idx].zp, app_ctx->output_attrs[score_idx].scale,
                                     (uint8_t *)score_sum, score_sum_zp, score_sum_scale,
                                     grid_h, grid_w, stride, dfl_len,
                                     filterBoxes, objProbs, classId, conf_threshold);
#else
            validCount += process_i8((int8_t *)_outputs[box_idx].buf, app_ctx->output_attrs[box_idx].zp, app_ctx->output_attrs[box_idx].scale,
                                     (int8_t *)_outputs[score_idx].buf, app_ctx->output_attrs[score_idx].zp, app_ctx->output_attrs[score_idx].scale,
                                     (int8_t *)score_sum, score_sum_zp, score_sum_scale,
                                     grid_h, grid_w, stride, dfl_len,
                                     filterBoxes, objProbs, classId, conf_threshold);
#endif
        }
        else
        {
            validCount += process_fp32((float *)_outputs[box_idx].buf, (float *)_outputs[score_idx].buf, (float *)score_sum,
                                       grid_h, grid_w, stride, dfl_len,
                                       filterBoxes, objProbs, classId, conf_threshold);
        }
#endif
    }

    // no object detect
    if (validCount <= 0)
    {
        return 0;
    }
    std::vector<int> indexArray;
    for (int i = 0; i < validCount; ++i)
    {
        indexArray.push_back(i);
    }
    quick_sort_indice_inverse(objProbs, 0, validCount - 1, indexArray);

    std::set<int> class_set(std::begin(classId), std::end(classId));

    for (auto c : class_set)
    {
        nms(validCount, filterBoxes, classId, indexArray, c, nms_threshold);
    }

    int last_count = 0;
    od_results->count = 0;

    /* box valid detect target */
    for (int i = 0; i < validCount; ++i)
    {
        if (indexArray[i] == -1 || last_count >= OBJ_NUMB_MAX_SIZE)
        {
            continue;
        }
        int n = indexArray[i];

        float x1 = filterBoxes[n * 4 + 0] - letter_box->x_pad;
        float y1 = filterBoxes[n * 4 + 1] - letter_box->y_pad;
        float x2 = x1 + filterBoxes[n * 4 + 2];
        float y2 = y1 + filterBoxes[n * 4 + 3];
        int id = classId[n];
        float obj_conf = objProbs[i];

        od_results->results[last_count].box.left = (int)(clamp(x1, 0, model_in_w) / letter_box->scale);
        od_results->results[last_count].box.top = (int)(clamp(y1, 0, model_in_h) / letter_box->scale);
        od_results->results[last_count].box.right = (int)(clamp(x2, 0, model_in_w) / letter_box->scale);
        od_results->results[last_count].box.bottom = (int)(clamp(y2, 0, model_in_h) / letter_box->scale);
        od_results->results[last_count].prop = obj_conf;
        od_results->results[last_count].cls_id = id;
        last_count++;
    }
    od_results->count = last_count;
    return 0;
}

/**
 * @brief 后处理模块初始化，加载类别标签文件
 * @return 成功返回0，失败返回负数
 */
int init_post_process()
{
    int ret = 0;
    ret = loadLabelName(LABEL_NALE_TXT_PATH, labels);
    if (ret < 0)
    {
        printf("Load %s failed!\n", LABEL_NALE_TXT_PATH);
        return -1;
    }
    return 0;
}

/**
 * @brief 根据类别ID获取类别名称字符串
 * @param cls_id 类别编号
 * @return 类别名字符串指针；越界返回"null"
 */
char *coco_cls_to_name(int cls_id)
{

    if (cls_id >= OBJ_CLASS_NUM)
    {
        return "null";
    }

    if (labels[cls_id])
    {
        return labels[cls_id];
    }

    return "null";
}

/**
 * @brief 释放标签名称内存，后处理资源反初始化
 */
void deinit_post_process()
{
    for (int i = 0; i < OBJ_CLASS_NUM; i++)
    {
        if (labels[i] != nullptr)
        {
            free(labels[i]);
            labels[i] = nullptr;
        }
    }
}