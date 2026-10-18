#pragma once

#include "coco_detect.hpp"
#include "esp_timer.h"
#if TAB5_INFERENCE_AUDIT
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

// Use the component's supplied model, preprocessing and postprocessing. Only
// split DetectImpl::run into separately timed stages; never modify managed code.
class ProfiledCocoDetector : public coco_detect::Yolo11n {
public:
    ProfiledCocoDetector() : Yolo11n("coco_detect_yolo11n_320_s8_v1.espdl", 0.25f, 0.5f)
    {
        set_score_thr(0.25f);
        set_nms_thr(0.5f);
    }

    float preprocess_ms = 0;
    float model_ms = 0;
    float postprocess_ms = 0;

#if TAB5_INFERENCE_AUDIT
    bool audit = false;
    bool audit_no_switch = false;
    uint32_t input_crc = 0, output_crc = 0, post_crc = 0, settled_crc = 0, result_crc = 0;
    std::map<std::string, std::vector<uint8_t>> audit_previous;

    static uint32_t tensor_crc(dl::TensorBase *tensor, uint32_t crc = 0)
    {
        // Hash logical elements, excluding allocator tail padding.
        size_t count = 1;
        for (int dim : tensor->shape) count *= dim;
        return esp_rom_crc32_le(crc, static_cast<const uint8_t *>(tensor->data),
                               count * tensor->get_dtype_bytes());
    }

    uint32_t outputs_crc()
    {
        uint32_t crc = 0;
        for (auto &[name, tensor] : m_model->get_outputs()) {
            crc = esp_rom_crc32_le(crc, reinterpret_cast<const uint8_t *>(name.data()), name.size());
            crc = tensor_crc(tensor, crc);
        }
        return crc;
    }

    void log_tensors()
    {
        for (auto &[name, tensor] : m_model->get_outputs()) {
            ESP_LOGI("inference_audit", "tensor=%s ptr=%p bytes=%d type=%s exp=%d simd_aligned=%d",
                     name.c_str(), tensor->data, tensor->get_bytes(), tensor->get_dtype_string(),
                     int(tensor->exponent), int((uintptr_t(tensor->data) % 16) == 0));
        }
    }

    void compare_outputs()
    {
        for (auto &[name, tensor] : m_model->get_outputs()) {
            size_t count = tensor->get_dtype_bytes();
            for (int dim : tensor->shape) count *= dim;
            auto *data = static_cast<const uint8_t *>(tensor->data);
            auto &previous = audit_previous[name];
            unsigned differences = 0, first = 0;
            if (previous.size() == count) {
                for (size_t i = 0; i < count; ++i) {
                    if (data[i] != previous[i]) {
                        if (!differences) first = i;
                        ++differences;
                    }
                }
            }
            ESP_LOGI("inference_audit", "head=%s crc=%08lx changed=%u first=%u old=%u new=%u bytes=%u",
                     name.c_str(), (unsigned long)tensor_crc(tensor), differences, first,
                     previous.empty() ? 0 : unsigned(previous[first]), unsigned(data[first]), unsigned(count));
            previous.assign(data, data + count);
        }
    }
#endif

    std::list<dl::detect::result_t> &run(const dl::image::img_t &image) override
    {
        const int64_t start = esp_timer_get_time();
        m_image_preprocessor->preprocess(image);
#if TAB5_INFERENCE_AUDIT
        if (audit) input_crc = tensor_crc(m_model->get_input());
#endif
        const int64_t prepared = esp_timer_get_time();
#if TAB5_INFERENCE_AUDIT
        if (audit && audit_no_switch) vTaskSuspendAll();
#endif
        m_model->run();
#if TAB5_INFERENCE_AUDIT
        if (audit && audit_no_switch) xTaskResumeAll();
#endif
        const int64_t inferred = esp_timer_get_time();
#if TAB5_INFERENCE_AUDIT
        if (audit) { output_crc = outputs_crc(); compare_outputs(); }
#endif
        m_postprocessor->clear_result();
        m_postprocessor->postprocess();
        auto &results = m_postprocessor->get_result(image.width, image.height);
        const int64_t finished = esp_timer_get_time();
#if TAB5_INFERENCE_AUDIT
        if (audit) {
            post_crc = outputs_crc();
            result_crc = 0;
            for (const auto &result : results) {
                result_crc = esp_rom_crc32_le(result_crc, reinterpret_cast<const uint8_t *>(&result.category), sizeof(result.category));
                result_crc = esp_rom_crc32_le(result_crc, reinterpret_cast<const uint8_t *>(&result.score), sizeof(result.score));
                result_crc = esp_rom_crc32_le(result_crc, reinterpret_cast<const uint8_t *>(result.box.data()), result.box.size() * sizeof(int));
            }
            // Let camera/PPA/LVGL activity run without starting another model.
            vTaskDelay(pdMS_TO_TICKS(100));
            settled_crc = outputs_crc();
        }
#endif
        preprocess_ms = (prepared - start) / 1000.0f;
        model_ms = (inferred - prepared) / 1000.0f;
        postprocess_ms = (finished - inferred) / 1000.0f;
        return results;
    }
};
