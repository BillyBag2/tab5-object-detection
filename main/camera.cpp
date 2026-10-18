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
#include "esp_timer.h"
#include "esp_video_ioctl.h"

namespace {
constexpr char TAG[] = "camera";
esp_err_t failure(const char *operation)
{
    ESP_LOGE(TAG, "%s failed (errno %d)", operation, errno);
    return ESP_FAIL;
}
}

esp_err_t Camera::configure_sensor_rate()
{
    esp_cam_sensor_format_t original = {};
    if (ioctl(fd_, VIDIOC_G_SENSOR_FMT, &original)) return failure("get sensor format");
    if (original.width != 1280 || original.height != 720 || original.fps != 30 ||
        original.format != ESP_CAM_SENSOR_PIXFORMAT_RAW8 || !original.isp_info ||
        original.isp_info->isp_v1_info.vts != 1250 || original.regs_size <= 0) {
        ESP_LOGE(TAG, "1 fps timing requires the pinned SC202CS 1280 x 720 RAW8 30 fps base mode");
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Keep PLL, MIPI bitrate, HTS and line/exposure timing unchanged. Extend
    // vertical blanking 30-fold: VTS 1250 -> 37500 lines, one frame per second.
    // Keep the custom format and register storage alive for the sensor driver.
    sensor_format_ = original;
    sensor_isp_info_ = std::make_unique<esp_cam_sensor_isp_info_t>(*original.isp_info);
    const uint32_t vts = original.isp_info->isp_v1_info.vts * original.fps / kTargetFps;
    if (vts > UINT16_MAX) return ESP_ERR_INVALID_SIZE;
    const auto *regs = static_cast<const sc202cs_reginfo_t *>(original.regs);
    int count = 0;
    while (count < original.regs_size && regs[count].reg != 0xffff) ++count;
    if (count == original.regs_size) return ESP_ERR_INVALID_RESPONSE;
    sensor_registers_.assign(regs, regs + count);
    sensor_registers_.push_back({0x320e, uint8_t(vts >> 8)});
    sensor_registers_.push_back({0x320f, uint8_t(vts)});
    sensor_registers_.push_back({0xffff, 0});
    sensor_isp_info_->isp_v1_info.vts = vts;
    sensor_format_.name = "TAB5_SC202CS_1280X720_RAW8_1FPS";
    sensor_format_.fps = kTargetFps;
    sensor_format_.regs = sensor_registers_.data();
    sensor_format_.regs_size = sensor_registers_.size();
    sensor_format_.isp_info = sensor_isp_info_.get();
    if (ioctl(fd_, VIDIOC_S_SENSOR_FMT, &sensor_format_)) return failure("set 1 fps sensor timing");

    // Verify that the frame-length bits actually took effect before streaming.
    uint32_t actual_vts = 0;
    for (uint32_t reg : {0x320e, 0x320f}) {
        esp_cam_sensor_reg_val_t value = {reg, 0};
        v4l2_ext_control control = {};
        control.id = ESP_CAM_SENSOR_IOC_G_REG;
        control.size = sizeof(value);
        control.p_u8 = reinterpret_cast<uint8_t *>(&value);
        v4l2_ext_controls controls = {};
        controls.ctrl_class = V4L2_CTRL_CLASS_ESP_CAM_IOCTL;
        controls.count = 1;
        controls.controls = &control;
        if (ioctl(fd_, VIDIOC_S_EXT_CTRLS, &controls)) return failure("read back sensor VTS");
        actual_vts = (actual_vts << 8) | (value.value & 0xff);
    }
    if (actual_vts != vts) {
        ESP_LOGE(TAG, "Sensor VTS readback %lu does not match requested %lu",
                 (unsigned long)actual_vts, (unsigned long)vts);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "Sensor configured at %lu fps: VTS=%lu, HTS=%d, line=%lu ns; MIPI bitrate unchanged",
             (unsigned long)kTargetFps, (unsigned long)vts, sensor_isp_info_->isp_v1_info.hts,
             (unsigned long)sensor_isp_info_->isp_v1_info.tline_ns);
    return ESP_OK;
}

esp_err_t Camera::start()
{
    esp_err_t err = bsp_camera_start(nullptr);
    if (err != ESP_OK) return err;
    fd_ = open(BSP_CAMERA_DEVICE, O_RDONLY);
    if (fd_ < 0) return failure("open CSI device");
    err = configure_sensor_rate();
    if (err != ESP_OK) return err;
    timeval timeout = {};
    timeout.tv_sec = 3;
    if (ioctl(fd_, VIDIOC_S_DQBUF_TIMEOUT, &timeout)) return failure("set low-fps capture timeout");

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

    // The sensor now runs at 1 fps. Request the same delivery rate, so the
    // driver's skip factor is 1: every physical frame reaches the application.
    v4l2_streamparm rate = {};
    rate.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rate.parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
    rate.parm.capture.timeperframe.numerator = 1;
    rate.parm.capture.timeperframe.denominator = kTargetFps;
    if (ioctl(fd_, VIDIOC_S_PARM, &rate)) {
        return failure("disable camera frame skipping");
    } else {
        delivery_rate_limited = true;
        rate = {};
        rate.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_G_PARM, &rate) == 0) {
            ESP_LOGI(TAG, "Camera delivery rate: %lu/%lu fps (sensor rate; no frame skipping)",
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
    const int64_t now = esp_timer_get_time();
    if (!cadence_start_us_) {
        cadence_start_us_ = now;
    } else if (++cadence_frames_ == 5) {
        ESP_LOGI(TAG, "Sensor cadence: %.3f fps across 5 frame intervals (no driver skipping)",
                 5000000.0 / (now - cadence_start_us_));
        cadence_start_us_ = now;
        cadence_frames_ = 0;
    }
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
