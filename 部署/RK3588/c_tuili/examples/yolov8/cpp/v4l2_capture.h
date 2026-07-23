// 直接 V4L2 摄像头采集, 绕过 OpenCV 的 videoio 和系统的 libv4l2
// MJPG 用 libturbojpeg 解码, YUYV/UYVY/NV12 用 OpenCV cvtColor 转 RGB
// 同时支持 Single-planar (USB 摄像头) 和 Multi-planar (MIPI RKISP 摄像头)
#ifndef V4L2_CAPTURE_H
#define V4L2_CAPTURE_H

#include <opencv2/core.hpp>
#include <string>

class V4L2Capture
{
public:
    V4L2Capture();
    ~V4L2Capture();

    // 打开 /dev/videoX, 优先 MJPG, 再试 NV12, YUYV, UYVY
    // 自动检测 Single-planar / Multi-planar 设备
    int open(const char *dev, int req_w = 640, int req_h = 480, int req_fps = 30);
    void close();
    bool isOpened() const { return fd_ >= 0; }

    // 拉一帧, 输出 RGB Mat, 0=成功, -1=失败
    int read(cv::Mat &frame);

    int width()  const { return width_; }
    int height() const { return height_; }
    double fps() const { return fps_; }
    const char *formatName() const { return format_name_; }

private:
    int fd_ = -1;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 0;
    int bytesperline_ = 0;          // Y / 亮度平面行 stride
    char format_name_[16] = {0};
    unsigned int pixfmt_ = 0;
    bool multiplanar_ = false;      // Multi-planar 设备 (MIPI RKISP)
    int n_planes_ = 0;              // Multi-planar 的平面数 (1 或 2)

    struct buffer {
        void *start[2] = {nullptr, nullptr};   // mmap 指针 (最多 2 个平面)
        size_t length[2] = {0, 0};             // mmap 长度
    };
    buffer *buffers_ = nullptr;
    int n_buffers_ = 0;

    // libturbojpeg (MJPG 解码)
    void *tj_ = nullptr;

    int try_format(unsigned int pixfmt, const char *name, int w, int h, int fps);
    int init_mmap();
    int start_streaming();
    int stop_streaming();
    void decode_mjpg(const void *data, size_t len, cv::Mat &out);
};

#endif
