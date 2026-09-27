# Display reliability review — 2026-09-26

This candidate repairs reproducible display and input defects found while
reviewing `v3.4.2..v4.7.1` (12 commits, 86 changed files). It is based on
`v4.7.1` / `11f4d60820ad08f3f59788add04d16b2040f344c`, on local branch
`codex/display-reliability`. The embedded version remains 4.7.1; this is a
hardware-test candidate, not a published release.

The control checkout at `/Users/austinmcchord/Development/M5Tab-Macintosh`
already contained uncommitted performance work. It was preserved. This work
uses the attached checkout at
`/Users/austinmcchord/.codex/worktrees/display-reliability/M5Tab-Macintosh`.
Existing display fixes in that work supplied the initial dirty-range and
direct-surface approach; they were reviewed, consolidated, and extended here.
CPU optimizations and performance instrumentation from that work are not part
of this candidate. The two branches have not been integrated.

## Scope and conclusions

The release delta was reviewed across the following areas, with the deepest
code review and new executable coverage concentrated on display correctness
and its input/automation boundaries. Review does not establish hardware
compatibility or prove every guest application correct.

| Release area | Disposition |
| --- | --- |
| 3.4.6 USB disk mode, per-board SDK pins | Reviewed USB/SD ownership and build configuration; retained board-specific SDKs and media behavior. |
| 4.0 touch overlays | Replaced concurrently mutable render state with a published state and one immutable snapshot per frame batch. |
| 4.1–4.5 HID, keyboard, panel/touch rotation, audio, SD, ExtFS/CD | Fixed report-ID parsing and rejected-packet handling; aligned display rotation; retained audio, storage and board initialization policies for hardware validation. |
| 4.6 QuickDraw and interpreter acceleration | Fixed full-range damage publication and dirty-bit races; restricted unsafe QuickDraw cases. JIT, compact dispatch and trace cache remain disabled as in 4.7.1. |
| 4.7 production silicon, clock and recovery | Retained chip variants, SDK pins, persistence and recovery; existing host tests and all firmware profiles are included in validation. |
| 4.7.1 serial automation and preboot | Protected screenshot lifetime during video exit and serialized physical/automation key ownership. Retained the protocol and boot-control behavior. |

Several faults, including endpoint-only bulk dirty tracking and the unused
strip allocation, were already present in 3.4.2. Later bulk drawing and
concurrent overlay/automation work increased the ways those paths were used.
They should not all be attributed to a particular recent release.

## Defects repaired

| Defect | Resulting behavior |
| --- | --- |
| Large framebuffer writes marked only their ends; an epoch/last-tile cache could suppress a write racing bitmap collection. | Every intersected tile is published at 1/2/4/8-bit depth. Every write publishes an atomic dirty bit; concurrent writes remain pending for the next render. |
| An early frame notification could be consumed before the cadence deadline and leave damage waiting for another notification. | The worker drains damage on a bounded timeout while preserving the existing 45 ms minimum cadence. |
| Mode, palette and redraw consumption were separate; errors could acknowledge pixels that never reached the panel. | A batch captures mode and palette together. Tiles are acknowledged only after all copies and cache publication succeed. Failed batches retry. |
| Each board had a duplicate asynchronous tile path with scratch-buffer/DMA lifetime hazards. Tab5 tiles used a hardcoded flip. | Both boards share one synchronous tile writer into the panel driver's framebuffer and use the saved rotation consistently with boot graphics. |
| Boot UI could clear its dirty flag after a concurrent draw. | Publication consumes the flag before cache writeback; concurrent drawing and failed writeback leave another flush pending. |
| Overlay keys/layout could change while tiles were being composited; damage and state publication could be reordered. | Input publishes state before damage. Rendering collects damage before taking a fixed overlay snapshot. Hiding/shutdown redraws the old overlay area. |
| Packed stipples could be mistaken for a uniform frame; splash timeout depended on further activity. | Handoff inspects packed pixels correctly and the five-second ceiling works even if guest writes stop. |
| Shutdown waited a guessed delay before freeing memory; task-creation failure claimed an absent fallback. | Exit waits for the renderer's completion handshake. Screenshot access holds a lifetime mutex. Failed initialization unwinds and reports failure. |
| Native QuickDraw copies could ignore differing palettes or overlapping bitmap views; recording/complex clipping cases were too permissive. | Unsupported formats, differing color-table handles, unsafe aliases, recorded MoveTo and complex ScrollRect clipping fall back before native changes. |
| Mouse descriptor fields could be combined across report IDs; rejected packets could be reinterpreted heuristically. | Select a complete layout within one report ID and reject wrong-ID/truncated packets without changing mouse state. |
| Serial automation and physical input could race key-claim transitions and ADB event order. | A task mutex covers ownership and event ordering, including release-all. No spinlock is held around blocking ADB calls. |

Removed the unused 204,800-byte strip allocation, duplicate RGB tile buffer,
obsolete DMA callbacks/semaphores, unused HAL entry points, and the separate
MiniGfx-owned canvas path.

## Display ownership and ordering

1. Guest CPU writes the indexed framebuffer, then release-publishes dirty bits.
2. The renderer captures mode/palette, acquire-collects dirty bits into its
   retained pending bitmap, and snapshots the published overlay.
3. Each dirty tile is copied locally, expanded 2× to native RGB565 and composited.
4. The shared board surface rotates the tile into the panel-owned portrait
   framebuffer. One synchronous cache writeback publishes the changed row span.
5. Successful publication clears only the consumer's pending bitmap. Producer
   bits written during the batch remain available to the next frame.

Board files own hardware bring-up/backlight. `board_display_surface.cpp` owns
the shared surface/batch contract. MiniGfx supplies boot primitives and cache
publication. `video_esp32.cpp` owns guest decoding, damage and renderer lifetime.
`panel_surface.h` contains the pure rotation mapping.

The selected ESP-IDF DPI drivers use continuously scanned framebuffers; their
external-source color-transfer callback reports copy completion, not a vsync
fence. The direct writeback path follows the framebuffer/cache contract in
the [IDF 5.5.1 driver](https://github.com/espressif/esp-idf/blob/v5.5.1/components/esp_lcd/dsi/esp_lcd_panel_dpi.c)
and [IDF 5.5.4 driver](https://github.com/espressif/esp-idf/blob/v5.5.4/components/esp_lcd/dsi/esp_lcd_panel_dpi.c).
This remains a single scanout buffer: a display refresh can intersect a write.
The work prevents stranded updates and scratch-buffer corruption, but does
not promise tear-free, atomic optical frames.

## Verification

Run from the candidate checkout, using PlatformIO 6.2.0 and the existing
project Python environment:

```sh
python scripts/build_assets.py --force
python -m unittest discover -s test -p 'test_*.py'
pio run -e esp32p4_pioarduino -e waveshare_p4_101 \
  -e esp32p4_pioarduino_rev3 -e waveshare_p4_101_rev3 \
  -e esp32p4_pioarduino_debug
git diff --check
```

- Final host suite: **46 tests passed**. The six added native test targets
  compile production source with AddressSanitizer and UndefinedBehaviorSanitizer.
- Random/reference coverage includes all packed depths, padding/overflow
  dirty ranges, both panel sizes and rotations, whole-frame tile/palette
  output, publication failure/retry, writes during collection/publication,
  boot dirty flags, splash, task startup failure, exit and capture lifetime.
- Overlay tests check publication order, immutable batches, key highlights
  and tile bounds. QuickDraw tests check conservative fallbacks and 10,000
  overlapping monochrome copies against a snapshot reference.
- HID tests reproduce mixed IDs/truncated reports. Input tests exercise
  physical/automation modifier ownership with three concurrent threads.
- Final firmware matrix: **all five profiles passed** (206.077 seconds total):
  `esp32p4_pioarduino`, `waveshare_p4_101`, `esp32p4_pioarduino_rev3`,
  `waveshare_p4_101_rev3`, and `esp32p4_pioarduino_debug`.
- `git diff --check` passed. Existing compiler warnings remain.
- Evidence: ignored `artifacts/display-reliability/host-tests-final.log` and
  `artifacts/display-reliability/build-final.log` in this checkout.

No device was flashed or controlled in this task. No branch was pushed and no
release was published. Performance and optical behavior remain unmeasured.
More conservative QuickDraw fallbacks may trade some throughput for correctness.

## Hardware acceptance checklist

Use the matching board/silicon image from the artifact README. The previously
documented Tab5 is pre-v3 revision 1.3 and used the opt-in debug SDK profile;
its candidate is labeled accordingly. Production images require revision 3.1+.

1. Boot from power-off several times, including settings, splash and Finder.
   Check SD mounting, panel colors, orientation and absence of stranded splash.
2. Exercise both saved rotations and touch alignment at all four corners.
3. Switch through B&W, 4, 16 and 256 colors repeatedly. Open/close menus, move
   overlapping windows, scroll text and redraw the whole desktop. Look for
   persistent stale tiles or wrong palette regions after motion stops.
4. Show/hide keyboard and game overlays; hold multiple keys while dragging
   windows, release them and disable touch. Check highlights, restored pixels,
   modifiers and pointer position.
5. Exercise USB mouse movement/buttons/wheel and reconnect the device. If
   available, include a mouse with multiple report IDs and the Tab5 keyboard.
6. Run the usual graphics workload, then combine graphics with audio and disk
   activity. Observe responsiveness and any transient tearing separately from
   pixels that remain wrong after the scene settles.
7. Use serial screenshots/actions during activity and return to preboot.
   Repeat boot/exit cycles. A serial screenshot contains guest pixels, not
   panel scanout or overlays; photograph physical-only glitches as well.

For any failure retain the candidate SHA, board/revision, mode, rotation,
reproduction steps and serial log. The next step is Austin's hardware test,
then integration with the separate performance branch if accepted.
