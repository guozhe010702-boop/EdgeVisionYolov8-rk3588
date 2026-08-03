/*
================================================================================
【文件总说明】
文件：mjpeg_http_sink.cc
功能：MJPEG-over-HTTP 推流服务器实现，将检测画面以 MJPEG 流形式推送到浏览器
用途：提供 MjpegHttpSink 类，支持多客户端同时观看、异步编码发送

【整体工作流程】
1. start(port)：创建监听 socket，启动 accept 线程和 sender 线程
2. accept 线程循环：
   - accept() 等待客户端连接
   - 每个连接创建 detached 客户端线程处理 HTTP 握手
   - 客户端线程：recv HTTP GET → 发送 200 + multipart 头 → 加入 client_fds_ 列表
3. 检测线程调用 submit(rgb_frame, quality)：
   - 与 pending_frame_ 做 std::swap（O(1) 浅拷贝），不阻塞检测线程
   - notify 唤醒 sender 线程
4. sender 线程循环：
   - 等待 pending_cv_ 唤醒，swap 拿走待发帧
   - encode_and_broadcast()：tjCompress2 编码 JPEG + writev 广播到所有客户端
   - 慢客户端跳帧（EAGAIN/短写），超限则断开
5. stop()：关监听 fd → join accept 线程 → 等客户端线程退出 → join sender 线程 → 清理

关键技术点：
- 异步 producer-consumer：检测线程 swap 即返回，编码/网络在独立 sender 线程
- libturbojpeg tjCompress2 + TJFLAG_NOREALLOC + 预分配 buffer，避免每帧 malloc/free
- writev() 合并 multipart header + JPEG + boundary 为一次系统调用
- 客户端 fd 设 O_NONBLOCK，防止慢客户端阻塞 sender 线程
- 无人观看时 submit() 直接返回，不做 swap 和 cv 唤醒
================================================================================
*/

#include "mjpeg_http_sink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

extern "C" {
#include <turbojpeg.h>
}

#define BOUNDARY "mjpegboundary"

// 头模板: Content-Length 用 %lu 占位, sender 每帧 snprintf 一次
// 单次分配到栈, 避免 snprintf 多次
static const char *FRAME_HEADER_FMT =
    "--" BOUNDARY "\r\n"
    "Content-Type: image/jpeg\r\n"
    "Content-Length: %lu\r\n"
    "\r\n";

// =====================================================================
// 构造 / 析构
// =====================================================================
/**
 * @brief 构造函数，成员初始化为默认值
 */
MjpegHttpSink::MjpegHttpSink() {}

/**
 * @brief 析构函数，停止推流并释放 turbojpeg 资源
 */
MjpegHttpSink::~MjpegHttpSink() { stop(); if (tj_) tjDestroy((tjhandle)tj_); tj_ = nullptr; if (tj_buf_) free(tj_buf_); tj_buf_ = nullptr; tj_buf_size_ = 0; }

/**
 * @brief 获取当前连接的客户端数量
 * @return 客户端数量
 */
int MjpegHttpSink::clientCount()
{
    std::lock_guard<std::mutex> lock(client_mutex_);
    return (int)client_fds_.size();
}

// =====================================================================
// 异步提交: 检测线程只做一次 Mat 句柄 swap 就返回, 编码/发送在 sender 线程
// =====================================================================
/**
 * @brief 异步提交一帧 RGB 图像供 sender 线程编码发送
 * @param rgb_frame RGB cv::Mat（CV_8UC3），调用后与 pending_frame_ 交换，调用方可继续复用
 * @param quality JPEG 编码质量（1-100），推荐 80
 */
void MjpegHttpSink::submit(cv::Mat &rgb_frame, int quality)
{
    if (rgb_frame.empty()) return;

    // 修正 (P1-3): 无人观看时不要做任何工作
    // 节省: 1 次 mutex lock + 1 次 std::swap + sender 线程 1 次 cv wakeup + 1 次 mutex lock
    if (clientCount() == 0) return;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        // cv::Mat::swap 是浅操作, 只交换数据指针和 refcount, 不拷贝像素
        // 之后 rgb_frame 持有的就是上次 swap 留下的"空壳", 调用方可继续复用
        // 注: OpenCV 3.x 没有 cv::Mat::swap 成员, 用 std::swap
        std::swap(rgb_frame, pending_frame_);
        pending_quality_ = quality;
    }
    pending_cv_.notify_one();
}

/**
 * @brief sender 线程入口静态函数，调用 sender_loop()
 * @param arg MjpegHttpSink 对象指针
 * @return nullptr
 */
void *MjpegHttpSink::sender_thread_fn(void *arg)
{
    MjpegHttpSink *self = (MjpegHttpSink *)arg;
    self->sender_loop();
    return nullptr;
}

/**
 * @brief sender 线程主循环：等待新帧 → swap 拿走 → encode_and_broadcast 编码广播
 *        stop_flag_ 置位且无待发帧时退出
 */
void MjpegHttpSink::sender_loop()
{
    while (true)
    {
        cv::Mat frame;
        int quality = 80;

        {
            std::unique_lock<std::mutex> lock(pending_mutex_);
            pending_cv_.wait(lock, [this] {
                return stop_flag_.load() || !pending_frame_.empty();
            });
            if (stop_flag_.load() && pending_frame_.empty()) break;
            // 再 swap 一次, 拿走的就属于 sender 独占, 检测线程下一帧可以安全写
            std::swap(pending_frame_, frame);
            quality = pending_quality_;
        }

        if (!frame.empty())
        {
            encode_and_broadcast(frame, quality);
        }
    }
    // 收尾: 退出时还有一帧没发就丢掉, 反正 destroy 之后客户端都得断
    pending_frame_.release();
}

/**
 * @brief 将 RGB 帧编码为 JPEG 并广播到所有已连接的客户端
 * @param rgb RGB cv::Mat（CV_8UC3）
 * @param quality JPEG 编码质量（1-100）
 * @return 0成功，-1编码失败，1无客户端跳过
 */
int MjpegHttpSink::encode_and_broadcast(const cv::Mat &rgb, int quality)
{
    if (rgb.channels() != 3 || rgb.depth() != CV_8U) return -1;

    int n = clientCount();
    if (n == 0) return 1;  // 无客户端, 跳过编码

    // 懒初始化 turbojpeg 句柄 + 预分配输出 buffer
    if (!tj_) tj_ = tjInitCompress();
    if (!tj_)
    {
        printf("[http] tjInitCompress failed\n");
        return -1;
    }
    int w = rgb.cols;
    int h = rgb.rows;
    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;

    // TJFLAG_NOREALLOC: 复用 tj_buf_, 不再每帧 malloc/free
    // buffer 容量取 W*H*2 兜底 (RGB888 压缩后一般 < 100KB)
    unsigned long need = (unsigned long)w * (unsigned long)h * 2;
    if (need > tj_buf_size_)
    {
        if (tj_buf_) free(tj_buf_);
        tj_buf_ = (unsigned char *)malloc(need);
        if (!tj_buf_) { tj_buf_size_ = 0; return -1; }
        tj_buf_size_ = need;
    }

    unsigned long jpeg_size = 0;
    int tj_ret = tjCompress2((tjhandle)tj_,
                             rgb.data, w, 0, h, TJPF_RGB,
                             &tj_buf_, &jpeg_size,
                             TJSAMP_420, quality,
                             TJFLAG_FASTDCT | TJFLAG_NOREALLOC);
    if (tj_ret != 0)
    {
        printf("[http] tjCompress2 failed: %s\n", tjGetErrorStr());
        return -1;
    }

    // 拼 boundary 头 (栈上, 短字符串)
    char head[256];
    int hlen = snprintf(head, sizeof(head), FRAME_HEADER_FMT, jpeg_size);
    if (hlen <= 0 || hlen >= (int)sizeof(head)) return -1;

    // 广播: 一次 writev 替代 head + jpeg + "\r\n" 三次 send
    std::lock_guard<std::mutex> lock(client_mutex_);
    for (auto it = client_fds_.begin(); it != client_fds_.end(); )
    {
        int fd = *it;
        int r = send_frame(fd, head, (size_t)hlen, tj_buf_, jpeg_size);

        if (r == 0)
        {
            client_skip_[fd] = 0;
            ++it;
        }
        else if (r == 1)
        {
            int skips = ++client_skip_[fd];
            if (skips >= MAX_SKIP_FRAMES)
            {
                printf("[http] client %d too slow (skip %d frames), disconnect\n", fd, skips);
                ::close(fd);
                client_skip_.erase(fd);
                it = client_fds_.erase(it);
            }
            else
            {
                ++it;
            }
        }
        else
        {
            printf("[http] client %d disconnected\n", fd);
            ::close(fd);
            client_skip_.erase(fd);
            it = client_fds_.erase(it);
        }
    }
    return 0;
}

/**
 * @brief 单客户端一次 writev 发送（multipart header + JPEG body + boundary tail）
 * @param fd 客户端 socket fd（非阻塞）
 * @param head multipart header 数据
 * @param hlen header 长度
 * @param body JPEG 数据
 * @param blen body 长度
 * @return 0发送成功，1客户端慢（EAGAIN/短写，应跳帧），-1连接错误
 */
int MjpegHttpSink::send_frame(int fd, const void *head, size_t hlen,
                              const void *body, size_t blen)
{
    const char tail[2] = {'\r', '\n'};
    struct iovec iov[3];
    iov[0].iov_base = (void *)head;
    iov[0].iov_len  = hlen;
    iov[1].iov_base = (void *)body;
    iov[1].iov_len  = blen;
    iov[2].iov_base = (void *)tail;
    iov[2].iov_len  = 2;

    // writev 原子性: 一次系统调用, 内核按顺序发, 客户端按顺序收
    // 部分写 (EAGAIN) 视为慢, 整帧直接扔 (MJPEG 流允许丢帧)
    ssize_t n = ::writev(fd, iov, 3);
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
        return -1;
    }
    // writev 全部写完才算成功, 短写视为慢
    if ((size_t)n < hlen + blen + 2) return 1;
    return 0;
}

/**
 * @brief 处理单个客户端的 HTTP 握手：recv GET → 发送 200 multipart 头 → 加入 client_fds_
 * @param fd 客户端 socket fd
 */
void MjpegHttpSink::handle_client(int fd)
{
    // recv 设超时, 避免 stop() 后线程卡在阻塞 recv
    struct timeval tv = {2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 关键: client fd 设非阻塞
    // 否则 sender 线程 writev 在 TCP 发送缓冲满时会永久阻塞,
    // 一个慢客户端会让所有客户端的画面都卡住
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    char req[2048] = {0};
    int r = (int)::recv(fd, req, sizeof(req) - 1, 0);
    if (r <= 0)
    {
        ::close(fd);
        return;
    }

    bool is_get = (strncmp(req, "GET ", 4) == 0);
    if (!is_get)
    {
        const char *resp = "HTTP/1.0 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";
        ::send(fd, resp, strlen(resp), 0);
        ::close(fd);
        return;
    }

    // 1MB 发送缓冲, 缓冲 1~2 秒的 MJPEG 流, 避免偶发网络抖动就把客户端踢掉
    int sndbuf = 1 << 20;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF,   &sndbuf, sizeof(sndbuf));
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one,    sizeof(one));

    char head[512];
    int hlen = snprintf(head, sizeof(head),
        "HTTP/1.0 200 OK\r\n"
        "Server: rknn_yolov8_stream\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Expires: 0\r\n"
        "Connection: close\r\n"
        "Content-Type: multipart/x-mixed-replace;boundary=" BOUNDARY "\r\n"
        "\r\n");
    if (::send(fd, head, (size_t)hlen, 0) < 0)
    {
        ::close(fd);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        if (stop_flag_)
        {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            return;
        }
        client_fds_.push_back(fd);
        client_skip_[fd] = 0;
    }
    printf("[http] client connected, total=%d\n", clientCount());
}

struct ClientThreadArg
{
    MjpegHttpSink *self;
    int fd;
};

/**
 * @brief 客户端线程入口函数，提取参数后调用 handle_client，完成后递减 active_clients_
 * @param arg ClientThreadArg 指针（含 self 和 fd），函数内 delete
 * @return nullptr
 */
void *MjpegHttpSink::client_thread_fn(void *arg)
{
    ClientThreadArg *a = (ClientThreadArg *)arg;
    // 先把要用的成员指针取出来, 再 delete a, 避免 a 释放后再访问 a->self 的 UAF
    MjpegHttpSink *self = a->self;
    int fd = a->fd;
    delete a;

    self->handle_client(fd);

    {
        std::lock_guard<std::mutex> lock(self->clients_done_mutex_);
        self->active_clients_--;
    }
    self->clients_done_cv_.notify_all();
    return nullptr;
}

/**
 * @brief accept 线程主循环：accept 新连接，为每个连接创建 detached 客户端线程
 */
void MjpegHttpSink::accept_loop()
{
    while (!stop_flag_.load())
    {
        sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int fd = ::accept(listen_fd_, (sockaddr *)&cli, &len);
        if (fd < 0)
        {
            if (errno == EINTR) continue;
            if (stop_flag_.load()) break;
            // 任何瞬时错误 (EMFILE=fd 耗尽, ENFILE=系统 fd 耗尽, ECONNABORTED, EPERM 等)
            // 都不应该让 accept_loop 退出 — listen_fd_ 还活着, 一旦退出没人 accept,
            // 后续新连接会在 listen 队列里堆积直到被拒
            // 退避 100ms 后重试, 给系统恢复的窗口
            printf("[http] accept error: %s, retry in 100ms\n", strerror(errno));
            usleep(100 * 1000);
            continue;
        }
        // 每个客户端交给独立 detached 线程处理,
        // 避免 accept_loop 卡在第一个客户端的阻塞 recv 上,
        // 后面排队的连接拿不到服务
        ClientThreadArg *a = new ClientThreadArg{this, fd};
        active_clients_++;
        pthread_t tid;
        if (pthread_create(&tid, nullptr, client_thread_fn, a) != 0)
        {
            printf("[http] pthread_create for client failed\n");
            ::close(fd);
            delete a;
            active_clients_--;
            continue;
        }
        pthread_detach(tid);
    }
}

/**
 * @brief accept 线程入口静态函数，调用 accept_loop()
 * @param arg MjpegHttpSink 对象指针
 * @return nullptr
 */
void *MjpegHttpSink::accept_thread_fn(void *arg)
{
    MjpegHttpSink *self = (MjpegHttpSink *)arg;
    self->accept_loop();
    return nullptr;
}

/**
 * @brief 启动 MJPEG 推流服务器：创建监听 socket、启动 accept 线程和 sender 线程
 * @param port 监听端口号
 * @return 0成功，-1失败（socket/bind/listen/线程创建失败）
 */
int MjpegHttpSink::start(int port)
{
    if (listen_fd_ >= 0)
    {
        printf("[http] already running on port %d\n", port_);
        return 0;
    }

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
    {
        printf("[http] socket() failed: %s\n", strerror(errno));
        return -1;
    }

    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((unsigned short)port);
    if (::bind(listen_fd_, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        printf("[http] bind(:%d) failed: %s\n", port, strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -1;
    }
    if (::listen(listen_fd_, 8) < 0)
    {
        printf("[http] listen() failed: %s\n", strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -1;
    }
    port_ = port;
    stop_flag_.store(false);

    if (pthread_create(&accept_thread_, nullptr, accept_thread_fn, this) != 0)
    {
        printf("[http] pthread_create(accept) failed\n");
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -1;
    }
    thread_started_ = true;

    if (pthread_create(&sender_thread_, nullptr, sender_thread_fn, this) != 0)
    {
        printf("[http] pthread_create(sender) failed\n");
        stop_flag_.store(true);
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
        pthread_join(accept_thread_, nullptr);
        thread_started_ = false;
        return -1;
    }
    sender_thread_started_ = true;

    printf("[http] MJPEG push server listening on 0.0.0.0:%d\n", port);
    printf("[http] open http://<board-ip>:%d/ in browser to view\n", port);
    return 0;
}

/**
 * @brief 停止 MJPEG 推流服务器：关监听 → join accept 线程 → 等客户端退出 → join sender 线程 → 清理 fd
 */
void MjpegHttpSink::stop()
{
    if (listen_fd_ < 0) return;
    stop_flag_.store(true);

    // 关监听 socket, 让 accept() 退出
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;

    if (thread_started_)
    {
        pthread_join(accept_thread_, nullptr);
        thread_started_ = false;
    }

    // 关掉所有已接入的 client, 让 send_frame 立即 EPIPE
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        for (int fd : client_fds_) ::shutdown(fd, SHUT_RDWR);
    }

    // 等所有客户端握手线程退出 (recv 超时 2s)
    {
        std::unique_lock<std::mutex> lock(clients_done_mutex_);
        clients_done_cv_.wait_for(lock, std::chrono::seconds(3),
            [this] { return active_clients_.load() == 0; });
    }
    if (active_clients_.load() > 0)
    {
        printf("[http] WARN: %d client thread(s) still alive after 3s\n", active_clients_.load());
    }

    // 唤醒 sender 线程, 让它走退出路径
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
    }
    pending_cv_.notify_all();
    if (sender_thread_started_)
    {
        pthread_join(sender_thread_, nullptr);
        sender_thread_started_ = false;
    }

    // 最后关 client fd + 清表
    std::lock_guard<std::mutex> lock(client_mutex_);
    for (int fd : client_fds_) ::close(fd);
    client_fds_.clear();
    client_skip_.clear();
    printf("[http] stopped\n");
}
