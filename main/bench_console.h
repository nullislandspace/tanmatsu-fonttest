// Debug-console command listener for the PAX font-rendering benchmark.
//
// Reads newline-terminated tokens from the USB-serial/JTAG peripheral, using
// the driver API rather than stdin. This is the pattern the Tanmatsu launcher
// uses in main/usb_debug_listener.c: ESP_LOG/printf output and the driver's RX
// path coexist fine, but mixing the driver with stdin/getchar() loses bytes
// non-deterministically, so this module never touches stdin.

#pragma once

#include <stdbool.h>

typedef enum {
    BENCH_CMD_NONE = 0,
    BENCH_CMD_PING,        // liveness probe, answered by the console task itself
    BENCH_CMD_RUN,         // run the full matrix
    BENCH_CMD_RUN_ONE,     // run only the base cell
    BENCH_CMD_RUN_CELL,    // run one cell or an index range (arg = "N" or "N-M"),
                           // and stay listening afterwards instead of handing the
                           // badge back -- this is the loop for chasing one bad cell
    BENCH_CMD_LIST_CELLS,  // emit one CELLDEF record per cell, no measurement
    BENCH_CMD_DUMP_CORPUS, // emit corpus layout and accounting per font cell
    BENCH_CMD_DUMP_FB,     // stream one cell's rendered framebuffer (arg = cell id)
    BENCH_CMD_RENDER_DEMO, // draw a sample page to the real display
    BENCH_CMD_IDLE_TIMEOUT, // no command arrived in time; return to the launcher
    BENCH_CMD_EXIT,         // hand the badge back to the launcher now
} bench_cmd_t;

// Invoked from the console task for every recognised command except PING.
// `arg` is whatever followed the token on the line, or "" if nothing did.
typedef void (*bench_console_cb_t)(bench_cmd_t cmd, char const* arg);

// Start the listener task. `cells` is reported in the READY banner.
void bench_console_start(bench_console_cb_t cb, unsigned cells);

// Suspend/resume the listener. The runner suspends it for the duration of a
// measurement so neither its RX interrupts nor its banner writes land inside a
// timed region.
void bench_console_suspend(void);
void bench_console_resume(void);

// Resume the listener and give the host `ms` to ask for something else before
// the app hands the badge back. Any recognised command restarts the clock, so a
// host that is mid-dump is never cut off.
void bench_console_grace(unsigned ms);
