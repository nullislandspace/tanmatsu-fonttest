#include "bench_runner.h"

#include <string.h>

#include "bench_config.h"
#include "bench_console.h"
#include "bench_report.h"
#include "bsp/device.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static char const TAG[] = "bench";

static void*             s_tile_int   = NULL;
static void*             s_tile_psram = NULL;
static void*             s_scrub      = NULL;
static TaskHandle_t      s_task       = NULL;
static esp_pm_lock_handle_t s_pm_lock = NULL;

// Command handed from the console task to the driver task. Only one command is
// ever in flight: the console stops advertising readiness once a run starts.
static volatile bench_cmd_t s_pending     = BENCH_CMD_NONE;
static char                 s_pending_arg[32];

bool bench_buffers_allocate(void) {
    uint32_t const caps_int   = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_CACHE_ALIGNED;
    uint32_t const caps_psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_CACHE_ALIGNED;

    // Round the length up to a cache line as well as the start address: for a
    // buffer the display DMA may touch, a partial trailing line can drag
    // neighbouring data into a writeback.
    size_t const bytes = (BENCH_TILE_BYTES + 63) & ~(size_t)63;

    s_tile_int = heap_caps_aligned_alloc(64, bytes, caps_int);
    if (s_tile_int == NULL) {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        char   detail[160];
        snprintf(detail, sizeof(detail), "{\"need\":%u,\"int_largest\":%u,\"int_free\":%u,\"tile_dim\":%d}",
                 (unsigned)bytes, (unsigned)largest, (unsigned)free, BENCH_TILE_DIM);
        bench_report_abort("internal_tile_alloc_failed", detail);
        return false;
    }

    s_tile_psram = heap_caps_aligned_alloc(64, bytes, caps_psram);
    if (s_tile_psram == NULL) {
        bench_report_abort("psram_tile_alloc_failed", NULL);
        return false;
    }

    s_scrub = heap_caps_aligned_alloc(64, BENCH_SCRUB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_scrub == NULL) {
        bench_report_abort("scrub_alloc_failed", NULL);
        return false;
    }

    ESP_LOGI(TAG, "Tiles: internal %p, PSRAM %p, %u bytes each; scrub %p", s_tile_int, s_tile_psram, (unsigned)bytes,
             s_scrub);
    return true;
}

void* bench_tile_internal(void) {
    return s_tile_int;
}

void* bench_tile_psram(void) {
    return s_tile_psram;
}

unsigned bench_cell_count(void) {
    // Filled in once the matrix exists.
    return 0;
}

// Stream the scrub block to evict the previous cell's tile from L2, so every
// cell starts from a comparable cache state and cell ordering stops mattering.
static void scrub_cache(void) {
    volatile uint32_t* p     = (volatile uint32_t*)s_scrub;
    size_t             words = BENCH_SCRUB_BYTES / sizeof(uint32_t);
    for (size_t i = 0; i < words; i++) {
        p[i] = (uint32_t)i;
    }
    uint32_t sink = 0;
    for (size_t i = 0; i < words; i++) {
        sink += p[i];
    }
    // Keep the reads from being elided.
    __asm__ __volatile__("" : : "r"(sink) : "memory");
}

// Hand the badge back to the launcher, giving the USB FIFO time to drain first:
// the reset takes the peripheral away, and a truncated final record would leave
// the host client waiting for a sentinel that never arrives.
static void return_to_launcher(void) {
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "Returning to launcher");
    bsp_device_restart_to_launcher();
}

static void run_matrix(bool single_cell) {
    int64_t started = esp_timer_get_time();

    // Hold the CPU at its nominal frequency for the whole run: DFS is enabled
    // in this build (as it is in the launcher), and without the lock the clock
    // does not stay put and nothing is comparable between runs.
    if (s_pm_lock == NULL) {
        esp_err_t res = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "bench", &s_pm_lock);
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create PM lock: %d", res);
        }
    }
    if (s_pm_lock != NULL) {
        esp_pm_lock_acquire(s_pm_lock);
    }

    bench_console_suspend();

    float cpu_mhz = bench_measure_cpu_mhz(50000);
    bench_report_begin((uint16_t)bench_cell_count(), cpu_mhz, s_tile_int, s_tile_psram);

    scrub_cache();
    // Cells are measured here once the matrix lands.
    (void)single_cell;

    bench_report_end((uint16_t)bench_cell_count(), esp_timer_get_time() - started, "ok");

    bench_console_resume();
    if (s_pm_lock != NULL) {
        esp_pm_lock_release(s_pm_lock);
    }

    return_to_launcher();
}

static void runner_task(void* arg) {
    (void)arg;
    while (true) {
        if (s_pending == BENCH_CMD_NONE) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        bench_cmd_t cmd = s_pending;
        s_pending       = BENCH_CMD_NONE;

        switch (cmd) {
            case BENCH_CMD_RUN:     run_matrix(false); break;
            case BENCH_CMD_RUN_ONE: run_matrix(true); break;

            case BENCH_CMD_IDLE_TIMEOUT:
                bench_report_emitf("END", "{\"t\":\"end\",\"cells\":0,\"status\":\"idle_timeout\"}");
                return_to_launcher();
                break;

            default:
                ESP_LOGW(TAG, "Command %d not implemented yet", (int)cmd);
                break;
        }
    }
}

void bench_runner_start(void) {
    xTaskCreatePinnedToCore(runner_task, "bench", BENCH_TASK_STACK, NULL, BENCH_TASK_PRIO, &s_task, BENCH_TASK_CORE);
}

void bench_runner_command(bench_cmd_t cmd, char const* arg) {
    strncpy(s_pending_arg, arg ? arg : "", sizeof(s_pending_arg) - 1);
    s_pending_arg[sizeof(s_pending_arg) - 1] = '\0';
    s_pending                                = cmd;
}
