#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/lcd_types.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "pax_types.h"
#include "portmacro.h"
#include "screens.h"
#include "storage.h"
#include "ui_theme.h"

#if !defined(CONFIG_BSP_TARGET_TANMATSU) && !defined(CONFIG_BSP_TARGET_KONSOOL)
#error "This app reads the SD card wiring for Tanmatsu/Konsool directly (see storage.c) and does not support other boards."
#endif

// Constants
static char const TAG[] = "main";

// Global variables
static size_t                     display_h_res        = 0;
static size_t                     display_v_res        = 0;
static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t   display_data_endian  = 0;
static size_t                     ui_h_res             = 0;
static size_t                     ui_v_res             = 0;
static pax_buf_t                  fb                   = {0};
static QueueHandle_t              input_event_queue    = NULL;
static app_state_t                app_state            = {0};

static void blit(void) {
    esp_err_t res = bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to blit to display: %d", res);
    }
}

static void present_app(void *ctx) {
    (void)ctx;
    blit();
}

static bool splash_skip_requested(void) {
    if (!input_event_queue) {
        return false;
    }

    bsp_input_event_t event;
    return xQueueReceive(input_event_queue, &event, 0) == pdTRUE;
}

static void draw_logo_word(float y, const char *text) {
    float size = 118;
    pax_vec2f text_size = pax_text_size(GUIDE_FONT_TITLE, size, text);
    float x = ((float)ui_h_res - text_size.x) * 0.5f;
    if (x < 0) {
        x = 0;
    }

    pax_draw_text(&fb, pax_col_rgb(0x30, 0x20, 0x00), GUIDE_FONT_TITLE, size, x - 6, y - 6, text);
    pax_draw_text(&fb, pax_col_rgb(0x30, 0x20, 0x00), GUIDE_FONT_TITLE, size, x + 6, y - 6, text);
    pax_draw_text(&fb, pax_col_rgb(0x30, 0x20, 0x00), GUIDE_FONT_TITLE, size, x - 6, y + 6, text);
    pax_draw_text(&fb, pax_col_rgb(0x30, 0x20, 0x00), GUIDE_FONT_TITLE, size, x + 6, y + 6, text);
    pax_draw_text(&fb, pax_col_rgb(0x7a, 0x58, 0x00), GUIDE_FONT_TITLE, size, x - 3, y, text);
    pax_draw_text(&fb, pax_col_rgb(0x7a, 0x58, 0x00), GUIDE_FONT_TITLE, size, x + 3, y, text);
    pax_draw_text(&fb, pax_col_rgb(0x7a, 0x58, 0x00), GUIDE_FONT_TITLE, size, x, y - 3, text);
    pax_draw_text(&fb, pax_col_rgb(0x7a, 0x58, 0x00), GUIDE_FONT_TITLE, size, x, y + 3, text);
    pax_draw_text(&fb, GUIDE_COLOR_RED_DIM, GUIDE_FONT_TITLE, size, x + 4, y + 5, text);
    pax_draw_text(&fb, GUIDE_COLOR_AMBER, GUIDE_FONT_TITLE, size, x + 2, y + 2, text);
    pax_draw_text(&fb, GUIDE_COLOR_YELLOW, GUIDE_FONT_TITLE, size, x, y, text);
}

static void boot_splash(void) {
    if (pax_buf_get_width(&fb) == 0) {
        ESP_LOGI(TAG, "Boot: DON'T PANIC");
        return;
    }

    theme_clear(&fb);
    draw_logo_word(54, "DON'T");
    draw_logo_word(196, "PANIC!");
    theme_draw_scanlines(&fb, (float)ui_h_res, (float)ui_v_res);
    blit();

    TickType_t start = xTaskGetTickCount();
    TickType_t limit = pdMS_TO_TICKS(10000);
    while ((xTaskGetTickCount() - start) < limit) {
        if (splash_skip_requested()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void boot_message(const char *message) {
    if (pax_buf_get_width(&fb) == 0) {
        ESP_LOGI(TAG, "Boot: %s", message);
        return;
    }
    theme_clear(&fb);
    theme_draw_panel(&fb, 6, 6, (float)ui_h_res - 12, (float)ui_v_res - 12, GUIDE_COLOR_BLUE_DIM);
    pax_draw_text(&fb, GUIDE_COLOR_BLUE, GUIDE_FONT, 20, GUIDE_MARGIN, GUIDE_MARGIN, "THE GUIDE");
    pax_draw_text(&fb, GUIDE_COLOR_GREEN, GUIDE_FONT, 16, GUIDE_MARGIN, GUIDE_MARGIN + 42, message);
    theme_draw_scanlines(&fb, (float)ui_h_res, (float)ui_v_res);
    blit();
}

void app_main(void) {
    // Start the GPIO interrupt service
    gpio_install_isr_service(0);

    // Initialize the Non Volatile Storage partition
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        res = nvs_flash_erase();
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase NVS flash: %d", res);
            return;
        }
        res = nvs_flash_init();
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS flash: %d", res);
        return;
    }

    // Initialize the Board Support Package
    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_24_888RGB,
                .num_fbs                = 1,
            },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BSP: %d", res);
        return;
    }

    // Get display parameters and rotation
    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    if (res != ESP_ERR_NOT_SUPPORTED) {
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get display parameters: %d", res);
            return;
        }

        // Convert ESP-IDF color format into PAX buffer type
        pax_buf_type_t format = PAX_BUF_24_888RGB;
        switch (display_color_format) {
            case BSP_DISPLAY_COLOR_FORMAT_1_PAL:
                format = PAX_BUF_1_PAL;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_2_PAL:
                format = PAX_BUF_2_PAL;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_4_PAL:
                format = PAX_BUF_4_PAL;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_8_PAL:
                format = PAX_BUF_8_PAL;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_16_PAL:
                format = PAX_BUF_16_PAL;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_1_GREY:
                format = PAX_BUF_1_GREY;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_2_GREY:
                format = PAX_BUF_2_GREY;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_4_GREY:
                format = PAX_BUF_4_GREY;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_8_GREY:
                format = PAX_BUF_8_GREY;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_8_332RGB:
                format = PAX_BUF_8_332RGB;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_16_565RGB:
                format = PAX_BUF_16_565RGB;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_4_1111ARGB:
                format = PAX_BUF_4_1111ARGB;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_8_2222ARGB:
                format = PAX_BUF_8_2222ARGB;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_16_4444ARGB:
                format = PAX_BUF_16_4444ARGB;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_24_888RGB:
                format = PAX_BUF_24_888RGB;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_32_8888ARGB:
                format = PAX_BUF_32_8888ARGB;
                break;
            case BSP_DISPLAY_COLOR_FORMAT_18_666RGB:
            default:
                ESP_LOGW(TAG, "BSP requests color format not supported by PAX (%u)", format);
                break;
        }

        // Convert BSP display rotation format into PAX orientation type
        bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();
        pax_orientation_t      orientation      = PAX_O_UPRIGHT;
        switch (display_rotation) {
            case BSP_DISPLAY_ROTATION_90:
                orientation = PAX_O_ROT_CCW;
                break;
            case BSP_DISPLAY_ROTATION_180:
                orientation = PAX_O_ROT_HALF;
                break;
            case BSP_DISPLAY_ROTATION_270:
                orientation = PAX_O_ROT_CW;
                break;
            case BSP_DISPLAY_ROTATION_0:
            default:
                orientation = PAX_O_UPRIGHT;
                break;
        }

        // Initialize graphics stack
        printf("Initializing framebuffer with w=%d h=%d format=%d endian=%d orientation=%d\n", display_h_res,
               display_v_res, format, display_data_endian, orientation);
        pax_buf_init(&fb, NULL, display_h_res, display_v_res, format);
        pax_buf_reversed(&fb, display_data_endian == BSP_DISPLAY_ENDIAN_BIG);
        pax_buf_set_orientation(&fb, orientation);
        ui_h_res = (size_t)pax_buf_get_width(&fb);
        ui_v_res = (size_t)pax_buf_get_height(&fb);
        ESP_LOGI(TAG, "Framebuffer raw=%ux%u UI=%ux%u orientation=%d", (unsigned)display_h_res,
                 (unsigned)display_v_res, (unsigned)ui_h_res, (unsigned)ui_v_res, orientation);
    } else {
        ESP_LOGI(TAG, "This board has no display support");
    }

    // Get input event queue from BSP
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    // A calm green "sub-etha" glow instead of the default rainbow LEDs.
    for (int i = 0; i < 6; i++) {
        bsp_led_set_pixel(i, 0x104010);
    }
    bsp_led_send();
    bsp_led_set_mode(false);

    boot_splash();
    boot_message("Mounting Sub-Etha data cartridge...");
    storage_mount_sdcard();

    app_state_init(&app_state, &fb, (float)ui_h_res, (float)ui_v_res);
    screens_set_present_callback(&app_state, present_app, NULL);
    screens_refresh_datasets(&app_state);

    ESP_LOGI(TAG, "Rendering initial screen=%d", app_state.screen);
    screen_render(&app_state);
    blit();
    ESP_LOGI(TAG, "Initial screen rendered");

    while (1) {
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        bool dirty = false;

        switch (event.type) {
            case INPUT_EVENT_TYPE_NAVIGATION: {
                if (!event.args_navigation.state) {
                    // Ignore key-release events, only act on key-press.
                    break;
                }
                ESP_LOGI(TAG, "Navigation key=%d", event.args_navigation.key);
                if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F1) {
                    ESP_LOGI(TAG, "F1 pressed: restarting to launcher");
                    bsp_device_restart_to_launcher();
                } else if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F2) {
                    bsp_input_set_backlight_brightness(0);
                } else if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F3) {
                    bsp_input_set_backlight_brightness(100);
                } else {
                    screen_on_navigation(&app_state, event.args_navigation.key);
                    dirty = true;
                }
                break;
            }
            case INPUT_EVENT_TYPE_KEYBOARD: {
                ESP_LOGI(TAG, "Keyboard ascii=%d '%c'", (int)event.args_keyboard.ascii,
                         event.args_keyboard.ascii >= 0x20 && event.args_keyboard.ascii <= 0x7e
                             ? event.args_keyboard.ascii
                             : '.');
                screen_on_keyboard(&app_state, event.args_keyboard.ascii);
                dirty = true;
                break;
            }
            default:
                break;
        }

        if (dirty && app_state.frame_presented) {
            app_state.frame_presented = false;
        } else if (dirty && pax_buf_get_width(&fb) > 0) {
            screen_render(&app_state);
            blit();
        }
    }
}
