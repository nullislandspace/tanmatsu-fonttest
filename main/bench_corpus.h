// Text corpus and per-cell layout for the PAX font-rendering benchmark.
//
// The corpus is versioned (BENCH_CORPUS_VERSION): changing the strings changes
// the workload, so results either side of a change are not comparable and the
// host refuses to compare them.
//
// Layout is a pure function of (font, size, tile geometry), so it is bit
// identical across runs. String byte lengths are truncated at setup so no glyph
// is ever clipped: work per iteration then equals what the accounting says, and
// pax's per-glyph clip test never has anything to trim.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pax_types.h"

// Which subset of the corpus a cell draws. The short and long variants exist to
// straddle PAX_SSO_BUF_LEN (32): under the async renderer a string longer than
// that is malloc'd and reference counted per draw call.
typedef enum {
    BENCH_CORPUS_MIXED = 0,
    BENCH_CORPUS_SHORT,
    BENCH_CORPUS_LONG,
} bench_corpus_variant_t;

#define BENCH_MAX_ROWS 64

typedef struct {
    char const* text;
    size_t      len;  // truncated to what fits the drawable width
    float       x;
    float       y;
} bench_row_t;

typedef struct {
    bench_row_t rows[BENCH_MAX_ROWS];
    uint16_t    row_count;
    // Workload accounting, computed by mirroring pax's own glyph lookup.
    uint32_t glyphs;   // glyphs actually rasterised (space is skipped by pax)
    uint32_t chars;    // codepoints walked, including spaces
    uint32_t src_px;   // glyph bitmap pixels read
    uint32_t dest_px;  // framebuffer pixels written, the normalisation base
    bool     clipped;  // asserted false: layout must never overrun the tile
} bench_layout_t;

// Lay the corpus out for one cell and compute its accounting. `width` and
// `height` are the oriented dimensions of the target buffer.
void bench_corpus_layout(bench_layout_t* out, pax_font_t const* font, float font_size, bench_corpus_variant_t variant,
                         int width, int height);

// Emit one DUMPCORPUS record per font/size combination, for verifying the
// layout and accounting without running a measurement.
void bench_corpus_dump(void);
