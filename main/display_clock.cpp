#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "sdkconfig.h"

// The pinned BSP hardcodes its DPI clock. Override the configuration at this
// public driver boundary so dependency reinstalls cannot discard our setting.
extern "C" esp_err_t __real_esp_lcd_new_panel_dpi(esp_lcd_dsi_bus_handle_t bus,
                                                const esp_lcd_dpi_panel_config_t *config,
                                                esp_lcd_panel_handle_t *panel);

extern "C" esp_err_t __wrap_esp_lcd_new_panel_dpi(esp_lcd_dsi_bus_handle_t bus,
                                                const esp_lcd_dpi_panel_config_t *config,
                                                esp_lcd_panel_handle_t *panel)
{
    // Identify the original Tab5 ILI9881C timing. Other Tab5 panel revisions
    // use different clocks/porches and retain their supplied configuration.
    if (!config) return __real_esp_lcd_new_panel_dpi(bus, config, panel);
    const auto &t = config->video_timing;
    const bool original_panel = config->dpi_clock_freq_mhz == 60 &&
                                t.h_size == 720 && t.v_size == 1280 &&
                                t.hsync_back_porch == 140 && t.hsync_pulse_width == 40 &&
                                t.hsync_front_porch == 40 && t.vsync_back_porch == 20 &&
                                t.vsync_pulse_width == 4 && t.vsync_front_porch == 20;
    if (!original_panel) {
        ESP_LOGW("display_clock", "Unrecognized panel timing; retaining BSP pixel clock %.1f MHz",
                 double(config->dpi_clock_freq_mhz));
        return __real_esp_lcd_new_panel_dpi(bus, config, panel);
    }

    auto adjusted = *config;
    adjusted.dpi_clock_freq_mhz = CONFIG_TAB5_LCD_PIXEL_CLOCK_MHZ;
    const double h_total = t.h_size + t.hsync_back_porch + t.hsync_pulse_width + t.hsync_front_porch;
    const double v_total = t.v_size + t.vsync_back_porch + t.vsync_pulse_width + t.vsync_front_porch;
    const double refresh_hz = adjusted.dpi_clock_freq_mhz * 1000000.0 / (h_total * v_total);
    ESP_LOGI("display_clock", "ILI9881C pixel clock %.1f -> %.1f MHz; calculated scanout %.2f Hz, RGB565 payload %.1f MB/s",
             double(config->dpi_clock_freq_mhz), double(adjusted.dpi_clock_freq_mhz), refresh_hz,
             t.h_size * t.v_size * 2.0 * refresh_hz / 1000000.0);
    return __real_esp_lcd_new_panel_dpi(bus, &adjusted, panel);
}
