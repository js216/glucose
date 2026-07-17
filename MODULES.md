<!-- SPDX-License-Identifier: GPL-3.0 -->
# main.c module split — plan

`src/main.c` is ~3000 lines and holds several single-purpose concerns. It should
be split, but deliberately as **its own reviewable change, after the current
behavioral fixes are verified on-device** — bundling a 3 kloc structural move
with the untested threading/reconnect changes would make any regression
impossible to attribute. Compilation verifies the move is behavior-preserving,
but the value (maintainability) is lost if it is rushed. This file records the
target so it can be executed cleanly.

## Target modules (from src/main.c)
- **store.c / store.h** — reading history + rolling stats + persistence:
  `hist_insert`, `store_append/load/count`, `stat_add/stat_window/stat_load`,
  and the `*_save/*_load` for settings/alarm/code/info. Owns `g_hist`,
  `g_nhist`, `g_cur_*`, `g_hours`, `g_stored` and the `*_path` strings; exposes
  them via `store.h` (extern + a few accessors).
- **render.c / render.h** — all `draw_*` (`draw_impl`, `draw_glucose`,
  `draw_info`, `draw_settings`, `draw_pair`, `draw_devlist`, `draw_gate`,
  `draw_str/draw_cell`, `menu_row`) + the hit-box geometry it produces.
- **input.c** — `on_input`, hit-testing, `menu_action`, pairing/keypad actions.
- **sys.c** — permission / battery / orientation JNI helpers, `notify_update`.
- **app.c (main.c)** — `ANativeActivity_onCreate`, lifecycle callbacks,
  `on_timer`, crash handler, the `stealo_*` BLE hooks and JNI registration.

## The coupling to design for (do not skip)
- The **render busy-flag / history lock** (`g_draw_busy`, `hist_lock/unlock`,
  `on_main`) is shared by the writer (store) and the reader (render). Put it in a
  small `app_sync.h` both include, rather than duplicating it.
- Rendering reads nearly all state; expose it through a narrow `store.h` /
  `ui_state.h`, NOT a giant extern dump — keep each module's internals `static`.
- `on_input` reads the hit-boxes `render.c` builds; both are main-thread only
  (guaranteed by `on_main`), so no locking is needed between them, but the
  arrays must live in one owner (render) and be read via its header.

## Status (2026-07-16)
DONE — the cleanly-separable data + config layers are extracted, each its own
green-building step (build + clang-tidy + offline self-tests all pass):
- `util.c/.h`   — time/format helpers (realtime_s, now_ms, clampn)
- `stats.c/.h`  — rolling TIR/avg; the hourly-bucket ring is fully private
- `store.c/.h`  — reading history + the append-only CSV (the data model)
- `settings.c/.h` — alarm/display/device-info/code config + persistence

main.c: 3018 -> 2642 lines.

## UI redesign — functional core / imperative shell (in progress)

The earlier worry (a mechanical `render.c` split would just export ~26 hit-box
globals) is resolved by making the UI a **pure function of an immutable
snapshot** instead of a reader of globals. `ui.c` now owns:
- `ui_render(fb, model, hits)` — draws a `struct screen` (built fresh by the
  shell each frame) into the framebuffer and records every touch target into a
  `struct hits`. No globals, no getters, no callbacks.
- `ui_hit(hits, x, y)` — pure map from a tap to a `struct action` the shell
  executes. The hit-box geometry that used to be ~10 shared globals is now
  private, produced and consumed through the `struct hits` value.

The shell (`main.c`) owns all mutable state, builds the `struct screen`
(`build_model`), calls `ui_render`, and executes the returned actions — it keeps
only the genuinely-shell concerns (BLE hooks, JNI, Android lifecycle, gesture
timers). Because the UI is pure, it is verified **offline** in `test/uitest.c`:
feed a model → render to PPM/PNG on the host; feed a tap → assert the action. No
phone required (`make uitest`).

### Status — COMPLETE (2026-07-16)

All five screens are on the functional core (build + clang-tidy + offline
harness all green, every screen render + hit-test verified with no phone):
- **Main** — big number, trend/age/RSSI, plot tabs, plot, alarm row, info panel
  (state/session/pred, TIR/AVG/A1C), STALE/LOW/HIGH banner, SENSOR-EXPIRED
  prompt, and the pre-reading scan/sensor list. Targets: open-settings,
  plot-tab, scrub, alarm +/-, pair-new. The shell keeps only the drag /
  hold-repeat gesture state.
- **Settings** — the whole portrait table; each actionable row is an `ACT_MENU`
  target carrying the existing `menu_action` code.
- **Keypad / pair** — title, entry field, 3x4 grid; keys + close carry
  `menu_action` codes (100-113) via `ACT_MENU`.
- **Device list** — RSSI-sorted picker; pick/cancel via `ACT_MENU` (199, 200+).
- **Gate** — first-run rationale + `ACT_GATE_CONTINUE`.

`on_input` now has exactly one modal branch (`if (g_menu)` → `ui_hit` →
`ACT_MENU` → `menu_action`) plus the gate and main-screen branches. All hit-box
geometry is private to ui.c, passed as a `struct hits` value — the ~26 UI-state
globals and ~10 geometry globals that blocked the earlier `render.c` split are
gone. `draw_impl` is now just `build_model` + `ui_render`.

main.c: 2460 -> 1767 lines; ui.c: 872 lines (pure, host-testable).

Still in the shell, correctly: the BLE hooks (`stealo_*`), the JNI method table
+ `sys_*` + `notify_update`, the Android lifecycle callbacks, and scan/timer
control. That is the irreducible imperative shell.
