// Console output protocol for the PAX font-rendering benchmark.
//
// Records are framed as
//
//     @@BENCH-<KIND>@@ <compact-json> @@<crc32-hex8>@@
//
// and written as a single write of a pre-formatted buffer, never through
// ESP_LOGx. The CRC covers the JSON substring only; the host recomputes it and
// discards any line that fails, which is what makes the protocol survive an
// ESP_LOG from another task interleaving into the stream.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Emit one framed record. `kind` is the bare kind ("BEGIN", "CELL", ...) and
// `json` the payload without surrounding whitespace. Serialised internally, so
// it is safe to call from any task.
void bench_report_emit(char const* kind, char const* json);

// Same, but formats the payload first. Truncates at BENCH_LINE_MAX.
void bench_report_emitf(char const* kind, char const* fmt, ...) __attribute__((format(printf, 2, 3)));

// The READY banner, emitted periodically until a run starts, and the PONG reply
// to a PING probe. Both carry the same identity fields so the host can name a
// run from whichever it sees first. `idle_left_s` is how long the app will keep
// waiting before returning to the launcher.
void bench_report_ready(uint16_t cells, unsigned idle_left_s);
void bench_report_pong(uint16_t cells, unsigned idle_left_s);

// The BEGIN record: build, hardware, config and memory metadata that makes a
// result file self-describing. `cpu_mhz_meas` is an independent measurement of
// what DFS is actually doing, not an echo of the configured value.
void bench_report_begin(uint16_t cells, float cpu_mhz_meas, void const* tile_int, void const* tile_psram);

// The END record.
void bench_report_end(uint16_t cells, int64_t total_us, char const* status);

// The ABORT record, for a failure the run cannot continue past.
void bench_report_abort(char const* reason, char const* detail_json);

// CRC32 (IEEE, same polynomial and result as zlib.crc32) over a byte range.
uint32_t bench_crc32(void const* data, size_t len);

// FNV-1a 32-bit hash, used for the per-cell framebuffer canary.
uint32_t bench_fnv1a(void const* data, size_t len);

// Measure effective CPU frequency in MHz by comparing the cycle counter against
// esp_timer over `window_us`. Must be called from a core-pinned task.
float bench_measure_cpu_mhz(int window_us);
