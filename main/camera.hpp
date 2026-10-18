#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "esp_err.h"
#include "esp_cam_sensor_types.h"
#include "sc202cs_types.h"

// A dequeued frame belongs to the caller until release(). Never pass it to LVGL
// or inference after release(): the CSI DMA engine will overwrite it.
class Camera {
public:
    static constexpr uint32_t kTargetFps = 5;
    esp_err_t start();
    esp_err_t acquire(const uint8_t *&pixels);
    esp_err_t release();
    ~Camera();
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    bool delivery_rate_limited = false;

private:
    esp_err_t configure_sensor_rate();
    esp_cam_sensor_format_t sensor_format_ = {};
    std::unique_ptr<esp_cam_sensor_isp_info_t> sensor_isp_info_;
    std::vector<sc202cs_reginfo_t> sensor_registers_;
    int64_t cadence_start_us_ = 0;
    unsigned cadence_frames_ = 0;
    static constexpr unsigned kBuffers = 3;
    int fd_ = -1;
    bool streaming_ = false;
    int held_ = -1;
    void *buffers_[kBuffers] = {};
    size_t lengths_[kBuffers] = {};
};
