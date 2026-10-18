#include <algorithm>
#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "bsp/esp-bsp.h"
#include "profiled_detector.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "esp_private/esp_cache_private.h"
#include "camera.hpp"
#include "coco_labels.hpp"

namespace {
constexpr char TAG[] = "tab5_detect";
constexpr int kMaxBoxes = 10;
constexpr int kModelSize = 320;
constexpr int kInferenceCropSize = kModelSize * 2;
constexpr size_t kInferenceBytes = kModelSize * kModelSize * sizeof(uint16_t);
constexpr int64_t kPreviewPeriodUs = 1000000 / 5;

struct Box {
    int x1, y1, x2, y2, category;
    float score;
};
struct Results {
    unsigned sequence = 0;
    int count = 0;
    float inference_ms = 0;
    float prepare_ms = 0;
    int64_t captured_us = 0;
    Box boxes[kMaxBoxes] = {};
};

Camera camera;
uint16_t *preview_pixels;
uint16_t *canvas_pixels;
uint16_t *inference_pixels;
uint16_t *analysing_pixels;
uint16_t *previous_pixels;
constexpr int image_width = kModelSize, image_height = kModelSize;
constexpr size_t image_bytes = kInferenceBytes;
size_t preview_buffer_size;
ppa_client_handle_t preview_ppa;
int64_t inference_captured_us;
int64_t inference_ready_us;
float inference_ppa_ms;
int crop_x, crop_y;
QueueHandle_t result_queue;
SemaphoreHandle_t inference_free;
TaskHandle_t detector_task;
lv_obj_t *canvas, *status_label, *analysing_canvas, *previous_canvas;
lv_obj_t *rectangles[2][kMaxBoxes];
lv_obj_t *box_labels[2][kMaxBoxes];
lv_obj_t *progress_squares[5];
int64_t guide_start_us = 0;
int64_t guide_duration_us = 0;

void status(const char *message)
{
    ESP_LOGI(TAG, "%s", message);
    bsp_display_lock(0);
    lv_label_set_text(status_label, message);
    bsp_display_unlock();
}

void show_error(const char *operation, esp_err_t err)
{
    char message[160];
    snprintf(message, sizeof(message), "%s: %s\nSee USB serial log; reset to retry.",
             operation, esp_err_to_name(err));
    status(message);
}

void create_screen()
{
    bsp_display_lock(0);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_set_style_text_font(screen, &lv_font_montserrat_28, 0);
    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_label_set_text(title, "TAB5  |  COCO OBJECT DETECTION");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 32);
    status_label = lv_label_create(screen);
    lv_obj_set_width(status_label, BSP_LCD_H_RES - 48);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_label_set_text(status_label, "Starting camera and YOLO11n INT8 (320)...");
    bsp_display_unlock();
}

void update_progress(lv_timer_t *)
{
    // LVGL calls this under its display lock. The first run has no estimate.
    int active = -1;
    if (guide_start_us && guide_duration_us > 0) {
        const int64_t elapsed = std::max<int64_t>(0, esp_timer_get_time() - guide_start_us);
        active = int(std::min<int64_t>(4, elapsed * 4 / guide_duration_us));
    }
    for (int i = 0; i < 5; ++i) {
        lv_obj_set_style_bg_color(progress_squares[i],
                                 lv_color_hex(i == active ? 0x50ff90 : 0x263746), 0);
    }
}

void create_overlays(lv_obj_t *parent, int view)
{
    for (int i = 0; i < kMaxBoxes; ++i) {
        auto *rect = rectangles[view][i] = lv_obj_create(parent);
        lv_obj_remove_style_all(rect);
        lv_obj_set_style_border_color(rect, lv_color_hex(0x50ff90), 0);
        lv_obj_set_style_border_width(rect, 2, 0);
        lv_obj_remove_flag(rect, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(rect, LV_OBJ_FLAG_HIDDEN);
        auto *label = box_labels[view][i] = lv_label_create(parent);
        lv_obj_set_style_max_width(label, image_width, 0);
        lv_obj_set_style_bg_color(label, lv_color_hex(0x102820), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x50ff90), 0);
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
}

void caption(const char *text, int x, int y, int width)
{
    auto *label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, text);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(label, x, y);
}

lv_obj_t *create_canvas(uint16_t *pixels, int x, int y)
{
    auto *view = lv_canvas_create(lv_screen_active());
    lv_canvas_set_buffer(view, pixels, image_width, image_height, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(view, x, y);
    return view;
}

void create_preview()
{
    bsp_display_lock(0);
    caption("Live", 200, 78, 320);
    canvas = create_canvas(canvas_pixels, 200, 118);
    create_overlays(canvas, 0);
    caption("Next sample estimate", 24, 468, 672);
    for (int i = 0; i < 5; ++i) {
        progress_squares[i] = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(progress_squares[i]);
        lv_obj_set_size(progress_squares[i], 32, 32);
        lv_obj_set_pos(progress_squares[i], 256 + i * 44, 514);
        lv_obj_set_style_bg_opa(progress_squares[i], LV_OPA_COVER, 0);
    }
    update_progress(nullptr);
    lv_timer_create(update_progress, 100, nullptr);
    caption("Analysing", 24, 592, 320);
    caption("Previous result", 376, 592, 320);
    analysing_canvas = create_canvas(analysing_pixels, 24, 638);
    previous_canvas = create_canvas(previous_pixels, 376, 638);
    create_overlays(previous_canvas, 1);
    bsp_display_unlock();
}

// PPA handles the strided reads required for rotation without monopolizing the
// CPU on PSRAM cache misses. Blocking mode sleeps until DMA completes, keeping
// the source camera buffer owned until the accelerator has finished reading it.
esp_err_t prepare_frame(const uint8_t *source, uint16_t *destination)
{
    ppa_srm_oper_config_t operation = {};
    operation.in.buffer = source;
    operation.in.pic_w = camera.stride / sizeof(uint16_t);
    operation.in.pic_h = camera.height;
    operation.in.block_w = kInferenceCropSize;
    operation.in.block_h = kInferenceCropSize;
    operation.in.block_offset_x = crop_x;
    operation.in.block_offset_y = crop_y;
    operation.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    operation.out.buffer = destination;
    operation.out.buffer_size = preview_buffer_size;
    operation.out.pic_w = kModelSize;
    operation.out.pic_h = kModelSize;
    operation.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    operation.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
    operation.scale_x = 0.5f;
    operation.scale_y = 0.5f;
    operation.mode = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_scale_rotate_mirror(preview_ppa, &operation);
}

void draw_boxes(const Results &results, int view)
{
    const bool has_result = results.sequence != 0;
    for (int i = 0; i < kMaxBoxes; ++i) {
        if (!has_result || i >= results.count) {
            lv_obj_add_flag(rectangles[view][i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(box_labels[view][i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const Box &box = results.boxes[i];
        lv_obj_set_pos(rectangles[view][i], box.x1, box.y1);
        lv_obj_set_size(rectangles[view][i], box.x2 - box.x1 + 1, box.y2 - box.y1 + 1);
        const char *name = box.category >= 0 && box.category < 80 ? kCocoLabels[box.category] : "object";
        lv_label_set_text_fmt(box_labels[view][i], "%s %d%%", name, int(box.score * 100));
        lv_obj_update_layout(box_labels[view][i]);
        const int label_x = std::max(0, std::min(box.x1, image_width - int(lv_obj_get_width(box_labels[view][i]))));
        const int label_height = lv_obj_get_height(box_labels[view][i]);
        const int label_y = std::clamp(box.y1 - label_height - 4, 0,
                                     std::max(0, image_height - label_height));
        lv_obj_set_pos(box_labels[view][i], label_x, label_y);
        lv_obj_remove_flag(rectangles[view][i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(box_labels[view][i], LV_OBJ_FLAG_HIDDEN);
    }
}

void show_completed(const Results &results)
{
    // Consume completion before replacing the analysing still. This pairs the
    // previous still with exactly the pixels that produced these boxes.
    bsp_display_lock(0);
    memcpy(previous_pixels, analysing_pixels, image_bytes);
    lv_obj_invalidate(previous_canvas);
    draw_boxes(results, 1);
    guide_duration_us = int64_t(results.inference_ms * 1000.0f);
    bsp_display_unlock();
}

void update_screen(const Results &results, float preview_fps, const uint16_t *pixels, bool submitted)
{
    // Keep the last completed result while the next inference runs. Expiring
    // by capture time discarded every result when inference exceeded 1.5 s.
    const bool has_result = results.sequence != 0;
    const double frame_age_s = has_result ? (esp_timer_get_time() - results.captured_us) / 1000000.0 : 0;
    bsp_display_lock(0);
    // LVGL's renderer holds this same lock. Its canvas buffer is never a DMA
    // buffer, and remains unchanged until the next locked update.
    memcpy(canvas_pixels, pixels, image_bytes);
    lv_obj_invalidate(canvas);
    draw_boxes(results, 0);
    if (submitted) {
        memcpy(analysing_pixels, pixels, image_bytes);
        lv_obj_invalidate(analysing_canvas);
        guide_start_us = inference_ready_us;
        update_progress(nullptr);
    }
    char text[192];
    if (has_result) {
        snprintf(text, sizeof(text), "YOLO11n 320 | Live + analysing + previous result\nPreview %.1f fps | Detection %.0f ms | PPA %.1f ms\n%d objects | Result #%u | Frame age %.1f s",
                 double(preview_fps), double(results.inference_ms), double(results.prepare_ms), results.count, results.sequence, frame_age_s);
    } else {
        snprintf(text, sizeof(text), "YOLO11n 320 | COCO 80 classes | INT8\nPreview %.1f fps | Waiting for detection...", double(preview_fps));
    }
    lv_label_set_text(status_label, text);
    bsp_display_unlock();
}

void detect_task(void *)
{
    // Eager load once; model allocation never runs while holding the LVGL lock.
    ESP_LOGI(TAG, "Loading YOLO11n 320 model");
    ProfiledCocoDetector detector;
    ESP_LOGI(TAG, "Model loaded; waiting for camera frame");
    unsigned sequence = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        dl::image::img_t image = {};
        image.data = inference_pixels;
        image.width = kModelSize;
        image.height = kModelSize;
        image.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;
        const int64_t start = esp_timer_get_time();
        auto &detections = detector.run(image);
        Results results;
        results.sequence = ++sequence;
        results.captured_us = inference_captured_us;
        results.inference_ms = (esp_timer_get_time() - start) / 1000.0f;
        results.prepare_ms = inference_ppa_ms;
        for (const auto &detection : detections) {
            if (results.count == kMaxBoxes) break;
            if (detection.box.size() < 4) continue;
            Box box = {
                std::clamp(detection.box[0], 0, kModelSize - 1),
                std::clamp(detection.box[1], 0, kModelSize - 1),
                std::clamp(detection.box[2], 0, kModelSize - 1),
                std::clamp(detection.box[3], 0, kModelSize - 1),
                detection.category, detection.score
            };
            if (box.x2 > box.x1 && box.y2 > box.y1) results.boxes[results.count++] = box;
        }
        const double total_ms = (esp_timer_get_time() - results.captured_us) / 1000.0;
        ESP_LOGI(TAG, "Detection #%u: %d objects in %.0f ms", results.sequence, results.count,
                 double(results.inference_ms));
        ESP_LOGI(TAG, "PROFILE #%u%s: preview=%.3f ppa=%.3f wait=%.3f pre=%.3f model=%.3f post=%.3f total=%.3f ms",
                 results.sequence, results.sequence == 1 ? " cold" : "",
                 0.0, double(inference_ppa_ms),
                 (start - inference_ready_us) / 1000.0,
                 double(detector.preprocess_ms), double(detector.model_ms), double(detector.postprocess_ms),
                 total_ms);
        xQueueOverwrite(result_queue, &results);
        // This handoff protects the pixels AND timestamp from capture writes.
        xSemaphoreGive(inference_free);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void capture_task(void *)
{
    Results latest;
    int64_t last_preview = 0, fps_start = esp_timer_get_time();
    unsigned frames = 0;
    float fps = 0;
    for (;;) {
        const uint8_t *pixels = nullptr;
        esp_err_t err = camera.acquire(pixels);
        if (err != ESP_OK) {
            show_error("Camera capture failed", err);
            vTaskDelete(nullptr);
            return;
        }
        const int64_t now = esp_timer_get_time();
        // Process every delivered 5 fps frame. Applying another exact 200 ms
        // gate to that stream would drop frames due to small arrival jitter.
        if (camera.delivery_rate_limited || now - last_preview >= kPreviewPeriodUs) {
            const bool submit = xSemaphoreTake(inference_free, 0) == pdTRUE;
            // A successful take guarantees the completion was already queued.
            // Read it before replacing either the inference or analysing image.
            Results received;
            if (xQueueReceive(result_queue, &received, 0) == pdTRUE) {
                latest = received;
                show_completed(latest);
            }
            // When idle, render directly into the private inference buffer.
            // Both display and detector only read it. While inference runs,
            // subsequent preview frames use the other PPA buffer instead.
            uint16_t *destination = submit ? inference_pixels : preview_pixels;
            const int64_t ppa_start = esp_timer_get_time();
            const esp_err_t transform_err = prepare_frame(pixels, destination);
            if (submit) {
                inference_ppa_ms = (esp_timer_get_time() - ppa_start) / 1000.0f;
                inference_captured_us = now;
            }
            // The single PPA operation finishes before returning the source.
            err = camera.release();
            if (err != ESP_OK) {
                show_error("Camera buffer release failed", err);
                vTaskDelete(nullptr);
                return;
            }
            if (transform_err != ESP_OK) {
                show_error("PPA camera transform failed", transform_err);
                vTaskDelete(nullptr);
                return;
            }
            last_preview = now;
            if (submit) {
                inference_ready_us = esp_timer_get_time();
                xTaskNotifyGive(detector_task);
            }
            ++frames;
            if (now - fps_start >= 1000000) {
                fps = frames * 1000000.0f / (now - fps_start);
                fps_start = now;
                frames = 0;
            }
            update_screen(latest, fps, destination, submit);
        } else {
            err = camera.release();
            if (err != ESP_OK) {
                show_error("Camera buffer release failed", err);
                vTaskDelete(nullptr);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
}

extern "C" void app_main()
{
    if (!bsp_display_start()) {
        ESP_LOGE(TAG, "Display initialization failed");
        return;
    }
    ESP_ERROR_CHECK(bsp_display_backlight_on());
    create_screen();
    esp_err_t err = camera.start();
    if (err != ESP_OK) {
        show_error("Camera initialization failed", err);
        return;
    }
    if (camera.width < kInferenceCropSize || camera.height < kInferenceCropSize || camera.stride % 2) {
        show_error("Camera must provide at least 640 x 640 RGB565", ESP_ERR_INVALID_SIZE);
        return;
    }
    // Equivalent field of view to centre crop 720 -> halve 360 -> crop 320.
    crop_x = (camera.width - kInferenceCropSize) / 2;
    crop_y = (camera.height - kInferenceCropSize) / 2;
    size_t alignment = 0;
    err = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alignment);
    if (err != ESP_OK || !alignment) {
        show_error("PPA buffer alignment failed", err != ESP_OK ? err : ESP_ERR_INVALID_SIZE);
        return;
    }
    preview_buffer_size = (image_bytes + alignment - 1) & ~(alignment - 1);
    preview_pixels = static_cast<uint16_t *>(heap_caps_aligned_calloc(alignment, 1, preview_buffer_size,
                                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    canvas_pixels = static_cast<uint16_t *>(heap_caps_calloc(1, image_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    inference_pixels = static_cast<uint16_t *>(heap_caps_aligned_calloc(alignment, 1, preview_buffer_size,
                                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    analysing_pixels = static_cast<uint16_t *>(heap_caps_calloc(1, image_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    previous_pixels = static_cast<uint16_t *>(heap_caps_calloc(1, image_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    result_queue = xQueueCreate(1, sizeof(Results));
    inference_free = xSemaphoreCreateBinary();
    if (!preview_pixels || !canvas_pixels || !inference_pixels || !analysing_pixels || !previous_pixels || !result_queue || !inference_free) {
        show_error("Frame buffer allocation failed", ESP_ERR_NO_MEM);
        return;
    }
    ppa_client_config_t ppa_config = {};
    ppa_config.oper_type = PPA_OPERATION_SRM;
    err = ppa_register_client(&ppa_config, &preview_ppa);
    if (err != ESP_OK) {
        show_error("PPA initialization failed", err);
        return;
    }
    create_preview();
    xSemaphoreGive(inference_free);
    // ESP-DL loading/inference can compute for longer than the watchdog period
    // without blocking. Time-slice with IDLE1 even inside those calls; a delay
    // after detector.run() alone cannot protect against a long first inference.
    if (xTaskCreatePinnedToCore(detect_task, "coco_detect", 16384, nullptr, tskIDLE_PRIORITY, &detector_task, 1) != pdPASS) {
        show_error("Detector task creation failed", ESP_ERR_NO_MEM);
        return;
    }
    if (xTaskCreatePinnedToCore(capture_task, "camera_preview", 8192, nullptr, 5, nullptr, 0) != pdPASS) {
        vTaskDelete(detector_task);
        show_error("Camera task creation failed", ESP_ERR_NO_MEM);
        return;
    }
    ESP_LOGI(TAG, "PPA preview %d x %d; detector uses latest available frame", image_width, image_height);
    ESP_LOGI(TAG, "Inference PPA crop (%d,%d) %d x %d -> %d x %d shared preview/detection area; buffer %u bytes",
             crop_x, crop_y, kInferenceCropSize, kInferenceCropSize, kModelSize, kModelSize,
             unsigned(kInferenceBytes));
}
