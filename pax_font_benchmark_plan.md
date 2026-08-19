# PAX font-rendering benchmark harness and optimization baseline

## Context

PAX graphics renders text slowly. The prior root-cause analysis in
`pax_font_rendering_optimization.md` (done against the Tanmatsu launcher) traced
`pax_draw_text()` down to RISC-V machine code and found that **~74% of the ~120
instructions per antialiased glyph pixel is independent of pixel format, orientation
and device**: five function calls per pixel, a non-inlined `pax_col_merge`, a
loop-invariant division, no early-out for transparent pixels, and four function
pointers reloaded from `buf` on every iteration.

That analysis is entirely static — instruction counts from disassembly, no
measurements. Its own §9 admits it: the frame-time split was never measured, the
`long double` conversion cost in the shader path was never confirmed, PSRAM stall
contribution is excluded.

This project turns the analysis into something measurable. It converts the fonttest
app into an automated benchmark harness, captures a **solid baseline**, and only then
optimizes — each optimization a separate commit in the pax checkout, benchmarked
against that baseline.

Constraints that shape everything:

- **No device-specific optimizations.** pax-gfx is shared across devices and apps.
  RGB565 and RGB888 both measured; wins must be generic.
- **Rendering only.** `bsp_display_blit()` is never called during a measurement.
- **Fully automatic.** `make build install run && sleep 20 && make testrun`.
- **pax-gfx is a separate git repo** (`components/robotman2412__pax-gfx`, origin
  `nullislandspace/pax-graphics`, gitignored here, wired in via `override_path`).
  Optimizations commit there, on a feature branch, one commit each.
- **Both `-Og` and `-Os` are measured** (user decision). Optimization level is a
  build-time property, so it is a *run-level* axis: two builds, two runs per cycle.

---

## 1. Firmware architecture

`main/main.c` stays thin: NVS, `bsp_device_initialize`, radio off, **allocate the tile
buffers immediately** (before anything else fragments internal RAM), start the console
listener, start the benchmark task, then idle on the input queue (keeping F1 →
`bsp_device_restart_to_launcher()` as a manual escape hatch).

| File | Responsibility |
|---|---|
| `main/bench_config.h` | Tunables: `BENCH_TILE_DIM` 256, `BENCH_N_SAMPLES` 15, `BENCH_N_WARMUP` 2, `BENCH_TARGET_SAMPLE_US` 110000, `BENCH_CELL_BUDGET_US` 2000000, `BENCH_SCRUB_BYTES` 512K, `BENCH_CORPUS_VERSION` |
| `main/bench_console.c/.h` | USB-serial/JTAG token listener |
| `main/bench_corpus.c/.h` | Versioned string corpus, per-cell layout, exact glyph/pixel accounting |
| `main/bench_matrix.c/.h` | Axis enums, `bench_cell_t`, matrix expansion |
| `main/bench_runner.c/.h` | Driver task: PM lock, buffers, cache scrub, calibration, timing, stats |
| `main/bench_report.c/.h` | CRC32-framed record emission, metadata collection |

`main/CMakeLists.txt`: add the sources, add `esp_driver_usb_serial_jtag esp_pm
esp_timer esp_app_format` to `PRIV_REQUIRES`, add the git-hash embedding block (§3.3).

### 1.1 Console listener

Modelled directly on `tanmatsu-launcher/main/usb_debug_listener.c:20-60` — the proven
pattern on this hardware:

```c
usb_serial_jtag_driver_install(&(usb_serial_jtag_driver_config_t){
    .rx_buffer_size = 256, .tx_buffer_size = 1024});
n = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);  // blocking task
```

Blocking read, not a poll loop — a blocked task adds zero scheduler jitter. Do **not**
call `usb_serial_jtag_vfs_use_driver()` and do not touch `stdin`; `printf`/`ESP_LOG`
keep the non-driver console path, exactly as the launcher does. Mixing the driver with
`getchar()` loses bytes non-deterministically.

Tokens (newline-terminated, `\r` stripped): `PING` → `@@BENCH-PONG@@`; `BENCHRUN` →
start the matrix **and suspend the listener task** for the run (no TX contention, no RX
interrupts in the measurement window); `BENCHRUN1` → base cell only;
`DUMPFB <cell-id>` → stream that cell's rendered framebuffer for correctness diffing
(§7.2); `RENDERDEMO` → draw a sample page to the real display for a human look;
`LISTCELLS` and `DUMPCORPUS` → dev aids. Until a run starts, a `@@BENCH-READY@@` banner
is emitted every 2 s carrying schema, git hashes, opt level, cell count.

No USB mode handling is needed: the launcher calls `usb_mode_set(USB_DEBUG)` in
`prepare_device_for_app_launch()` (`tanmatsu-launcher/main/menu/apps.c:136-140`) before
booting an app, so we come up with the debug console already on the bus.

### 1.2 Driver task

`xTaskCreatePinnedToCore(bench_driver_task, "bench", 8192, NULL, 5, &h, 0)` — core 0,
priority 5. Pinning is mandatory because `esp_cpu_get_cycle_count()` is per-core. pax's
own workers (`MCRW0`/`MCRW1`, `pax_renderer_softasync.c:95,114`) run at priority 1 on
cores 0 and 1; during async cells the driver blocks inside `pax_join()`, so they are
never starved.

PM lock, created once and held across the whole matrix so acquire/release never lands
inside a measurement:

```c
esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "bench", &pm_lock);
esp_pm_lock_acquire(pm_lock);
```

`CONFIG_PM_ENABLE`/`CONFIG_PM_DFS_INIT_AUTO` (`sdkconfigs/tanmatsu:30-31`) stay **on**
so the build matches production; the lock neutralises DFS. Never trust it — per cell,
derive effective MHz from `esp_cpu_get_cycle_count()` against `esp_timer_get_time()`
and flag any cell deviating >2% from 360.

### 1.3 Buffers — 256×256 square tiles

Allocated once in `app_main`, before anything else fragments internal RAM, never freed:

```c
tile_int   = heap_caps_aligned_alloc(64, 256*256*3,
               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_CACHE_ALIGNED);
tile_psram = heap_caps_aligned_alloc(64, 256*256*3, MALLOC_CAP_SPIRAM | ...);
scrub      = heap_caps_aligned_alloc(64, 512*1024,  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
```

Square, and 256, for two reasons that both matter:

1. **Orientation becomes apples-to-apples.** All 8 orientations present the same
   drawable rectangle, same row count, same corpus. A 480×64 strip would become 64×480
   under `PAX_O_ROT_CW` and the orientation axis would compare two different workloads.
2. **It must not fit in L2.** 256·256·3 = 196,608 B and 256·256·2 = 131,072 B, both
   above the 128 KB L2 (`CONFIG_CACHE_L2_CACHE_SIZE=0x20000`). A 480×64 tile is 92 KB
   — fully L2-resident, which would make the PSRAM-vs-SRAM axis measure nothing.

A full-screen internal buffer is impossible (480×800×3 = 1,152,000 B against ~500 KB
internal heap; even RGB565 needs 768,000 B), which is why tiles are the only honest way
to run this axis. `BENCH_TILE_DIM` is the single knob; on internal-allocation failure
the app emits `@@BENCH-ABORT@@` with `heap_caps_get_largest_free_block()` and reboots —
loud, not silent. Documented fallback: 224 (150,528 B, still > L2).

Per cell: `memset(tile, 0, used)`, `pax_buf_init(&buf, tile, DIM, DIM, fmt)`,
`pax_buf_reversed(&buf,false)`, `pax_buf_set_orientation`, `pax_noclip`. Teardown is
`pax_buf_destroy` (which will not free, since `do_free == false` for user memory).
`pax_buf_init` does **not** memset user memory — we do it ourselves, for a deterministic
framebuffer hash. Buffer addresses are constant for the whole run, so cache-set
aliasing is not a between-run variable.

### 1.4 One measurement

The unit of work is one **iteration** = one pass over the cell's laid-out corpus.
`pax_draw_text_adv()` is called directly everywhere (never the `pax_draw_text` inline)
so alignment is an explicit parameter and the call shape is uniform.

Per cell:

1. **Setup (untimed)** — format/orientation, corpus layout, `memset`, engine switch if
   it changed, `pax_join()`.
2. **Cache scrub (untimed)** — stream-write then stream-read the 512 KB PSRAM block.
   Evicts the previous cell's tile and code from L2 so every cell starts comparable and
   cell ordering stops mattering. This is what makes cross-run per-cell comparison
   legitimate even when neighbouring cells change.
3. **Calibration (untimed)** — one iteration at `t1`, then
   `reps = clamp(ceil(110000 / t1), 1, 4096)`, `n_samples` from the remaining budget
   (7..15). Both are emitted, so every run is auditable.
4. **Warm-up** — 2 discarded samples (cold I-cache: the pax rasteriser executes XIP
   from flash through cache; plus first-touch of the tile).
5. **Measure** — `n_samples` samples of:
   ```c
   t0 = esp_timer_get_time();
   for (r = 0; r < reps; r++) bench_run_iteration(cell, &buf);
   pax_join();          /* inside the timed region, once per sample */
   t1 = esp_timer_get_time();
   ```
   `pax_join()` is called unconditionally so the code path is identical across
   renderers (it is `NULL` and free for the sync engine). Once per *sample*, not per
   iteration: with `CONFIG_PAX_QUEUE_SIZE=32` the queues backpressure, which is the
   honest user-visible throughput of the async engine. The schema records
   `join_placement:"per_sample"` so this is never ambiguous.
6. **Correctness canary (untimed)** — `memset`, exactly **one** iteration, `pax_join()`,
   FNV-1a over the used bytes → `fb_hash`. A dedicated single-iteration pass makes the
   hash independent of `reps` and therefore directly comparable across runs.
7. **Emit** the CELL and RAW lines plus a human-readable `ESP_LOGI` summary.

Deliberately **not** in the timed region: no `pax_background()` clear — it would cost
more than the text and add variance, and the blend cost does not depend on framebuffer
content. Dead-code defences (insurance; pax is a separate non-LTO static library so
inlining-away is not actually possible): the `pax_2vec2f` return accumulates into a
`volatile`, and `fb_hash` is computed from real framebuffer bytes.

**Clock**: `esp_timer_get_time()` — int64 µs, systimer-backed, `ESP_TIMER_IN_IRAM=y`.
`esp_cpu_get_cycle_count()` is 32-bit (wraps ~11.9 s at 360 MHz) and per-core, used
only as the DFS guard from the pinned task.

---

## 2. The test matrix — 71 cells, ~2.5-3.5 min

A full cross-product is 13,824 cells ≈ 8.4 hours. Instead: **one saturated core cube
plus one-axis-at-a-time sweeps.**

**Core cube (48 cells)** — the axes whose *interactions* the analysis predicts:

| Axis | Values |
|---|---|
| format | `16_565RGB`, `24_888RGB` |
| pixel loop | `fast1` (Sky @9, ratio 1.0, bpp 1, opaque → `blit_char_direct_set`); `fast2` (Saira Regular @18, ratio 1.0, bpp 2 → `blit_char_alpha_blend`); `shader` (Saira Regular @27, ratio 1.5 → `pax_rect_shaded_resuv` + `pax_shader_font_bmp_aa`) |
| orientation | `PAX_O_UPRIGHT` (dx=1, contiguous), `PAX_O_ROT_CW` (dx=width, strided) |
| placement | internal SRAM, PSRAM |
| renderer | sync soft, async multithreaded |

These are the three genuinely distinct pixel loops in pax, crossed against both memory
hierarchies, both access patterns, both threading models, in both formats. This is what
makes each optimization individually attributable — e.g. R6 must collapse the
UPRIGHT/ROT_CW gap, and if it moves 888/PSRAM more than 565/SRAM, the cube shows it.

**Sweeps (23 cells)** from base = `(888, fast2, ROT_CW, psram, async2)` — the launcher's
actual production configuration:

| Group | n | Values |
|---|---|---|
| `orient` | 6 | the six orientations not in the cube (all 8 covered in total) |
| `font` | 9 | `sky@18` (2.0), `sky@27` (3.0), `sky_mono@9` (mono, 3 ranges), `sky@13` (1.444, shader/bpp1), `saira_regular@36` (2.0), `saira_regular@27 aa=false`, `marker@22` (1.0), `marker@33` (1.5), `saira_condensed@45` (1.0) |
| `rend` | 1 | async single-threaded |
| `alpha` | 2 | colour alpha `0x80` on `fast1` (forces bpp-1 off `direct_set`) and on `fast2` |
| `len` | 2 | all-short corpus (≤16 B, always SSO) and all-long (≥40 B, forces the per-draw `malloc`, `PAX_SSO_BUF_LEN=32`) |
| `align` | 1 | `PAX_ALIGN_CENTER` (extra measure pass inside *each* worker, analysis §5.1) |
| `drift` | 2 | the base cell repeated as the **first** and **last** cell of the run |

Axis coverage vs. the requested list: format ✓, font size ✓ (ratios 0.489…3.0),
orientation ✓ (all 8), PSRAM vs SRAM ✓, sync vs async ✓ (all three engines), integer vs
fractional ratio ✓, AA on/off ✓, glyph bpp ✓ (1 vs 2 vs mono).

AA on/off is implemented as a RAM copy of `pax_font_saira_regular_raw` with
`recommend_aa` flipped, sharing the same `ranges` pointer — the only way to isolate
`pax_shader_font_bmp_aa` (4 glyph samples + 2 `floorf` per pixel) from
`pax_shader_font_bmp`, hitting exactly the branch at `pax_text.c:216`. On the bpp-1
shader cell, AA is irrelevant because `dispatch_glyph` picks `pax_shader_font_bmp_pal`
first; that cell records `aa_effective:false` rather than misleading.

**Corpus and layout** (`bench_corpus.c`): a versioned ASCII string table (version in the
metadata, so a corpus change invalidates comparisons loudly). Rows at
`line_h = round(glyph_height * ratio) + 2` until the tile is full, strings cycled.
Each row is measured with `pax_text_size_adv()` and its **byte length truncated** to the
longest UTF-8-safe prefix that fits. Consequences, all desirable: the workload fills the
whole tile so the working set is the full tile for every font (which is what makes the
memory axis meaningful everywhere); nothing is ever clipped, so `blit_char_clip` never
truncates and work per iteration equals the accounting; and layout is a pure function of
(font, size, tile, orientation), hence bit-identical across runs.

**Glyph accounting**: mirror pax's `text_get_range()` scan, skip `0x20` (pax skips it
entirely), accumulate `src_px` from `pax_bmpv_t.draw_w/draw_h` and
`dest_px = round(draw_w*s) * round(draw_h*s)`. `ns_per_dest_px` is the only metric
comparable *across* cells; `ns_per_iter` is the comparison basis across runs.

Ordering is fixed and deterministic, sorted by renderer first (minimising
`pax_set_renderer` teardown/rebuild). Order-independence is not assumed — it is
*measured*, by the `drift` pair.

**Duration**: 71 × 2.0 s budget + ~0.15 s/cell overhead + ~10 s boot ≈ **2 min 45 s**.
`BENCH_CELL_BUDGET_US` is the dial (3.5 s → ~4 min 20 s), all inside the target.

---

## 3. Console output protocol

### 3.1 Framing

```
@@BENCH-<KIND>@@ <compact-json> @@<crc32-hex8>@@
```

`KIND` ∈ `READY|PONG|BEGIN|CELL|RAW|CELLDEF|END|ABORT`. Written with a single
`printf("%s\n", line)` of a pre-formatted buffer — **never** through `ESP_LOGx` (whose
`I (1234) tag:` prefix and ANSI colour would have to be stripped). The CRC32 covers the
JSON substring; the host recomputes it with `zlib.crc32` and discards+counts any line
that fails. That is the defence against another task's `ESP_LOG` interleaving mid-write.
Emission is additionally mutex-serialised, and the listener task is suspended during a
run. Lines stay under 640 B; the per-cell sample array goes on its own `RAW` line so it
can never push the summary over the limit.

`@@BENCH-END@@` is the host's stop condition, `@@BENCH-ABORT@@` the failure one —
neither depends on the reboot. After END: flush, 300 ms delay, then
`bsp_device_restart_to_launcher()` (`bsp/device.h:42`, already used by F1 at
`main/main.c:256`).

### 3.2 Self-describing metadata

The BEGIN record carries everything needed to know what a number means:
`schema`, `cells`; `build{app_git, app_dirty, pax_git, pax_dirty, pax_branch, idf, opt,
elf_sha256}`; `hw{chip, rev, cores, cpu_mhz_nom, cpu_mhz_meas, l2_kb, l2_line, psram,
psram_mode, psram_mhz, dsi_active}`; `cfg{pm_enable, pm_lock, tick_hz, wdt,
pax{bounds_check, orientation, async, range_setter, range_merger, fixed_point,
long_fixed_point, queue_size, sso_len, version}}`; `mem{tile_dim, int_addr, psram_addr,
int_free, int_largest}`; `bench{n_samples, warmup, target_sample_us, cell_budget_us,
corpus_ver, join_placement, driver_core, driver_prio, worker_prio}`.

All `cfg.pax.*` and `build.opt` come from `sdkconfig.h` macros resolved at compile time,
so they cannot drift from the binary. `hw.cpu_mhz_meas` is an *independent* measurement
of what DFS is actually doing, not a config echo.

Each CELL record carries the axes (including the **detected render path** —
`blit/direct`, `blit/alpha`, `shader/aa`, `shader/pal` — derived from the same
predicates `dispatch_glyph` at `pax_text.c:186-201` and `pax_swr_blit_char` at
`pax_renderer_soft.c:570-578` use), the workload accounting, the statistics, quality
flags, and `fb_hash`.

### 3.3 Embedding git hashes

A CMake function in `main/CMakeLists.txt` runs `git -C <dir> rev-parse --short=12 HEAD`,
`--abbrev-ref HEAD` and `status --porcelain` for **both** repos and passes them as
compile definitions. Critically it also sets

```cmake
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${dir}/.git/HEAD" "${dir}/.git/index")
```

so a moved HEAD or changed index forces a reconfigure and the embedded hash cannot go
stale. A dirty tree in either repo is recorded and marks the run non-reproducible. The
host independently runs `git rev-parse` and warns loudly on disagreement — that is the
"you forgot to rebuild and reflash" detector.

---

## 4. Dual optimization level (`-Og` and `-Os`)

Optimization level is a build-time property, so it is a **run-level axis**: each cycle
builds and runs twice. `Makefile:10,11,8,51` already parameterise everything needed —
`SDKCONFIG_DEFAULTS`, `SDKCONFIG` and `BUILD` are all `?=` and all flow into
`IDF_PARAMS`. So:

```make
OPT ?= os                                  # os | og
SDKCONFIG_DEFAULTS ?= sdkconfigs/general;sdkconfigs/$(DEVICE);sdkconfigs/opt-$(OPT)
SDKCONFIG ?= sdkconfig_$(DEVICE)_$(OPT)
BUILD ?= build/$(DEVICE)-$(OPT)
```

Two new tracked fragments: `sdkconfigs/opt-os` (`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`)
and `sdkconfigs/opt-og` (`CONFIG_COMPILER_OPTIMIZATION_DEBUG=y`). Separate build dirs
and sdkconfigs per level means switching needs no `make clean` and both stay cached.
`sdkconfig_*` and `build/` are already gitignored.

```make
bench:            # one level
	$(MAKE) build install run OPT=$(OPT); sleep 20; $(MAKE) testrun OPT=$(OPT)
benchboth:
	$(MAKE) bench OPT=os
	$(MAKE) bench OPT=og
```

Full cycle wall time ≈ 2 × (build + install + ~3 min run) ≈ 8-10 min. `-Os` is the
primary series — it is what the launcher ships and therefore what an optimization must
actually improve. `-Og` is the control: it inlines less, so it exaggerates exactly the
call-boundary overhead the analysis blames, and comparing the two shows how much of each
win survives the optimizer. The host **refuses to compare runs across levels** (
`build.opt` mismatch) unless `--allow-mismatch`; `PROGRESS.md` keeps them as separate
series with separate baselines.

---

## 5. Host client

`tools/` is not gitignored (only `tools/__pycache__` is), so these are tracked. Pure
stdlib + pyserial, as `Makefile:264` already relies on.

**`tools/testrun.py`** —
`--port URL [--out-dir results] [--label NAME] [--baseline F] [--set-baseline]
[--ready-timeout 90] [--run-timeout 600] [--line-timeout 45] [--open-retries 15]
[--allow-mismatch]`

- **Connect**: `serial.serial_for_url(url, baudrate=115200, timeout=1,
  do_not_open=True)` then `.open()` in a retry loop catching `SerialException`/`OSError`.
  Handles both `/dev/ttyACM0` and `rfc2217://localhost:4001`; **never hardcode** — it
  comes from `$(PORT)`.
- **Handshake**: read for up to `--ready-timeout`, sending `PING\n` once a second,
  accepting `READY` or `PONG`. This replaces trusting `sleep 20` — it tolerates the
  device still booting, the rfc2217 proxy reopening after re-enumeration, or the app
  having been up for a while. Reconnect-and-retry during this phase.
- **Run**: send `BENCHRUN\n`, read until END/ABORT, with a per-line inactivity timeout
  (45 s, longer than the slowest cell) and an overall timeout. On timeout, save what was
  captured to `results/failed/` with `"status":"timeout"` and exit non-zero.
- **Parse**: `^@@BENCH-([A-Z]+)@@ (.*) @@([0-9a-f]{8})@@$`, verify CRC, count
  `corrupt_lines`, tee non-matching lines (ESP_LOG noise) verbatim to `results/raw/`.
- **Stop cleanly at END** — the device reboots ~300 ms later and the port drops.
- **Write** `results/runs/<UTC>-<label>-<opt>-<pax_git8>.json`, then regenerate reports.
- **Exit codes**: 0 ok, 1 connection/timeout, 2 firmware ABORT, 3 unstable run,
  4 correctness regression.

**`tools/bench_report.py`** — separate so reports regenerate offline without a device
(`make testreport`). Deterministic and idempotent: regenerating twice must produce
byte-identical output (that is a verification step). **`tools/paxlog.sh`** regenerates
`results/pax-commits.md` from the pax checkout's log.

Correctness flags on the same client: `--capture-refs` (dump every cell's framebuffer to
`results/refs/`, done once at baseline), and `--bless <cells> --reason "..."` (accept a
changed reference). On a hash mismatch during a normal run the client reconnects and
dumps only the affected cells automatically — no extra flag needed.

Makefile targets: `testrun`, `testbaseline` (`--set-baseline`), `testrefs`
(`--capture-refs`), `testreport`, `paxlog`, `bench`, `benchboth`. `$(PORT)` (not
`$(BADGELINKPORT)`) — the benchmark speaks over the debug console, not BadgeLink. The
user's documented cycle works unchanged.

---

## 6. Statistical soundness

**Repetitions**: 2 discarded warm-up + 15 measured samples per cell (min 7 for the
expensive shader cells). 15 is odd, so the median is a real observation. Each sample is
`reps` iterations, auto-calibrated to ≈110 ms — long enough that the `esp_timer` call
pair and a single 10 ms FreeRTOS tick are noise, short enough to fit the budget.

**Headline metric: the median.** Noise on a CPU-bound workload (preemption, interrupts,
PSRAM contention, DSI DMA) is strictly *additive*, so the distribution is right-skewed
and the mean is a poor estimator. The **minimum** best estimates true instruction cost
and is most repeatable, but is a best case reachable by a luckily-warm cache. The median
is robust to outliers *and* reflects sustained throughput. So: median is headline, min
reported alongside as the clean-room diagnostic — and they must move together; a change
that improves one but not the other is itself a signal.

**Instability flags** per cell: `cv = sd/median` > 3% (sync) / 6% (async — worker
scheduling genuinely adds variance and a tighter threshold would just cry wolf);
`spread = (max-min)/median` > 10% / 15%; count of samples > 1.5× median; DFS deviation
> 2%. Run-level: `status = "unstable"` if >5% of cells are flagged or any cell has DFS.

**`base_drift_pct`** — the base cell measured as cell 0 and again as the last cell. An
assumption-free measurement of thermal/DFS/ordering drift across the run; `|drift| > 2%`
marks the run untrustworthy. This single number says whether to believe the run at all.

**Baseline comparison**: cells matched by `id`; any cell in one run and not the other is
reported as added/removed, never silently dropped. Per cell
`pct = (new.ns_per_iter / base.ns_per_iter - 1) × 100`, plus `pct_min`, plus the
baseline `cv` so a "10% win" on a 12%-CV cell is visibly not significant — a change is
called **significant** only if `|pct| > 2 × max(cv_new, cv_base)`.

**Aggregate: geometric mean of per-cell ratios**, not the arithmetic mean of percentages
(ratios compose multiplicatively; averaging speedup percentages is simply wrong).
Reported overall and per group: `fast1`/`fast2`/`shader`, per format, per placement, per
renderer.

**Comparability gate**: refuse to compare runs differing in `build.opt`, `corpus_ver`,
`tile_dim`, `n_samples`, `cfg.pax.*` or `schema` without `--allow-mismatch`.

---

## 7. Correctness verification

A fast optimization that renders wrongly is worse than no optimization, and several of
the planned changes (R1's coverage early-out, R5's mask-aware run primitive, R6's loop
re-nesting) rewrite exactly the code that decides what each pixel becomes. Speed numbers
alone cannot catch that. So correctness is a **two-tier system**: a cheap hash on every
cell of every run, and reference framebuffers captured once from the baseline that any
later run can be diffed against pixel by pixel.

### 7.1 Tier 1 — hash gate, every cell, every run

Already described in §1.4 step 6: a dedicated single-iteration deterministic pass,
FNV-1a over the used framebuffer bytes, emitted as `fb_hash`. Independent of `reps`, so
directly comparable across runs. The host compares every cell's hash against the
baseline; any mismatch is `CORRECTNESS REGRESSION` and exit code 4. Costs nothing and
runs always.

This tier answers "did anything change?" — never "what changed?".

### 7.2 Tier 2 — reference framebuffers and pixel diffs

**Capture.** A `DUMPFB <cell-id>` console token makes the firmware re-render that cell's
canary pass and stream the raw framebuffer out as base64 in `@@BENCH-FB@@` chunk records
(chunk index, total, CRC per chunk). 256×256×3 = 196,608 B → ~262 KB of base64, a few
seconds over USB-CDC — acceptable because it is on demand, not part of a normal run.

`tools/testrun.py --capture-refs` walks every cell, dumps each framebuffer, and writes
`results/refs/<cell-id>.png` plus a `results/refs/manifest.json` recording the pax
commit, opt level, corpus version and hash each reference was captured at. Done **once,
at baseline time**, and committed. PNG conversion is hand-rolled (zlib + a minimal PNG
writer, ~40 lines) so the tooling stays stdlib-only — the references are then viewable
in any image viewer and diffable in the browser on GitHub.

**Compare.** When a later run reports a hash mismatch for cell X, the client
automatically reconnects, issues `DUMPFB X`, and produces:

- `results/diffs/<runid>/<cell-id>.png` — a three-panel image: reference, actual, and an
  amplified absolute difference.
- Numeric verdict in the run JSON and in the report: `pixels_differing`,
  `pct_differing`, `max_channel_delta`, `mean_channel_delta`, and the bounding box of
  the differing region.

That last part is what makes the tier useful rather than merely alarming. The numbers
distinguish the three cases that matter:

| Signature | Almost certainly |
|---|---|
| `max_channel_delta == 1`, differences scattered over glyph interiors | a rounding change in the blend — usually acceptable, needs a human decision |
| whole glyphs missing, or a differing region with a rectangular bounding box | a real bug: a skipped run, an off-by-one in the loop bounds, a clipping error |
| differences only at coverage extremes | R1's early-out is not bit-exact — exactly the failure it was written to risk |

**Re-blessing.** When a difference is inspected and judged acceptable (a deliberate
rounding change, say), `tools/testrun.py --bless <cell-id>[,<cell-id>...]` updates that
cell's reference PNG and its manifest entry, recording the pax commit that introduced
the change and a mandatory `--reason` string. Re-blessing is never automatic and never
silent: the manifest keeps the full history, and `PROGRESS.md` shows a `blessed` column
so a run whose "correctness OK" rests on a re-blessed reference is visibly distinct from
one that matched the original baseline exactly.

**Visual sanity check.** Independently of the diff machinery, a `RENDERDEMO` token
renders a fixed sample page (all five built-in fonts, several sizes, both paths) to the
real display and blits it. This is not part of any measurement — it exists so a human
can look at the screen after a risky optimization and see immediately that text still
looks like text. Cheap to add, and it catches the class of error where every cell's
hash changed and the diff images are equally wrong.

---

## 8. What lives in git

Everything the project needs to be resumed later lives **in the repo**, not in any
external scratch location — including this plan itself:

```
pax_font_rendering_optimization.md        # existing: the root-cause analysis
pax_font_benchmark_plan.md                # THIS PLAN, committed alongside it
results/
  PROGRESS.md                             # generated tracking table, never hand-edited
  baseline-os.json  baseline-og.json      # pinned references, one per opt level
  runs/<UTC>-<label>-<opt>-<paxgit>.json  # every run, forever
  raw/<same>.log                          # verbatim console capture
  detail/<same>.md                        # per-cell table vs baseline
  refs/<cell-id>.png  refs/manifest.json  # baseline reference framebuffers (§7.2)
  diffs/<runid>/<cell-id>.png             # three-panel diffs for mismatched cells
  pax-commits.md                          # generated by `make paxlog`
tools/  testrun.py  bench_report.py  paxlog.sh
```

`pax_font_benchmark_plan.md` is committed as **step 1** of the implementation, before
any code, and updated as decisions change — so picking the project back up months later
needs nothing but the repo. It carries the design rationale (why 256×256, why the
median, why the cache scrub, why two optimization levels); `results/PROGRESS.md` carries
the running record of what has actually been measured.

`PROGRESS.md` is one row per run: date, pax commit, opt level, overall geomean, then
per-group columns (`fast1`, `fast2`, `shader`, `565`, `888`, `SRAM`, `PSRAM`), drift, and
the correctness verdict. Plus an optimization log: one entry per pax commit with full
hash, subject, files touched, measured geomean, and any regressed cells.

**Tying results to code**: the pax repo is gitignored here, so a bare hash would be
unresolvable by anyone else. Three measures: (1) firmware-embedded hash + branch + dirty
flag, kept fresh by `CMAKE_CONFIGURE_DEPENDS`; (2) `make paxlog` writes hashes, subjects
and diffstats into the app repo; (3) the feature branch is pushed to the
`nullislandspace/pax-graphics` remote and the baseline commit tagged `bench-baseline`.

All optimizations on **one** branch `opt/text-rendering`, **one commit each**, so any can
be reverted or bisected and separately re-benchmarked. Each commit message ends with the
measured geomean and correctness verdict.

---

## 9. Implementation sequence

Each step ends with a concrete verification.

**0. Commit the plan.** Write this document to `pax_font_benchmark_plan.md` in the repo
root, next to the existing analysis, and commit it. Everything below is then resumable
from the repo alone.

**1. Add the opt-level fragments and Makefile parameterisation** (§4). *Verify:*
`make build OPT=os` and `OPT=og` both succeed into separate build dirs;
`grep CONFIG_COMPILER_OPTIMIZATION sdkconfig_tanmatsu_os` shows `_SIZE=y`;
`make size-components OPT=os` vs `OPT=og` shows the pax component differing.

**2. `bench_config.h` + `bench_report.c/.h`** — CRC32, framing, JSON, metadata, git
hashes. *Verify:* emit BEGIN once from `app_main`; capture with a one-line
`serial_for_url` reader; payload passes `python3 -m json.tool` and the CRC matches
`zlib.crc32`.

**3. `bench_console.c/.h`.** *Verify:* `PING` → `PONG`; READY banner every 2 s;
`ESP_LOG` still works and is not garbled.

**4. `bench_corpus.c/.h`.** *Verify:* `DUMPCORPUS` prints
`rows/strings/glyphs/chars/src_px/dest_px` per font cell; hand-check Sky @9 (height 9,
`line_h` 11, 23 rows in 256 px) and that `dest_px == src_px` at ratio 1.0 and ≈4× at 2.0.

**5. `bench_matrix.c/.h`.** *Verify:* `LISTCELLS` emits exactly 71 lines; `sort|uniq -d`
on ids is empty; every axis value from §2 appears at least once.

**6. `bench_runner.c/.h`.** *Verify:* `BENCHRUN1` gives a plausible `reps` (5-10),
`cv < 3%`, `mhz ≈ 360`, and `ns_px` in the low hundreds (the analysis predicts ~120
cycles/px ≈ 330 ns at 360 MHz — wildly off means the accounting or timing is wrong).
Deliberately draw half the rows and confirm `fb_hash` changes.

**7. Full run.** *Verify:* 2-5 min wall time, zero aborts, `base_drift_pct < 1%`, few
unstable cells, clean return to the launcher.

**8. Host tooling.** *Verify:* `make testrun` writes loadable JSON with
`corrupt_lines == 0`; `make testreport` twice produces byte-identical `PROGRESS.md`;
test the reconnect path by restarting the rfc2217 proxy during the READY wait.

**9. Repeatability gate — do not skip.** Three consecutive `make testrun` with no
rebuild and no reboot between them; require per-cell median spread < 3% (sync) / < 6%
(async). **If this fails, no optimization result will be attributable to code** — fix it
here (raise the cell budget, raise `N_SAMPLES`, hunt the noise) before going further.

**10. Capture the baseline, and the correctness references.** `make benchboth` with
`--set-baseline` for each level, then `make testrefs` (`testrun.py --capture-refs`) to
dump every cell's reference framebuffer to `results/refs/`. Commit
`results/baseline-*.json`, the runs, raw logs, `refs/`, `PROGRESS.md`, `pax-commits.md`.
Tag the pax commit `bench-baseline`, branch `opt/text-rendering` from it. *Verify:* spot
-check a handful of reference PNGs by eye — they must contain legible text; a reference
captured from broken rendering would silently bless the bug forever.

**11+. The optimizations** — each its own pax commit, each followed by a full cycle,
each gated on `fb_hash` being unchanged for all 71 cells (and on a pixel diff being
inspected and explicitly blessed where it is not):

| # | Rec | Change | Where | Expected signal |
|---|---|---|---|---|
| 11 | **R2** | call `pax_col_merge_inlined()` (already `always_inline`, `pax_internal.h:417`) instead of `pax_col_merge()` | `pax_renderer_soft.c:545` | large on `fast2`, **nothing** on `fast1` — a perfect attribution check |
| 12 | **R3** | hoist `buf->getter/setter/buf2col/col2buf` into locals, mirroring `pax_swr_scaled_image` at `pax_renderer_soft.c:145-149` | `pax_swr_blit_char_impl` | broad, both blit paths, both formats |
| 13 | **R1** | early-out on `value == 0`; loop-hoisted `col2buf(color)` store at full coverage | `pax_swr_blit_char_impl` | biggest on 2-bpp AA; the primary customer of the §7.2 pixel diff |
| 14 | **R4** | strength-reduce `x/scale`, `y/scale` into counters | `pax_renderer_soft.c:530` | modest; largest at scale > 1 (`sky@18/27`, `saira@36`) |
| 15 | **R9** | `set_source_files_properties(... COMPILE_OPTIONS "-O2")` on the rasteriser TUs inside pax's own `core/CMakeLists.txt` | pax build | broad; also bounds how much of the gap is purely compiler-level |
| 16 | **R6** | orientation-adaptive loop nesting — inner axis chosen by `abs(stride)==1` | `pax_swr_blit_char_impl` | must collapse the UPRIGHT/ROT_CW gap; largest on 888/PSRAM |
| 17 | *(verify §6 of the analysis)* | `nm …/pax_dh_shaded.cpp.obj \| grep -E '__divtf3\|__floatditf\|__trunctfsf2'`; if present, fix the `fixpt_t`→`float` conversion | `pax_fixpt.hpp` | shader cells only — isolated by design |
| 18 | **R5** | mask-aware range merger in the setter table, sibling to `pax_range_merger_888rgb/565rgb` | `pax_setters.c`, `pax_renderer_soft.c` | the structural fix; format-specialised, caller-blind |
| 19 | **R7** | draw entry point that skips the discarded measure pass; share the measure between workers | `pax_text.c:502`, `pax_renderer_softasync.c:1322` | async cells and the `align=CENTER` cell |
| 20 | **R8** | glyph/string cache in buffer-native format | `pax_text.c` | order-of-magnitude candidate; largest and riskiest, hence last |

R10-R12 from the analysis are launcher-side and out of scope here — though R11's payoff
is quantified directly by the `fast2` vs `shader` ratio.

---

## 10. Risks

| Risk | Mitigation |
|---|---|
| **DFS** silently drops the clock (`CONFIG_PM_ENABLE`, `CONFIG_PM_DFS_INIT_AUTO`) | `ESP_PM_CPU_FREQ_MAX` lock for the whole matrix; keep PM enabled so the build matches production; **independently verify** per cell via cycle-count/timer ratio and flag >2% deviation — never trust the lock |
| **Cache state carried between cells** makes results order-dependent | 512 KB PSRAM scrub between cells + 2 warm-up samples; buffers allocated once so addresses (and cache-set aliasing) are constant; order-independence then *measured* by the drift pair |
| **Tile fits in L2** → the memory axis measures nothing | 256×256 = 196,608 B (888) / 131,072 B (565), both > 128 KB L2. This is exactly why a 480×64 strip is the wrong shape |
| **Internal allocation fails** (~500 KB free, fragmented) | allocate the internal tile first, before anything else fragments; on failure emit ABORT with `largest_free_block` and reboot; documented fallback `BENCH_TILE_DIM=224` |
| **USB re-enumeration** on reboot; the rfc2217 proxy (`tanmatsu-badgefs/rfc2217proxy.c`) opens the device once with no reopen logic | `do_not_open=True` + open-retry loop; READY/PONG handshake instead of trusting `sleep 20`; reconnect during the READY wait; stop reading at END so the reboot never corrupts the capture |
| **Async worker noise** (workers at priority 1, contending with BSP/coprocessor tasks) | 15 samples + median; separate looser thresholds for async cells; raw sample arrays published; worker priority deliberately left alone (measure what production does) but recorded |
| **`ESP_LOG` interleaving into a result line** | single-`printf` of a pre-formatted line + per-record CRC32 + emitter mutex + listener suspended during the run; non-zero `corrupt_lines` invalidates the run |
| **An optimization that renders wrongly looks like a huge win** | Two tiers (§7): `fb_hash` per cell on every run (mismatch = exit code 4), plus baseline reference framebuffers in `results/refs/` so any mismatch produces a real pixel diff with differing-pixel counts and max channel delta. Re-blessing a changed reference requires an explicit `--bless` with a reason and shows up in `PROGRESS.md` |
| **A reference captured from already-broken rendering** blesses the bug permanently | References are captured at baseline, before any optimization, and spot-checked by eye at step 10; the `RENDERDEMO` token renders a sample page to the real display for a human look |
| **Stale flash** — measuring a binary that predates the change | firmware-embedded git hashes forced fresh via `CMAKE_CONFIGURE_DEPENDS`; host cross-checks against local `git rev-parse` and warns; dirty trees flagged in the result and in `PROGRESS.md` |
| **DSI panel** continuously streaming 1.15 MB from PSRAM | a *constant* background load, identical in every run, so it does not bias comparisons; recorded as `hw.dsi_active`; deliberately not disabled, since the launcher runs with it |
| **`reps` differing between compared runs** changes cache behaviour subtly | normalise per iteration; scrub + warm-up per cell; host warns when matched cells differ >4× in `reps` |
| **Measuring the wrong thing** — `pax_draw_text_adv` walks the string 2-3× regardless | a companion `pax_text_size_adv`-only cell per font makes the measure-pass share visible and subtractable |
