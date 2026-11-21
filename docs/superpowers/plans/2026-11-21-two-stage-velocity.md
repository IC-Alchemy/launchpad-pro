# Two-Stage Velocity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Third velocity mode, "Two-stage": two virtual switches (two thresholds on raw 12-bit pad reading), time between crossings becomes 16-bit velocity. Plus lower library press threshold so Zero-latency and Peak-window also play softer.

**Architecture:** Pure, host-tested engine `src/pads/twostage.c` fed one raw ADC frame per 1 kHz `app_timer_event`. Engine owns press and release for grid pads in Two-stage mode; library pad events ignored there. Library threshold lowered once in `app_init` via `pad_setcalibration`. Checkpoint app gets runtime mode switch (Setup + pad) until Stage 2 menu exists. Thresholds come from measurement build first.

**Tech Stack:** C99, arm-none-eabi-gcc, vendor `lib/launchpad_pro.a`, host test harness `tests/` (`make test`), simulator (`build/simulator`).

**Spec:** `docs/superpowers/specs/2026-11-21-midi2-scale-instrument-design.md` (Task 1 amends it). Read "Velocity" (lines ~110-132) and "Pad engine" (section 3) first.

## Why (from player)

Dynamic playing whole point of project. Extremely soft playing most important. Two-stage may be weak at loud end; player accepts. Three modes, three feels, each own quirks: feature, not bug.

## Facts from library disassembly (verified, do not re-derive)

- TIM2 50 kHz (prescaler 9, period 143, 72 MHz). Main loop gets 1 kHz flag (50 ticks) and 200 Hz flag (250 ticks).
- ADC scans 4 banks x 16 channels into `ADC_Direct[64]` (12-bit, ADC_MAP order). After full frame, `ADCDataReady=1` stops scanning until main loop `surface_1khz` clears it. So each pad updates about once per ms, phase tied to main loop, not to app timer.
- `app_timer_event` = TIM4 ISR, 1 kHz (prescaler 35, period 1999). Buffer passed to `app_init` is `ADC_Direct`.
- `pad_calculate` (200 Hz, main loop) uses `PadCalibration[0]` = threshold `t` (main passes 256). Idle press needs `ADC_Max >= t + 80` and velocity table index `400*(ADC_Max - t - 80)/(t + 320) >= 15` (table zero below index 15). So trigger ≈ `1.0375*t + 92` (10-bit). t=256 gives 358 (35%); t=20 gives 113 (11%).
- Held/release threshold uses `t' = max(t - 10, 4)` with same formula, release after 4 checks below (~20 ms). Below t≈20 press and release levels overlap: chatter.
- Library fires press at once only when table value is 127 (index >= 479). With t=256 never happens (needs 1026); with t=20 happens at `ADC_Max >= 507`. Otherwise waits one 200 Hz check for peak. Side effect accepted: Zero-latency reads some loud hits mid-rise.
- `pad_setcalibration(u16 threshold, u16 c1, u16 aftertouch_lo, u16 aftertouch_hi)` is global; main calls `pad_setcalibration(0x100, 0x3ff, 0x200, 0x301)` immediately before `app_init`, from main thread before main loop starts. Calling it again inside `app_init` is race-free.

## Design decisions (made; do not reopen)

- Scope: Two-stage does own detection from raw 12-bit ADC. Zero-latency and Peak-window keep library press event, with library threshold lowered to `PAD_LIB_THRESHOLD = 20`. Whether to move them to own detection decided later from measurement.
- Two-stage per-pad states: `TS_IDLE`, `TS_ARMED`, `TS_HELD`.
- Crossing times interpolated linearly between previous and current sample, fixed point Q8 ms (1/256 ms). Hard hit crossing both thresholds in one tick still gets finite dt (effectively rise slope).
- Velocity = `min(0xFFFF, 0xFFFF * TS_DT_FAST_Q8 / dt_q8)`, then `max(TS_VEL_MIN)`. Reciprocal curve: short time loud, long time soft, most resolution at soft end. `TS_DT_FAST_Q8 = 256` (1 ms). `TS_VEL_MIN = 0x0200` (7-bit value 1, so MIDI 1.0 fallback never sends 0).
- ARMED and reading reaches `TS_T2`: Note On, go HELD.
- ARMED for `TS_TIMEOUT_MS` without reaching T2, reading still >= T1: Note On with `ts_velocity(TS_TIMEOUT_MS*256)` (continuous with curve; very slow gentle press still sounds). Go HELD.
- ARMED and reading drops below `TS_REL`: silent, back to IDLE (brush, not note).
- HELD: Note Off after `TS_REL_TICKS` consecutive samples below `TS_REL`.
- First tick after init only stores `prev` (no false crossing from prev=0).
- Default constants (12-bit units), replaced after Task 3 measurement: `TS_T1 = 120`, `TS_T2 = 480`, `TS_REL = 80`, `TS_TIMEOUT_MS = 80`, `TS_REL_TICKS = 3`. Must hold `TS_REL < TS_T1 < TS_T2`.
- Curves (Soft/Linear/Hard) of Stage 2 apply after engine output, same as other modes. Not this plan.
- Library poly pressure in Two-stage mode: forward only for pads engine holds.

## Global Constraints

- `include/app.h` not modified. New API in new headers.
- Firmware build needs `arm-none-eabi-gcc` toolchain and `git submodule update --init` (README). `make test` also needs `arm-none-eabi-ar`/`objcopy` (golden descriptor extraction).
- Tests: `-std=c99 -Wall -Werror -DHOST_TEST`. Firmware flash cap 101K (`tools/check_image.sh`).
- `app_timer_event` runs in interrupt context. Engine: no division by zero, no 64-bit math, no malloc, bounded work (64 pads per tick).
- Velocity 16-bit MIDI 2.0; send via `hal_send_note_on2` / `hal_send_note_off2` on `USBSTANDALONE`, channel 0, note = pad index (checkpoint app convention).
- No model names in commits/docs. Commit messages normal style.

## Review Focus

1. Pad pressed exactly at frame where `x == prev` (stale or repeated frame): interpolation divisor zero. Expect: treat crossing at current tick, no division by zero. Test in Task 4 (`test_ts_flat_step_no_div0`).
2. Hard hit: prev below T1, current far above T2 in one tick. Expect: one Note On, velocity near top, not two events. Test in Task 4 (`test_ts_single_tick_hard_hit`).
3. Pad held through mode switch. Expect: Note Off for every sounding note before mode changes; no stuck notes, no orphan Off. Test in Task 5 (`test_app_mode_switch_releases`, simulator trace).
4. Reading hovering around TS_REL while held (noise). Expect: no Off/On flicker; Off only after TS_REL_TICKS consecutive low samples. Test in Task 4 (`test_ts_release_debounce`).
5. Library press event arrives for pad while Two-stage active (library threshold now low). Expect: ignored, no duplicate Note On. Test in Task 5 (`test_app_twostage_ignores_lib_press`).

---

### Task 1: Spec amendment

**Files:**
- Modify: `docs/superpowers/specs/2026-11-21-midi2-scale-instrument-design.md`

- [ ] **Step 1: Edit spec.** Goal 6: "three velocity algorithms": Peak-window, Zero-latency, Two-stage. Velocity approaches section: add Two-stage paragraph (copy "Design decisions" above, short), add library threshold lowering (`PAD_LIB_THRESHOLD = 20`, floor ~11% vs ~35%, chatter below 20). Options page row 1: `Peak-window / Zero-latency / Two-stage / Fixed`. PE `X-PlayOptions.velocityMode`: add `twoStage`. Pad engine section: Two-stage owns press/release from raw ADC, library pad events ignored in that mode. Left-to-builder: thresholds from measurement (Task 3). Checkpoint doc line "softest hit ... about 0x5400": correct to ~0x5980 (358) for stock threshold, note new threshold changes it.
- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/specs/2026-11-21-midi2-scale-instrument-design.md docs/checkpoint-1-transport.md
git commit -m "spec: add two-stage velocity mode and lower library press threshold"
```

### Task 2: Lower library press threshold

**Files:**
- Modify: `src/platform/pads.h`, `src/app.c` (`app_init`), `tools/sim_midi2.c`, `tests/test_pads.c`

**Interfaces:**
- Produces: `#define PAD_LIB_THRESHOLD 20`; `void pad_setcalibration(u16 threshold, u16 c1, u16 aftertouch_lo, u16 aftertouch_hi);` declared in `pads.h`; `static inline u16 pad_lib_trigger_level(u16 t)` and `static inline u16 pad_lib_release_level(u16 t)` (10-bit, models library math, for tests and docs).

- [ ] **Step 1: Failing tests** in `tests/test_pads.c`, add to `test_pads_all`:

```c
static void test_lib_levels(void)
{
	CHECK_EQ(pad_lib_trigger_level(256), 358);
	CHECK_EQ(pad_lib_trigger_level(20), 113);
	CHECK(pad_lib_release_level(PAD_LIB_THRESHOLD) < pad_lib_trigger_level(PAD_LIB_THRESHOLD));
	CHECK(pad_lib_release_level(0) >= pad_lib_trigger_level(0));   /* why floor is 20 */
}
```

- [ ] **Step 2: Run** `make test`. Expect: compile FAIL (undeclared).
- [ ] **Step 3: Implement.** Trigger level = smallest `m` with `m - t >= 80` and `400*(m - t - 80)/(t + 320) >= 15`. Release level = same with `t' = max(t - 10, 4)` for the subtraction, `t + 320` divisor unchanged (smallest `m` that still counts as pressed). Compute in closed form or small loop; both fine. `app_init`: call `pad_setcalibration(PAD_LIB_THRESHOLD, 0x3ff, 0x200, 0x301)` first. Sim: stub `pad_setcalibration` printing args.
- [ ] **Step 4: Run** `make test` PASS, `make` builds, simulator prints `pad_setcalibration(20, ...)`.
- [ ] **Step 5: Commit** `git commit -m "pads: lower library press threshold to 20 for softer Zero-latency"`

### Task 3: Measurement build (pad scope)

Throwaway-grade but kept behind build flag. Gives noise floor, crosstalk, attack shapes, frame freshness. Sets Task 4 constants.

**Files:**
- Create: `src/pads/padscope.h`, `src/pads/padscope.c`, `tests/test_padscope.c`
- Modify: `Makefile` (`PADSCOPE=1` adds `-DPADSCOPE` to firmware CFLAGS; add `src/pads/*.c` to `FW_SOURCES` and `SOURCES`-for-sim as needed; test sources), `tests/run_tests.c`, `src/app.c`

**Interfaces:**
- Produces:
  - `u8 padscope_pack12(const u16 *vals, u8 count, u8 *out)` — each 12-bit value as two 7-bit bytes (hi 5 bits then lo 7 bits... use: `out[0] = (v >> 7) & 0x1F`, `out[1] = v & 0x7F`), returns bytes written.
  - `void padscope_tick(const u16 *raw)` — call every `app_timer_event`.
  - `void padscope_dump(void)` — sends SysEx on `USBSTANDALONE` via `hal_send_sysex`.
- Behaviour:
  - Idle stats: per ADC channel min and max over ticks where channel below 400 (12-bit), reset on dump.
  - Attack capture: watched pad = first grid pad whose raw value crosses 200. Keep 8-sample pre-trigger ring; record 120 samples of that pad plus per-sample max of its 4 grid neighbours (look up via ADC_MAP). Also count stale ticks (whole 64-value frame identical to previous tick).
  - Dump capture after watched pad below 100 for 50 ms. Dump idle stats when Setup pressed (in PADSCOPE build Setup does this instead of RX pause).
  - SysEx format: `F0 00 20 29 7D <type> <payload> F7`; type 1 = capture (`pad index, stale count lo/hi 7-bit, 128 pad samples packed, 128 neighbour samples packed`), type 2 = idle stats (64 x min, 64 x max packed, ADC order). Manufacturer `00 20 29` (Focusrite), `7D` = private test id. Split into messages under 320 bytes if transport requires (see `midi2.h` SysEx notes).

- [ ] **Step 1: Failing tests** `tests/test_padscope.c`: `test_pack12` (`{0x000, 0xFFF, 0x0A5}` gives `{0x00,0x00, 0x1F,0x7F, 0x01,0x25}`, returns 6), `test_capture_trigger` (feed synthetic frames: pad 11 ramp 0..1000 step 100, check capture starts at 200 crossing with 8 pre-samples), `test_stale_count` (same frame twice counts 1). Expose capture buffer read accessor under `HOST_TEST` only.
- [ ] **Step 2: Run** `make test` FAIL.
- [ ] **Step 3: Implement.** Hook in `app.c` under `#ifdef PADSCOPE` only; default build unchanged.
- [ ] **Step 4: Run** `make test` PASS; `make` PASS; `make clean && make PADSCOPE=1` PASS and image under cap.
- [ ] **Step 5: Add host decoder** `tools/padscope_decode.py` (reads pasted hex SysEx lines from stdin, prints CSV: `t_ms,pad,neighbour_max`, and idle min/max table). Python 3 stdlib only.
- [ ] **Step 6: Commit** `git commit -m "pads: add PADSCOPE measurement build for raw attack and noise capture"`

### Task 4: Two-stage engine

**Files:**
- Create: `src/pads/twostage.h`, `src/pads/twostage.c`, `tests/test_twostage.c`
- Modify: `Makefile` (TEST_SOURCES, firmware sources, simulator sources), `tests/run_tests.c`

**Interfaces:**
- Produces (`twostage.h`):

```c
#define TS_T1 120
#define TS_T2 480
#define TS_REL 80
#define TS_TIMEOUT_MS 80
#define TS_REL_TICKS 3
#define TS_DT_FAST_Q8 256
#define TS_VEL_MIN 0x0200

enum { TS_IDLE, TS_ARMED, TS_HELD };

typedef struct {
	u8 state;
	u8 low_count;     /* consecutive samples below TS_REL while held */
	u16 prev;         /* previous raw sample */
	u32 t1_q8;        /* T1 crossing time, Q8 ms */
} ts_pad;

typedef struct {
	ts_pad pad[64];   /* ADC channel order */
	u32 now_ms;
	u8 primed;        /* 0 until first frame stored */
} ts_engine;

/* on = 1: Note On with velocity; on = 0: Note Off (velocity 0) */
typedef void (*ts_note_fn)(void *ctx, u8 adc_ch, u8 on, u16 velocity);

void ts_init(ts_engine *e);
void ts_tick(ts_engine *e, const u16 *raw, ts_note_fn fn, void *ctx);
void ts_release_all(ts_engine *e, ts_note_fn fn, void *ctx);   /* Off for every HELD pad, all to IDLE */
u16 ts_velocity(u32 dt_q8);
u32 ts_cross_q8(u32 now_ms, u16 prev, u16 x, u16 level);       /* interpolated crossing time */
```

- Crossing: `ts_cross_q8 = (now_ms - 1) * 256 + ((level - prev) * 256) / (x - prev)`, when `x > prev`; if `x <= prev` return `now_ms * 256`. Caller guarantees `prev < level <= x` for normal case. Clamp fraction to 0..256.
- `dt_q8` minimum 1 before division.

- [ ] **Step 1: Failing tests** `tests/test_twostage.c` (helper recorder collects events into array):
  - `test_ts_velocity_curve`: `ts_velocity(256) == 0xFFFF`; `ts_velocity(128) == 0xFFFF`; `ts_velocity(512) == 0x7FFF`; `ts_velocity(80*256) == 0x0333`; `ts_velocity(1000*256) == TS_VEL_MIN`; `ts_velocity(0) == 0xFFFF`; monotonic non-increasing over 1..30000.
  - `test_ts_cross_interp`: `ts_cross_q8(10, 0, 240, 120) == 9*256 + 128`; `ts_cross_q8(10, 100, 100, 120) == 2560`.
  - `test_ts_primed_first_frame`: first frame all 1000, no events.
  - `test_ts_slow_press`: ramp pad ch 0 by +10 per tick from 0: T1 at tick 12, T2 at tick 48, one On with `ts_velocity(36*256)` (±1), no Off while held.
  - `test_ts_single_tick_hard_hit`: 0 then 2000: one On, velocity `ts_velocity(((480-120)*256)/2000)` = `0xFFFF` (dt < 1 ms).
  - `test_ts_timeout_gentle`: hold at 200 (between T1 and T2) for 100 ticks: one On at tick `TS_TIMEOUT_MS` after T1 crossing, velocity `ts_velocity(TS_TIMEOUT_MS*256)`.
  - `test_ts_brush_silent`: 0, 200, 200, 50: no events, state IDLE.
  - `test_ts_release_debounce`: held pad samples `70, 90, 70, 70, 90, 70, 70, 70`: exactly one Off, at last sample (3 consecutive lows).
  - `test_ts_flat_step_no_div0`: prev 100, frames 100, 100, then 600: one On, no crash.
  - `test_ts_retrigger_needs_release`: held pad stays 1000 for 200 ticks: one On only.
  - `test_ts_release_all`: two held pads: two Offs, both IDLE.
- [ ] **Step 2: Run** `make test` FAIL.
- [ ] **Step 3: Implement** `twostage.c`. Per tick: `now_ms++`; if not primed store prev for all, set primed, return. Per channel state machine per Design decisions. Update `prev = x` every tick. On fire from ARMED: `fn(ctx, ch, 1, ts_velocity(t2 - t1))`; timeout fire uses `TS_TIMEOUT_MS*256`. Add `_Static_assert`-equivalent compile check (`typedef char ts_order[(TS_REL < TS_T1 && TS_T1 < TS_T2) ? 1 : -1];`).
- [ ] **Step 4: Run** `make test` PASS; `make` PASS.
- [ ] **Step 5: Commit** `git commit -m "pads: add two-stage (time between thresholds) velocity engine"`

### Task 5: Checkpoint app mode switch

**Files:**
- Modify: `src/app.c`, `tools/simulator.c` (scripted scenario), `tools/sim_midi2.c`
- Test: `tests/test_app_modes.c` (link `src/app.c` with stubs; add stubs for `hal_plot_led`, `hal_send_midi`, `hal_send_note_on2/off2`, `hal_send_poly_pressure2`, `midi2_*`, `pad_setcalibration`, `ADC_Max` in `tests/stubs/` if missing), `tests/run_tests.c`, `Makefile`

**Interfaces:**
- Consumes: Task 2 `PAD_LIB_THRESHOLD`, Task 4 `ts_*`.
- Produces: in `app.c`: `enum { VEL_ZERO_LATENCY, VEL_TWO_STAGE }; static u8 g_vel_mode;` (default `VEL_TWO_STAGE` so hardware test starts in new mode).
- Behaviour:
  - `app_timer_event`: if mode Two-stage, `ts_tick(&g_ts, g_ADC, on_ts_note, 0)`. `on_ts_note` maps `ADC_MAP[adc_ch]` to pad index, sends `hal_send_note_on2/off2(USBSTANDALONE, 0, index, v)`, sets `g_held`, LED level `v >> 10`.
  - `app_surface_event` TYPEPAD for grid pads: Zero-latency path unchanged; Two-stage ignores grid pad events (non-grid buttons still handled).
  - While Setup held: pad 11 selects Zero-latency, pad 12 selects Two-stage; pads 11/12 lit (selected bright) while Setup held. Before switching: release all notes of old mode (`ts_release_all` or Note Off for each `g_held`), reset `g_held`.
  - `app_aftertouch_event`: forward only if `g_held[index]`.
- [ ] **Step 1: Failing tests:** `test_app_mode_switch_releases` (Two-stage: feed frames holding pad via `app_timer_event`, then Setup + pad 11: one Note Off before mode change, no later Off), `test_app_twostage_ignores_lib_press` (Two-stage: `app_surface_event(TYPEPAD, 11, 100)` sends nothing), `test_app_zero_latency_unchanged` (Zero-latency: press with `ADC_Max[slot]=512` sends `0x8000`).
- [ ] **Step 2: Run** `make test` FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `make test` PASS; `make` PASS (simulator run in `make` passes); image under cap.
- [ ] **Step 5: Commit** `git commit -m "app: selectable Zero-latency / Two-stage velocity in checkpoint app"`

### Task 6: Hardware procedure

**Files:**
- Create: `docs/checkpoint-1b-velocity.md`

- [ ] **Step 1: Write procedure** (style of `docs/checkpoint-1-transport.md`, plain, numbered):
  1. Flash `PADSCOPE=1` image. Untouched 10 s, press Setup, save idle dump. Then for pads 11, 44, 88: 5 feather-light, 5 soft, 5 medium, 5 hard strikes, 3 slow firm pushes; save each capture dump. Decode with `tools/padscope_decode.py`, send CSVs.
  2. From data (builder, not player): noise floor = max idle max; crosstalk = max neighbour reading during hard hits. Set `TS_REL` above both with margin, `TS_T1` = `TS_REL` + 40, `TS_T2` low enough that feather-light captures reach it; `TS_TIMEOUT_MS` from slowest soft press T1-to-T2 time. Check `PAD_LIB_THRESHOLD = 20` trigger (113 10-bit = ~452 12-bit) clears noise and crosstalk; if not, raise it.
  3. Flash default image with tuned constants. Play both modes (Setup + 11 / Setup + 12). Record: softest playable note each mode (value from monitor), ghost notes when hitting neighbours, stuck notes, feel notes.
  - Results table with Expected / Result / Notes columns.
- [ ] **Step 2: Commit** `git commit -m "docs: checkpoint 1b velocity measurement and feel test"`

### Task 7: Apply measured constants

Blocked on player's Task 6 step 1 data.

**Files:**
- Modify: `src/pads/twostage.h`, maybe `src/platform/pads.h` (`PAD_LIB_THRESHOLD`), `tests/test_twostage.c` (expected values derived from constants, keep tests constant-relative), spec "Left to the builder" note with chosen values and data source.

- [ ] **Step 1:** Update constants; `make test` PASS; `make` PASS.
- [ ] **Step 2: Commit** `git commit -m "pads: tune two-stage thresholds from padscope measurements"`

## Later (not this plan)

- Stage 2 pad engine absorbs `twostage.c` as third mode next to Peak-window; menu row 1 and PE enum per Task 1 spec edit; curves applied to engine output.
- Decide from Task 6 data whether Zero-latency and Peak-window move to own raw detection.
