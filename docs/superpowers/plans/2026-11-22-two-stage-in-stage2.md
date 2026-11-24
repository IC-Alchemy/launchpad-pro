# Two-stage Velocity in the Stage 2 Instrument

> **For agentic workers:** use superpowers:subagent-driven-development. Steps use `- [ ]`.

**Goal:** Two-stage becomes the fourth velocity mode of the stage 2 instrument (menu Options, velocity row), next to Peak-window, Zero-latency and Fixed. The library press threshold is lowered so Zero-latency and Peak-window also play softer. Thresholds are tuned later from MIDI logs of the normal image (no measurement build).

**Spec:** `docs/superpowers/specs/2026-11-21-midi2-scale-instrument-design.md` (velocity section already describes Two-stage and `PAD_LIB_THRESHOLD`).

**Starting point (on this branch):** `src/pads/twostage.{h,c}` + `tests/test_twostage.c` (engine, host-tested, written for 1 tick = 1 ms), `src/platform/pads.h` (`PAD_LIB_THRESHOLD 20`, `pad_setcalibration` declaration, `pad_lib_trigger_level/release_level`). Stage 2: `src/pads/pad_engine.{h,c}` (main-loop only, `pe_poll(now)` once per ms runs `pressure_pass`), `src/instrument/settings.{h,c}` (`VEL_PEAK_WINDOW, VEL_ZERO_LATENCY, VEL_FIXED`, flash record), `src/instrument/ui.c` (menu, `ui_pad` → `pe_press(pad, scale_pad_pitch(...), ch, din, now)`), `src/app.c`.

## Design decisions

- Engine runs from `pe_poll`, main loop, once per new ms (same place as `pressure_pass`, before it). No ISR involvement, so no critical sections needed.
- Main loop may skip ms. `ts_tick` takes the absolute ms counter; crossing interpolation spans the real gap since the previous sample (`span = now - last`, clamped 1..255 so `frac * span` stays in 32 bits).
- `VEL_TWO_STAGE = 3`, appended so stored flash records keep their meaning. `settings_valid`: `velocity_mode < 4`.
- Menu velocity row shows 4 options in spec order Peak-window / Zero-latency / Two-stage / Fixed: column c → value `{VEL_PEAK_WINDOW, VEL_ZERO_LATENCY, VEL_TWO_STAGE, VEL_FIXED}[c]`. Other option rows keep 3 columns.
- In Two-stage, `pe_press` / `pe_release` from the library are ignored. The engine needs pitch, channel and DIN flag at its Note On: pad engine gets a target callback `typedef int16_t (*pe_target_fn)(uint8_t pad, uint8_t *ch, uint8_t *din)` set with `pe_set_target(fn)`; returns pitch or -1 (no note). The UI provides it: -1 while the menu is open, else `scale_pad_pitch`, `g_s.channel`, `g_s.din_mirror`.
- Engine Note On: latch pitch/ch/din, velocity `pad_curve(v, curve)`, then the existing sounding path (note sharing, pressure, sounding callback). Engine Note Off: existing release path for a HELD pad.
- `pe_release_all`, `pe_forget_all`, and `pe_configure` switching into Two-stage call `ts_init`: pads still down stay silent until released and pressed again (engine arms only on an upward T1 crossing).
- `app_init` calls `pad_setcalibration(PAD_LIB_THRESHOLD, 1023, 512, 769)` before `pe_init` (other three values = what main passes). `pad_velocity16` already reads `PadCalibration[0]`, so Zero-latency/Peak-window rescale automatically.

## Global constraints

- `include/app.h` not modified. Tests `-std=c99 -Wall -Werror -DHOST_TEST`; host `u32`/`unsigned long` is 64-bit (twostage.c masks Q8 math with `M32`). Flash cap (`tools/check_image.sh`). No malloc, no 64-bit math on target, bounded work.
- Notes on the instrument io seam (`instrument/io.h`), same as other modes.
- No model names in commits/docs.

### Task 1: Absolute-time engine tick

**Files:** `src/pads/twostage.{h,c}`, `tests/test_twostage.c`

- [ ] Change `ts_tick(ts_engine *e, const u16 *raw, u32 now_ms, ts_note_fn fn, void *ctx)`: first call primes (stores prev and `last_ms = now_ms`); later calls with `now_ms == last_ms` do nothing; otherwise `span = clamp(now_ms - last_ms, 1, 255)`. Change `ts_cross_q8(u32 last_ms, u32 span, u16 prev, u16 x, u16 level)` = `last_ms*256 + frac*span` (frac 0..256 as before; `x <= prev` → `(last_ms + span)*256`). Rename/replace `now_ms` field with `last_ms`. Engine types: may switch to `uint*_t` from `<stdint.h>` to match stage 2 sources.
- [ ] Update existing tests to pass now explicitly (consecutive ms → identical results to before). Add: `test_ts_gap_interpolates` (samples 2 ms apart: crossing lands inside the 2 ms span, dt correct), `test_ts_same_ms_ignored`.
- [ ] `make test`, `make`; commit `twostage: tick on the absolute ms counter`.

### Task 2: Two-stage in settings and menu

**Files:** `src/instrument/settings.{h,c}`, `src/instrument/ui.c`, `tests/test_settings.c`, `tests/test_ui.c`

- [ ] `VEL_TWO_STAGE = 3` appended; `settings_valid` accepts 0..3, rejects 4. Record round-trip with 3.
- [ ] Menu velocity row: 4 columns mapped as in Design decisions; selected/unselected colours as the other option items; other rows unchanged. Tests: pressing each velocity column selects the mapped value; column 4 of velocity row and column 3 of other rows do nothing.
- [ ] `make test`, `make`; commit `instrument: Two-stage velocity option`.

### Task 3: Pad engine runs Two-stage

**Files:** `src/pads/pad_engine.{h,c}`, `src/instrument/ui.c`, `src/app.c`, `tools/sim_midi2.c` / `tools/simulator.c` (stubs, Makefile SIM_SOURCES add `src/pads/twostage.c`), `tests/test_pad_engine.c`, `tests/test_ui.c`, `Makefile`

- [ ] Implement per Design decisions (target callback, ignore library press/release in Two-stage, engine in `pe_poll`, sounding via existing path with given velocity, resets). Refactor `sound()` so a velocity can be passed in; don't duplicate the sounding logic.
- [ ] `app_init`: `pad_setcalibration(PAD_LIB_THRESHOLD, 1023, 512, 769)` before `pe_init`. Simulator: stub `pad_setcalibration` (sets `PadCalibration`, prints).
- [ ] Tests (pad engine, with raw buffer frames + `pe_poll(now)` per ms): slow ramp gives one Note On with curve-applied velocity on the target's pitch/channel, release below `TS_REL` for `TS_REL_TICKS` gives Note Off; library `pe_press` in Two-stage sends nothing; target returning -1 → no note and no later Off; two pads same pitch share the note (one On, Off after both release); `pe_release_all` while held → Off, pad still high stays silent, sounds again after release + new press; `pe_forget_all` likewise silent; switching mode via `pe_configure` releases nothing by itself but resets engine; poly pressure flows for an engine-held pad. UI test: target is -1 while menu open.
- [ ] `make test`, `make` (simulator runs), image under cap; commit `pads: Two-stage velocity mode in the pad engine; lower library threshold`.

### Task 4: Docs

**Files:** spec, `docs/checkpoint-2-instrument.md`

- [ ] Spec: Two-stage runs in the main-loop pad engine (not the timer ISR), absolute-ms interpolation, target callback, enum value 3 with menu order mapping, thresholds tuned from MIDI logs. Remove any mention of the PADSCOPE build.
- [ ] Checkpoint 2 doc: how to select Two-stage (Setup → Options → velocity row, 3rd pad), and a short log procedure for tuning: in each velocity mode play softest possible, soft-to-hard sweep, slow firm pushes, light rest then strike, hard hits watching neighbours; export the MIDI monitor log; note feel.
- [ ] Commit `docs: Two-stage in the stage 2 instrument and log-based tuning`.

## Later

- Tune `TS_*` and maybe `PAD_LIB_THRESHOLD` from the player's logs.
