#include "bench_report.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bench_build_info.h"
#include "bench_config.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#if defined(CONFIG_SPIRAM)
#include "esp_psram.h"
#endif

// Build-time identity, injected by the CMake git block. Defaults keep the file
// compilable outside that setup.
#ifndef BENCH_APP_GIT
#define BENCH_APP_GIT "unknown"
#endif
#ifndef BENCH_APP_DIRTY
#define BENCH_APP_DIRTY 0
#endif
#ifndef BENCH_APP_BRANCH
#define BENCH_APP_BRANCH "unknown"
#endif
#ifndef BENCH_PAX_GIT
#define BENCH_PAX_GIT "unknown"
#endif
#ifndef BENCH_PAX_DIRTY
#define BENCH_PAX_DIRTY 0
#endif
#ifndef BENCH_PAX_BRANCH
#define BENCH_PAX_BRANCH "unknown"
#endif

// Compiler optimization level, from the sdkconfig fragment in use.
#if defined(CONFIG_COMPILER_OPTIMIZATION_SIZE)
#define BENCH_OPT_NAME "Os"
#elif defined(CONFIG_COMPILER_OPTIMIZATION_DEBUG)
#define BENCH_OPT_NAME "Og"
#elif defined(CONFIG_COMPILER_OPTIMIZATION_PERF)
#define BENCH_OPT_NAME "O2"
#elif defined(CONFIG_COMPILER_OPTIMIZATION_NONE)
#define BENCH_OPT_NAME "O0"
#else
#define BENCH_OPT_NAME "unknown"
#endif

// PAX build configuration, resolved at compile time so it cannot drift from the
// binary. The async renderer is a tri-state choice in PAX's Kconfig.
#if defined(CONFIG_PAX_COMPILE_ASYNC_RENDERER_MULTITHREAD)
#define BENCH_PAX_ASYNC "multithread"
#elif defined(CONFIG_PAX_COMPILE_ASYNC_RENDERER_SINGLETHREAD)
#define BENCH_PAX_ASYNC "singlethread"
#else
#define BENCH_PAX_ASYNC "none"
#endif

#define BENCH_BOOL(x) ((x) ? "true" : "false")

#ifdef CONFIG_PAX_BOUNDS_CHECK
#define BENCH_PAX_BOUNDS 1
#else
#define BENCH_PAX_BOUNDS 0
#endif
#ifdef CONFIG_PAX_COMPILE_ORIENTATION
#define BENCH_PAX_ORIENT 1
#else
#define BENCH_PAX_ORIENT 0
#endif
#ifdef CONFIG_PAX_RANGE_SETTER
#define BENCH_PAX_RSETTER 1
#else
#define BENCH_PAX_RSETTER 0
#endif
#ifdef CONFIG_PAX_RANGE_MERGER
#define BENCH_PAX_RMERGER 1
#else
#define BENCH_PAX_RMERGER 0
#endif
#ifdef CONFIG_PAX_USE_FIXED_POINT
#define BENCH_PAX_FIXPT 1
#else
#define BENCH_PAX_FIXPT 0
#endif
#ifdef CONFIG_PAX_USE_LONG_FIXED_POINT
#define BENCH_PAX_LONGFIXPT 1
#else
#define BENCH_PAX_LONGFIXPT 0
#endif
#ifdef CONFIG_PM_ENABLE
#define BENCH_PM_ENABLE 1
#else
#define BENCH_PM_ENABLE 0
#endif
#ifdef CONFIG_ESP_TASK_WDT_INIT
#define BENCH_WDT 1
#else
#define BENCH_WDT 0
#endif

static SemaphoreHandle_t emit_mutex = NULL;

// CRC32 with the IEEE polynomial, reflected, matching zlib.crc32 so the host
// can verify with the stdlib. Table-free: 256 bytes of table would be faster,
// but emission is off the measured path and this keeps the footprint down.
uint32_t bench_crc32(void const* data, size_t len) {
    uint8_t const* p   = (uint8_t const*)data;
    uint32_t       crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = -(crc & 1u);
            crc           = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

uint32_t bench_fnv1a(void const* data, size_t len) {
    uint8_t const* p = (uint8_t const*)data;
    uint32_t       h = 0x811C9DC5u;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

float bench_measure_cpu_mhz(int window_us) {
    int64_t  t0 = esp_timer_get_time();
    uint32_t c0 = esp_cpu_get_cycle_count();
    while (esp_timer_get_time() - t0 < window_us) {
        // Busy-wait: sleeping would let the core idle and defeat the point.
    }
    uint32_t c1 = esp_cpu_get_cycle_count();
    int64_t  t1 = esp_timer_get_time();
    // Unsigned subtraction is correct across a single 32-bit wrap.
    uint32_t cycles = c1 - c0;
    int64_t  us     = t1 - t0;
    if (us <= 0) {
        return 0.0f;
    }
    return (float)cycles / (float)us;
}

static void report_lock_init(void) {
    if (emit_mutex == NULL) {
        emit_mutex = xSemaphoreCreateMutex();
    }
}

void bench_report_emit(char const* kind, char const* json) {
    report_lock_init();
    static char line[BENCH_LINE_MAX + 64];

    uint32_t crc = bench_crc32(json, strlen(json));
    int      n   = snprintf(line, sizeof(line), "@@BENCH-%s@@ %s @@%08" PRIx32 "@@\n", kind, json, crc);
    if (n < 0) {
        return;
    }

    if (emit_mutex != NULL) {
        xSemaphoreTake(emit_mutex, portMAX_DELAY);
    }
    // One write of a fully pre-formatted buffer: the smallest window in which
    // another task's log output could interleave.
    fwrite(line, 1, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1), stdout);
    fflush(stdout);
    if (emit_mutex != NULL) {
        xSemaphoreGive(emit_mutex);
    }
}

void bench_report_emitf(char const* kind, char const* fmt, ...) {
    static char json[BENCH_LINE_MAX];
    va_list     args;
    va_start(args, fmt);
    vsnprintf(json, sizeof(json), fmt, args);
    va_end(args);
    bench_report_emit(kind, json);
}

static void report_identity(char const* kind, char const* type, uint16_t cells, unsigned idle_left_s) {
    // reset: esp_reset_reason() from *this* boot. A run that ends without an END
    // record and comes back with reset != 1 (POWERON) says the firmware died
    // rather than the link dropping -- the one distinction the console cannot
    // otherwise make, since a panic that reboots takes its own output with it.
    bench_report_emitf(kind,
                       "{\"t\":\"%s\",\"schema\":%d,\"cells\":%u,\"opt\":\"%s\","
                       "\"app_git\":\"%s\",\"app_dirty\":%s,\"pax_git\":\"%s\",\"pax_dirty\":%s,"
                       "\"corpus_ver\":%d,\"idle_left_s\":%u,\"reset\":%d}",
                       type, BENCH_SCHEMA_VERSION, (unsigned)cells, BENCH_OPT_NAME, BENCH_APP_GIT,
                       BENCH_BOOL(BENCH_APP_DIRTY), BENCH_PAX_GIT, BENCH_BOOL(BENCH_PAX_DIRTY),
                       BENCH_CORPUS_VERSION, idle_left_s, (int)esp_reset_reason());
}

void bench_report_ready(uint16_t cells, unsigned idle_left_s) {
    report_identity("READY", "ready", cells, idle_left_s);
}

void bench_report_pong(uint16_t cells, unsigned idle_left_s) {
    report_identity("PONG", "pong", cells, idle_left_s);
}

void bench_report_begin(uint16_t cells, float cpu_mhz_meas, void const* tile_int, void const* tile_psram) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    esp_app_desc_t const* app = esp_app_get_description();

    size_t int_free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    // Split across two buffers: the whole record would exceed one line budget
    // otherwise, and the host reassembles by key anyway.
    bench_report_emitf("BEGIN",
                       "{\"t\":\"begin\",\"schema\":%d,\"cells\":%u,\"boot_us\":%lld,\"reset\":%d,"
                       "\"build\":{\"app_git\":\"%s\",\"app_dirty\":%s,\"app_branch\":\"%s\","
                       "\"pax_git\":\"%s\",\"pax_dirty\":%s,\"pax_branch\":\"%s\","
                       "\"idf\":\"%s\",\"opt\":\"%s\",\"app_ver\":\"%s\",\"app_date\":\"%s %s\"},"
                       "\"hw\":{\"chip\":\"esp32p4\",\"rev\":%d,\"cores\":%d,\"cpu_mhz_nom\":%d,"
                       "\"cpu_mhz_meas\":%.2f,\"l2_kb\":%d,\"l2_line\":%d,\"psram\":%s},"
                       "\"cfg\":{\"pm_enable\":%s,\"pm_lock\":true,\"tick_hz\":%d,\"wdt\":%s,"
                       "\"pax\":{\"bounds_check\":%s,\"orientation\":%s,\"async\":\"%s\","
                       "\"range_setter\":%s,\"range_merger\":%s,\"fixed_point\":%s,"
                       "\"long_fixed_point\":%s,\"queue_size\":%d,\"sso_len\":%d}},"
                       "\"mem\":{\"tile_dim\":%d,\"int_addr\":\"%p\",\"psram_addr\":\"%p\","
                       "\"int_free\":%u,\"int_largest\":%u,\"psram_free\":%u},"
                       "\"bench\":{\"n_samples\":%d,\"warmup\":%d,\"target_sample_us\":%d,"
                       "\"cell_budget_us\":%d,\"corpus_ver\":%d,\"join_placement\":\"per_sample\","
                       "\"driver_core\":%d,\"driver_prio\":%d,\"scrub_bytes\":%d}}",
                       BENCH_SCHEMA_VERSION, (unsigned)cells, (long long)esp_timer_get_time(), (int)esp_reset_reason(),
                       BENCH_APP_GIT,
                       BENCH_BOOL(BENCH_APP_DIRTY), BENCH_APP_BRANCH, BENCH_PAX_GIT, BENCH_BOOL(BENCH_PAX_DIRTY),
                       BENCH_PAX_BRANCH, IDF_VER, BENCH_OPT_NAME, app ? app->version : "?", app ? app->date : "?",
                       app ? app->time : "?", chip.revision, chip.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, cpu_mhz_meas,
                       CONFIG_CACHE_L2_CACHE_SIZE / 1024, CONFIG_CACHE_L2_CACHE_LINE_SIZE,
#if defined(CONFIG_SPIRAM)
                       BENCH_BOOL(esp_psram_is_initialized()),
#else
                       BENCH_BOOL(false),
#endif
                       BENCH_BOOL(BENCH_PM_ENABLE), CONFIG_FREERTOS_HZ, BENCH_BOOL(BENCH_WDT),
                       BENCH_BOOL(BENCH_PAX_BOUNDS), BENCH_BOOL(BENCH_PAX_ORIENT), BENCH_PAX_ASYNC,
                       BENCH_BOOL(BENCH_PAX_RSETTER), BENCH_BOOL(BENCH_PAX_RMERGER), BENCH_BOOL(BENCH_PAX_FIXPT),
                       BENCH_BOOL(BENCH_PAX_LONGFIXPT), CONFIG_PAX_QUEUE_SIZE, 32, BENCH_TILE_DIM, tile_int,
                       tile_psram, (unsigned)int_free, (unsigned)int_largest, (unsigned)psram_free, BENCH_N_SAMPLES,
                       BENCH_N_WARMUP, BENCH_TARGET_SAMPLE_US, BENCH_CELL_BUDGET_US, BENCH_CORPUS_VERSION,
                       BENCH_TASK_CORE, BENCH_TASK_PRIO, BENCH_SCRUB_BYTES);
}

void bench_report_end(uint16_t cells, int64_t total_us, char const* status) {
    bench_report_emitf("END", "{\"t\":\"end\",\"cells\":%u,\"total_us\":%lld,\"status\":\"%s\"}", (unsigned)cells,
                       (long long)total_us, status);
}

void bench_report_abort(char const* reason, char const* detail_json) {
    bench_report_emitf("ABORT", "{\"t\":\"abort\",\"reason\":\"%s\",\"detail\":%s}", reason,
                       detail_json ? detail_json : "null");
}
