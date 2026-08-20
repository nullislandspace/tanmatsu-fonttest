# PAX font rendering — benchmark progress

Tracking file for the optimization project described in
[`../pax_font_benchmark_plan.md`](../pax_font_benchmark_plan.md), which measures
the rendering costs analysed in
[`../pax_font_rendering_optimization.md`](../pax_font_rendering_optimization.md).

Blocks between `<!-- generated -->` markers are written by
`tools/bench_report.py` (`make testreport`) and must not be hand-edited. The
prose and the status table are maintained by hand.

---

## Status

| Step | State | Verification |
|---|---|---|
| 0. Plan in repo | done | `pax_font_benchmark_plan.md` committed |
| 1. Dual optimization level | done | `-Os` 363,568 B vs `-Og` 427,568 B, correct `CONFIG_COMPILER_OPTIMIZATION_*` in each sdkconfig |
| 2. Report protocol + git identity | done | CRC verified host-side, zero corrupt records; embedded hashes match both repos |
| 3. Console listener | done | `PING`→`PONG`, READY banner, `ESP_LOG` coexists with the driver |
| 4. Corpus + accounting | done | Sky@9 → 23 rows as predicted; `dest_px == src_px` at ratio 1.0; `dest_px == 4×src_px` at ratio 2.0 |
| 5. Matrix | done | exactly 71 cells, no duplicate ids, all 8 orientations / 3 renderers / 2 formats / 2 memories / 5 render paths present |
| 6. Timing loop | done | see step 7 |
| 7. Full run | done | 71/71 cells, `status=ok`, `corrupt_lines=0`, 117.8 s wall, drift +0.10%, zero flagged cells, 360.00 MHz on every cell |
| 8. Host tooling | done | `testrun.py` captures; `bench_report.py` compares, gates and regenerates this file |
| 9. Repeatability gate | **passed** | three back-to-back runs, 71 cells: worst spread 0.47% against a 3% limit, median 0.06%, zero cells over limit |
| 10. Baseline + reference framebuffers | done | `baseline-os.json` and `baseline-og.json`, both 71/71 at zero corrupt lines and zero flagged cells; 71 reference PNGs, every hash matching its measured cell, four spot-checked by eye; pax tagged `bench-baseline`, `opt/text-rendering` branched from it |
| 11+. Optimizations | **ready to start** | everything a result needs to be attributable is in place |

## Where the time goes

Per-pixel cost of the three distinct pixel loops, from the most recent run.
This is the table that decides what to optimize; the ratio tables below only say
whether a change helped.

<!-- generated: headline -->
From `20260820-102913-baseline-Os-964533a6` (Os, pax `964533a6d894`).

| Pixel loop | Cells | Median ns/dest px | Min | Max |
|---|---|---|---|---|
| fast1 | 16 | 360.1 | 190.7 | 397.7 |
| fast2 | 16 | 447.9 | 308.8 | 475.4 |
| shader | 16 | 7,137.6 | 3,760.2 | 7,490.9 |
<!-- /generated: headline -->

## What the first full run says

Four results from the first complete matrix, all of which change the priority
order in the plan's §9 optimization table:

- **The shader path costs 16× the alpha-blend path and 20× the direct-blit
  path, per destination pixel.** A font drawn at a fractional scale does not get
  gradually slower, it falls off a cliff onto `pax_rect_shaded_resuv`. This is
  the single largest effect in the whole matrix and it dwarfs every
  micro-optimization in the plan's list.
- **Pixel format barely matters.** 565 → 888 is ×1.007 across 24 matched cube
  pairs. The analysis argued ~74% of the per-pixel cost is format-independent;
  measured, it is closer to 99%. Optimizations do not need to be format-specific
  to pay off, which is convenient given the no-device-specific-code constraint.
- **Orientation barely matters** — ×1.021 for `UPRIGHT` → `ROT_CW`. R6
  (orientation-adaptive loop nesting) was predicted to collapse a large gap.
  There is no large gap to collapse, so R6 should drop down the list until
  something else explains the cost.
- **Memory placement barely matters** — internal SRAM is only 1.5% faster than
  PSRAM, despite a 192 KB tile against a 128 KB L2. The workload is
  instruction-bound, not memory-bound. That is a direct confirmation of the
  analysis's central claim, and it means the wins have to come from executing
  fewer instructions per pixel, not from better locality.

One more, worth its own line because it is a target rather than a finding: the
async multithreaded renderer is only **12% faster than the synchronous one** on
a dual-core part. Near-linear scaling is not expected, but 1.12× from a second
core is poor enough to suggest the per-worker duplicated measure pass (R7) is
real and costly.

## Harness quality

The numbers above are only worth having if the harness is quiet, and it is:
across all 71 cells of the first full run, the worst coefficient of variation
was 0.25% against a 3% limit, no cell raised any quality flag, the measured CPU
frequency was 360.00 MHz everywhere (so the PM lock held and DFS never moved),
and the base cell measured first and last differed by 0.10% — meaning cell
ordering and thermal drift do not affect results, so per-cell comparison across
runs is legitimate.

## The -Og control series

`-Og` exists to bound how much of the cost is the compiler rather than the code.
The answer is: not much.

| Group | -Og vs -Os |
|---|---|
| overall | +4.7% |
| fast1 | +10.6% |
| fast2 | +5.5% |
| shader | **-2.1%** |

Two things follow. The entire span between a size-optimized build and a
debug-optimized one is smaller than the effect the shader path has on a single
draw, so no amount of compiler flag work addresses what actually makes text
rendering slow here. And `-Og` being *faster* on the shader path is a loose
thread worth pulling: whatever `-Os` does there, it is a pessimisation.

Rendering is bit-identical between the two levels on all 71 cells, which is a
real check on both the harness and PAX's determinism -- the reference PNGs were
captured from an `-Os` build and every `-Og` cell matched them.

## What counts as a real win

Two noise floors, and they are an order of magnitude apart:

- **Re-running the same binary**: 0.01% between runs, overall. The four runs on
  the pre-fix binary in the table below land within a 0.01% band of each other.
- **Rebuilding without changing behaviour**: up to **1.2%** on a group. The same
  benchmark code, recompiled after an unrelated fix to a console argument
  buffer, moved `fast1` by 1.2% and `fast2` by 0.4% in opposite directions. That
  is code layout -- alignment, cache-set placement, branch offsets -- not
  rendering.

The second number is the one that matters. A claimed win under roughly 1.5% on a
single group is not distinguishable from having relinked the binary, no matter
how many samples back it. Real optimizations should be judged on the overall
geometric mean, on moving the group they were predicted to move, and on
*not* moving groups they should not touch -- R2 in the plan's table is a good
example, since it must show up on `fast2` and be silent on `fast1`.

## Results

One row per run in `results/runs/`. The diagnostic-phase runs that produced the
findings above were measured on earlier app binaries and are kept in
`results/runs/pre-baseline/`, out of this table, because the ~1% rebuild noise
below makes them not directly comparable.

One row per captured run. `Overall` and the per-group columns are geometric
means of per-cell ratios against the baseline for that optimization level;
below 1.0 is faster. `Correct` compares every cell's framebuffer hash.

<!-- generated: results -->
| Run | pax commit | Opt | Cells | Overall | fast1 | fast2 | shader | Drift | Correct |
|---|---|---|---|---|---|---|---|---|---|
| `20260820-102913-baseline-Os-964533a6` | `964533a6d894` | Os | 71 | 1.0000 | - | - | - | +0.10% | baseline |
| `20260820-103259-baseline-Og-964533a6` | `964533a6d894` | Og | 71 | 1.0000 | - | - | - | -0.14% | baseline |
| `20260820-111843-Os-f18645c3` | `f18645c3ac7e` | Os | 71 | 0.9702 | 0.9961 | 0.9388 | 1.0006 | +0.04% | ok |
| `20260820-112224-Og-f18645c3` | `f18645c3ac7e` | Og | 71 | 0.9791 | 1.0045 | 0.9535 | 0.9994 | +0.02% | ok |
| `20260820-112745-Os-c3ad1df6` | `c3ad1df6f621` | Os | 71 | 0.8252 | 1.0123 | 0.6555 | 1.0034 | +0.07% | ok |
| `20260820-113129-Og-c3ad1df6` | `c3ad1df6f621` | Og | 71 | 0.8299 | 1.0027 | 0.6702 | 0.9959 | +0.10% | ok |
<!-- /generated: results -->

## Correctness references

`results/refs/` holds one PNG per cell, captured from the baseline firmware and
verified two ways: the streamed bytes hash to what the dump claimed, and that
hash matches what the same cell reported during the measured run. The second
check exists because the first one passed on two references that had been
captured from the wrong cell entirely.

A useful by-product: dumps are rendered through the synchronous engine while the
measured hashes come from each cell's own renderer, so the match across all 71
cells means the async multithreaded engine produces byte-identical output to the
synchronous one. It is not buying its 12% with a rendering shortcut.

## Optimization log

One entry per pax commit on the `opt/text-rendering` branch.

### `c3ad1df6f621` — skip transparent glyph pixels, short-circuit covered ones

`pax_renderer_soft.c`, `pax_renderer_softasync.c`. The plan's **R1**. The blit did
a full read, two colour conversions, a blend and a write for every pixel in a
glyph's bounding box, including the ones the blend cannot change. Both cases are
provable from `pax_col_merge_inlined()` rather than assumed: coefficient 0 makes
the lerp return `base` exactly, and coefficient 255 makes it return the drawing
colour, which is loop-invariant and now converted once before the loop.

| Group | -Os | -Og | Predicted |
|---|---|---|---|
| overall | **-17.48%** | -17.01% | |
| fast2 (alpha blend) | **-34.45%** | -32.98% | biggest |
| fast1 (direct set) | +1.23% | +0.27% | nothing |
| shader | +0.34% | -0.41% | nothing |

Correctness: all 71 framebuffer hashes unchanged at both optimization levels.
That was a prediction, not a hope — neither branch can alter output — so a
mismatch would have meant the reasoning was wrong.

Best cells hit **-48.7%**. In absolute terms `c888.fast2.upright.psram.sync`
goes from 447.9 to 237.6 ns per destination pixel.

**The `fast1` regression is layout, and it was worth proving rather than
assuming.** `pax_swr_blit_char_impl` is constant-folded into two specialisations,
so the direct-set path never sees the new branches — and extracting
`.text.pax_swr_blit_char_direct_set` from the object files before and after gives
518 bytes that are byte-for-byte identical. What changed is that the alpha-blend
function grew from 0x2c2 to 0x2f2, shifting everything after it in flash and
changing how the direct-set loop lands in cache on XIP.

That is worth generalising: layout effects are **deterministic per binary, not
statistical**. Every `fast1` sync cell moved by a consistent +2.5%, which looks
exactly like a real regression and reproduces perfectly across runs. Only the
disassembly settles it. The ~1.5% attribution floor stated above is the right
magnitude, but the reason is not noise — it is that a rebuild deterministically
relocates code.

### `f18645c3ac7e` — inline the per-pixel colour merge in the glyph blit loops

`core/src/renderer/pax_renderer_soft.c`, `core/src/renderer/pax_renderer_softasync.c`
(one line each). The plan's **R2**. `pax_col_merge()` is an out-of-line function
that does nothing but forward to `pax_col_merge_inlined()`, which is marked
`always_inline` and used directly everywhere else in PAX. Both software renderers
were calling the out-of-line version from the innermost per-pixel loop.

| Group | -Os | -Og | Predicted |
|---|---|---|---|
| overall | **-2.98%** | -2.09% | |
| fast2 (alpha blend) | **-6.12%** | -4.65% | large |
| fast1 (direct set) | -0.39% | +0.45% | nothing |
| shader | +0.06% | -0.06% | nothing |

Correctness: all 71 framebuffer hashes unchanged, so rendering is bit-identical.

This was chosen first as a test of the harness rather than for its size, and it
passes that test cleanly. Only the alpha-blending branch of the loop was touched,
and only the alpha-blending cells moved: the six best cells in the run are all
`blit/alpha`, all at -10.4%, while every `blit/direct` and `shader` cell sits
inside ±0.8% — below the rebuild noise floor, which is to say unchanged. A win
that showed up everywhere would have meant the harness was measuring something
other than what changed.

The control series says something the percentages hide. `-Og` gains *less*
(-4.65% against -6.12%), which is the opposite of the plan's expectation that a
less-aggressive build would exaggerate call overhead. In absolute terms, on
`c888.fast2.upright.psram.sync`, `-Os` saves 46.6 ns per destination pixel and
`-Og` saves 33.4 -- so the call really was cheaper for `-Og` to make, and the
larger relative figure at `-Os` comes from `-Os` also having the faster baseline
to divide into. Relative and absolute disagree here, and the absolute number is
the one describing the code.

Two things worth carrying forward. The best cells gain 10.4% while the `fast2`
group gains 6.1%, because the **async cells gain roughly a third less** than the
sync ones (-2.68% vs -3.55% overall) — in the async engine the per-pixel loop is
a smaller share of the total, which is itself evidence for R7. And a single
removed function call per pixel is worth 10% of a glyph blit, which sets the
scale for the remaining per-pixel work in R1, R3 and R4.

## Running a cycle

```bash
make bench                  # build + install + run + measure at -Os
make bench OPT=og           # the -Og control series
make benchboth              # both levels
make testbaseline           # record the current run as the reference
make testrepeat             # the three-run repeatability gate
make testcompare            # newest run vs baseline, regenerates this file
make testreport             # regenerate this file only, no device needed
```

The badge must be in the launcher when a cycle starts, because `install` and
`run` both need BadgeLink. If the benchmark app is running, send it `EXIT` on
the debug console, press F1, or wait for its five-minute idle timeout.

For chasing a single suspect cell, `BENCHCELL <n>` or `BENCHCELL <n>-<m>` on the
debug console measures just that range and leaves the app running afterwards, so
attempts can be repeated without reinstalling between them.
