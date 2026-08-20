#include "bench_dump.h"

#include <stdio.h>
#include <string.h>

#include "bench_config.h"
#include "bench_corpus.h"
#include "bench_matrix.h"
#include "bench_measure.h"
#include "bench_report.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pax_gfx.h"

static char const TAG[] = "bench_dump";

// 720 raw bytes encode to 960 base64 characters. With the record framing and
// the JSON keys that lands near 1050 characters, comfortably inside
// BENCH_LINE_MAX -- and a chunk that overran the line budget would be truncated
// into a CRC failure rather than an obvious error, so the margin is deliberate.
#define FB_CHUNK_BYTES 720
#define FB_CHUNK_B64   ((FB_CHUNK_BYTES + 2) / 3 * 4)

static char const B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Standard base64 with padding, so the host can decode with the stdlib.
static size_t b64_encode(uint8_t const* in, size_t len, char* out) {
    size_t o = 0;
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++]   = B64[(v >> 18) & 0x3F];
        out[o++]   = B64[(v >> 12) & 0x3F];
        out[o++]   = B64[(v >> 6) & 0x3F];
        out[o++]   = B64[v & 0x3F];
    }

    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++]   = B64[(v >> 18) & 0x3F];
        out[o++]   = B64[(v >> 12) & 0x3F];
        out[o++]   = '=';
        out[o++]   = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++]   = B64[(v >> 18) & 0x3F];
        out[o++]   = B64[(v >> 12) & 0x3F];
        out[o++]   = B64[(v >> 6) & 0x3F];
        out[o++]   = '=';
    }
    out[o] = '\0';
    return o;
}

bool bench_dump_fb(unsigned index, void* tile_internal, void* tile_psram) {
    bench_matrix_build();
    bench_cell_t const* cell = bench_matrix_cell(index);
    if (cell == NULL) {
        bench_report_abort("dumpfb_bad_index", NULL);
        return false;
    }

    pax_buf_t      buf;
    bench_layout_t layout;
    if (!bench_render_cell_once(cell, tile_internal, tile_psram, &buf, &layout)) {
        bench_report_abort("dumpfb_render_failed", NULL);
        return false;
    }

    uint8_t const* pixels = (uint8_t const*)pax_buf_get_pixels(&buf);
    size_t const   total  = pax_buf_get_size(&buf);
    unsigned const chunks = (unsigned)((total + FB_CHUNK_BYTES - 1) / FB_CHUNK_BYTES);
    uint32_t const hash   = bench_fnv1a(pixels, total);

    // The hash here is the same FNV-1a the CELL records carry, so a reference
    // capture can be tied back to the exact measurement it came from.
    bench_report_emitf("FBBEGIN",
                       "{\"t\":\"fbbegin\",\"i\":%u,\"id\":\"%s\",\"fmt\":\"%s\",\"orient\":\"%s\","
                       "\"tile\":%d,\"w\":%d,\"h\":%d,\"bytes\":%u,\"chunk\":%d,\"chunks\":%u,"
                       "\"h\":\"%08lx\"}",
                       index, cell->id, bench_format_name(cell->fmt), bench_orient_name(cell->orient),
                       BENCH_TILE_DIM, pax_buf_get_width(&buf), pax_buf_get_height(&buf), (unsigned)total,
                       FB_CHUNK_BYTES, chunks, (unsigned long)hash);

    static char b64[FB_CHUNK_B64 + 1];
    for (unsigned c = 0; c < chunks; c++) {
        size_t const offset = (size_t)c * FB_CHUNK_BYTES;
        size_t const len    = (total - offset) < FB_CHUNK_BYTES ? (total - offset) : FB_CHUNK_BYTES;
        b64_encode(pixels + offset, len, b64);
        bench_report_emitf("FB", "{\"i\":%u,\"c\":%u,\"d\":\"%s\"}", index, c, b64);

        // Yield periodically: this is a few hundred records back to back, and
        // starving the USB driver's own task is how the FIFO backs up.
        if ((c & 0x0F) == 0x0F) {
            vTaskDelay(1);
        }
    }

    bench_report_emitf("FBEND", "{\"t\":\"fbend\",\"i\":%u,\"id\":\"%s\",\"chunks\":%u,\"h\":\"%08lx\"}", index,
                       cell->id, chunks, (unsigned long)hash);

    ESP_LOGI(TAG, "Dumped %s: %u bytes in %u chunks", cell->id, (unsigned)total, chunks);
    pax_buf_destroy(&buf);
    return true;
}
