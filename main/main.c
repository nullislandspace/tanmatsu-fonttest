#include <stdio.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "hal/lcd_types.h"
#include "nvs_flash.h"
#include "bench_console.h"
#include "bench_report.h"
#include "bench_runner.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "pax_types.h"
#include "portmacro.h"

// Constants
static char const TAG[] = "main";

#if defined(CONFIG_BSP_TARGET_KAMI)
#define BLACK 0
#define WHITE 1
#define RED   2
#else
#define BLACK 0xFF000000
#define WHITE 0xFFFFFFFF
#define RED   0xFFFF0000
#endif

// Global variables
static size_t                     display_h_res        = 0;
static size_t                     display_v_res        = 0;
static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t   display_data_endian  = 0;
static pax_buf_t                  fb                   = {0};
static QueueHandle_t              input_event_queue    = NULL;

#if defined(CONFIG_BSP_TARGET_KAMI)
// Temporary addition for supporting epaper devices (irrelevant for Tanmatsu)
static pax_col_t palette[] = {0xffffffff, 0xff000000, 0xffff0000};  // white, black, red
#endif

static void blit(void) {
    esp_err_t res = bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to blit to display: %d", res);
    }
}

// The benchmark is driven entirely from the debug console, so the panel exists
// only to say what the badge is doing and how to get out of it. Nothing here
// runs during a measurement: the runner draws into its own off-screen tiles and
// never touches the display.
static void display_status(void) {
    if (pax_buf_get_width(&fb) <= 0) {
        ESP_LOGI(TAG, "PAX font benchmark ready, %u cells", bench_cell_count());
        return;
    }

    char line[64];
    snprintf(line, sizeof(line), "%u cells, waiting for BENCHRUN", bench_cell_count());

    pax_background(&fb, BLACK);
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 20, 0, 0, "PAX font rendering benchmark");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 28, line);
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 50, "Host: make testrun");
    pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 72, "F1 or EXIT: back to launcher");
    blit();
}

// RENDERDEMO: every built-in font at an integer and a fractional scale, drawn to
// the real panel. Not a measurement -- this exists so a human can look at the
// badge after a risky optimization and see that text still renders as text,
// which is the one failure mode a framebuffer diff cannot flag (every cell wrong
// in the same way looks self-consistent).
static void render_demo(void) {
    if (pax_buf_get_width(&fb) <= 0) {
        ESP_LOGW(TAG, "No display buffer for RENDERDEMO");
        return;
    }

    static struct {
        pax_font_t const* font;
        char const*       name;
    } const FONTS[] = {
        {pax_font_sky,             "sky"            },
        {pax_font_sky_mono,        "sky_mono"       },
        {pax_font_saira_regular,   "saira_regular"  },
        {pax_font_saira_condensed, "saira_condensed"},
        {pax_font_marker,          "marker"         },
    };

    pax_background(&fb, BLACK);
    float y = 0;
    for (size_t i = 0; i < sizeof(FONTS) / sizeof(FONTS[0]); i++) {
        pax_font_t const* font = FONTS[i].font;
        char              line[96];

        // Integer scale takes the blit fast paths, the 1.5x below takes the
        // shader rasteriser: both are on screen at once, so a change that only
        // breaks one of them is still visible.
        snprintf(line, sizeof(line), "%s 1x: Sphinx of black quartz, judge my vow. 0123456789",
                 FONTS[i].name);
        pax_draw_text(&fb, WHITE, font, font->default_size, 0, y, line);
        y += font->default_size + 4;

        snprintf(line, sizeof(line), "%s 1.5x: Sphinx of black quartz, judge my vow.", FONTS[i].name);
        pax_draw_text(&fb, WHITE, font, font->default_size * 1.5f, 0, y, line);
        y += font->default_size * 1.5f + 12;
    }

    blit();
    ESP_LOGI(TAG, "RENDERDEMO drawn");
}

static void display_message(const char* message) {
    if (pax_buf_get_width(&fb) > 0) {
        pax_background(&fb, BLACK);
        pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 0, message);
        blit();
    } else {
        ESP_LOGI(TAG, "Message: %s", message);
    }
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

    // Claim the benchmark tile buffers before anything else can fragment the
    // internal heap. The internal tile is the tight one: only a few hundred KB
    // are free and it needs 192 KB contiguous, so it has to be asked for first.
    if (!bench_buffers_allocate()) {
        // bench_buffers_allocate() has already emitted an ABORT record saying
        // how much contiguous internal memory was actually available.
        ESP_LOGE(TAG, "Benchmark buffer allocation failed, returning to launcher");
        vTaskDelay(pdMS_TO_TICKS(300));
        bsp_device_restart_to_launcher();
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
#if defined(CONFIG_BSP_TARGET_KAMI)
        // Temporary addition for supporting Kami
        fb.palette      = palette;
        fb.palette_size = sizeof(palette) / sizeof(pax_col_t);
#endif
        pax_buf_set_orientation(&fb, orientation);
    } else {
        ESP_LOGI(TAG, "This board has no display support");
    }

    // Get input event queue from BSP
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    // LEDs
    bsp_led_set_pixel(0, 0xFF0000);  // Red
    bsp_led_set_pixel(1, 0x00FF00);  // Green
    bsp_led_set_pixel(2, 0x0000FF);  // Blue
    bsp_led_set_pixel(3, 0xFFFF00);  // Yellow
    bsp_led_set_pixel(4, 0x00FFFF);  // Magenta
    bsp_led_set_pixel(5, 0xFF00FF);  // Cyan
    bsp_led_send();                  // Send data to the coprocessor
    bsp_led_set_mode(false);         // Take control over all LEDs by disabling automatic mode

    // This app never uses the network, so keep the C6 radio powered down: a
    // running radio means coprocessor traffic and interrupts that would add
    // variance to the measurements.
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);

    // Benchmark harness: the runner waits for a command, the console listener
    // supplies it. Nothing is measured until the host asks for a run.
    bench_runner_start();
    bench_runner_set_demo(render_demo);
    bench_console_start(bench_runner_command, bench_cell_count());

    // Main section of the app

    // This example shows how to read from the BSP event queue to read input events

    // If you want to run something at an interval in this same main thread you can replace portMAX_DELAY with an amount
    // of ticks to wait, for example pdMS_TO_TICKS(1000)

    display_status();

    while (1) {
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, portMAX_DELAY) == pdTRUE) {
            switch (event.type) {
                case INPUT_EVENT_TYPE_KEYBOARD: {
                    if (event.args_keyboard.ascii != '\b' ||
                        event.args_keyboard.ascii != '\t') {  // Ignore backspace & tab keyboard events
                        if (pax_buf_get_height(&fb) <= 128) {
                            char text[64];
                            snprintf(text, sizeof(text), "%c %s M=%02" PRIx32, event.args_keyboard.ascii,
                                     event.args_keyboard.utf8, event.args_keyboard.modifiers);
                            display_message(text);
                        } else {
                            ESP_LOGI(TAG, "Keyboard event %c (%02x) %s", event.args_keyboard.ascii,
                                     (uint8_t)event.args_keyboard.ascii, event.args_keyboard.utf8);
                            if (pax_buf_get_width(&fb) > 0) {
                                pax_simple_rect(&fb, BLACK, 0, 0, pax_buf_get_width(&fb), 72);
                                pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 0, "Keyboard event");
                                char text[64];
                                snprintf(text, sizeof(text), "ASCII:     %c (0x%02x)", event.args_keyboard.ascii,
                                         (uint8_t)event.args_keyboard.ascii);
                                pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 18, text);
                                snprintf(text, sizeof(text), "UTF-8:     %s", event.args_keyboard.utf8);
                                pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 36, text);
                                snprintf(text, sizeof(text), "Modifiers: 0x%0" PRIX32, event.args_keyboard.modifiers);
                                pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 54, text);
                                blit();
                            }
                        }
                    }
                    break;
                }
                case INPUT_EVENT_TYPE_NAVIGATION: {
                    ESP_LOGI(TAG, "Navigation event %0" PRIX32 ": %s", (uint32_t)event.args_navigation.key,
                             event.args_navigation.state ? "pressed" : "released");

                    if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F1) {
                        bsp_device_restart_to_launcher();
                    }
                    if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F2) {
                        bsp_input_set_backlight_brightness(0);
                    }
                    if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F3) {
                        bsp_input_set_backlight_brightness(100);
                    }

                    if (pax_buf_get_height(&fb) <= 128) {
                        char text[64];
                        snprintf(text, sizeof(text), "%02" PRIx32 " %s M=%02" PRIx32,
                                 (uint32_t)event.args_navigation.key, event.args_navigation.state ? "P" : "R",
                                 event.args_navigation.modifiers);
                        display_message(text);
                    } else {
                        pax_simple_rect(&fb, BLACK, 0, 100, pax_buf_get_width(&fb), 72);
                        pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 100 + 0, "Navigation event");
                        char text[64];
                        snprintf(text, sizeof(text), "Key:       0x%0" PRIX32, (uint32_t)event.args_navigation.key);
                        pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 100 + 18, text);
                        snprintf(text, sizeof(text), "State:     %s",
                                 event.args_navigation.state ? "pressed" : "released");
                        pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 100 + 36, text);
                        snprintf(text, sizeof(text), "Modifiers: 0x%0" PRIX32, event.args_navigation.modifiers);
                        pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 100 + 54, text);
                        blit();
                    }
                    break;
                }
                case INPUT_EVENT_TYPE_ACTION: {
                    ESP_LOGI(TAG, "Action event 0x%0" PRIX32 ": %s", (uint32_t)event.args_action.type,
                             event.args_action.state ? "yes" : "no");
                    if (pax_buf_get_height(&fb) <= 128) {
                        char text[64];
                        snprintf(text, sizeof(text), "%02" PRIx32 " %s" PRIx32, (uint32_t)event.args_action.type,
                                 event.args_action.state ? "Y" : "N");
                        display_message(text);
                    } else {
                        pax_simple_rect(&fb, BLACK, 0, 200 + 0, pax_buf_get_width(&fb), 72);
                        pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 200 + 0, "Action event");
                        char text[64];
                        snprintf(text, sizeof(text), "Type:      0x%0" PRIX32, (uint32_t)event.args_action.type);
                        pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 200 + 36, text);
                        snprintf(text, sizeof(text), "State:     %s", event.args_action.state ? "yes" : "no");
                        pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 200 + 54, text);
                        blit();
                    }
                    break;
                }
                case INPUT_EVENT_TYPE_SCANCODE: {
                    ESP_LOGI(TAG, "Scancode event 0x%0" PRIX32, (uint32_t)event.args_scancode.scancode);
                    if (pax_buf_get_width(&fb) > 0) {
                        pax_simple_rect(&fb, BLACK, 0, 300 + 0, pax_buf_get_width(&fb), 72);
                        pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 300 + 0, "Scancode event");
                        char text[64];
                        snprintf(text, sizeof(text), "Scancode:  0x%0" PRIX32, (uint32_t)event.args_scancode.scancode);
                        pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 300 + 36, text);
                        blit();
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
}
