#include "bench_matrix.h"

#include <stdio.h>
#include <string.h>

#include "bench_config.h"
#include "bench_report.h"
#include "pax_fonts.h"

// Upper bound: 48 cube cells + 23 sweep cells, with slack.
#define BENCH_MAX_CELLS 96
#define BENCH_ID_MAX    48

static bench_cell_t s_cells[BENCH_MAX_CELLS];
static char         s_ids[BENCH_MAX_CELLS][BENCH_ID_MAX];
static unsigned     s_count      = 0;
static unsigned     s_base_index = 0;
static bool         s_built      = false;

// Saira Regular with antialiasing turned off, sharing the same glyph ranges.
// This is the only way to isolate pax_shader_font_bmp_aa (four glyph samples and
// two floorf per pixel) from pax_shader_font_bmp: the choice is made from the
// font's recommend_aa flag in dispatch_glyph (pax_text.c:216).
static pax_font_t s_saira_noaa;

// The three distinct pixel loops, as font/size pairs.
//   fast1  bpp 1, opaque  -> direct set, no read-modify-write
//   fast2  bpp 2          -> alpha blend, the launcher's common case
//   shader non-integer scale ratio -> shader rasteriser
#define FAST1_FONT pax_font_sky
#define FAST1_SIZE 9.0f
#define FAST2_FONT pax_font_saira_regular
#define FAST2_SIZE 18.0f
#define SHADER_FONT pax_font_saira_regular
#define SHADER_SIZE 27.0f

char const* bench_format_name(pax_buf_type_t fmt) {
    switch (fmt) {
        case PAX_BUF_16_565RGB:   return "565";
        case PAX_BUF_24_888RGB:   return "888";
        case PAX_BUF_32_8888ARGB: return "8888";
        case PAX_BUF_8_332RGB:    return "332";
        default:                  return "other";
    }
}

char const* bench_orient_name(pax_orientation_t orient) {
    switch (orient) {
        case PAX_O_UPRIGHT:          return "upright";
        case PAX_O_ROT_CCW:          return "rotccw";
        case PAX_O_ROT_HALF:         return "rothalf";
        case PAX_O_ROT_CW:           return "rotcw";
        case PAX_O_FLIP_H:           return "fliph";
        case PAX_O_ROT_CCW_FLIP_H:   return "rotccwfliph";
        case PAX_O_ROT_HALF_FLIP_H:  return "rothalffliph";
        case PAX_O_ROT_CW_FLIP_H:    return "rotcwfliph";
        default:                     return "?";
    }
}

char const* bench_renderer_name(bench_renderer_t renderer) {
    switch (renderer) {
        case BENCH_RENDER_SYNC:   return "sync";
        case BENCH_RENDER_ASYNC1: return "async1";
        case BENCH_RENDER_ASYNC2: return "async2";
        default:                  return "?";
    }
}

char const* bench_path_name(bench_path_t path) {
    switch (path) {
        case BENCH_PATH_BLIT_DIRECT:   return "blit/direct";
        case BENCH_PATH_BLIT_ALPHA:    return "blit/alpha";
        case BENCH_PATH_SHADER_AA:     return "shader/aa";
        case BENCH_PATH_SHADER_PLAIN:  return "shader/plain";
        case BENCH_PATH_SHADER_PAL:    return "shader/pal";
        default:                       return "?";
    }
}

// Glyph bit depth of a font's first range: 1 for the Sky fonts, 2 for the
// antialiased ones. Determines whether the blitter can use the direct-set path.
static uint8_t font_bpp(pax_font_t const* font) {
    if (font->n_ranges == 0) {
        return 1;
    }
    pax_font_range_t const* range = &font->ranges[0];
    return range->type == PAX_FONT_TYPE_BITMAP_MONO ? range->bitmap_mono.bpp : range->bitmap_var.bpp;
}

bench_path_t bench_cell_path(bench_cell_t const* cell) {
    float const scale = cell->size / cell->font->default_size;

    // dispatch_glyph (pax_text.c:191) takes the fast blitter only when the
    // effective glyph scale is integral; the matrix never pushes a matrix, so
    // matrix.a0 is 1 and the other three conditions hold by construction.
    float const frac      = scale - (float)(int)scale;
    bool const  fast_path = (frac < 0.01f) || (frac > 0.99f);

    uint8_t const bpp = font_bpp(cell->font);

    if (fast_path) {
        // pax_swr_blit_char (pax_renderer_soft.c:571) uses direct set only for
        // a 1 bpp glyph drawn fully opaque, or a palette buffer.
        return (bpp == 1 && cell->alpha == 255) ? BENCH_PATH_BLIT_DIRECT : BENCH_PATH_BLIT_ALPHA;
    }

    // Shader path: the palette/cutout shader wins first for opaque 1 bpp text,
    // regardless of the font's antialiasing hint (pax_text.c:214).
    if (bpp == 1 && cell->alpha == 255) {
        return BENCH_PATH_SHADER_PAL;
    }
    return cell->font->recommend_aa ? BENCH_PATH_SHADER_AA : BENCH_PATH_SHADER_PLAIN;
}

static bench_cell_t* add_cell(char const* group, char const* id) {
    if (s_count >= BENCH_MAX_CELLS) {
        return NULL;
    }
    bench_cell_t* cell = &s_cells[s_count];
    memset(cell, 0, sizeof(*cell));

    snprintf(s_ids[s_count], BENCH_ID_MAX, "%s", id);
    cell->id    = s_ids[s_count];
    cell->group = group;

    // Defaults are the base configuration; callers override one axis at a time.
    cell->fmt          = PAX_BUF_24_888RGB;
    cell->font         = FAST2_FONT;
    cell->font_name    = "saira_regular";
    cell->size         = FAST2_SIZE;
    cell->orient       = PAX_O_ROT_CW;
    cell->mem_internal = false;
    cell->renderer     = BENCH_RENDER_ASYNC2;
    cell->alpha        = 255;
    cell->halign       = PAX_ALIGN_BEGIN;
    cell->corpus       = BENCH_CORPUS_MIXED;

    s_count++;
    return cell;
}

// Apply one of the three pixel-loop configurations to a cell.
static void set_path_config(bench_cell_t* cell, int path_index) {
    switch (path_index) {
        case 0:
            cell->font      = FAST1_FONT;
            cell->font_name = "sky";
            cell->size      = FAST1_SIZE;
            break;
        case 1:
            cell->font      = FAST2_FONT;
            cell->font_name = "saira_regular";
            cell->size      = FAST2_SIZE;
            break;
        default:
            cell->font      = SHADER_FONT;
            cell->font_name = "saira_regular";
            cell->size      = SHADER_SIZE;
            break;
    }
}

static void build_cube(void) {
    static pax_buf_type_t const FORMATS[] = {PAX_BUF_16_565RGB, PAX_BUF_24_888RGB};
    static char const* const    PATHS[]   = {"fast1", "fast2", "shader"};
    static pax_orientation_t const ORIENTS[] = {PAX_O_UPRIGHT, PAX_O_ROT_CW};
    static bench_renderer_t const  RENDERERS[] = {BENCH_RENDER_SYNC, BENCH_RENDER_ASYNC2};

    for (size_t f = 0; f < 2; f++) {
        for (int p = 0; p < 3; p++) {
            for (size_t o = 0; o < 2; o++) {
                for (int m = 0; m < 2; m++) {
                    for (size_t r = 0; r < 2; r++) {
                        char id[BENCH_ID_MAX];
                        snprintf(id, sizeof(id), "c%s.%s.%s.%s.%s", bench_format_name(FORMATS[f]), PATHS[p],
                                 bench_orient_name(ORIENTS[o]), m ? "sram" : "psram",
                                 bench_renderer_name(RENDERERS[r]));

                        bench_cell_t* cell = add_cell("cube", id);
                        if (cell == NULL) {
                            return;
                        }
                        cell->fmt          = FORMATS[f];
                        cell->orient       = ORIENTS[o];
                        cell->mem_internal = (m != 0);
                        cell->renderer     = RENDERERS[r];
                        set_path_config(cell, p);
                    }
                }
            }
        }
    }
}

static void build_sweeps(void) {
    // Orientation: the six not already covered by the cube.
    static pax_orientation_t const REST[] = {
        PAX_O_ROT_CCW, PAX_O_ROT_HALF, PAX_O_FLIP_H, PAX_O_ROT_CCW_FLIP_H, PAX_O_ROT_HALF_FLIP_H, PAX_O_ROT_CW_FLIP_H,
    };
    for (size_t i = 0; i < sizeof(REST) / sizeof(REST[0]); i++) {
        char id[BENCH_ID_MAX];
        snprintf(id, sizeof(id), "orient.%s", bench_orient_name(REST[i]));
        bench_cell_t* cell = add_cell("orient", id);
        if (cell != NULL) {
            cell->orient = REST[i];
        }
    }

    // Fonts, sizes and scale ratios, including both bpp-1 shader and AA-off.
    static struct {
        char const*       id;
        pax_font_t const* font;
        char const*       name;
        float             size;
    } const FONTS[] = {
        {"font.sky@18",          pax_font_sky,             "sky",             18.0f},
        {"font.sky@27",          pax_font_sky,             "sky",             27.0f},
        {"font.skymono@9",       pax_font_sky_mono,        "sky_mono",        9.0f },
        {"font.sky@13",          pax_font_sky,             "sky",             13.0f},
        {"font.saira@36",        pax_font_saira_regular,   "saira_regular",   36.0f},
        {"font.marker@22",       pax_font_marker,          "marker",          22.0f},
        {"font.marker@33",       pax_font_marker,          "marker",          33.0f},
        {"font.condensed@45",    pax_font_saira_condensed, "saira_condensed", 45.0f},
    };
    for (size_t i = 0; i < sizeof(FONTS) / sizeof(FONTS[0]); i++) {
        bench_cell_t* cell = add_cell("font", FONTS[i].id);
        if (cell != NULL) {
            cell->font      = FONTS[i].font;
            cell->font_name = FONTS[i].name;
            cell->size      = FONTS[i].size;
        }
    }

    // Antialiasing off, same font and size as the cube's shader cell, so the
    // pair isolates the cost of the AA shader itself.
    bench_cell_t* noaa = add_cell("font", "font.saira@27.noaa");
    if (noaa != NULL) {
        noaa->font      = &s_saira_noaa;
        noaa->font_name = "saira_regular_noaa";
        noaa->size      = SHADER_SIZE;
    }

    // The third renderer.
    bench_cell_t* async1 = add_cell("rend", "rend.async1");
    if (async1 != NULL) {
        async1->renderer = BENCH_RENDER_ASYNC1;
    }

    // Translucent colour: forces even 1 bpp text off the direct-set path.
    bench_cell_t* alpha1 = add_cell("alpha", "alpha.fast1.a128");
    if (alpha1 != NULL) {
        set_path_config(alpha1, 0);
        alpha1->alpha = 128;
    }
    bench_cell_t* alpha2 = add_cell("alpha", "alpha.fast2.a128");
    if (alpha2 != NULL) {
        alpha2->alpha = 128;
    }

    // String length either side of PAX_SSO_BUF_LEN (32): the long corpus forces
    // a refcounted allocation per draw call under the async renderers.
    bench_cell_t* shortc = add_cell("len", "len.short");
    if (shortc != NULL) {
        shortc->corpus = BENCH_CORPUS_SHORT;
    }
    bench_cell_t* longc = add_cell("len", "len.long");
    if (longc != NULL) {
        longc->corpus = BENCH_CORPUS_LONG;
    }

    // Centre alignment makes each worker run an extra measuring pass.
    bench_cell_t* align = add_cell("align", "align.center");
    if (align != NULL) {
        align->halign = PAX_ALIGN_CENTER;
    }
}

void bench_matrix_build(void) {
    if (s_built) {
        return;
    }

    s_saira_noaa              = pax_font_saira_regular_raw;
    s_saira_noaa.recommend_aa = false;

    s_count = 0;

    // The base cell runs first and again last: the difference between the two is
    // a direct measurement of drift across the run, with no assumptions.
    s_base_index = s_count;
    add_cell("drift", "drift.first");

    build_cube();
    build_sweeps();

    add_cell("drift", "drift.last");

    s_built = true;
}

unsigned bench_matrix_count(void) {
    return s_count;
}

bench_cell_t const* bench_matrix_cell(unsigned index) {
    return index < s_count ? &s_cells[index] : NULL;
}

int bench_matrix_find(char const* id) {
    bench_matrix_build();
    if (id == NULL || id[0] == '\0') {
        return -1;
    }
    for (unsigned i = 0; i < bench_matrix_count(); i++) {
        bench_cell_t const* cell = bench_matrix_cell(i);
        if (cell != NULL && strcmp(cell->id, id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

unsigned bench_matrix_base_index(void) {
    return s_base_index;
}

void bench_matrix_dump(void) {
    bench_matrix_build();
    for (unsigned i = 0; i < s_count; i++) {
        bench_cell_t const* cell = &s_cells[i];
        bench_report_emitf("CELLDEF",
                           "{\"t\":\"celldef\",\"i\":%u,\"id\":\"%s\",\"g\":\"%s\",\"fmt\":\"%s\","
                           "\"font\":\"%s\",\"size\":%.1f,\"ratio\":%.4f,\"path\":\"%s\","
                           "\"orient\":\"%s\",\"mem\":\"%s\",\"rend\":\"%s\",\"alpha\":%u,"
                           "\"halign\":%u,\"corpus\":%u}",
                           i, cell->id, cell->group, bench_format_name(cell->fmt), cell->font_name,
                           (double)cell->size, (double)(cell->size / cell->font->default_size),
                           bench_path_name(bench_cell_path(cell)), bench_orient_name(cell->orient),
                           cell->mem_internal ? "sram" : "psram", bench_renderer_name(cell->renderer),
                           (unsigned)cell->alpha, (unsigned)cell->halign, (unsigned)cell->corpus);
    }
}
