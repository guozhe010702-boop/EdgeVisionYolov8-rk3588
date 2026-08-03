/*
================================================================================
【文件总说明】
文件：rtmp_sink.cc
功能：RTMP 推流实现，基于 MPP 硬件 H.264 编码 + librtmp 推流
用途：提供 RtmpSink 类，将检测画面编码为 H.264 并推送到 RTMP 服务器（如 mediamtx）

【整体工作流程】
1. open()：初始化 MPP 编码器 + 连接 RTMP 服务器 + 启动 sender 线程
2. init_mpp()：配置 MPP H.264 编码器（CBR、High Profile、GOP=1s）
   - 预分配 MppFrame + MppBuffer 跨帧复用，避免 double free
   - 预导入 MPP DMA buffer 到 RGA，缓存 handle 用于 RGB→NV12 硬件加速
3. 检测线程调用 submit(rgb_frame)：
   - 与 pending_frame_ 做 std::swap（O(1) 浅拷贝），不阻塞检测线程
   - notify 唤醒 sender 线程
4. sender 线程循环：
   - 等待 pending_cv_ 唤醒，swap 拿走待发帧
   - encode_and_send()：
     a. RGB→NV12：优先 RGA 硬件加速，失败回退 CPU rgb_to_nv12()
     b. MPP 编码：encode_put_frame + encode_get_packet
     c. 解析 H.264 NAL：SPS+PPS 合并为 AVC sequence header，IDR/P 用 AVCC 格式
     d. RTMP_SendPacket 发送到服务器
5. close()：停止 sender 线程 → 断开 RTMP → deinit_mpp 释放编码器资源

关键技术点：
- MPP H.264 硬编码：CBR 1Mbps, High Profile, Level 4.0, CABAC
- RGB→NV12 双路径：RGA 硬件加速（~1ms）或 CPU 回退（~3ms）
- SPS/PPS 通过 EACH_IDR 模式从码流中提取，构造 AVCDecoderConfigurationRecord
- RTMPPacket 内存安全：置 NULL m_vecChannelsOut 副本的 m_body 防 double-free
- MppFrame/MppBuffer 跨帧复用，避免每帧 mpp_frame_deinit 导致 buffer 过早释放
- sender 线程编码/发送失败时置 send_failed_ 标志退出，不在线程内调 close() 防死锁
================================================================================
*/
#include "rtmp_sink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

extern "C" {
#include <librtmp/rtmp.h>
#include <librtmp/log.h>
}

// RGA 硬件加速 (RGB→NV12 色彩空间转换)
#include "im2d.h"
#include "rga.h"

// =====================================================================
// 构造 / 析构
// =====================================================================
/**
 * @brief 构造函数，成员初始化为默认值
 */
RtmpSink::RtmpSink() {}

/**
 * @brief 析构函数，自动关闭 RTMP 连接并释放资源
 */
RtmpSink::~RtmpSink() { close(); }

// =====================================================================
// RGB888 → NV12 (CPU 实现)
// NV12: Y 平面 (stride_h * stride_w) + UV 交错平面 (stride_h/2 * stride_w)
// src_w/src_h: 原始 RGB 尺寸 (相机分辨率)
// enc_w/enc_h: 编码器 NV12 尺寸 (16 对齐后, >= src)
// 超出 src 范围的像素填 0 (黑), 保证 MPP 编码不会读越界或花屏
// =====================================================================
/**
 * @brief CPU 实现 RGB888 → NV12 色彩空间转换，BT.601 full range，UV 取 2x2 平均
 * @param rgb 输入 RGB888 数据（3字节/像素，紧密排列）
 * @param src_w 输入图像宽度
 * @param src_h 输入图像高度
 * @param nv12 [OUT] 输出 NV12 数据（Y 平面 + UV 交错平面）
 * @param enc_w 输出 NV12 宽度（16 对齐，>= src_w）
 * @param enc_h 输出 NV12 高度（16 对齐，>= src_h）
 * @return 固定返回0
 */
int RtmpSink::rgb_to_nv12(const unsigned char *rgb, int src_w, int src_h,
                           unsigned char *nv12, int enc_w, int enc_h)
{
    unsigned char *y_plane  = nv12;
    unsigned char *uv_plane = nv12 + enc_w * enc_h;

    bool need_padding = (src_w != enc_w || src_h != enc_h);
    if (need_padding)
    {
        memset(nv12, 0, (size_t)enc_w * enc_h * 3 / 2);
    }

    for (int j = 0; j < src_h; j++)
    {
        for (int i = 0; i < src_w; i++)
        {
            int r = rgb[(j * src_w + i) * 3 + 0];
            int g = rgb[(j * src_w + i) * 3 + 1];
            int b = rgb[(j * src_w + i) * 3 + 2];

            int y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            y_plane[j * enc_w + i] = (unsigned char)(y < 0 ? 0 : y > 255 ? 255 : y);

            if ((j & 1) == 0 && (i & 1) == 0)
            {
                int r0 = r, g0 = g, b0 = b;
                int r1 = (i + 1 < src_w) ? rgb[(j * src_w + i + 1) * 3 + 0] : r0;
                int g1 = (i + 1 < src_w) ? rgb[(j * src_w + i + 1) * 3 + 1] : g0;
                int b1 = (i + 1 < src_w) ? rgb[(j * src_w + i + 1) * 3 + 2] : b0;
                int r2 = (j + 1 < src_h) ? rgb[((j + 1) * src_w + i) * 3 + 0] : r0;
                int g2 = (j + 1 < src_h) ? rgb[((j + 1) * src_w + i) * 3 + 1] : g0;
                int b2 = (j + 1 < src_h) ? rgb[((j + 1) * src_w + i) * 3 + 2] : b0;
                int r3 = (j + 1 < src_h && i + 1 < src_w) ? rgb[((j + 1) * src_w + i + 1) * 3 + 0] : r0;
                int g3 = (j + 1 < src_h && i + 1 < src_w) ? rgb[((j + 1) * src_w + i + 1) * 3 + 1] : g0;
                int b3 = (j + 1 < src_h && i + 1 < src_w) ? rgb[((j + 1) * src_w + i + 1) * 3 + 2] : b0;

                int rr = (r0 + r1 + r2 + r3 + 2) / 4;
                int gg = (g0 + g1 + g2 + g3 + 2) / 4;
                int bb = (b0 + b1 + b2 + b3 + 2) / 4;

                int u = ((-38 * rr - 74 * gg + 112 * bb + 128) >> 8) + 128;
                int v = ((112 * rr - 94 * gg - 18 * bb + 128) >> 8) + 128;

                int uv_idx = (j / 2) * enc_w + i;
                uv_plane[uv_idx + 0] = (unsigned char)(u < 0 ? 0 : u > 255 ? 255 : u);
                uv_plane[uv_idx + 1] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
    }
    return 0;
}

// =====================================================================
// RGA 硬件加速: RGB888 → NV12 色彩空间转换
// RGA 是 Rockchip 2D 硬件加速器, 可在 DMA 上直接做色彩转换和缩放,
// 省 CPU ~1-2ms/帧 (640x480). MPP DMA buffer 预导入缓存 (rga_dst_handle_).
// 失败时返回 false, 调用方回退 CPU rgb_to_nv12().
// =====================================================================
/**
 * @brief 尝试使用 RGA 硬件将 RGB888 Mat 转换为 NV12 并写入 MPP DMA 缓冲
 * @param rgb 输入 RGB cv::Mat（CV_8UC3，需连续存储）
 * @return true 成功，false 失败（需回退 CPU rgb_to_nv12）
 */
bool RtmpSink::try_rga_rgb_to_nv12(const cv::Mat &rgb)
{
    if (rga_dst_handle_ == 0) return false;       // MPP buffer 未导入 RGA
    if (!rgb.isContinuous())   return false;       // 非连续 Mat 不支持

    int src_w = rgb.cols;
    int src_h = rgb.rows;

    // 导入 RGB 虚拟地址到 RGA (每帧导入, 因为 Mat 地址可能变)
    rga_buffer_handle_t src_handle = importbuffer_virtualaddr(
        (void *)rgb.data, src_w, src_h, RK_FORMAT_RGB_888);
    if (src_handle == 0) return false;

    rga_buffer_t src_img = wrapbuffer_handle(src_handle, src_w, src_h, RK_FORMAT_RGB_888);
    rga_buffer_t dst_img = wrapbuffer_handle(rga_dst_handle_, enc_width_, enc_height_,
                                             RK_FORMAT_YCbCr_420_SP);

    // RGA 同时做色彩转换 + 缩放 (若 src != enc 尺寸)
    IM_STATUS status = imcvtcolor(src_img, dst_img,
                                  RK_FORMAT_RGB_888, RK_FORMAT_YCbCr_420_SP);

    releasebuffer_handle(src_handle);

    if (status != IM_STATUS_SUCCESS)
    {
        printf("[rtmp] RGA imcvtcolor failed (%d), fall back to CPU\n", status);
        return false;
    }
    return true;
}

/**
 * @brief 初始化 MPP H.264 硬件编码器，配置编码参数并预分配帧缓冲
 * @param width 编码宽度（会被 16 对齐）
 * @param height 编码高度（会被 16 对齐）
 * @param fps 编码帧率
 * @param bitrate_kbps 目标码率（kbps）
 * @return 0成功，-1失败
 */
int RtmpSink::init_mpp(int width, int height, int fps, int bitrate_kbps)
{
    MPP_RET ret = MPP_OK;

    // 宽高 16 对齐 (MPP 要求)
    enc_width_  = (width  + 15) & ~15;
    enc_height_ = (height + 15) & ~15;
    enc_fps_     = fps;
    enc_bitrate_ = bitrate_kbps;

    ret = mpp_create(&mpp_ctx_, &mpp_api_);
    if (ret != MPP_OK)
    {
        printf("[rtmp] mpp_create failed: %d\n", ret);
        printf("[rtmp] This board may not have MPP hardware encoder.\n");
        printf("[rtmp] Please use MJPEG mode instead: ./rknn_yolov8_demo_stream ... 8080\n");
        return -1;
    }

    // 输入输出都走 poll + 非阻塞
    ret = mpp_init(mpp_ctx_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK)
    {
        printf("[rtmp] mpp_init(H.264 encoder) failed: %d\n", ret);
        printf("[rtmp] H.264 encoding may not be supported on this SoC.\n");
        printf("[rtmp] Please use MJPEG mode instead: ./rknn_yolov8_demo_stream ... 8080\n");
        return -1;
    }

    // 编码配置
    ret = mpp_enc_cfg_init(&mpp_enc_cfg_);
    if (ret != MPP_OK)
    {
        printf("[rtmp] mpp_enc_cfg_init failed: %d\n", ret);
        return -1;
    }

    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "prep:width",       enc_width_);
    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "prep:height",      enc_height_);
    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "prep:hor_stride",  enc_width_);
    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "prep:ver_stride",  enc_height_);
    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "prep:format",      MPP_FMT_YUV420SP);

    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "rc:mode",          MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "rc:fps_in_num",    enc_fps_);
    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "rc:fps_out_num",   enc_fps_);
    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "rc:fps_out_denorm",1);
    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "rc:bps_target",    bitrate_kbps * 1000);
    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "rc:bps_max",       bitrate_kbps * 1000 * 3 / 2);
    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "rc:bps_min",       bitrate_kbps * 1000 / 2);
    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "rc:gop",           enc_fps_);  // 1秒一个关键帧
    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "rc:fps_in_flex",   0);
    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "rc:fps_out_flex",  0);

    // codec:type 已在 mpp_init 中指定, 不需要再设
    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "h264:profile",     100);  // High
    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "h264:level",       40);   // Level 4.0
    mpp_enc_cfg_set_s32(mpp_enc_cfg_, "h264:cabac_en",    1);    // CABAC

    ret = mpp_api_->control(mpp_ctx_, MPP_ENC_SET_CFG, mpp_enc_cfg_);
    if (ret != MPP_OK)
    {
        printf("[rtmp] MPP_ENC_SET_CFG failed: %d\n", ret);
        return -1;
    }

    // 设置 header mode: 每个 IDR 帧前插入 SPS/PPS
    // MPP_ENC_GET_HDR_SYNC 可能返回 NULL, 依赖 EACH_IDR 模式从码流中获取
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    ret = mpp_api_->control(mpp_ctx_, MPP_ENC_SET_HEADER_MODE, &header_mode);
    if (ret != MPP_OK)
    {
        printf("[rtmp] MPP_ENC_SET_HEADER_MODE failed: %d\n", ret);
        return -1;
    }

    // 帧缓冲组
    ret = mpp_buffer_group_get_internal(&mpp_frm_grp_, MPP_BUFFER_TYPE_ION);
    if (ret != MPP_OK)
    {
        printf("[rtmp] mpp_buffer_group_get_internal failed: %d\n", ret);
        return -1;
    }

    // 预分配 MppFrame + MppBuffer, 跨帧复用 (避免每帧 mpp_buffer_get/mpp_frame_deinit
    // 导致 buffer 被 mpp_frame_deinit 内部 mpp_buffer_put 释放, 编码器还在用就 double free)
    enc_buf_size_ = (size_t)enc_width_ * enc_height_ * 3 / 2;
    ret = mpp_buffer_get(mpp_frm_grp_, &enc_buf_, enc_buf_size_);
    if (ret != MPP_OK || !enc_buf_)
    {
        printf("[rtmp] mpp_buffer_get(enc_buf) failed: %d\n", ret);
        return -1;
    }

    mpp_frame_init(&enc_frame_);
    mpp_frame_set_width(enc_frame_, enc_width_);
    mpp_frame_set_height(enc_frame_, enc_height_);
    mpp_frame_set_hor_stride(enc_frame_, enc_width_);
    mpp_frame_set_ver_stride(enc_frame_, enc_height_);
    mpp_frame_set_fmt(enc_frame_, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(enc_frame_, enc_buf_);

    mpp_inited_ = true;

    // 预导入 MPP DMA buffer 到 RGA (缓存 handle, 避免每帧 importbuffer_fd)
    int dst_fd = mpp_buffer_get_fd(enc_buf_);
    if (dst_fd >= 0)
    {
        rga_dst_handle_ = importbuffer_fd(dst_fd, enc_width_, enc_height_,
                                          RK_FORMAT_YCbCr_420_SP);
        if (rga_dst_handle_ == 0)
            printf("[rtmp] RGA importbuffer_fd(enc_buf) failed, will use CPU for RGB→NV12\n");
        else
            printf("[rtmp] RGA acceleration enabled (RGB→NV12 hardware)\n");
    }

    printf("[rtmp] MPP encoder inited: %dx%d@%dfps %dkbps CBR\n",
           enc_width_, enc_height_, enc_fps_, enc_bitrate_);
    return 0;
}

/**
 * @brief 释放 MPP 编码器资源：RGA handle、帧缓冲、编码配置、buffer 组、编码器实例
 */
void RtmpSink::deinit_mpp()
{
    // 释放 RGA 缓存 handle
    if (rga_dst_handle_ != 0)
    {
        releasebuffer_handle(rga_dst_handle_);
        rga_dst_handle_ = 0;
    }

    // 先释放预分配的帧 (内部会 mpp_buffer_put, 但 enc_buf_ 有自己的引用)
    if (enc_frame_)
    {
        mpp_frame_deinit(&enc_frame_);
        enc_frame_ = nullptr;
    }
    if (enc_buf_)
    {
        mpp_buffer_put(enc_buf_);
        enc_buf_ = nullptr;
    }
    if (mpp_enc_cfg_)
    {
        mpp_enc_cfg_deinit(mpp_enc_cfg_);
        mpp_enc_cfg_ = nullptr;
    }
    if (mpp_frm_grp_)
    {
        mpp_buffer_group_put(mpp_frm_grp_);
        mpp_frm_grp_ = nullptr;
    }
    if (mpp_ctx_)
    {
        mpp_destroy(mpp_ctx_);
        mpp_ctx_ = nullptr;
        mpp_api_ = nullptr;
    }
    mpp_inited_ = false;
    sps_pps_sent_ = false;
}

/**
 * @brief 发送 RTMP 包并安全清理，防止 m_vecChannelsOut 副本导致 double-free
 * @param packet 待发送的 RTMPPacket 指针（函数内释放）
 * @return 0成功，-1发送失败
 */
int RtmpSink::send_rtmp_packet(RTMPPacket *packet)
{
    if (!RTMP_SendPacket((RTMP *)rtmp_, packet, FALSE))
    {
        RTMPPacket_Free(packet);
        return -1;
    }
    // RTMP_SendPacket 在 m_vecChannelsOut[channel] 存了 packet 副本 (含 m_body 指针),
    // RTMP_Close 时可能通过副本释放 m_body. 为防止 double-free, 先置 NULL.
    RTMP *r = (RTMP *)rtmp_;
    if (r->m_vecChannelsOut && packet->m_nChannel < (uint32_t)r->m_channelsAllocatedOut
        && r->m_vecChannelsOut[packet->m_nChannel])
    {
        r->m_vecChannelsOut[packet->m_nChannel]->m_body = nullptr;
    }
    RTMPPacket_Free(packet);
    return 0;
}

/**
 * @brief 解析 H.264 码流中的 NAL 单元，构造 RTMP 包并发送
 *        SPS+PPS 合并为 AVCDecoderConfigurationRecord，IDR/P 帧用 AVCC 格式
 * @param data H.264 码流数据（含起始码 00 00 00 01 / 00 00 01）
 * @param len 码流数据长度
 * @param pts_ms 时间戳（毫秒）
 * @return 0成功，-1发送失败
 */
int RtmpSink::send_h264_nalus(const unsigned char *data, size_t len, int64_t pts_ms)
{
    if (!rtmp_) return -1;

    // --- 第一遍: 解析所有 NAL, 收集 SPS/PPS 和视频帧 ---
    const unsigned char *p = data;
    const unsigned char *end = data + len;

    const unsigned char *sps_ptr = nullptr; size_t sps_len = 0;
    const unsigned char *pps_ptr = nullptr; size_t pps_len = 0;

    struct VclNal { const unsigned char *ptr; size_t len; int type; };
    VclNal vcl_nals[64];
    int vcl_count = 0;

    while (p + 4 <= end)
    {
        // 找起始码 (00 00 01 或 00 00 00 01)
        const unsigned char *nal_start = nullptr;
        const unsigned char *q = p;
        while (q + 3 <= end)
        {
            if (q[0] == 0 && q[1] == 0)
            {
                if (q[2] == 1) { nal_start = q + 3; break; }
                if (q + 3 < end && q[2] == 0 && q[3] == 1) { nal_start = q + 4; break; }
            }
            q++;
        }
        if (!nal_start) break;

        // 找下一个起始码
        const unsigned char *nal_end = end;
        const unsigned char *s = nal_start;
        while (s + 3 <= end)
        {
            if (s[0] == 0 && s[1] == 0)
            {
                if (s[2] == 1) { nal_end = s; break; }
                if (s + 3 < end && s[2] == 0 && s[3] == 1) { nal_end = s; break; }
            }
            s++;
        }

        size_t nal_len = nal_end - nal_start;
        if (nal_len == 0) { p = nal_end; continue; }

        int nal_type = nal_start[0] & 0x1F;

        if (nal_type == 7 && !sps_ptr) { sps_ptr = nal_start; sps_len = nal_len; }
        else if (nal_type == 8 && !pps_ptr) { pps_ptr = nal_start; pps_len = nal_len; }
        else if ((nal_type == 5 || nal_type == 1) && vcl_count < 64)
        {
            vcl_nals[vcl_count++] = {nal_start, nal_len, nal_type};
        }

        p = nal_end;
    }

    // --- 发送 AVC sequence header (SPS + PPS 合并) ---
    if (sps_ptr && pps_ptr)
    {
        // AVCDecoderConfigurationRecord 格式:
        //   [0-4] 5B RTMP video header (0x17, 0x00, 0x00, 0x00, 0x00)
        //   [5] 1B configurationVersion
        //   [6-8] 3B profile/compat/level
        //   [9] 1B 0xFF (reserved + lengthSizeMinusOne)
        //   [10] 1B 0xE1 (reserved + numSPS)
        //   [11-12] 2B SPS length
        //   [13..13+sps_len-1] SPS data
        //   [13+sps_len] 1B numPPS
        //   [13+sps_len+1..13+sps_len+2] 2B PPS length
        //   [13+sps_len+3..] PPS data
        size_t body_size = 5 + 1 + 3 + 1 + 1 + 2 + sps_len + 1 + 2 + pps_len;

        RTMPPacket packet;
        RTMPPacket_Reset(&packet);
        packet.m_packetType = RTMP_PACKET_TYPE_VIDEO;
        packet.m_nBodySize = body_size;
        if (!RTMPPacket_Alloc(&packet, body_size))
        {
            return -1;
        }

        unsigned char *body = (unsigned char *)packet.m_body;
        body[0] = 0x17;  // keyframe + AVC
        body[1] = 0x00;  // AVC sequence header
        body[2] = 0x00; body[3] = 0x00; body[4] = 0x00;  // composition time
        body[5] = 0x01;  // configurationVersion = 1
        body[6] = sps_ptr[1];  // AVCProfileIndication
        body[7] = sps_ptr[2];  // profile_compatibility
        body[8] = sps_ptr[3];  // AVCLevelIndication
        body[9] = 0xFF;        // 6-bit reserved + lengthSizeMinusOne=3
        body[10] = 0xE1;       // 3-bit reserved + numSPS=1
        body[11] = (sps_len >> 8) & 0xFF;
        body[12] = sps_len & 0xFF;
        memcpy(body + 13, sps_ptr, sps_len);
        size_t off = 13 + sps_len;
        body[off] = 0x01;  // numPPS = 1
        body[off + 1] = (pps_len >> 8) & 0xFF;
        body[off + 2] = pps_len & 0xFF;
        memcpy(body + off + 3, pps_ptr, pps_len);

        packet.m_nChannel = 0x04;
        packet.m_nTimeStamp = 0;
        packet.m_hasAbsTimestamp = TRUE;
        packet.m_headerType = RTMP_PACKET_SIZE_LARGE;
        packet.m_nInfoField2 = ((RTMP *)rtmp_)->m_stream_id;

        if (send_rtmp_packet(&packet) != 0)
        {
            printf("[rtmp] send AVC sequence header failed\n");
            return -1;
        }
        sps_pps_sent_ = true;
    }

    // --- 发送 IDR/P 帧 (AVCC 格式: 4字节大端NAL长度 + NAL数据) ---
    for (int i = 0; i < vcl_count; i++)
    {
        const unsigned char *nal_ptr = vcl_nals[i].ptr;
        size_t nal_len = vcl_nals[i].len;
        int nal_type = vcl_nals[i].type;

        size_t body_size = 5 + 4 + nal_len;

        RTMPPacket packet;
        RTMPPacket_Reset(&packet);
        packet.m_packetType = RTMP_PACKET_TYPE_VIDEO;
        packet.m_nBodySize = body_size;
        if (!RTMPPacket_Alloc(&packet, body_size))
        {
            continue;
        }

        unsigned char *body = (unsigned char *)packet.m_body;
        body[0] = (nal_type == 5) ? 0x17 : 0x27;
        body[1] = 0x01;  // AVC NALU
        body[2] = 0x00; body[3] = 0x00; body[4] = 0x00;  // CTS offset
        body[5] = (nal_len >> 24) & 0xFF;
        body[6] = (nal_len >> 16) & 0xFF;
        body[7] = (nal_len >> 8)  & 0xFF;
        body[8] = nal_len & 0xFF;
        memcpy(body + 9, nal_ptr, nal_len);

        packet.m_nChannel = 0x04;
        packet.m_nTimeStamp = (uint32_t)pts_ms;
        packet.m_hasAbsTimestamp = TRUE;
        packet.m_headerType = (nal_type == 5) ? RTMP_PACKET_SIZE_LARGE : RTMP_PACKET_SIZE_MEDIUM;
        packet.m_nInfoField2 = ((RTMP *)rtmp_)->m_stream_id;

        if (send_rtmp_packet(&packet) != 0)
        {
            return -1;
        }
    }

    return 0;
}

/**
 * @brief 编码一帧 RGB 图像为 H.264 并通过 RTMP 发送
 *        流程：RGB→NV12 → MPP 编码 → 解析 NAL → RTMP 发送
 * @param rgb 输入 RGB cv::Mat（CV_8UC3）
 * @return 0成功，-1编码或发送失败
 */
int RtmpSink::encode_and_send(const cv::Mat &rgb)
{
    if (!mpp_inited_ || !rtmp_) return -1;

    int src_w = rgb.cols;
    int src_h = rgb.rows;

    // RGB → NV12: 优先 RGA 硬件加速, 失败时回退 CPU
    if (!try_rga_rgb_to_nv12(rgb))
    {
        // CPU 回退: 直接写入 MPP DMA 缓冲
        void *frm_ptr = mpp_buffer_get_ptr(enc_buf_);
        if (!frm_ptr)
        {
            printf("[rtmp] mpp_buffer_get_ptr(enc_buf) failed\n");
            return -1;
        }
        rgb_to_nv12(rgb.data, src_w, src_h, (unsigned char *)frm_ptr, enc_width_, enc_height_);
    }

    // 更新帧时间戳 (其他参数在 init_mpp 中已设置, 不变)
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    int64_t pts_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    mpp_frame_set_pts(enc_frame_, pts_ms);

    // 喂帧 (使用预分配的 enc_frame_, 不调 mpp_frame_deinit)
    MPP_RET ret = mpp_api_->encode_put_frame(mpp_ctx_, enc_frame_);
    if (ret != MPP_OK)
    {
        printf("[rtmp] encode_put_frame failed: %d\n", ret);
        return -1;
    }

    // 取编码结果
    MppPacket pkt = nullptr;
    ret = mpp_api_->encode_get_packet(mpp_ctx_, &pkt);
    if (ret != MPP_OK || !pkt)
    {
        return 0;  // 可能还没编码完, 不算错
    }

    const unsigned char *pkt_data = (const unsigned char *)mpp_packet_get_data(pkt);
    size_t pkt_len = mpp_packet_get_length(pkt);
    int64_t pkt_pts = mpp_packet_get_pts(pkt);

    if (pkt_data && pkt_len > 0)
    {
        // EACH_IDR 模式下, SPS/PPS 会嵌入每个 IDR 帧的码流,
        // send_h264_nalus 自动从中提取并构造 AVC sequence header
        if (send_h264_nalus(pkt_data, pkt_len, pkt_pts) != 0)
        {
            mpp_packet_deinit(&pkt);
            printf("[rtmp] send failed (RTMP connection lost?)\n");
            return -1;
        }
    }

    mpp_packet_deinit(&pkt);
    return 0;
}

/**
 * @brief 异步提交一帧 RGB 图像供 sender 线程编码发送
 * @param rgb_frame RGB cv::Mat（CV_8UC3），调用后与 pending_frame_ 交换，调用方可继续复用
 * @param quality 未使用（保持与 MjpegHttpSink 接口一致）
 */
void RtmpSink::submit(cv::Mat &rgb_frame, int quality)
{
    (void)quality;
    if (rgb_frame.empty() || !running_.load()) return;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        std::swap(rgb_frame, pending_frame_);
    }
    pending_cv_.notify_one();
}

/**
 * @brief sender 线程入口静态函数，调用 sender_loop()
 * @param arg RtmpSink 对象指针
 * @return nullptr
 */
void *RtmpSink::sender_thread_fn(void *arg)
{
    RtmpSink *self = (RtmpSink *)arg;
    self->sender_loop();
    return nullptr;
}

/**
 * @brief sender 线程主循环：等待新帧 → swap 拿走 → encode_and_send 编码发送
 *        编码/发送失败时置 send_failed_ 标志并退出（不在线程内调 close() 防死锁）
 */
void RtmpSink::sender_loop()
{
    while (running_.load())
    {
        cv::Mat frame;

        {
            std::unique_lock<std::mutex> lock(pending_mutex_);
            pending_cv_.wait(lock, [this] {
                return !running_.load() || !pending_frame_.empty();
            });
            if (!running_.load() && pending_frame_.empty()) break;
            std::swap(pending_frame_, frame);
        }

        if (!frame.empty())
        {
            if (encode_and_send(frame) != 0)
            {
                printf("[rtmp] encode/send failed, stopping sender\n");
                send_failed_.store(true);
                // 不能在 sender 线程内调 close() (会 pthread_join 死锁)
                // 只退出循环, 让 main 线程的 close() 负责清理
                break;
            }
        }
    }
    pending_frame_.release();
}

/**
 * @brief 打开 RTMP 推流通道：初始化 MPP 编码器 → 连接 RTMP 服务器 → 启动 sender 线程
 * @param rtmp_url RTMP 推流地址（如 rtmp://127.0.0.1:1936/live/stream）
 * @param width 编码宽度，0 使用默认值 640
 * @param height 编码高度，0 使用默认值 480
 * @param fps 编码帧率，0 使用默认值 30
 * @param bitrate_kbps 目标码率（kbps），0 使用默认值 1000
 * @return 0成功，-1失败
 */
int RtmpSink::open(const char *rtmp_url, int width, int height, int fps, int bitrate_kbps)
{
    // 1. 初始化 MPP 编码器
    if (init_mpp(width, height, fps, bitrate_kbps) != 0)
    {
        printf("[rtmp] MPP init failed\n");
        return -1;
    }

    // 2. 连接 RTMP 服务器
    rtmp_ = RTMP_Alloc();
    if (!rtmp_)
    {
        printf("[rtmp] RTMP_Alloc failed\n");
        deinit_mpp();
        return -1;
    }

    RTMP_Init((RTMP *)rtmp_);
    RTMP_LogSetLevel(RTMP_LOGWARNING);

    if (!RTMP_SetupURL((RTMP *)rtmp_, (char *)rtmp_url))
    {
        printf("[rtmp] RTMP_SetupURL(%s) failed\n", rtmp_url);
        RTMP_Free((RTMP *)rtmp_);
        rtmp_ = nullptr;
        deinit_mpp();
        return -1;
    }

    RTMP_EnableWrite((RTMP *)rtmp_);

    // 超时 5s
    ((RTMP *)rtmp_)->Link.timeout = 5;

    if (!RTMP_Connect((RTMP *)rtmp_, nullptr))
    {
        printf("[rtmp] RTMP_Connect failed: %s\n", rtmp_url);
        RTMP_Free((RTMP *)rtmp_);
        rtmp_ = nullptr;
        deinit_mpp();
        return -1;
    }

    if (!RTMP_ConnectStream((RTMP *)rtmp_, 0))
    {
        printf("[rtmp] RTMP_ConnectStream failed\n");
        RTMP_Close((RTMP *)rtmp_);
        RTMP_Free((RTMP *)rtmp_);
        rtmp_ = nullptr;
        deinit_mpp();
        return -1;
    }

    // 3. SPS/PPS 在首个 IDR 帧码流中由 EACH_IDR 模式自动携带,
    //    send_h264_nalus 会从中提取并构造 AVC sequence header

    // 4. 启动 sender 线程
    running_.store(true);
    rtmp_url_ = rtmp_url;

    if (pthread_create(&sender_thread_, nullptr, sender_thread_fn, this) != 0)
    {
        printf("[rtmp] pthread_create(sender) failed\n");
        running_.store(false);
        RTMP_Close((RTMP *)rtmp_);
        RTMP_Free((RTMP *)rtmp_);
        rtmp_ = nullptr;
        deinit_mpp();
        return -1;
    }
    sender_thread_started_ = true;

    printf("[rtmp] connected to %s, H.264 %dx%d@%dfps %dkbps\n",
           rtmp_url, enc_width_, enc_height_, enc_fps_, enc_bitrate_);
    return 0;
}

/**
 * @brief 关闭 RTMP 推流通道：停止 sender 线程 → 断开 RTMP 连接 → 释放 MPP 编码器资源
 */
void RtmpSink::close()
{
    running_.store(false);

    // 唤醒 sender 线程
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
    }
    pending_cv_.notify_all();

    if (sender_thread_started_)
    {
        pthread_join(sender_thread_, nullptr);
        sender_thread_started_ = false;
    }

    // 断开 RTMP
    if (rtmp_)
    {
        RTMP_Close((RTMP *)rtmp_);
        RTMP_Free((RTMP *)rtmp_);
        rtmp_ = nullptr;
    }

    deinit_mpp();
}
