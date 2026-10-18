idf_component_register(SRCS "main.cpp" "camera.cpp"
    INCLUDE_DIRS "."
    REQUIRES m5stack_tab5 coco_detect esp_video lvgl esp_timer esp_psram esp_driver_ppa esp_mm)
