# PAX font rendering performance — root cause analysis

Analysis of why text rendering is slow in the Tanmatsu launcher, traced from
`pax_draw_text()` down to generated RISC-V machine code.

**Status:** analysis only, no code changed.
**Target analysed:** `CONFIG_BSP_TARGET_TANMATSU` (`sdkconfig_tanmatsu`), ESP32-P4 @ 360 MHz.
**pax-gfx:** `managed_components/robotman2412__pax-gfx`.

---

## 1. Summary

Rendering one antialiased glyph pixel costs roughly **120 RISC-V instructions plus a
hardware divide and five function calls**, of which only about **26% is specific to the
framebuffer pixel format**. The remaining ~74% is call-boundary overhead, a
non-inlined blend function, a loop-invariant division, and the absence of any
early-out for transparent pixels — all of it independent of colour format, screen
size, orientation and panel technology.

That distinction matters: the launcher targets several devices, apps using pax-gfx
choose their own buffer formats, and pax-gfx is shared by every app. A fix that
specialises for one device or one pixel format addresses the smallest part of the
problem. The large part is generic and belongs upstream in pax-gfx, where every
consumer inherits it.

A secondary and much worse path exists for any text whose
`font_size / font->default_size` is not an integer — currently the on-screen
keyboard and the chat view. Those fall off the fast glyph blitter onto the generic
shader rasteriser.

---

## 2. Configuration established

Facts gathered from the build configuration and BSP, all verified in-tree:

| Property | Value | Source |
|---|---|---|
| SoC / clock | ESP32-P4, RV32, 360 MHz | `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=360` |
| Compiler optimisation | `-Os` (size) | `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` |
| pax-gfx optimisation override | none | `managed_components/robotman2412__pax-gfx/CMakeLists.txt` |
| L1 / L2 cache line | 64 B | `CONFIG_CACHE_L1_CACHE_LINE_SIZE`, `CONFIG_CACHE_L2_CACHE_LINE_SIZE` |
| L2 cache size | 128 KB | `CONFIG_CACHE_L2_CACHE_SIZE=0x20000` |
| PSRAM | HEX mode, 200 MHz, in malloc heap | `CONFIG_SPIRAM_MODE_HEX`, `CONFIG_SPIRAM_USE_MALLOC=y` |
| Panel | ST7701 MIPI-DSI, native **480×800** | `dsi_panel_nicolaielectronics_st7701.c:30` |
| DSI framebuffers | `num_fbs = 1` | `main.c:415` |
| Requested colour format | `24_888RGB` → `PAX_BUF_24_888RGB` | `main.c:413`, `common/display.c:90` |
| Default rotation | `ROTATION_270` → `PAX_O_ROT_CW` | `targets/tanmatsu/badge_bsp_display.c:159` |
| PAX framebuffer | 480×800×3 B = **1,152,000 B**, `aligned_alloc(64,…)` → PSRAM | `common/display.c:120`, `pax_gfx.c:112` |
| Renderer | async, multithreaded (2 workers) | `main.c:426`, `CONFIG_PAX_COMPILE_ASYNC_RENDERER_MULTITHREAD=y` |
| Bounds checking | off (good) | `CONFIG_PAX_BOUNDS_CHECK` not set |
| Fixed point | 16.48 in `int64_t`, `__int128` intermediates | `CONFIG_PAX_USE_LONG_FIXED_POINT=y` |

### Font

`main/chakrapetchmedium.c` — used for header, footer and all menus:

- `default_size = 16`, `recommend_aa = true`
- 101 ranges, all `PAX_FONT_TYPE_BITMAP_VAR`, **`bpp = 2`** (4 coverage levels)
- ASCII (`0x20`–`0x7e`) is range index 1, so `text_get_range()`'s linear scan costs
  two iterations for Latin text — **not** a bottleneck despite the 101 ranges

`main/rajdhani.c` — used for chat: `default_size = 16`.

### Theme sizes and which render path they select

`main/common/theme.c`:

| Element | Font | Size | `size/default` | Path taken |
|---|---|---|---|---|
| Header / footer / menu | chakrapetchmedium | 16 | 1.0 | fast `blit_char` |
| Chat | rajdhani | 24 | 1.5 | **shader rasteriser** |
| On-screen keyboard | chakrapetchmedium | 27 | 1.6875 | **shader rasteriser** |

---

## 3. The rendering path

```
pax_draw_text(…)                                   [pax_text.h macro, discards return value]
└─ pax_draw_text_adv()                             [pax_text.c:502]
   ├─ pax_dispatch_text()                          → queues ONE task to EACH of 2 worker queues
   └─ pax_internal_text_generic(do_render = false) → measures the string ON THE CALLING THREAD
                                                      purely to build the discarded return value

worker N: pax_mcrwN_text()                         [pax_renderer_softasync.c:959 / 1087]
└─ pax_internal_text_generic(do_render = true)
   └─ text_line_generic()                          [pax_text.c:442]
      │  ALIGN_BEGIN → 1 pass
      │  ALIGN_CENTER / ALIGN_END → measure pass, then render pass
      └─ text_bitmap_var() → dispatch_glyph()      [pax_text.c:187]
         ├─ fast path  → renderfuncs->blit_char()  ← identity matrix AND integer scale
         └─ slow path  → shaded_rect() + per-pixel shader callback
```

### 3.1 Fast path selection

`dispatch_glyph()` (`pax_text.c:191`) takes the fast blitter only when:

```c
ctx->matrix.a0 > 0
&& fabsf(ctx->matrix.a0 - ctx->matrix.b1) < 0.01
&& fabsf(mat_scale - (int)mat_scale) < 0.01     // integer scale required
&& matrix_2d_is_identity2(ctx->matrix)
```

Any non-integer scale ratio silently drops to the shader rasteriser. This is why
the OSK (27 px on a 16 px font) and chat (24 px on a 16 px font) are
disproportionately slow.

### 3.2 Blend path selection

`pax_swr_blit_char()` (`pax_renderer_soft.c:570`) chooses the cheap `direct_set`
variant only when `(rsdata.bpp == 1 && color >> 24 == 255)` or the buffer is
palette-typed. The launcher's fonts are **2 bpp**, so text always takes
`pax_swr_blit_char_alpha_blend` — a full read-modify-write per pixel.

---

## 4. Measured evidence — the glyph inner loop

Disassembly of `pax_swr_blit_char_alpha_blend` from
`build/tanmatsu/esp-idf/robotman2412__pax-gfx/…/renderer/pax_renderer_soft.c.obj`
(568 bytes total). The pixel loop is `.L246`, `0x156`–`0x1d2`: **53 instructions**.

### 4.1 What those 53 instructions are

| Category | Count | Note |
|---|---|---|
| Stack spill / reload (`sw`/`lw` on `sp`) | **18** | forced by the 5 calls clobbering caller-saved registers |
| Reloads of `buf->getter/buf2col/setter/col2buf` | **4** | `lw` from `s1` offsets 52/56/60/64, **every pixel** |
| Call instructions (`jalr` ×5, `auipc` ×1) | 6 | |
| `div` | 1 | `div a2,a3,s9` @ `0x166` — `x / scale` |
| `mul` | 3 | |
| Glyph bit extraction + alpha math | ~13 | the only genuinely necessary work |
| Loop control and register shuffling | ~12 | |

The four function pointers are reloaded from `buf` on every iteration because the
compiler cannot prove an indirect callee does not write to `*buf`.

`y / scale` **was** hoisted by GCC into the row loop (`div t3,a5,s9` @ `0x14e`);
`x / scale` was not. `scale` is loop-invariant and is `1` in every launcher use.

### 4.2 The five callees

Sizes from `nm --print-size`:

| Callee | Size | ~Instr | Notes |
|---|---|---|---|
| `pax_index_getter_24bpp` | 32 B | 12 | `mul` + 3× `lbu` |
| `pax_888rgb_to_col` | 8 B | 3 | `lui`, `or`, `ret` — behind a full indirect call |
| `pax_col_merge` | **124 B** | **~37** | 4× `mul`, 2× `mulhu`; **not inlined** |
| `pax_col_conv_dummy` | 4 B | 2 | `mv a0,a1; ret` — a **no-op paying full call cost** |
| `pax_index_setter_24bpp` | 38 B | 14 | `mul`, 3× `sb`, and reloads `buf->buf` **three times** |

`pax_col_merge` (`pax_gfx.c:668`) is a one-line wrapper around
`pax_col_merge_inlined()`, which already exists as an `always_inline` static in
`pax_internal.h:417`. The blit loop calls the out-of-line wrapper.

For `PAX_BUF_24_888RGB`, `pax_get_col_conv()` assigns
`col2buf = pax_col_conv_dummy` (`pax_setters.c:704`) — so one of the five
per-pixel calls does nothing at all, yet still costs a call, a return, and its
share of the surrounding spills.

### 4.3 Total, and how much of it is format-specific

**≈ 120–126 instructions + 1 hardware divide + 5 call/return pairs per glyph pixel.**

| Cost | ~Instr | Format / device specific? |
|---|---|---|
| Call-boundary overhead (spills, fn-ptr reloads, call/ret) | 33 | No |
| `pax_col_merge` body | 36 | No |
| Glyph bit extraction + alpha math | 13 | No |
| `div` (`x / scale`) | 1 | No |
| Loop control | 12 | No |
| getter + setter + buf2col + col2buf | **31** | **Yes** |

**≈ 74% of the per-pixel cost is independent of pixel format, orientation and
panel.** Only the last row changes when an app selects RGB565 instead of RGB888.

### 4.4 No early-out for transparent pixels

The loop runs over the glyph's entire bounding box. There is no `value == 0`
check, so a blank pixel inside that box executes the identical ~120 instructions
to read a framebuffer pixel and write back the value it just read. At full
coverage the blend is likewise a no-op that could be a plain store. With a 2 bpp
font there are only four coverage levels, and levels 0 and 3 dominate.

### 4.5 Memory access pattern

With `PAX_O_ROT_CW`, `pax_swr_blit_char_impl` (`pax_renderer_soft.c:492`) sets
`dx = buf->width`, `dy = -1`. The **inner** loop iterates glyph-x and therefore
advances the framebuffer index by `buf->width` — **1440 bytes** per pixel on this
panel. Consecutive writes never share a 64-byte cache line. The outer loop, over
glyph-y, is the one that moves contiguously (3 bytes).

The loop nesting is fixed in the source and does not adapt to orientation.

### 4.6 RGB565 would not fix the glyph loop

Recorded here because it is a tempting and wrong conclusion. Symbol sizes:

| | RGB888 | RGB565 |
|---|---|---|
| `pax_index_getter_*` | 32 B | 12 B |
| `pax_index_setter_*` | 38 B | 12 B |
| `*_to_col` | 8 B | 60 B |
| `pax_col_to_*` | 4 B | 30 B |
| **total** | **82 B** | **114 B** |

RGB565 trades cheaper getters/setters for more expensive channel expansion, and
comes out slightly *worse* on instruction count. Its real benefit is 33% less
PSRAM traffic and better cache-line density — which helps the background fill and
the full-screen blit, not the glyph blend.

---

## 5. Redundant work above the pixel loop

These are algorithmic, and independent of device and pixel format.

### 5.1 Every string is walked 3–5 times

| Walker | Passes | Where |
|---|---|---|
| Calling thread, to build the return value `pax_draw_text` discards | 1 | `pax_text.c:531-536` |
| Worker 0 | 1 (2 if centre/right aligned) | `pax_renderer_softasync.c:959` |
| Worker 1 | 1 (2 if centre/right aligned) | `pax_renderer_softasync.c:1087` |

`pax_sasr_text()` (`pax_renderer_softasync.c:1322`) queues the *same* task to both
worker queues; each worker then runs the full `pax_internal_text_generic()` —
UTF-8 decode, range lookup, glyph dimension lookup, matrix maths — and only the
per-pixel writes are split, by scanline parity. All glyph *setup* is duplicated.

For `PAX_ALIGN_CENTER` / `PAX_ALIGN_END`, `text_line_generic()`
(`pax_text.c:442`) runs a measure pass before the render pass, inside each worker.

### 5.2 Callers measure again on top

- `gui_icontext_draw()` (`components/gui/gui_element_icontext.c:17`) calls
  `pax_draw_text()` and then `pax_text_size()` on the same string.
- `gui_icontext_width()` measures a third time for right-aligned footer items.
- `menu_render_grid()` (`components/gui/gui_menu_render.c:197`) calls
  `pax_text_size()` per item before drawing it.

Menu item widths are recomputed from scratch on every frame although the labels
change rarely.

### 5.3 Per-draw-call allocation and queue traffic

- `PAX_SSO_BUF_LEN = 32` (`pax_types.h:453`). Strings longer than 32 bytes take a
  `malloc()` per draw call, reference-counted and freed by the workers
  (`pax_renderer_softasync.c:1340`).
- `pax_task_t` is ~100 bytes and is copied into **two** queues per draw call, each
  send taking a `pthread_mutex` and posting semaphores (`ptq.h`).

---

## 6. The shader path (OSK and chat)

When `dispatch_glyph()` rejects the fast path, rendering goes through
`pixel_aligned_render()` → `shaded_rect` → `pax_rect_shaded()`. The glyph's UVs
satisfy `v0==v1 && v2==v3 && u0==u3 && u1==u2`, so it lands in
`pax_rect_shaded_resuv` (`pax_dh_shaded.cpp:151`). Per pixel that loop performs:

1. an indirect shader callback — `pax_shader_font_bmp_aa` (`pax_shaders.c:78`),
   which does **four** glyph samples plus two `floorf()` calls for bilinear
   filtering, because `recommend_aa = true`;
2. an indirect `buf->getter` and `buf2col`;
3. an indirect setter;
4. two `fixpt_t` → `float` conversions, because the shader signature takes
   `float u, float v` while the rasteriser carries UVs as `fixpt_t`.

Point 4 is the concerning one. With `CONFIG_PAX_USE_LONG_FIXED_POINT=y`,
`fixpt_t` is 16.48 fixed point in `int64_t`, and its conversion operator is
(`pax_fixpt.hpp`):

```c
template <typename T> static constexpr T _to(fixpt_raw_t in) {
    return (T)(in / (long double)PAX_FIXPT_MUL);
}
```

On RV32 GCC, `long double` is IEEE binary128, implemented entirely in software.
If this is not constant-folded, each conversion costs an `int64` → `binary128`
conversion, a `binary128` division and a `binary128` → `float` narrowing — on the
order of hundreds of cycles, twice per pixel.

> **Unverified.** I did not confirm this in the generated object code. Before
> acting on it, check whether `pax_dh_shaded.cpp.obj` references `__divtf3`,
> `__floatditf` or `__trunctfsf2`. GCC may fold the division by a compile-time
> constant into a multiply, or narrow the whole expression. This is the one claim
> in this document not backed by disassembly.

Independently of that: text at a non-integer size ratio is on a fundamentally more
expensive path, and the cheapest fix is to make the ratio integral (ship font
variants at the sizes actually used, or pick sizes that are multiples of
`default_size`).

Note also that the fast path passes `floorf(mat_scale + 0.5)` as an integer scale
to `blit_char`, i.e. text above `default_size` is nearest-neighbour upscaled and
will look blocky.

---

## 7. Frame-level costs outside the text path

Included for context, because they bound how much glyph optimisation is worth.

- **Full-screen blit on every render.** `display_blit_buffer()`
  (`main/common/display.c:142`) calls `pax_join()` and then blits the entire
  screen unconditionally. With `num_fbs = 1`, `esp_lcd_panel_draw_bitmap()`
  copies 1.15 MB PSRAM → PSRAM. Moving the menu selection dirties two 32-pixel
  rows but still pays the full copy.
- **Dirty tracking already exists but is unused at this level.**
  `pixel_aligned_render()` calls `pax_mark_dirty2()`, so pax already tracks a
  dirty rectangle; the BSP blit ignores it.
- **Background fill.** `pax_range_setter_24bpp` (`pax_setters.c:401`) loops
  calling `pax_index_setter_24bpp` per pixel — three byte stores each — over
  384,000 pixels.
- **`partial` redraw exists in the menu code** (`gui_menu_render.c:111`) and skips
  re-rendering unchanged rows, but the blit that follows is still full-screen.

---

## 8. Recommendations

Ordered by payoff per unit of effort. Generality is called out explicitly, since
pax-gfx is shared across apps, formats and devices.

### Generic — belongs in pax-gfx, benefits every consumer

**R1. Early-out on coverage extremes.** Skip `value == 0` entirely; at maximum
coverage store a `col2buf(color)` hoisted out of the loop instead of doing a
read-modify-write. For a 2 bpp AA font these are levels 0 and 3, the majority of
pixels. Independent of format, orientation and font. *Small change, large win.*

**R2. Inline the blend.** Call `pax_col_merge_inlined()` (already `always_inline`
in `pax_internal.h`) instead of the out-of-line `pax_col_merge()`. Removes ~37
instructions and a call/return per pixel, on every format. *One-word change.*

**R3. Hoist the function pointers before the loop.**
`pax_swr_scaled_image()` in the same file already does exactly this
(`pax_renderer_soft.c:145-149`); the glyph blitter does not. Removes 4 loads per
pixel and helps the register allocator reduce the 18 spills.

**R4. Strength-reduce the divisions.** `scale` is loop-invariant; replace
`x / scale` and `y / scale` with counters. GCC already hoisted one of the two,
demonstrating it is safe to do so.

**R5. Push the loop below the callback boundary.** Add a per-format
"blend a run of pixels with a coverage mask" primitive to `pax_render_funcs_t` /
the setter table. pax already uses this shape — `pax_get_setters()` hands out a
per-format `range_setter` *and* `range_merger` (`pax_range_merger_888rgb`,
`pax_range_merger_565rgb`), which blend a *constant* colour over a run. AA text
needs per-pixel coverage, so it needs a mask-aware sibling. This collapses five
indirect calls per pixel into one per run, and keeps format specialisation where
pax already puts it — callers stay format-blind, and new formats inherit it
automatically. *This is the structural answer to "different apps use different
pixel formats".*

**R6. Orientation-adaptive loop nesting.** Choose the inner axis by whichever of
`dx`/`dy` has magnitude 1, rather than fixing glyph-x as the inner loop. Correct
for all eight orientations and every panel geometry, with no per-device code.

**R7. Remove the redundant string walks.** Add a draw entry point that does not
compute the size (the `pax_draw_text` macro discards it anyway), and let workers
share a measure result instead of each recomputing it for centre/right alignment.

**R8. Glyph or string cache.** The only item here that offers an order of
magnitude rather than a factor of two or three, and equally device-agnostic. Cache
rendered glyphs — or whole strings, since menu labels are stable across frames —
in a buffer-format-native form.

**R9. Build the pixel loops at `-O2` inside pax-gfx's own CMakeLists.** As a
*project* setting this is a stopgap that every consuming app would have to repeat.
As a decision the library makes for its own rasteriser translation units, it is a
legitimate upstream fix that all consumers inherit.

### Launcher-side, device-independent

**R10. Cache measured text widths** in `menu_item_t` instead of calling
`pax_text_size()` per item per frame, and drop the duplicate measurement in
`gui_icontext_draw()`.

**R11. Use integer font size ratios.** Set chat and OSK sizes to multiples of the
font's `default_size`, or ship font variants at 24 px and 27 px, to keep them on
the fast blitter. See §6.

**R12. Bound the blit to the dirty rectangle.** pax already tracks it via
`pax_mark_dirty2()`; `display_blit_buffer()` ignores it and always pushes 1.15 MB.
Likely the single largest whole-frame win for menu navigation, independent of
anything in the text path.

### Not recommended

- **Switching this device to RGB565** — see §4.6; it does not help the glyph loop,
  and app compatibility constrains the format anyway.
- **Device-specific specialisation of the glyph blitter** — addresses ~26% of the
  per-pixel cost and multiplies across devices, formats and orientations.

---

## 9. What has not been established

1. **The frame-time split.** No measurement was taken of how much of a frame goes
   to glyph rendering versus the background fill versus the 1.15 MB blit. If the
   blit dominates, R12 outranks everything else and the glyph work is secondary.
   Instrumenting one frame with `esp_timer` around those three phases should come
   before any implementation work.
2. **The `long double` conversion cost** in the shader path (§6) — asserted from
   source, not confirmed in generated code.
3. **PSRAM stall contribution.** Instruction counts above are static; they exclude
   cache-miss latency on a 1.15 MB framebuffer against a 128 KB L2, which the
   strided access pattern of §4.5 makes worse.
4. **Other targets.** Only `sdkconfig_tanmatsu` was examined. Devices with smaller
   screens, different orientations or palette buffers will have different
   balances — though the ~74% generic share of the per-pixel cost should hold,
   since none of it depends on those properties.
