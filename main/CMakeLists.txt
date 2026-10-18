idf_component_register(SRCS "main.cpp" "camera.cpp" "display_clock.cpp"
    INCLUDE_DIRS "."
    REQUIRES m5stack_tab5 coco_detect esp_video esp_cam_sensor lvgl esp_timer esp_psram esp_driver_ppa esp_mm esp_lcd esp_rom)

option(TAB5_INFERENCE_AUDIT "Run frozen-input integrity tests once at startup" OFF)
option(TAB5_AUDIT_DL_O2 "Use -O2 for ESP-DL during the inference audit" OFF)
target_compile_definitions(${COMPONENT_LIB} PRIVATE TAB5_INFERENCE_AUDIT=$<BOOL:${TAB5_INFERENCE_AUDIT}>)

# Force archive extraction: the BSP references the wrapped symbol after the
# main archive may already have been scanned by the linker.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_lcd_new_panel_dpi"
    "-Wl,--undefined=__wrap_esp_lcd_new_panel_dpi")
