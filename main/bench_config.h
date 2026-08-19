// Tunables for the PAX font-rendering benchmark.
//
// Everything here ends up in the run's metadata record, so a result file always
// says which values produced it. Changing any of them invalidates comparisons
// against an older baseline; the host client enforces that.

#pragma once

// Schema version of the console output protocol. Bump on any incompatible
// change to the record layout.
#define BENCH_SCHEMA_VERSION 1

// Version of the string corpus. Bump whenever the corpus changes, so old
// results are refused rather than silently compared against new work.
#define BENCH_CORPUS_VERSION 1

// Framebuffer tile geometry. Square, so all eight orientations present the same
// drawable rectangle and the orientation axis compares like with like.
//
// 256 is chosen so the tile does NOT fit in the 128 KB L2 cache:
//   256*256*3 = 196608 B (RGB888), 256*256*2 = 131072 B (RGB565).
// A smaller tile would be served from L2 after warm-up and the PSRAM-vs-SRAM
// axis would measure nothing. Fallback if the internal allocation fails: 224
// (150528 B, still above L2).
#define BENCH_TILE_DIM 256

// Widest supported format is 24 bpp, so this sizes both tiles.
#define BENCH_TILE_BYTES ((size_t)BENCH_TILE_DIM * BENCH_TILE_DIM * 3)

// Samples per cell. Odd, so the median is a real observation rather than an
// interpolation between two.
#define BENCH_N_SAMPLES 15

// Minimum samples for cells so expensive that a single sample already exceeds
// the target duration (the shader-path cells).
#define BENCH_N_SAMPLES_MIN 7

// Discarded samples taken before measurement, to warm the instruction cache
// (the pax rasteriser executes XIP from flash) and first-touch the tile.
#define BENCH_N_WARMUP 2

// Target duration of one sample, in microseconds. The number of iterations per
// sample is calibrated per cell to hit this. Long enough that the esp_timer
// call pair and a single 10 ms FreeRTOS tick are noise; short enough to fit the
// per-cell budget.
#define BENCH_TARGET_SAMPLE_US 110000

// Total time budget per cell, in microseconds. This is the dial for overall
// cycle length: 71 cells at 2 s is about 2 min 45 s including overhead.
#define BENCH_CELL_BUDGET_US 2000000

// Bounds on the calibrated iteration count.
#define BENCH_REPS_MIN 1
#define BENCH_REPS_MAX 4096

// Size of the PSRAM block streamed between cells to evict the previous cell's
// tile from L2. Four times the L2 size.
#define BENCH_SCRUB_BYTES (512 * 1024)

// Instability thresholds, in percent. Async cells get looser limits because
// worker scheduling genuinely adds variance and a tighter threshold would just
// cry wolf on every run.
#define BENCH_CV_LIMIT_SYNC      3.0f
#define BENCH_CV_LIMIT_ASYNC     6.0f
#define BENCH_SPREAD_LIMIT_SYNC  10.0f
#define BENCH_SPREAD_LIMIT_ASYNC 15.0f

// A sample above this multiple of the median counts as a slow outlier.
#define BENCH_OUTLIER_FACTOR 1.5f

// Permitted deviation of measured CPU frequency from nominal, in percent.
// Exceeding it means the PM lock did not hold and the cell is untrustworthy.
#define BENCH_MHZ_TOLERANCE_PCT 2.0f

// Driver task placement. Pinned because esp_cpu_get_cycle_count() is per-core.
#define BENCH_TASK_CORE       0
#define BENCH_TASK_PRIO       5
#define BENCH_TASK_STACK      8192
#define BENCH_CONSOLE_PRIO    4
#define BENCH_CONSOLE_STACK   3072

// How long the app waits for a command before giving up and returning to the
// launcher. Without this, a run that is never requested -- a host client that
// never connects, a cycle that failed earlier -- leaves the badge sitting in an
// app that does not speak BadgeLink, so the only way back is a button press or
// a power cycle.
#define BENCH_IDLE_TIMEOUT_MS 300000

// Console line buffer. Records are emitted as one write of a pre-formatted
// buffer, so this bounds a single record. The BEGIN record is the largest at
// roughly 1.2 KB; anything over this is truncated into invalid JSON, which the
// host counts as a corrupt line rather than silently accepting.
#define BENCH_LINE_MAX 1536

// Per-cell result quality flags.
#define BENCH_FLAG_UNSTABLE     (1u << 0)
#define BENCH_FLAG_DFS          (1u << 1)
#define BENCH_FLAG_CLIPPED      (1u << 2)
#define BENCH_FLAG_SLOW_OUTLIER (1u << 3)
#define BENCH_FLAG_SPREAD       (1u << 4)
