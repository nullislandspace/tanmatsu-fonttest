#include "bench_measure.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bench_report.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "pax_gfx.h"
#include "pax_text.h"

// Accumulator for the value pax_draw_text_adv returns. Volatile so the call
// cannot be optimised away, which is insurance rather than necessity: pax is a
// separate, non-LTO static library, so its calls cannot be inlined out anyway.
static volatile float s_sink = 0.0f;

// Stream the scrub block to evict the previous cell's tile from L2. Without
// this, a cell's numbers would depend on which cell ran before it, and per-cell
// comparison across runs would stop being meaningful whenever the matrix
// changed.
static void scrub_cache(void* scrub) {
    volatile uint32_t* p     = (volatile uint32_t*)scrub;
    size_t             words = BENCH_SCRUB_BYTES / sizeof(uint32_t);
    for (size_t i = 0; i < words; i++) {
        p[i] = (uint32_t)i;
    }
    uint32_t sink = 0;
    for (size_t i = 0; i < words; i++) {
        sink += p[i];
    }
    __asm__ __volatile__("" : : "r"(sink) : "memory");
}

static void select_renderer(bench_renderer_t renderer) {
    switch (renderer) {
        case BENCH_RENDER_ASYNC1: pax_set_renderer_async(false); break;
        case BENCH_RENDER_ASYNC2: pax_set_renderer_async(true); break;
        case BENCH_RENDER_SYNC:
        default:                  pax_set_render_engine_default(); break;
    }
}

// One iteration: draw every laid-out row once. This is the unit everything is
// normalised to.
static void run_iteration(bench_cell_t const* cell, pax_buf_t* buf, bench_layout_t const* layout, pax_col_t color) {
    float const centre_x = pax_buf_get_width(buf) * 0.5f;

    for (uint16_t i = 0; i < layout->row_count; i++) {
        bench_row_t const* row = &layout->rows[i];
        float const        x   = (cell->halign == PAX_ALIGN_BEGIN) ? row->x : centre_x;

        pax_2vec2f size = pax_draw_text_adv(buf, color, cell->font, cell->size, x, row->y, row->text, row->len,
                                            cell->halign, PAX_ALIGN_BEGIN, -1);
        s_sink += size.x0;
    }
}

static int compare_int32(void const* a, void const* b) {
    int32_t x = *(int32_t const*)a;
    int32_t y = *(int32_t const*)b;
    return (x > y) - (x < y);
}

static void compute_stats(bench_result_t* out) {
    int32_t sorted[BENCH_N_SAMPLES];
    memcpy(sorted, out->samples_us, sizeof(int32_t) * out->n_samples);
    qsort(sorted, out->n_samples, sizeof(int32_t), compare_int32);

    out->min_us = sorted[0];
    out->max_us = sorted[out->n_samples - 1];
    out->med_us = sorted[out->n_samples / 2];

    double sum = 0;
    for (uint16_t i = 0; i < out->n_samples; i++) {
        sum += out->samples_us[i];
    }
    out->mean_us = (float)(sum / out->n_samples);

    double var = 0;
    for (uint16_t i = 0; i < out->n_samples; i++) {
        double d = out->samples_us[i] - out->mean_us;
        var += d * d;
    }
    out->sd_us = (float)sqrt(var / out->n_samples);

    out->slow_outliers = 0;
    for (uint16_t i = 0; i < out->n_samples; i++) {
        if (out->samples_us[i] > out->med_us * BENCH_OUTLIER_FACTOR) {
            out->slow_outliers++;
        }
    }

    out->cv_pct     = out->med_us > 0 ? (out->sd_us / out->med_us) * 100.0f : 0.0f;
    out->spread_pct = out->med_us > 0 ? ((float)(out->max_us - out->min_us) / out->med_us) * 100.0f : 0.0f;
}

// Set up a cell's target buffer exactly as a measurement would. Factored out so
// the dump path and the measurement path cannot drift apart: a reference
// framebuffer captured from a differently configured buffer would be worse than
// no reference at all.
static bool setup_cell_buffer(bench_cell_t const* cell, void* tile_internal, void* tile_psram, pax_buf_t* buf,
                              bench_layout_t* layout, size_t* used_bytes) {
    void* mem = cell->mem_internal ? tile_internal : tile_psram;
    if (mem == NULL) {
        return false;
    }

    pax_buf_init(buf, mem, BENCH_TILE_DIM, BENCH_TILE_DIM, cell->fmt);
    pax_buf_reversed(buf, false);
    pax_buf_set_orientation(buf, cell->orient);
    pax_noclip(buf);

    *used_bytes = pax_buf_get_size(buf);
    memset(mem, 0, *used_bytes);

    bench_corpus_layout(layout, cell->font, cell->size, cell->corpus, pax_buf_get_width(buf),
                        pax_buf_get_height(buf));
    return true;
}

bool bench_render_cell_once(bench_cell_t const* cell, void* tile_internal, void* tile_psram, pax_buf_t* buf,
                            bench_layout_t* layout) {
    size_t used_bytes = 0;
    if (!setup_cell_buffer(cell, tile_internal, tile_psram, buf, layout, &used_bytes)) {
        return false;
    }

    // Always render through the synchronous engine here. The canary is about
    // what was drawn, not how fast, and the sync path removes any question of
    // whether a worker had finished when the bytes were read.
    select_renderer(BENCH_RENDER_SYNC);

    pax_col_t const color = ((pax_col_t)cell->alpha << 24) | 0x00FFFFFF;
    run_iteration(cell, buf, layout, color);
    pax_join();
    return true;
}

bool bench_measure_cell(bench_cell_t const* cell, void* tile_internal, void* tile_psram, void* scrub,
                        bench_result_t* out) {
    memset(out, 0, sizeof(*out));

    // pax_buf_init does not clear user-supplied memory, so the zeroing inside
    // the helper is what makes fb_hash deterministic.
    void*     mem = cell->mem_internal ? tile_internal : tile_psram;
    pax_buf_t buf;
    size_t    used_bytes = 0;
    if (!setup_cell_buffer(cell, tile_internal, tile_psram, &buf, &out->layout, &used_bytes)) {
        return false;
    }

    select_renderer(cell->renderer);
    out->path = bench_cell_path(cell);

    pax_col_t const color = ((pax_col_t)cell->alpha << 24) | 0x00FFFFFF;

    pax_join();
    scrub_cache(scrub);

    // Calibrate: one iteration decides how many fit in a sample of the target
    // duration, so cheap and expensive cells both get a stable measurement
    // window instead of a fixed draw count.
    int64_t cal_start = esp_timer_get_time();
    run_iteration(cell, &buf, &out->layout, color);
    pax_join();
    int64_t cal_us = esp_timer_get_time() - cal_start;
    if (cal_us < 1) {
        cal_us = 1;
    }

    int32_t reps = (int32_t)((BENCH_TARGET_SAMPLE_US + cal_us - 1) / cal_us);
    if (reps < BENCH_REPS_MIN) {
        reps = BENCH_REPS_MIN;
    }
    if (reps > BENCH_REPS_MAX) {
        reps = BENCH_REPS_MAX;
    }
    out->reps = (uint16_t)reps;

    // Fit the sample count to the remaining budget, never below the floor.
    int64_t sample_us  = cal_us * reps;
    int32_t n_samples  = BENCH_N_SAMPLES;
    if (sample_us > 0) {
        int64_t affordable = BENCH_CELL_BUDGET_US / sample_us - BENCH_N_WARMUP;
        if (affordable < BENCH_N_SAMPLES_MIN) {
            affordable = BENCH_N_SAMPLES_MIN;
        }
        if (affordable < n_samples) {
            n_samples = (int32_t)affordable;
        }
    }
    out->n_samples = (uint16_t)n_samples;

    // Warm-up: cold instruction cache on the pax rasteriser, first touch of the
    // tile. Discarded.
    for (int w = 0; w < BENCH_N_WARMUP; w++) {
        run_iteration(cell, &buf, &out->layout, color);
        pax_join();
    }

    // Measure. pax_join() is inside the timed region and called unconditionally,
    // so the code path is identical for the sync engine (where join is a no-op)
    // and the async ones (where the work is only queued until it runs).
    uint32_t cycles_total = 0;
    int64_t  us_total     = 0;
    for (uint16_t s = 0; s < out->n_samples; s++) {
        uint32_t c0 = esp_cpu_get_cycle_count();
        int64_t  t0 = esp_timer_get_time();
        for (int32_t r = 0; r < reps; r++) {
            run_iteration(cell, &buf, &out->layout, color);
        }
        pax_join();
        int64_t  t1 = esp_timer_get_time();
        uint32_t c1 = esp_cpu_get_cycle_count();

        out->samples_us[s] = (int32_t)(t1 - t0);
        cycles_total += c1 - c0;
        us_total += t1 - t0;
    }

    compute_stats(out);

    // Independent check that the PM lock actually held the clock: never trust
    // the lock, measure it.
    out->cpu_mhz = us_total > 0 ? (float)cycles_total / (float)us_total : 0.0f;

    out->ns_per_iter = (uint32_t)(((int64_t)out->med_us * 1000) / reps);
    out->ns_per_dest_px =
        out->layout.dest_px > 0 ? (float)out->ns_per_iter / (float)out->layout.dest_px : 0.0f;

    // Correctness canary: exactly one iteration onto a cleared buffer, so the
    // hash does not depend on how many reps were calibrated.
    memset(mem, 0, used_bytes);
    run_iteration(cell, &buf, &out->layout, color);
    pax_join();
    out->fb_hash = bench_fnv1a(pax_buf_get_pixels(&buf), used_bytes);

    // Quality flags.
    bool const  is_async     = cell->renderer != BENCH_RENDER_SYNC;
    float const cv_limit     = is_async ? BENCH_CV_LIMIT_ASYNC : BENCH_CV_LIMIT_SYNC;
    float const spread_limit = is_async ? BENCH_SPREAD_LIMIT_ASYNC : BENCH_SPREAD_LIMIT_SYNC;

    if (out->cv_pct > cv_limit) {
        out->flags |= BENCH_FLAG_UNSTABLE;
    }
    if (out->spread_pct > spread_limit) {
        out->flags |= BENCH_FLAG_SPREAD;
    }
    if (out->slow_outliers > 0) {
        out->flags |= BENCH_FLAG_SLOW_OUTLIER;
    }
    if (out->layout.clipped) {
        out->flags |= BENCH_FLAG_CLIPPED;
    }
    if (fabsf(out->cpu_mhz - CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ) >
        CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * (BENCH_MHZ_TOLERANCE_PCT / 100.0f)) {
        out->flags |= BENCH_FLAG_DFS;
    }

    pax_buf_destroy(&buf);
    return true;
}

void bench_measure_report(unsigned index, bench_cell_t const* cell, bench_result_t const* r) {
    char flags[64] = "";
    if (r->flags & BENCH_FLAG_UNSTABLE) {
        strcat(flags, "\"unstable\",");
    }
    if (r->flags & BENCH_FLAG_SPREAD) {
        strcat(flags, "\"spread\",");
    }
    if (r->flags & BENCH_FLAG_SLOW_OUTLIER) {
        strcat(flags, "\"outlier\",");
    }
    if (r->flags & BENCH_FLAG_CLIPPED) {
        strcat(flags, "\"clipped\",");
    }
    if (r->flags & BENCH_FLAG_DFS) {
        strcat(flags, "\"dfs\",");
    }
    size_t flen = strlen(flags);
    if (flen > 0) {
        flags[flen - 1] = '\0';  // drop the trailing comma
    }

    bench_report_emitf("CELL",
                       "{\"t\":\"cell\",\"i\":%u,\"id\":\"%s\",\"g\":\"%s\","
                       "\"ax\":{\"fmt\":\"%s\",\"font\":\"%s\",\"size\":%.1f,\"ratio\":%.4f,"
                       "\"path\":\"%s\",\"orient\":\"%s\",\"mem\":\"%s\",\"rend\":\"%s\","
                       "\"alpha\":%u,\"halign\":%u,\"corpus\":%u},"
                       "\"w\":{\"rows\":%u,\"glyphs\":%u,\"chars\":%u,\"src_px\":%u,\"dest_px\":%u},"
                       "\"m\":{\"reps\":%u,\"n\":%u,\"min\":%ld,\"med\":%ld,\"max\":%ld,"
                       "\"mean\":%.1f,\"sd\":%.1f},"
                       "\"d\":{\"ns_iter\":%lu,\"ns_px\":%.2f},"
                       "\"q\":{\"cv\":%.2f,\"spread\":%.2f,\"mhz\":%.1f,\"outliers\":%u,\"flags\":[%s]},"
                       "\"h\":\"%08lx\"}",
                       index, cell->id, cell->group, bench_format_name(cell->fmt), cell->font_name,
                       (double)cell->size, (double)(cell->size / cell->font->default_size),
                       bench_path_name(r->path), bench_orient_name(cell->orient),
                       cell->mem_internal ? "sram" : "psram", bench_renderer_name(cell->renderer),
                       (unsigned)cell->alpha, (unsigned)cell->halign, (unsigned)cell->corpus,
                       (unsigned)r->layout.row_count, (unsigned)r->layout.glyphs, (unsigned)r->layout.chars,
                       (unsigned)r->layout.src_px, (unsigned)r->layout.dest_px, (unsigned)r->reps,
                       (unsigned)r->n_samples, (long)r->min_us, (long)r->med_us, (long)r->max_us,
                       (double)r->mean_us, (double)r->sd_us, (unsigned long)r->ns_per_iter,
                       (double)r->ns_per_dest_px, (double)r->cv_pct, (double)r->spread_pct, (double)r->cpu_mhz,
                       (unsigned)r->slow_outliers, flags, (unsigned long)r->fb_hash);

    // Raw samples on their own record: a long array can never push the summary
    // over the line budget, and a lost RAW line degrades gracefully.
    char raw[BENCH_LINE_MAX];
    int  pos = snprintf(raw, sizeof(raw), "{\"i\":%u,\"s\":[", index);
    for (uint16_t i = 0; i < r->n_samples && pos < (int)sizeof(raw) - 16; i++) {
        pos += snprintf(raw + pos, sizeof(raw) - pos, "%s%ld", i ? "," : "", (long)r->samples_us[i]);
    }
    snprintf(raw + pos, sizeof(raw) - pos, "]}");
    bench_report_emit("RAW", raw);
}
