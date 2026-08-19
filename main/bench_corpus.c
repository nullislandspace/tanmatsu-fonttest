#include "bench_corpus.h"

#include <math.h>
#include <string.h>

#include "bench_config.h"
#include "bench_report.h"
#include "pax_fonts.h"
#include "pax_text.h"

// Versioned corpus. Plain ASCII, deliberately: multi-byte UTF-8 would exercise
// the decoder rather than the rasteriser, and the point here is glyph cost.
//
// The mix is the kind of text the launcher actually draws -- menu labels, status
// lines -- with one entry deliberately past PAX_SSO_BUF_LEN (32 bytes) so the
// async renderer's per-draw-call malloc path is exercised.
static char const* const CORPUS_MIXED[] = {
    "Hello",
    "The quick brown fox",
    "0123456789",
    "Menu item with icon",
    "Settings",
    "WiFi: connected (192.168.1.42)",
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJ",  // 36 bytes: crosses the SSO limit
    "Tanmatsu",
};

// All entries <= 16 bytes: always small-string optimised, never malloc'd.
static char const* const CORPUS_SHORT[] = {
    "Hello", "Settings", "0123456789", "Tanmatsu", "Apps", "Battery 87%", "OK", "File",
};

// All entries >= 40 bytes: forces a refcounted allocation per draw call.
static char const* const CORPUS_LONG[] = {
    "The quick brown fox jumps over the lazy dog today",
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWX",
    "Menu item with a rather long descriptive label here",
    "WiFi: connected to network 192.168.1.42 gateway .1",
};

#define CORPUS_COUNT(arr) (sizeof(arr) / sizeof((arr)[0]))

static char const* const* corpus_for(bench_corpus_variant_t variant, size_t* out_count) {
    switch (variant) {
        case BENCH_CORPUS_SHORT: *out_count = CORPUS_COUNT(CORPUS_SHORT); return CORPUS_SHORT;
        case BENCH_CORPUS_LONG:  *out_count = CORPUS_COUNT(CORPUS_LONG); return CORPUS_LONG;
        case BENCH_CORPUS_MIXED:
        default:                 *out_count = CORPUS_COUNT(CORPUS_MIXED); return CORPUS_MIXED;
    }
}

// Mirror of pax's text_get_range() (pax_text.c:313): a linear scan over the
// font's ranges. Mirrored rather than called because it is static in pax, and
// because the scan cost itself is one of the things being measured.
static pax_font_range_t const* find_range(pax_font_t const* font, uint32_t codepoint) {
    for (size_t i = 0; i < font->n_ranges; i++) {
        pax_font_range_t const* range = &font->ranges[i];
        if (codepoint >= range->start && codepoint <= range->end) {
            return range;
        }
    }
    return NULL;
}

// Height of one glyph row, in font units, taken from the first range.
static int font_line_height(pax_font_t const* font) {
    if (font->n_ranges == 0) {
        return font->default_size;
    }
    pax_font_range_t const* range = &font->ranges[0];
    return range->type == PAX_FONT_TYPE_BITMAP_MONO ? range->bitmap_mono.height : range->bitmap_var.height;
}

// Advance width of one glyph in font units, matching what pax's text_bitmap_*
// return as dims.x (pax_text.c:278, :304).
static float glyph_advance(pax_font_range_t const* range, uint32_t codepoint) {
    if (range->type == PAX_FONT_TYPE_BITMAP_MONO) {
        return range->bitmap_mono.width;
    }
    return range->bitmap_var.dims[codepoint - range->start].measured_width;
}

// Accumulate the source and destination pixel counts for one glyph. pax skips
// space entirely (pax_text.c:264, :288), so it costs an advance but no pixels.
static void account_glyph(bench_layout_t* out, pax_font_range_t const* range, uint32_t codepoint, float scale) {
    out->chars++;
    if (codepoint == 0x20) {
        return;
    }

    int w, h;
    if (range->type == PAX_FONT_TYPE_BITMAP_MONO) {
        w = range->bitmap_mono.width;
        h = range->bitmap_mono.height;
    } else {
        pax_bmpv_t const* dims = &range->bitmap_var.dims[codepoint - range->start];
        w                      = dims->draw_w;
        h                      = dims->draw_h;
    }

    out->glyphs++;
    out->src_px += (uint32_t)(w * h);

    // The fast blitter scales by an integer factor, so the destination area is
    // the rounded scale squared. This is the denominator for ns_per_dest_px,
    // the only figure comparable across fonts and sizes.
    int const sw = (int)lroundf(w * scale);
    int const sh = (int)lroundf(h * scale);
    out->dest_px += (uint32_t)(sw * sh);
}

// Longest prefix of `text` that fits `max_width`, in bytes. Only ASCII is in the
// corpus, so a byte prefix is always a valid string; the check is kept anyway so
// a future multi-byte corpus cannot silently split a codepoint.
static size_t fit_prefix(pax_font_t const* font, float font_size, char const* text, float max_width,
                         bench_layout_t* out, float scale) {
    float  width = 0;
    size_t i     = 0;
    size_t len   = strlen(text);

    while (i < len) {
        uint32_t codepoint = (uint8_t)text[i];
        if (codepoint >= 0x80) {
            // Not expected in this corpus; stop rather than mis-measure.
            break;
        }

        pax_font_range_t const* range = find_range(font, codepoint);
        if (range == NULL) {
            i++;
            continue;
        }

        float advance = glyph_advance(range, codepoint) * scale;
        if (width + advance > max_width) {
            break;
        }

        account_glyph(out, range, codepoint, scale);
        width += advance;
        i++;
    }

    (void)font_size;
    return i;
}

void bench_corpus_layout(bench_layout_t* out, pax_font_t const* font, float font_size, bench_corpus_variant_t variant,
                         int width, int height) {
    memset(out, 0, sizeof(*out));

    size_t             count;
    char const* const* corpus = corpus_for(variant, &count);

    float const scale   = font_size / font->default_size;
    int const   line_h  = (int)lroundf(font_line_height(font) * scale) + 2;
    float const max_w   = (float)(width - 2);
    size_t      next    = 0;

    for (int y = 1; y + line_h <= height && out->row_count < BENCH_MAX_ROWS; y += line_h) {
        char const* text = corpus[next++ % count];

        bench_row_t* row = &out->rows[out->row_count];
        row->text        = text;
        row->x           = 1.0f;
        row->y           = (float)y;
        row->len         = fit_prefix(font, font_size, text, max_w, out, scale);

        out->row_count++;
    }

    // The layout truncates rather than clipping, so this must stay false. If it
    // ever trips, the workload no longer matches the accounting.
    out->clipped = false;
}

void bench_corpus_dump(void) {
    static struct {
        char const*       name;
        pax_font_t const* font;
        float             size;
    } const CASES[] = {
        {"sky@9",              pax_font_sky,             9.0f },
        {"sky@18",             pax_font_sky,             18.0f},
        {"sky@13",             pax_font_sky,             13.0f},
        {"sky_mono@9",         pax_font_sky_mono,        9.0f },
        {"saira_regular@18",   pax_font_saira_regular,   18.0f},
        {"saira_regular@27",   pax_font_saira_regular,   27.0f},
        {"saira_regular@36",   pax_font_saira_regular,   36.0f},
        {"marker@22",          pax_font_marker,          22.0f},
        {"marker@33",          pax_font_marker,          33.0f},
        {"saira_condensed@45", pax_font_saira_condensed, 45.0f},
    };

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        bench_layout_t layout;
        bench_corpus_layout(&layout, CASES[i].font, CASES[i].size, BENCH_CORPUS_MIXED, BENCH_TILE_DIM, BENCH_TILE_DIM);

        bench_report_emitf("CORPUS",
                           "{\"t\":\"corpus\",\"id\":\"%s\",\"font\":\"%s\",\"size\":%.1f,"
                           "\"default_size\":%d,\"ratio\":%.4f,\"ranges\":%u,\"aa\":%s,"
                           "\"rows\":%u,\"glyphs\":%u,\"chars\":%u,\"src_px\":%u,\"dest_px\":%u,"
                           "\"clipped\":%s,\"corpus_ver\":%d}",
                           CASES[i].name, CASES[i].font->name, CASES[i].size, CASES[i].font->default_size,
                           (double)(CASES[i].size / CASES[i].font->default_size), (unsigned)CASES[i].font->n_ranges,
                           CASES[i].font->recommend_aa ? "true" : "false", (unsigned)layout.row_count,
                           (unsigned)layout.glyphs, (unsigned)layout.chars, (unsigned)layout.src_px,
                           (unsigned)layout.dest_px, layout.clipped ? "true" : "false", BENCH_CORPUS_VERSION);
    }
}
