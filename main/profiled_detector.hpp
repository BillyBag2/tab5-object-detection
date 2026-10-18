#pragma once

#include "coco_detect.hpp"
#include "esp_timer.h"

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

    std::list<dl::detect::result_t> &run(const dl::image::img_t &image) override
    {
        const int64_t start = esp_timer_get_time();
        m_image_preprocessor->preprocess(image);
        const int64_t prepared = esp_timer_get_time();
        m_model->run();
        const int64_t inferred = esp_timer_get_time();
        m_postprocessor->clear_result();
        m_postprocessor->postprocess();
        auto &results = m_postprocessor->get_result(image.width, image.height);
        const int64_t finished = esp_timer_get_time();
        preprocess_ms = (prepared - start) / 1000.0f;
        model_ms = (inferred - prepared) / 1000.0f;
        postprocess_ms = (finished - inferred) / 1000.0f;
        return results;
    }
};
