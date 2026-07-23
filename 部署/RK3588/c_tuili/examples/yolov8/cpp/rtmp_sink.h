// RTMP 推流 Sink: MPP 硬件 H.264 编码 + librtmp 推流
// 搭配 mediamtx 等流媒体服务器, 浏览器通过 WebRTC/HTTP-FLV 观看
//
// 数据流: RGB帧 → RGA转NV12 → MPP编码H.264 → librtmp发送
//
// 用法:
//   1. 板子运行 mediamtx
//   2. RtmpSink sink; sink.open("rtmp://127.0.0.1:1935/live/stream", w, h, fps);
//   3. 每帧: sink.submit(rgb_frame);
//   4. 浏览器打开 http://<ip>:8889/stream (mediamtx WebRTC)

#ifndef RTMP_SINK_H
#define RTMP_SINK_H

#include <opencv2/core.hpp>
#include <pthread.h>
#include <mutex>
#include <condition_variable>
#include <atomic>

// Rockchip MPP 前向声明
extern "C" {
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_venc_cfg.h>
#include <rockchip/rk_venc_cmd.h>
#include <rockchip/rk_venc_rc.h>
}

// librtmp 前向声明 (避免在头文件中 include librtmp)
struct RTMPPacket;

class RtmpSink
{
public:
    RtmpSink();
    ~RtmpSink();

    // 初始化: 连接 RTMP 服务器 + 创建 MPP 编码器
    // rtmp_url: 如 "rtmp://127.0.0.1:1935/live/stream"
    // width/height/fps: 编码参数
    // bitrate_kbps: 目标码率 (kbps), 0=自动 (默认 1000)
    int open(const char *rtmp_url, int width, int height, int fps, int bitrate_kbps = 1000);
    void close();
    bool isOpened() const { return running_.load(); }
    // RTMP 发送是否已失败 (连接断开), 主循环可据此退出
    bool sendFailed() const { return send_failed_.load(); }

    // 异步提交一帧 RGB 图像 (与 MjpegHttpSink 接口一致)
    void submit(cv::Mat &rgb_frame, int quality = 80 /* unused, 保留接口兼容 */);

private:
    // ==================== MPP 编码器 ====================
    MppCtx       mpp_ctx_        = nullptr;
    MppApi      *mpp_api_        = nullptr;
    MppEncCfg    mpp_enc_cfg_    = nullptr;
    MppBufferGroup mpp_frm_grp_ = nullptr;
    MppFrame     enc_frame_      = nullptr;  // 预分配帧, 跨帧复用
    MppBuffer    enc_buf_        = nullptr;  // 预分配 NV12 缓冲, 跨帧复用
    int          enc_width_      = 0;
    int          enc_height_     = 0;
    int          enc_fps_        = 0;
    int          enc_bitrate_    = 0;
    bool         mpp_inited_     = false;
    bool         sps_pps_sent_   = false;  // 是否已发送 SPS/PPS

    size_t       enc_buf_size_   = 0;      // NV12 DMA 缓冲大小

    // ==================== RTMP ====================
    void        *rtmp_           = nullptr;  // RTMP *
    std::string  rtmp_url_;

    // ==================== 异步推流 (生产者-消费者) ====================
    cv::Mat          pending_frame_;
    std::mutex       pending_mutex_;
    std::condition_variable pending_cv_;
    pthread_t        sender_thread_;
    bool             sender_thread_started_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> send_failed_{false};

    static void *sender_thread_fn(void *arg);
    void sender_loop();
    int  encode_and_send(const cv::Mat &rgb);
    int  send_h264_nalus(const unsigned char *data, size_t len, int64_t pts_ms);
    int  send_rtmp_packet(RTMPPacket *packet);

    // ==================== MPP 内部 ====================
    int  init_mpp(int width, int height, int fps, int bitrate_kbps);
    void deinit_mpp();
    int  rgb_to_nv12(const unsigned char *rgb, int src_w, int src_h,
                     unsigned char *nv12, int enc_w, int enc_h);
    bool try_rga_rgb_to_nv12(const cv::Mat &rgb);  // RGA 硬件加速, 失败返回 false

    // ==================== RGA 缓存 ====================
    uint32_t rga_dst_handle_ = 0;   // 预导入 MPP DMA buffer 的 RGA handle (0=未导入)
};

#endif
