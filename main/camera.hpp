#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"

// A dequeued frame belongs to the caller until release(). Never pass it to LVGL
// or inference after release(): the CSI DMA engine will overwrite it.
class Camera {
public:
    esp_err_t start();
    esp_err_t acquire(const uint8_t *&pixels);
    esp_err_t release();
    ~Camera();
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    bool delivery_rate_limited = false;

private:
    static constexpr unsigned kBuffers = 3;
    int fd_ = -1;
    bool streaming_ = false;
    int held_ = -1;
    void *buffers_[kBuffers] = {};
    size_t lengths_[kBuffers] = {};
};
