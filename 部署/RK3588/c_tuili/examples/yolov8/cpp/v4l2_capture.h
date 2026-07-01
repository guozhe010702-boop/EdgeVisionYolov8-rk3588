// 直接 V4L2 摄像头采集, 绕过 OpenCV 的 videoio 和系统的 libv4l2
// 支持 MJPG (用 libturbojpeg 解码) 和 YUYV (手动转 BGR)
#ifndef V4L2_CAPTURE_H
#define V4L2_CAPTURE_H

#include <opencv2/core.hpp>
#include <string>

class V4L2Capture
{
public:
    V4L2Capture();
    ~V4L2Capture();

    // 打开 /dev/videoX, 优先 MJPG, 失败再试 YUYV
    int open(const char *dev, int req_w = 640, int req_h = 480, int req_fps = 30);
    void close();
    bool isOpened() const { return fd_ >= 0; }

    // 拉一帧, 0=成功, -1=失败
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
    int bytesperline_ = 0;  // YUYV 实际每行字节数 (可能 > width_*2)
    char format_name_[16] = {0};
    unsigned int pixfmt_ = 0;

    struct buffer { void *start; size_t length; };
    buffer *buffers_ = nullptr;
    int n_buffers_ = 0;

    // libturbojpeg (MJPG 解码)
    void *tj_ = nullptr;
    unsigned char *tj_buf_ = nullptr;
    size_t tj_buf_size_ = 0;

    int try_format(unsigned int pixfmt, const char *name, int w, int h, int fps);
    int init_mmap();
    int start_streaming();
    int stop_streaming();
    void decode_mjpg(const void *data, size_t len, cv::Mat &out);
    void convert_yuyv_to_bgr(const void *data, size_t len, cv::Mat &out);
};

#endif
