#pragma once

#include <cassert>
#include <cstring>
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "profiled_detector.hpp"

// Test builds add a full cache line on either side of each application image.
// Normal builds retain the existing aligned allocation without instrumentation.
namespace inference_audit {
#if TAB5_INFERENCE_AUDIT
struct GuardedBuffer { uint8_t *base; size_t bytes, alignment; };
inline GuardedBuffer guarded[6] = {};
inline unsigned buffer_count = 0;
#endif

inline uint16_t *allocate(size_t alignment, size_t bytes)
{
#if TAB5_INFERENCE_AUDIT
    auto *base = static_cast<uint8_t *>(heap_caps_aligned_calloc(
        alignment, 1, bytes + 2 * alignment, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!base) return nullptr;
    memset(base, 0xa5, alignment);
    memset(base + alignment + bytes, 0xa5, alignment);
    assert(buffer_count < 6);
    guarded[buffer_count++] = {base, bytes, alignment};
    return reinterpret_cast<uint16_t *>(base + alignment);
#else
    return static_cast<uint16_t *>(heap_caps_aligned_calloc(
        alignment, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
}

#if TAB5_INFERENCE_AUDIT
inline bool guards_ok()
{
    for (unsigned i = 0; i < buffer_count; ++i) {
        const auto &g = guarded[i];
        for (size_t j = 0; j < g.alignment; ++j) {
            if (g.base[j] != 0xa5 || g.base[g.alignment + g.bytes + j] != 0xa5) {
                ESP_LOGE("inference_audit", "Guard damaged: buffer=%u offset=%u", i, unsigned(j));
                return false;
            }
        }
    }
    return true;
}

inline void run(ProfiledCocoDetector &detector, const dl::image::img_t &camera_image,
                size_t alignment, size_t bytes)
{
    auto *pattern = allocate(alignment, bytes);
    if (!pattern) { ESP_LOGE("inference_audit", "Pattern allocation failed"); return; }
    for (int y = 0; y < camera_image.height; ++y) {
        for (int x = 0; x < camera_image.width; ++x) {
            const unsigned r = (x * 3 + y) & 31;
            const unsigned g = (y * 5 + x / 3) & 63;
            const unsigned b = ((x ^ y) + y / 7) & 31;
            pattern[y * camera_image.width + x] = (r << 11) | (g << 5) | b;
        }
    }
    detector.audit = true;
    detector.log_tensors();
    unsigned failures = 0;
    for (int phase = 0; phase < 4; ++phase) {
        auto image = camera_image;
        if (!(phase & 1)) image.data = pattern;
        const char *fixtures[] = {"pattern", "camera", "pattern_no_switch", "camera_no_switch"};
        const char *fixture = fixtures[phase];
        detector.audit_no_switch = phase >= 2;
        detector.audit_previous.clear();
        const uint32_t raw = esp_rom_crc32_le(0, static_cast<const uint8_t *>(image.data), bytes);
        uint32_t expected_input = 0, expected_output = 0, expected_result = 0;
        for (int repeat = 0; repeat < 6; ++repeat) {
            const auto &results = detector.run(image);
            const uint32_t after = esp_rom_crc32_le(0, static_cast<const uint8_t *>(image.data), bytes);
            const bool heap = heap_caps_check_integrity_all(true);
            const bool guards = guards_ok();
            if (!repeat) {
                expected_input = detector.input_crc;
                expected_output = detector.output_crc;
                expected_result = detector.result_crc;
            }
            const bool pass = raw == after && heap && guards &&
                detector.input_crc == expected_input && detector.output_crc == expected_output &&
                detector.result_crc == expected_result && detector.output_crc == detector.post_crc &&
                detector.output_crc == detector.settled_crc;
            failures += !pass;
            ESP_LOGI("inference_audit", "fixture=%s repeat=%d l2=%d line=%d raw=%08lx input=%08lx output=%08lx post=%08lx settled=%08lx results=%08lx objects=%u heap=%d guards=%d pass=%d",
                     fixture, repeat, CONFIG_CACHE_L2_CACHE_SIZE, CONFIG_CACHE_L2_CACHE_LINE_SIZE,
                     (unsigned long)raw, (unsigned long)detector.input_crc, (unsigned long)detector.output_crc,
                     (unsigned long)detector.post_crc, (unsigned long)detector.settled_crc,
                     (unsigned long)detector.result_crc, unsigned(results.size()), heap, guards, pass);
            if (!repeat) {
                for (const auto &result : results) {
                    ESP_LOGI("inference_audit", "fixture=%s category=%d score=%.8f", fixture, result.category, double(result.score));
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    detector.audit = false;
    detector.audit_no_switch = false;
    ESP_LOGI("inference_audit", "DONE failures=%u", failures);
    // The last registered guarded allocation is the temporary pattern.
    heap_caps_free(guarded[--buffer_count].base);
}
#endif
}
