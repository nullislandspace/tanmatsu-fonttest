// Framebuffer streaming for the PAX font-rendering benchmark.
//
// Tier 2 of the correctness system (plan §7.2). The per-cell fb_hash says that
// something changed; this says what. One cell's rendered framebuffer is streamed
// out as base64 so the host can write a reference PNG and, later, a pixel diff
// against it.
//
// Streaming ~256 KB of base64 over USB-CDC takes a few seconds, which is why
// this is on demand and never part of a measured run.

#pragma once

#include <stdbool.h>

// Render cell `index` exactly once and stream its framebuffer. Emits an FBBEGIN
// record with the geometry and hash, then FB chunk records, then FBEND.
// Returns false if the cell does not exist or could not be rendered.
bool bench_dump_fb(unsigned index, void* tile_internal, void* tile_psram);
