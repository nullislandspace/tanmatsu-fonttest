#include "bench_console.h"

#include <string.h>

#include "bench_config.h"
#include "bench_report.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static char const TAG[] = "bench_console";

#define CONSOLE_LINE_MAX 64
#define READ_CHUNK    32
#define BANNER_PERIOD pdMS_TO_TICKS(2000)

static bench_console_cb_t s_callback      = NULL;
static unsigned           s_cells         = 0;
static TaskHandle_t       s_task          = NULL;
static bool               s_run_started   = false;
static int64_t            s_idle_deadline = 0;

// Seconds left before the app hands the badge back to the launcher.
static unsigned idle_left_s(void) {
    int64_t left = s_idle_deadline - esp_timer_get_time();
    return left > 0 ? (unsigned)(left / 1000000) : 0;
}

static struct {
    char const* token;
    bench_cmd_t cmd;
} const COMMANDS[] = {
    {"PING",        BENCH_CMD_PING       },
    {"BENCHRUN",    BENCH_CMD_RUN        },
    {"BENCHRUN1",   BENCH_CMD_RUN_ONE    },
    {"BENCHCELL",   BENCH_CMD_RUN_CELL   },
    {"LISTCELLS",   BENCH_CMD_LIST_CELLS },
    {"DUMPCORPUS",  BENCH_CMD_DUMP_CORPUS},
    {"DUMPFB",      BENCH_CMD_DUMP_FB    },
    {"RENDERDEMO",  BENCH_CMD_RENDER_DEMO},
    {"EXIT",        BENCH_CMD_EXIT       },
};

static void handle_line(char* line) {
    // Split the token from its argument at the first space.
    char* arg = strchr(line, ' ');
    if (arg != NULL) {
        *arg++ = '\0';
        while (*arg == ' ') {
            arg++;
        }
    } else {
        arg = line + strlen(line);
    }

    if (line[0] == '\0') {
        return;
    }

    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (strcmp(line, COMMANDS[i].token) != 0) {
            continue;
        }

        // Any recognised command counts as attention: push the idle deadline out
        // again so an interactive debugging session is not cut short.
        s_idle_deadline = esp_timer_get_time() + (int64_t)BENCH_IDLE_TIMEOUT_MS * 1000;

        if (COMMANDS[i].cmd == BENCH_CMD_PING) {
            bench_report_pong((uint16_t)s_cells, idle_left_s());
            return;
        }

        // Stop advertising readiness once a run has been asked for: the banner
        // would otherwise interleave with result records.
        if (COMMANDS[i].cmd == BENCH_CMD_RUN || COMMANDS[i].cmd == BENCH_CMD_RUN_ONE ||
            COMMANDS[i].cmd == BENCH_CMD_EXIT) {
            s_run_started = true;
        }

        if (s_callback != NULL) {
            s_callback(COMMANDS[i].cmd, arg);
        }
        return;
    }

    ESP_LOGW(TAG, "Unknown command: %s", line);
}

static void console_task(void* arg) {
    (void)arg;
    s_idle_deadline = esp_timer_get_time() + (int64_t)BENCH_IDLE_TIMEOUT_MS * 1000;

    usb_serial_jtag_driver_config_t config = {
        .rx_buffer_size = 256,
        .tx_buffer_size = 1024,
    };
    esp_err_t res = usb_serial_jtag_driver_install(&config);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB-serial/JTAG driver: %d", res);
        vTaskDelete(NULL);
        return;
    }

    char   line[CONSOLE_LINE_MAX];
    size_t line_len = 0;

    while (true) {
        uint8_t buf[READ_CHUNK];
        // Timed read rather than portMAX_DELAY so the READY banner can be
        // emitted while idle. During a run this task is suspended, so the
        // wake-ups never land inside a measurement.
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), BANNER_PERIOD);

        if (n <= 0) {
            if (s_run_started) {
                continue;
            }

            if (idle_left_s() == 0) {
                // Nobody asked for a run. Hand the badge back rather than
                // sitting here: this app does not speak BadgeLink, so an
                // unattended cycle that failed earlier would otherwise need a
                // button press or a power cycle to recover.
                ESP_LOGW(TAG, "No command within %d ms, returning to launcher", BENCH_IDLE_TIMEOUT_MS);
                s_run_started = true;  // stop the banner while we hand over
                if (s_callback != NULL) {
                    s_callback(BENCH_CMD_IDLE_TIMEOUT, "");
                }
                continue;
            }

            bench_report_ready((uint16_t)s_cells, idle_left_s());
            continue;
        }

        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                line[line_len] = '\0';
                handle_line(line);
                line_len = 0;
            } else if (line_len < sizeof(line) - 1) {
                line[line_len++] = c;
            } else {
                // Line overran the buffer; drop it and resync on the next newline.
                line_len = 0;
            }
        }
    }
}

void bench_console_start(bench_console_cb_t cb, unsigned cells) {
    s_callback = cb;
    s_cells    = cells;
    xTaskCreate(console_task, "bench_console", BENCH_CONSOLE_STACK, NULL, BENCH_CONSOLE_PRIO, &s_task);
}

void bench_console_suspend(void) {
    if (s_task != NULL) {
        vTaskSuspend(s_task);
    }
}

void bench_console_resume(void) {
    if (s_task != NULL) {
        vTaskResume(s_task);
    }
}

void bench_console_grace(unsigned ms) {
    s_idle_deadline = esp_timer_get_time() + (int64_t)ms * 1000;
    s_run_started   = false;  // advertise readiness again, so the host can tell we are still here
    bench_console_resume();
}
