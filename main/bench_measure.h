// Per-cell measurement for the PAX font-rendering benchmark.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bench_config.h"
#include "bench_corpus.h"
#include "bench_matrix.h"
#include "pax_gfx.h"

typedef struct {
    uint16_t reps;       // iterations per sample, calibrated per cell
    uint16_t n_samples;  // samples actually taken
    int32_t  samples_us[BENCH_N_SAMPLES];

    int32_t min_us;
    int32_t med_us;
    int32_t max_us;
    float   mean_us;
    float   sd_us;

    uint32_t ns_per_iter;
    float    ns_per_dest_px;

    float    cv_pct;
    float    spread_pct;
    float    cpu_mhz;
    uint8_t  slow_outliers;
    uint32_t flags;

    // FNV-1a over the framebuffer after exactly one iteration. Independent of
    // reps, so it compares directly across runs: any change to rendered output
    // shows up here even when the timings look identical.
    uint32_t fb_hash;

    bench_layout_t layout;
    bench_path_t   path;
} bench_result_t;

// Render one cell exactly once onto a cleared tile, leaving `buf` initialised
// and pointing at the result. This is the same deterministic single-iteration
// pass the fb_hash canary uses, exposed so a framebuffer can be dumped and
// diffed against a reference. The caller destroys `buf` when done.
bool bench_render_cell_once(bench_cell_t const* cell, void* tile_internal, void* tile_psram, pax_buf_t* buf,
                            bench_layout_t* layout);

// Measure one cell. `scrub` points at the cache-scrub block. Returns false if
// the cell could not be set up at all.
bool bench_measure_cell(bench_cell_t const* cell, void* tile_internal, void* tile_psram, void* scrub,
                        bench_result_t* out);

// Emit the CELL and RAW records for a completed measurement.
void bench_measure_report(unsigned index, bench_cell_t const* cell, bench_result_t const* result);
