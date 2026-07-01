// 直接 V4L2 摄像头采集, 绕过 OpenCV videoio 和 libv4l2 兼容层
// MJPG 用 libturbojpeg 解码, YUYV 手动转 BGR

#include "v4l2_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <opencv2/imgproc.hpp>

extern "C" {
#include <turbojpeg.h>
}

#define CLEAR(x) memset(&(x), 0, sizeof(x))

V4L2Capture::V4L2Capture() {}
V4L2Capture::~V4L2Capture() { close(); }

void V4L2Capture::close()
{
    if (fd_ >= 0)
    {
        stop_streaming();
        if (buffers_)
        {
            for (int i = 0; i < n_buffers_; i++)
            {
                if (buffers_[i].start)
                    munmap(buffers_[i].start, buffers_[i].length);
            }
            free(buffers_);
            buffers_ = nullptr;
        }
        ::close(fd_);
        fd_ = -1;
    }
    if (tj_)
    {
        tjDestroy((tjhandle)tj_);
        tj_ = nullptr;
    }
    if (tj_buf_)
    {
        free(tj_buf_);
        tj_buf_ = nullptr;
        tj_buf_size_ = 0;
    }
}

int V4L2Capture::try_format(unsigned int pixfmt, const char *name, int w, int h, int fps)
{
    struct v4l2_format fmt;
    CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = (unsigned)w;
    fmt.fmt.pix.height      = (unsigned)h;
    fmt.fmt.pix.pixelformat = pixfmt;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;

    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0)
    {
        printf("[v4l2] VIDIOC_S_FMT(%s) failed: %s\n", name, strerror(errno));
        return -1;
    }

    // 驱动可能悄悄改成别的格式 (e.g. 走 libv4l2 fallback), 必须按实际格式走
    if (fmt.fmt.pix.pixelformat != pixfmt)
    {
        printf("[v4l2] driver did not accept %s (got 0x%08x), try next\n",
               name, fmt.fmt.pix.pixelformat);
        return -1;
    }

    // 驱动可能调整了分辨率, 用回读的实际值
    width_  = (int)fmt.fmt.pix.width;
    height_ = (int)fmt.fmt.pix.height;
    bytesperline_ = (int)fmt.fmt.pix.bytesperline;
    if (bytesperline_ <= 0) bytesperline_ = width_ * 2;  // 安全兜底
    pixfmt_ = fmt.fmt.pix.pixelformat;
    strncpy(format_name_, name, sizeof(format_name_) - 1);

    // 设帧率
    struct v4l2_streamparm parm;
    CLEAR(parm);
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = (unsigned)fps;
    if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0)
    {
        // 帧率设置失败不致命, 读回实际帧率
    }
    else
    {
        fps_ = (int)parm.parm.capture.timeperframe.denominator /
               (parm.parm.capture.timeperframe.numerator
                ? parm.parm.capture.timeperframe.numerator : 1);
    }
    return 0;
}

int V4L2Capture::init_mmap()
{
    struct v4l2_requestbuffers req;
    CLEAR(req);
    req.count  = 4;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0)
    {
        printf("[v4l2] VIDIOC_REQBUFS failed: %s\n", strerror(errno));
        return -1;
    }
    if (req.count < 2)
    {
        printf("[v4l2] insufficient buffer memory\n");
        return -1;
    }

    n_buffers_ = (int)req.count;
    buffers_ = (buffer *)calloc(n_buffers_, sizeof(buffer));
    if (!buffers_) return -1;

    for (int i = 0; i < n_buffers_; i++)
    {
        struct v4l2_buffer buf;
        CLEAR(buf);
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = (unsigned)i;
        if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0)
        {
            printf("[v4l2] QUERYBUF failed: %s\n", strerror(errno));
            return -1;
        }
        buffers_[i].length = buf.length;
        buffers_[i].start  = mmap(NULL, buf.length,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd_, buf.m.offset);
        if (buffers_[i].start == MAP_FAILED)
        {
            printf("[v4l2] mmap failed: %s\n", strerror(errno));
            return -1;
        }
    }

    for (int i = 0; i < n_buffers_; i++)
    {
        struct v4l2_buffer buf;
        CLEAR(buf);
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = (unsigned)i;
        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0)
        {
            printf("[v4l2] QBUF failed: %s\n", strerror(errno));
            return -1;
        }
    }
    return 0;
}

int V4L2Capture::start_streaming()
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0)
    {
        printf("[v4l2] STREAMON failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int V4L2Capture::stop_streaming()
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd_, VIDIOC_STREAMOFF, &type);  // 失败也无所谓
    return 0;
}

void V4L2Capture::decode_mjpg(const void *data, size_t len, cv::Mat &out)
{
    if (!tj_) tj_ = tjInitDecompress();
    if (!tj_)
    {
        printf("[v4l2] tjInitDecompress failed\n");
        out = cv::Mat();
        return;
    }

    int jw = 0, jh = 0, jsub = 0, jcol = 0;
    if (tjDecompressHeader2((tjhandle)tj_,
                            (unsigned char *)data, (unsigned long)len,
                            &jw, &jh, &jsub) < 0)
    {
        printf("[v4l2] tjDecompressHeader2 failed: %s\n", tjGetErrorStr());
        out = cv::Mat();
        return;
    }
    (void)jcol;

    size_t need = (size_t)jw * (size_t)jh * 3;
    if (need > tj_buf_size_)
    {
        if (tj_buf_) free(tj_buf_);
        tj_buf_ = (unsigned char *)malloc(need);
        tj_buf_size_ = need;
    }
    if (!tj_buf_)
    {
        out = cv::Mat();
        return;
    }

    // 优化: 直接解码成 RGB (TJPF_RGB), 上层 letterbox 之前省一次 BGR2RGB 全帧 cvtColor
    if (tjDecompress2((tjhandle)tj_,
                      (unsigned char *)data, (unsigned long)len,
                      tj_buf_, jw, 0, jh, TJPF_RGB, 0) < 0)
    {
        printf("[v4l2] tjDecompress2 failed: %s\n", tjGetErrorStr());
        out = cv::Mat();
        return;
    }

    // 注意: jw/jh 可能是摄像头报告的 stride 对齐后的尺寸, 但大多数情况 = width_/height_
    if (jw == width_ && jh == height_)
    {
        out = cv::Mat(height_, width_, CV_8UC3, tj_buf_).clone();
    }
    else
    {
        cv::Mat tmp(jh, jw, CV_8UC3, tj_buf_);
        cv::resize(tmp, out, cv::Size(width_, height_));
    }
}

void V4L2Capture::convert_yuyv_to_bgr(const void *data, size_t len, cv::Mat &out)
{
    out.create(height_, width_, CV_8UC3);
    const unsigned char *src = (const unsigned char *)data;
    unsigned char *dst = out.data;
    int stride = bytesperline_ > 0 ? bytesperline_ : width_ * 2;  // YUYV 每像素 2 字节
    // 按 stride 计算, 不能用 width_*2 (驱动可能插入 padding)
    if ((size_t)stride * (size_t)height_ > len) return;  // 缓冲不够, 放弃

    for (int y = 0; y < height_; y++)
    {
        const unsigned char *row = src + y * stride;
        unsigned char *drow = dst + y * width_ * 3;
        int x = 0;
        // 一次处理 2 像素 (YUYV macro-pixel)
        for (; x + 1 < width_; x += 2)
        {
            int y0 = row[2*x + 0];
            int u  = row[2*x + 1];
            int y1 = row[2*x + 2];
            int v  = row[2*x + 3];

            // BT.601 full range
            int c0 = y0 - 16;
            int c1 = y1 - 16;
            int d  = u  - 128;
            int e  = v  - 128;

            int b0 = (298 * c0 + 516 * d           ) >> 8;
            int g0 = (298 * c0 - 100 * d - 208 * e ) >> 8;
            int r0 = (298 * c0           + 409 * e ) >> 8;
            int b1 = (298 * c1 + 516 * d           ) >> 8;
            int g1 = (298 * c1 - 100 * d - 208 * e ) >> 8;
            int r1 = (298 * c1           + 409 * e ) >> 8;

            drow[3*x + 0] = (unsigned char)(b0 < 0 ? 0 : b0 > 255 ? 255 : b0);
            drow[3*x + 1] = (unsigned char)(g0 < 0 ? 0 : g0 > 255 ? 255 : g0);
            drow[3*x + 2] = (unsigned char)(r0 < 0 ? 0 : r0 > 255 ? 255 : r0);
            drow[3*x + 3] = (unsigned char)(b1 < 0 ? 0 : b1 > 255 ? 255 : b1);
            drow[3*x + 4] = (unsigned char)(g1 < 0 ? 0 : g1 > 255 ? 255 : g1);
            drow[3*x + 5] = (unsigned char)(r1 < 0 ? 0 : r1 > 255 ? 255 : r1);
        }
        // 奇数宽度时, 单独处理最后一像素 (用前一个 U/V)
        if (x < width_)
        {
            int y0 = row[2*x + 0];
            int u  = row[2*x + 1];
            int v  = row[2*x + 1];  // YUYV macro-pixel 的 U/V 共享
            int c0 = y0 - 16;
            int d  = u  - 128;
            int e  = v  - 128;
            int b0 = (298 * c0 + 516 * d           ) >> 8;
            int g0 = (298 * c0 - 100 * d - 208 * e ) >> 8;
            int r0 = (298 * c0           + 409 * e ) >> 8;
            drow[3*x + 0] = (unsigned char)(b0 < 0 ? 0 : b0 > 255 ? 255 : b0);
            drow[3*x + 1] = (unsigned char)(g0 < 0 ? 0 : g0 > 255 ? 255 : g0);
            drow[3*x + 2] = (unsigned char)(r0 < 0 ? 0 : r0 > 255 ? 255 : r0);
        }
    }
}

int V4L2Capture::open(const char *dev, int req_w, int req_h, int req_fps)
{
    close();

    // 默认 640x480@30: 避免用户传 0 时摄像头退到最小分辨率 (如 160x120)
    if (req_w  <= 0) req_w  = 640;
    if (req_h  <= 0) req_h  = 480;
    if (req_fps <= 0) req_fps = 30;

    fd_ = ::open(dev, O_RDWR | O_NONBLOCK, 0);
    if (fd_ < 0)
    {
        printf("[v4l2] open %s failed: %s\n", dev, strerror(errno));
        return -1;
    }

    // 验证是 V4L2 capture 设备
    struct v4l2_capability cap;
    if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0)
    {
        printf("[v4l2] QUERYCAP failed: %s\n", strerror(errno));
        close();
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
    {
        printf("[v4l2] %s is not a V4L2 capture device\n", dev);
        close();
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING))
    {
        printf("[v4l2] %s does not support streaming I/O\n", dev);
        close();
        return -1;
    }

    // 优先 MJPG, 失败回退 YUYV
    if (try_format(V4L2_PIX_FMT_MJPEG, "MJPG", req_w, req_h, req_fps) != 0 &&
        try_format(V4L2_PIX_FMT_YUYV,  "YUYV", req_w, req_h, req_fps) != 0)
    {
        printf("[v4l2] no supported pixel format on %s\n", dev);
        close();
        return -1;
    }

    if (init_mmap() != 0)
    {
        close();
        return -1;
    }
    if (start_streaming() != 0)
    {
        close();
        return -1;
    }

    printf("[v4l2] opened %s, %dx%d %s @ %d fps\n",
           dev, width_, height_, format_name_, fps_);
    return 0;
}

int V4L2Capture::read(cv::Mat &frame)
{
    if (fd_ < 0) return -1;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);
    struct timeval tv = {2, 0};  // 2s 超时
    int r = select(fd_ + 1, &fds, NULL, NULL, &tv);
    if (r <= 0)
    {
        printf("[v4l2] select timeout\n");
        return -1;
    }

    struct v4l2_buffer buf;
    CLEAR(buf);
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0)
    {
        printf("[v4l2] DQBUF failed: %s\n", strerror(errno));
        return -1;
    }

    if (pixfmt_ == V4L2_PIX_FMT_MJPEG)
        decode_mjpg(buffers_[buf.index].start, buf.bytesused, frame);
    else if (pixfmt_ == V4L2_PIX_FMT_YUYV)
        convert_yuyv_to_bgr(buffers_[buf.index].start, buf.bytesused, frame);
    else
        frame = cv::Mat();

    if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0)
    {
        printf("[v4l2] QBUF requeue failed: %s\n", strerror(errno));
        return -1;
    }

    return frame.empty() ? -1 : 0;
}
