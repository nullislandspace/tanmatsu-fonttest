// Test matrix for the PAX font-rendering benchmark.
//
// A full cross-product of every axis is ~13800 cells, about 8 hours. Instead the
// matrix is one saturated core cube over the axes whose interactions matter,
// plus one-axis-at-a-time sweeps from a base configuration that mirrors the
// launcher's production setup. See pax_font_benchmark_plan.md section 2.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bench_corpus.h"
#include "pax_types.h"

// Which renderer a cell uses. Sync is PAX's startup default; the two async
// engines queue work to worker tasks and need pax_join() before the clock stops.
typedef enum {
    BENCH_RENDER_SYNC = 0,
    BENCH_RENDER_ASYNC1,
    BENCH_RENDER_ASYNC2,
} bench_renderer_t;

// Which of PAX's three distinct pixel loops a cell is expected to exercise.
// Recomputed from the actual predicates at run time and reported, so an
// optimization that silently moves a cell to another path is visible.
typedef enum {
    BENCH_PATH_BLIT_DIRECT = 0,  // pax_swr_blit_char_direct_set
    BENCH_PATH_BLIT_ALPHA,       // pax_swr_blit_char_alpha_blend
    BENCH_PATH_SHADER_AA,        // shader rasteriser, pax_shader_font_bmp_aa
    BENCH_PATH_SHADER_PLAIN,     // shader rasteriser, pax_shader_font_bmp
    BENCH_PATH_SHADER_PAL,       // shader rasteriser, pax_shader_font_bmp_pal
} bench_path_t;

typedef struct {
    char const*            id;     // stable identity, matched across runs
    char const*            group;  // cube | orient | font | rend | alpha | len | align | drift
    pax_buf_type_t         fmt;
    pax_font_t const*      font;
    char const*            font_name;
    float                  size;
    pax_orientation_t      orient;
    bool                   mem_internal;
    bench_renderer_t       renderer;
    uint8_t                alpha;   // colour alpha: 255 opaque, 128 forces blending
    pax_align_t            halign;
    bench_corpus_variant_t corpus;
} bench_cell_t;

// Build the matrix. Idempotent; safe to call more than once.
void bench_matrix_build(void);

// Number of cells, and access by index.
unsigned            bench_matrix_count(void);
bench_cell_t const* bench_matrix_cell(unsigned index);

// Index of a cell by its stable id, or -1 if there is no such cell. Ids are
// what results are matched on across runs, so this is how the host names a
// cell it wants dumped without having to know the matrix order.
int bench_matrix_find(char const* id);

// Index of the base cell, which the drift group repeats first and last.
unsigned bench_matrix_base_index(void);

// Which pixel loop this cell will actually take, from the same predicates PAX
// itself uses in dispatch_glyph() and pax_swr_blit_char().
bench_path_t bench_cell_path(bench_cell_t const* cell);
char const*  bench_path_name(bench_path_t path);
char const*  bench_format_name(pax_buf_type_t fmt);
char const*  bench_orient_name(pax_orientation_t orient);
char const*  bench_renderer_name(bench_renderer_t renderer);

// Emit one CELLDEF record per cell, without measuring anything.
void bench_matrix_dump(void);
