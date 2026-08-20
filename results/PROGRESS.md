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
| 9. Repeatability gate | in progress | three back-to-back runs, `make testrepeat` |
| 10. Baseline + reference framebuffers | not started | `DUMPFB` / `RENDERDEMO` not implemented yet |
| 11+. Optimizations | not started | |

## Where the time goes

Per-pixel cost of the three distinct pixel loops, from the most recent run.
This is the table that decides what to optimize; the ratio tables below only say
whether a change helped.

<!-- generated: headline -->
| Pixel loop | Cells | Median ns/dest px | Min | Max |
|---|---|---|---|---|
| fast1 | 16 | 359.9 | 190.9 | 398.5 |
| fast2 | 16 | 448.1 | 308.7 | 477.4 |
| shader | 16 | 7,143.6 | 3,758.4 | 7,571.7 |
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

## Results

One row per captured run. `Overall` and the per-group columns are geometric
means of per-cell ratios against the baseline for that optimization level;
below 1.0 is faster. `Correct` compares every cell's framebuffer hash.

<!-- generated: results -->
| Run | pax commit | Opt | Cells | Overall | fast1 | fast2 | shader | Drift | Correct |
|---|---|---|---|---|---|---|---|---|---|
| `20260820-095455-Os-964533a6` | `964533a6d894` | Os | 71 | - | - | - | - | +0.10% | - |
| `20260820-095933-repeat1-Os-964533a6` | `964533a6d894` | Os | 71 | - | - | - | - | +0.09% | - |
| `20260820-100147-repeat2-Os-964533a6` | `964533a6d894` | Os | 71 | - | - | - | - | +0.07% | - |
<!-- /generated: results -->

## Optimization log

_One entry per pax commit on the `opt/text-rendering` branch: full hash,
subject, files touched, measured geometric mean, and any cells that regressed._

_(none yet)_

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
