// Benchmark driver for the PAX font-rendering harness.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "bench_console.h"

// Allocate the tile buffers and the cache-scrub block. Must be called as early
// as possible in app_main: the internal tile needs ~192 KB contiguous out of a
// few hundred KB of internal heap, so anything that allocates first can make it
// impossible. Emits an ABORT record naming the largest free block on failure.
bool bench_buffers_allocate(void);

// Pointers to the allocated buffers, NULL before bench_buffers_allocate().
void* bench_tile_internal(void);
void* bench_tile_psram(void);

// Number of cells in the matrix.
unsigned bench_cell_count(void);

// Start the driver task. It waits for a command, runs it, and returns the badge
// to the launcher when a full run completes.
void bench_runner_start(void);

// Console command entry point, passed to bench_console_start().
void bench_runner_command(bench_cmd_t cmd, char const* arg);
