// 直接 V4L2 摄像头采集, 绕过 OpenCV 的 videoio 和系统的 libv4l2
// MJPG 用 libturbojpeg 解码, YUYV/UYVY/NV12 用 OpenCV cvtColor 转 RGB
// 支持 Single-planar (USB 摄像头) 和 Multi-planar (MIPI RKISP 摄像头)

#include "v4l2_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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
                for (int p = 0; p < 2; p++)
                {
                    if (buffers_[i].start[p])
                        munmap(buffers_[i].start[p], buffers_[i].length[p]);
                }
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
}

int V4L2Capture::try_format(unsigned int pixfmt, const char *name, int w, int h, int fps)
{
    struct v4l2_format fmt;
    CLEAR(fmt);

    if (multiplanar_)
    {
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.fmt.pix_mp.width       = (unsigned)w;
        fmt.fmt.pix_mp.height      = (unsigned)h;
        fmt.fmt.pix_mp.pixelformat = pixfmt;
        fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;

        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0)
        {
            printf("[v4l2] VIDIOC_S_FMT(%s, MPLANE) failed: %s (errno=%d)\n",
                   name, strerror(errno), errno);
            return -1;
        }

        if (fmt.fmt.pix_mp.pixelformat != pixfmt)
        {
            printf("[v4l2] driver did not accept %s (got 0x%08x), try next\n",
                   name, fmt.fmt.pix_mp.pixelformat);
            return -1;
        }

        printf("[v4l2] S_FMT(%s, MPLANE) OK: %dx%d, %d plane(s)\n",
               name, (int)fmt.fmt.pix_mp.width, (int)fmt.fmt.pix_mp.height,
               (int)fmt.fmt.pix_mp.num_planes);

        width_  = (int)fmt.fmt.pix_mp.width;
        height_ = (int)fmt.fmt.pix_mp.height;
        n_planes_ = (int)fmt.fmt.pix_mp.num_planes;
        bytesperline_ = (int)fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
        if (bytesperline_ <= 0) bytesperline_ = width_;
        pixfmt_ = fmt.fmt.pix_mp.pixelformat;
    }
    else
    {
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

        if (fmt.fmt.pix.pixelformat != pixfmt)
        {
            printf("[v4l2] driver did not accept %s (got 0x%08x), try next\n",
                   name, fmt.fmt.pix.pixelformat);
            return -1;
        }

        width_  = (int)fmt.fmt.pix.width;
        height_ = (int)fmt.fmt.pix.height;
        n_planes_ = 1;
        bytesperline_ = (int)fmt.fmt.pix.bytesperline;
        if (bytesperline_ <= 0) bytesperline_ = width_ * 2;  // YUYV 安全兜底
        pixfmt_ = fmt.fmt.pix.pixelformat;
    }

    strncpy(format_name_, name, sizeof(format_name_) - 1);

    // 设帧率
    struct v4l2_streamparm parm;
    CLEAR(parm);
    parm.type = multiplanar_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                              : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = (unsigned)fps;
    if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0)
    {
        // RKISP 等设备可能不支持帧率设置, 不致命
        printf("[v4l2] VIDIOC_S_PARM failed: %s (fps will be reported as 0)\n", strerror(errno));
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
    enum v4l2_buf_type buf_type = multiplanar_
        ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    struct v4l2_requestbuffers req;
    CLEAR(req);
    req.count  = 4;
    req.type   = buf_type;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0)
    {
        printf("[v4l2] VIDIOC_REQBUFS failed: %s (type=%d)\n", strerror(errno), buf_type);
        return -1;
    }
    if (req.count < 2)
    {
        printf("[v4l2] insufficient buffer memory\n");
        return -1;
    }

    n_buffers_ = (int)req.count;
    printf("[v4l2] REQBUFS: got %d buffers (type=%d)\n", n_buffers_, buf_type);
    buffers_ = (buffer *)calloc(n_buffers_, sizeof(buffer));
    if (!buffers_) return -1;

    if (multiplanar_)
    {
        // Multi-planar: 每个 buffer 有 n_planes_ 个平面, 分别 mmap
        for (int i = 0; i < n_buffers_; i++)
        {
            struct v4l2_buffer buf;
            struct v4l2_plane planes[2];
            CLEAR(buf);
            CLEAR(planes);
            buf.type   = buf_type;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index  = (unsigned)i;
            buf.m.planes = planes;
            buf.length   = n_planes_;

            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0)
            {
                printf("[v4l2] QUERYBUF(MPLANE) failed: %s\n", strerror(errno));
                return -1;
            }

            for (int p = 0; p < n_planes_; p++)
            {
                buffers_[i].length[p] = planes[p].length;
                buffers_[i].start[p]  = mmap(NULL, planes[p].length,
                                              PROT_READ | PROT_WRITE,
                                              MAP_SHARED, fd_,
                                              planes[p].m.mem_offset);
                if (buffers_[i].start[p] == MAP_FAILED)
                {
                    printf("[v4l2] mmap plane %d failed: %s\n", p, strerror(errno));
                    return -1;
                }
            }
        }
    }
    else
    {
        // Single-planar: 和之前一样
        for (int i = 0; i < n_buffers_; i++)
        {
            struct v4l2_buffer buf;
            CLEAR(buf);
            buf.type   = buf_type;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index  = (unsigned)i;
            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0)
            {
                printf("[v4l2] QUERYBUF failed: %s\n", strerror(errno));
                return -1;
            }
            buffers_[i].length[0] = buf.length;
            buffers_[i].start[0]  = mmap(NULL, buf.length,
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED, fd_, buf.m.offset);
            if (buffers_[i].start[0] == MAP_FAILED)
            {
                printf("[v4l2] mmap failed: %s\n", strerror(errno));
                return -1;
            }
        }
    }

    // 入队所有 buffer
    for (int i = 0; i < n_buffers_; i++)
    {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[2];
        CLEAR(buf);
        CLEAR(planes);
        buf.type   = buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = (unsigned)i;
        if (multiplanar_)
        {
            buf.m.planes = planes;
            buf.length   = n_planes_;
        }
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
    enum v4l2_buf_type type = multiplanar_
        ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0)
    {
        printf("[v4l2] STREAMON failed: %s (type=%d)\n", strerror(errno), type);
        return -1;
    }
    printf("[v4l2] STREAMON OK (type=%d)\n", type);
    return 0;
}

int V4L2Capture::stop_streaming()
{
    enum v4l2_buf_type type = multiplanar_
        ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd_, VIDIOC_STREAMOFF, &type);  // 失败也无所谓
    return 0;
}

// =====================================================================
// MJPG 解码 (libturbojpeg → RGB)
// =====================================================================
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

    if (jw == width_ && jh == height_)
    {
        if (out.rows != height_ || out.cols != width_ || out.type() != CV_8UC3)
            out.create(height_, width_, CV_8UC3);
        if (tjDecompress2((tjhandle)tj_,
                          (unsigned char *)data, (unsigned long)len,
                          out.data, width_, 0, height_, TJPF_RGB, 0) < 0)
        {
            printf("[v4l2] tjDecompress2 failed: %s\n", tjGetErrorStr());
            out = cv::Mat();
            return;
        }
    }
    else
    {
        cv::Mat tmp(jh, jw, CV_8UC3);
        if (tjDecompress2((tjhandle)tj_,
                          (unsigned char *)data, (unsigned long)len,
                          tmp.data, jw, 0, jh, TJPF_RGB, 0) < 0)
        {
            printf("[v4l2] tjDecompress2 failed: %s\n", tjGetErrorStr());
            out = cv::Mat();
            return;
        }
        cv::resize(tmp, out, cv::Size(width_, height_));
    }
}



// =====================================================================
// 打开设备
// =====================================================================
int V4L2Capture::open(const char *dev, int req_w, int req_h, int req_fps)
{
    close();

    // 默认 640x480@30
    if (req_w  <= 0) req_w  = 640;
    if (req_h  <= 0) req_h  = 480;
    if (req_fps <= 0) req_fps = 30;

    fd_ = ::open(dev, O_RDWR | O_NONBLOCK, 0);
    if (fd_ < 0)
    {
        printf("[v4l2] open %s failed: %s\n", dev, strerror(errno));
        return -1;
    }

    // 验证是 V4L2 capture 设备 (同时接受 Single-planar 和 Multi-planar)
    struct v4l2_capability cap;
    if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0)
    {
        printf("[v4l2] QUERYCAP failed: %s\n", strerror(errno));
        close();
        return -1;
    }

    if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
    {
        multiplanar_ = true;
        printf("[v4l2] %s: Multi-planar capture device\n", dev);
    }
    else if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)
    {
        multiplanar_ = false;
        printf("[v4l2] %s: Single-planar capture device\n", dev);
    }
    else
    {
        printf("[v4l2] %s is not a V4L2 capture device "
               "(capabilities=0x%08x, need CAPTURE or CAPTURE_MPLANE)\n",
               dev, cap.capabilities);
        close();
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING))
    {
        printf("[v4l2] %s does not support streaming I/O\n", dev);
        close();
        return -1;
    }

    // 格式优先级: MJPG → NV12 → YUYV → UYVY
    // USB 摄像头会命中 MJPG; MIPI RKISP 会命中 NV12
    int fmt_ok = -1;
    if ((fmt_ok = try_format(V4L2_PIX_FMT_MJPEG, "MJPG", req_w, req_h, req_fps)) != 0)
    {
        printf("[v4l2] MJPG not available, trying NV12...\n");
        if ((fmt_ok = try_format(V4L2_PIX_FMT_NV12,  "NV12", req_w, req_h, req_fps)) != 0)
        {
            printf("[v4l2] NV12 not available, trying YUYV...\n");
            if ((fmt_ok = try_format(V4L2_PIX_FMT_YUYV,  "YUYV", req_w, req_h, req_fps)) != 0)
            {
                printf("[v4l2] YUYV not available, trying UYVY...\n");
                fmt_ok = try_format(V4L2_PIX_FMT_UYVY,  "UYVY", req_w, req_h, req_fps);
            }
        }
    }
    if (fmt_ok != 0)
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

    printf("[v4l2] opened %s, %dx%d %s @ %d fps (%s, %d plane%s)\n",
           dev, width_, height_, format_name_, fps_,
           multiplanar_ ? "MPLANE" : "SPLANE",
           n_planes_, n_planes_ > 1 ? "s" : "");
    return 0;
}

// =====================================================================
// 读取一帧
// =====================================================================
int V4L2Capture::read(cv::Mat &frame)
{
    if (fd_ < 0) return -1;

    // poll 比 select 更轻量 (单 fd 无需 FD_SET 宏)
    struct pollfd pfd = {fd_, POLLIN, 0};
    int r = poll(&pfd, 1, 2000);  // 2s 超时
    if (r <= 0)
    {
        printf("[v4l2] poll timeout or error\n");
        return -1;
    }

    enum v4l2_buf_type buf_type = multiplanar_
        ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    struct v4l2_buffer buf;
    struct v4l2_plane planes[2];
    CLEAR(buf);
    CLEAR(planes);
    buf.type   = buf_type;
    buf.memory = V4L2_MEMORY_MMAP;
    if (multiplanar_)
    {
        buf.m.planes = planes;
        buf.length   = n_planes_;
    }

    if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0)
    {
        printf("[v4l2] DQBUF failed: %s\n", strerror(errno));
        return -1;
    }

    // 按格式解码 (OpenCV cvtColor 内部有 NEON/SSE 优化, 比手写循环快)
    if (pixfmt_ == V4L2_PIX_FMT_MJPEG)
    {
        size_t data_len = multiplanar_ ? planes[0].bytesused : buf.bytesused;
        decode_mjpg(buffers_[buf.index].start[0], data_len, frame);
    }
    else if (pixfmt_ == V4L2_PIX_FMT_NV12)
    {
        // NV12 → RGB: cvtColorTwoPlane 接受分离的 Y/UV Mat (支持 stride)
        const unsigned char *y_data = (const unsigned char *)buffers_[buf.index].start[0];
        const unsigned char *uv_data;
        int y_stride = bytesperline_;
        int uv_stride;

        if (multiplanar_ && n_planes_ == 2)
        {
            uv_data = (const unsigned char *)buffers_[buf.index].start[1];
            uv_stride = y_stride;  // NV12: UV stride == Y stride
        }
        else
        {
            uv_data = y_data + y_stride * height_;
            uv_stride = y_stride;
        }

        cv::Mat y_mat(height_, width_, CV_8UC1, (void *)y_data, y_stride);
        cv::Mat uv_mat(height_ / 2, width_ / 2, CV_8UC2, (void *)uv_data, uv_stride);
        cv::cvtColorTwoPlane(y_mat, uv_mat, frame, cv::COLOR_YUV2RGB_NV12);
    }
    else if (pixfmt_ == V4L2_PIX_FMT_YUYV)
    {
        size_t data_len = multiplanar_ ? planes[0].bytesused : buf.bytesused;
        int stride = bytesperline_ > 0 ? bytesperline_ : width_ * 2;
        if ((size_t)stride * (size_t)height_ > data_len)
        {
            printf("[v4l2] YUYV frame too short (need %d, got %zu)\n", stride * height_, data_len);
            frame = cv::Mat();
        }
        else
        {
            cv::Mat yuyv_mat(height_, width_, CV_8UC2, buffers_[buf.index].start[0], stride);
            cv::cvtColor(yuyv_mat, frame, cv::COLOR_YUV2RGB_YUY2);
        }
    }
    else if (pixfmt_ == V4L2_PIX_FMT_UYVY)
    {
        size_t data_len = multiplanar_ ? planes[0].bytesused : buf.bytesused;
        int stride = bytesperline_ > 0 ? bytesperline_ : width_ * 2;
        if ((size_t)stride * (size_t)height_ > data_len)
        {
            printf("[v4l2] UYVY frame too short (need %d, got %zu)\n", stride * height_, data_len);
            frame = cv::Mat();
        }
        else
        {
            cv::Mat uyvy_mat(height_, width_, CV_8UC2, buffers_[buf.index].start[0], stride);
            cv::cvtColor(uyvy_mat, frame, cv::COLOR_YUV2RGB_UYVY);
        }
    }
    else
    {
        frame = cv::Mat();
    }

    // 重新入队 (保留 DQBUF 返回的 buf.index)
    unsigned int idx = buf.index;
    CLEAR(buf);
    CLEAR(planes);
    buf.type   = buf_type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = idx;
    if (multiplanar_)
    {
        buf.m.planes = planes;
        buf.length   = n_planes_;
    }
    if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0)
    {
        printf("[v4l2] QBUF requeue failed: %s\n", strerror(errno));
        return -1;
    }

    return frame.empty() ? -1 : 0;
}
