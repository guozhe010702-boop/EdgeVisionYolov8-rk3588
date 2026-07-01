// MJPEG-over-HTTP 推流 Sink
// 输出格式完全兼容 mjpg_streamer 的 output_http.so:
//   HTTP/1.0 200 OK
//   Content-Type: multipart/x-mixed-replace;boundary=mjpegboundary
//   --mjpegboundary
//   Content-Type: image/jpeg
//   Content-Length: <size>
//   <jpeg bytes>
//
// 浏览器直接打开 http://board:port/ 即可看到实时画面
//
// 性能优化:
//   - 异步推流: 检测线程 submit() 只换出 Mat 句柄就返回; 编码/发送在独立 sender 线程
//   - JPEG 缓冲复用: TJFLAG_NOREALLOC + 预分配 tj_buf_, 省每帧 malloc
//   - 单次 writev() 发送 head + jpeg + \r\n, 3 次 send() 合 1 次系统调用
//   - tjCompress2 使用 TJPF_RGB, 与 V4L2 直出 RGB 对齐, 省一次 RGB→BGR 转换

#ifndef MJPEG_HTTP_SINK_H
#define MJPEG_HTTP_SINK_H

#include <opencv2/core.hpp>
#include <pthread.h>
#include <vector>
#include <mutex>
#include <map>
#include <atomic>
#include <condition_variable>

class MjpegHttpSink
{
public:
    MjpegHttpSink();
    ~MjpegHttpSink();

    // 启动 HTTP 服务监听指定端口
    int start(int port);
    // 停止服务
    void stop();
    bool isRunning() const { return listen_fd_ >= 0; }
    int clientCount();

    // 异步提交一帧 RGB 图像. submit() 通过 swap 接管传入 Mat 的数据,
    // 调用方下一帧可继续复用同一 Mat, 不会因发送缓慢而阻塞检测循环
    void submit(cv::Mat &rgb_frame, int quality = 80);

private:
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_flag_{false};
    pthread_t accept_thread_;
    bool thread_started_ = false;

    // ==================== 客户端管理 ====================
    mutable std::mutex client_mutex_;
    std::vector<int> client_fds_;
    // 每个客户端连续"慢跳过"的帧数; 超过阈值才踢掉
    std::map<int, int> client_skip_;
    static constexpr int MAX_SKIP_FRAMES = 30;  // 约 1s @30fps

    // 跟踪已 spawn 但还没退出的客户端线程, 防止 push_sink 析构后
    // 还有线程在访问 client_fds_ / client_skip_ (use-after-free)
    std::atomic<int> active_clients_{0};
    std::mutex clients_done_mutex_;
    std::condition_variable clients_done_cv_;

    // ==================== JPEG 编码 (单线程, sender 专用) ====================
    void *tj_ = nullptr;
    // TJFLAG_NOREALLOC 要求 buffer 足够大; 分配到 W*H*2 兜底 (640x480 RGB 压缩后远小于此)
    unsigned char *tj_buf_ = nullptr;
    unsigned long tj_buf_size_ = 0;

    // ==================== 异步推流 (生产者-消费者) ====================
    // 检测线程 swap 一帧进 pending_frame_; sender 线程 swap 出来编码
    // cv::Mat 的 swap 是浅操作, 不拷贝像素, 零成本
    cv::Mat  pending_frame_;
    int      pending_quality_ = 80;
    std::mutex           pending_mutex_;
    std::condition_variable pending_cv_;
    pthread_t sender_thread_;
    bool sender_thread_started_ = false;

    static void *accept_thread_fn(void *arg);
    static void *client_thread_fn(void *arg);
    static void *sender_thread_fn(void *arg);
    void accept_loop();
    void handle_client(int fd);
    void sender_loop();
    // 编码一帧并广播给所有 client; 返回 0=ok, 1=无客户端, -1=编码失败
    int  encode_and_broadcast(const cv::Mat &rgb, int quality);
    // 0=ok, 1=EAGAIN(客户端慢), -1=err(对端断开)
    int  send_frame(int fd, const void *head, size_t hlen,
                    const void *body, size_t blen);
};

#endif
