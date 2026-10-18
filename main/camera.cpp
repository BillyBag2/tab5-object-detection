#include "camera.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "linux/videodev2.h"
#include "bsp/esp-bsp.h"
#include "esp_video_device.h"
#include "esp_log.h"

namespace {
constexpr char TAG[] = "camera";
esp_err_t failure(const char *operation)
{
    ESP_LOGE(TAG, "%s failed (errno %d)", operation, errno);
    return ESP_FAIL;
}
}

esp_err_t Camera::start()
{
    esp_err_t err = bsp_camera_start(nullptr);
    if (err != ESP_OK) return err;
    fd_ = open(BSP_CAMERA_DEVICE, O_RDONLY);
    if (fd_ < 0) return failure("open CSI device");

    v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_G_FMT, &fmt)) return failure("get format");
    ESP_LOGI(TAG, "Initial CSI format: %lu x %lu, fourcc 0x%08lx, returned type %lu",
             (unsigned long)fmt.fmt.pix.width, (unsigned long)fmt.fmt.pix.height,
             (unsigned long)fmt.fmt.pix.pixelformat, (unsigned long)fmt.type);
    // esp_video 2.0.1 G_FMT copies the stored format over the entire request,
    // including a zero type on initial CSI startup. Never reuse that structure
    // for S_FMT: it would select an invalid stream (EINVAL). The default CSI
    // output is already RGB565, so only negotiate when a change is necessary.
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        v4l2_format requested = {};
        requested.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        requested.fmt.pix.width = fmt.fmt.pix.width;
        requested.fmt.pix.height = fmt.fmt.pix.height;
        requested.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        if (ioctl(fd_, VIDIOC_S_FMT, &requested)) return failure("set RGB565");
    }
    fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_G_FMT, &fmt)) return failure("read negotiated format");
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) return ESP_ERR_NOT_SUPPORTED;
    width = fmt.fmt.pix.width;
    height = fmt.fmt.pix.height;
    stride = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.bytesperline : width * 2;
    if (!width || !height || stride < width * 2) return ESP_ERR_INVALID_SIZE;

    // esp_video implements TIMEPERFRAME by skipping frames, not by changing
    // the SC202CS sensor timing. Deliver 5 fps to this application.
    v4l2_streamparm rate = {};
    rate.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rate.parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
    rate.parm.capture.timeperframe.numerator = 1;
    rate.parm.capture.timeperframe.denominator = 5;
    if (ioctl(fd_, VIDIOC_S_PARM, &rate)) {
        ESP_LOGW(TAG, "5 fps delivery request unsupported (errno %d); application still caps preview at 5 fps", errno);
    } else {
        delivery_rate_limited = true;
        rate = {};
        rate.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_G_PARM, &rate) == 0) {
            ESP_LOGI(TAG, "Camera delivery rate: %lu/%lu fps (driver frame skipping; sensor mode unchanged)",
                     (unsigned long)rate.parm.capture.timeperframe.denominator,
                     (unsigned long)rate.parm.capture.timeperframe.numerator);
        }
    }

    v4l2_requestbuffers req = {};
    req.count = kBuffers;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd_, VIDIOC_REQBUFS, &req)) return failure("request buffers");
    if (req.count < 2 || req.count > kBuffers) return ESP_ERR_NO_MEM;
    for (unsigned i = 0; i < req.count; ++i) {
        v4l2_buffer buf = {};
        buf.type = req.type;
        buf.memory = req.memory;
        buf.index = i;
        if (ioctl(fd_, VIDIOC_QUERYBUF, &buf)) return failure("query buffer");
        if (buf.length < stride * height) return ESP_ERR_INVALID_SIZE;
        void *ptr = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
        if (ptr == MAP_FAILED || ptr == nullptr) return failure("map buffer");
        buffers_[i] = ptr;
        lengths_[i] = buf.length;
        if (ioctl(fd_, VIDIOC_QBUF, &buf)) return failure("queue buffer");
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type)) return failure("start stream");
    streaming_ = true;
    ESP_LOGI(TAG, "CSI RGB565: %lu x %lu, stride %lu", (unsigned long)width,
             (unsigned long)height, (unsigned long)stride);
    return ESP_OK;
}

esp_err_t Camera::acquire(const uint8_t *&pixels)
{
    if (held_ >= 0) return ESP_ERR_INVALID_STATE;
    v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd_, VIDIOC_DQBUF, &buf)) return failure("capture frame");
    if (buf.index >= kBuffers || !buffers_[buf.index]) return ESP_ERR_INVALID_RESPONSE;
    held_ = buf.index;
    if ((buf.flags & V4L2_BUF_FLAG_ERROR) ||
        (buf.bytesused && buf.bytesused < stride * height)) {
        release();
        return ESP_ERR_INVALID_SIZE;
    }
    pixels = static_cast<const uint8_t *>(buffers_[buf.index]);
    return ESP_OK;
}

esp_err_t Camera::release()
{
    if (held_ < 0) return ESP_ERR_INVALID_STATE;
    v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = held_;
    if (ioctl(fd_, VIDIOC_QBUF, &buf)) return failure("release frame");
    held_ = -1;
    return ESP_OK;
}

Camera::~Camera()
{
    if (streaming_) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd_, VIDIOC_STREAMOFF, &type);
    }
    for (unsigned i = 0; i < kBuffers; ++i) {
        if (buffers_[i]) munmap(buffers_[i], lengths_[i]);
    }
    if (fd_ >= 0) close(fd_);
}
