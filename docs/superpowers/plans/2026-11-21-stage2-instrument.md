# Stage 2: Scale Instrument Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the stage 1 test app with the scale instrument. It has
Launchpad95-style scales and layouts, both velocity modes plus Fixed,
velocity curves, high-resolution poly or channel pressure from the raw ADC,
a Setup menu with Scale and Options pages, and settings that survive a
power cycle.

**Architecture:** Four host-testable modules sit under a thin `src/app.c`:
- the scale engine (`src/scales/`), pure pitch and colour maths;
- the settings model and flash record (`src/instrument/settings.*`);
- the pad engine (`src/pads/pad_engine.*`): velocity, pressure, note
  sharing and latching;
- the UI (`src/instrument/ui.*`): pages, LEDs and the Setup button.

Everything runs in the **main loop**: the pad callbacks already arrive
there, and a new main-loop hook from the transport layer drives the
peak-window timer, pressure scanning, the Setup LED and the deferred flash
save. The 1 ms timer only advances a millisecond counter. All output goes
through a small I/O seam (`src/instrument/io.h`), so the modules can be
tested on the host with a recorder.

**Tech Stack:** C99, arm-none-eabi-gcc (Cortex-M3), host gcc for tests and
the simulator. The stage 1 `midi2.h` API is used underneath.

**Spec:** `docs/superpowers/specs/2026-11-21-midi2-scale-instrument-design.md`.
This plan covers spec §3–§6, the instrument parts of §5 and §7, and delivery
stage 2. Stage 1 hardware results: `docs/checkpoint-1-results.md`.

## Global Constraints

- `include/app.h` and `include/app_defs.h` are not modified. New public API
  goes in `include/midi2.h`.
- No heap, no 64-bit multiply or divide in firmware code, no floating point.
- Fixed-width `uint8_t`/`uint16_t`/`uint32_t` in new modules. `u32` is
  `unsigned long`, which is 64-bit on the host, so use it only where calling
  `app.h`/`midi2.h`.
- RAM: new static data for stage 2 must be ≤ 2 KiB. Stage 1's
  `g_vel`/`g_held` (300 bytes) are removed. `.data` + `.bss` must stay
  ≤ 13 KiB, which leaves room for stage 3's ~1.5 KB and the stack in 20 KiB.
  Today the total is 10 928 bytes.
- Flash: the image must still pass `tools/check_image.sh`.
- Pads: grid pads are 11..88 with row = index / 10 and col = index % 10
  (both 1..8). ADC_Max is 10-bit and in library slot order
  (`pad_to_adcmax_slot`). The raw `adc_buffer` from `app_init` is 12-bit
  and in `ADC_MAP` order. Both orders were verified against the library's
  `PADADC` table (all 64 pads match).
- Library calibration at boot: `pad_setcalibration(256, 1023, 512, 769)`.
  The aftertouch range is 512..769 in 10-bit, or 2048..3076 in raw 12-bit.
- Octave model: stored octave 0..7. The bottom-left pad is
  `12 × (octave + 1) + root` (default octave 4, root C → 60). With absolute
  root on, the anchor is `12 × (octave + 1)`.
- Defaults: root C (0), scale Major (0), octave 4, layout In-key 4ths,
  absolute root off, velocity mode Peak-window, curve Linear, pressure Poly,
  threshold Med, DIN mirror off, channel 1 (stored 0).
- MIDI 1.0 destinations (DIN, USB alt 0, and Standalone in MIDI 1.0
  Protocol) get the top 7 bits. The `hal_send_*2` functions already do this
  scaling and clamp Note On to ≥ 1.
- The stage 1 MIDI clock and the "hold Setup pauses RX" debug behaviour are
  removed from the app. `midi2_debug_pause_rx` stays in the API.
- Bridge: USB "MIDI" port ↔ DIN channel-voice and system messages keep
  working as in stage 1 (`app_midi_event` unchanged in behaviour).
- No GPL code. Scale interval sets are the music-theory table in Task 1.
- Every commit ends with the session's attribution lines.

## Review Focus

1. **Pressing a pad again within or just after a peak-window capture.**
   Press, release and press again within 3 ms must give Note On, Note Off,
   Note On, in that order, with no stuck note. Test:
   `test_retrigger_within_window` (Task 5).
2. **Opening the menu while a peak-window capture is pending.** The capture
   is dropped: no Note On is ever sent and no Note Off either. Test:
   `test_setup_entry_during_capture_sends_nothing` (Task 5).
3. **Raw ADC jitter on a pad held at rest.** ±10 raw counts of noise around
   a steady press must not send more than a handful of messages a second,
   and must not cost pressure resolution (filter, not deadband).
   Test: `test_pressure_jitter_does_not_flood` (Task 6).
4. **The millisecond counter wrapping** (after 49.7 days of uptime). The
   peak window and pressure rate cap still work across the wrap. Test:
   `test_peak_window_across_ms_wrap` (Task 5).
5. **A pad held across opening and closing the menu, then released.**
   There is no note, no stray Note Off and no pressure. Test:
   `test_pad_held_across_menu_is_silent` (Task 7).

---

## File Structure

| File | Responsibility |
|---|---|
| `src/scales/scales.h`, `scales.c` | Scale table and names, layout → pitch, pitch → colour role |
| `src/instrument/settings.h`, `settings.c` | `struct settings`, defaults, validation, 17-byte flash record, debounced save |
| `src/instrument/io.h` | Output seam: notes, pressure, LEDs, flash, "is this port MIDI 2.0" |
| `src/instrument/io_hal.c` | Firmware and simulator implementation of `io.h` (not in tests) |
| `src/pads/pad_velocity.h`, `.c` | (extended) curves, raw ADC index, pressure normalisation and bit replication |
| `src/pads/pad_engine.h`, `.c` | Per-pad state, velocity modes, note sharing and latching, pressure output |
| `src/instrument/ui.h`, `ui.c` | Play page, Setup toggle/peek, Scale and Options pages, LED drawing, applying settings |
| `src/app.c` | Callbacks → UI / pad engine; ms counter; main-loop hook |
| `include/midi2.h`, `src/usb/usb_midi2.c` | `midi2_set_main_loop_hook` |
| `tests/stubs/io_stub.c`, `io_stub.h` | Recorder for `io.h` plus test `ADC_Max`/`PadCalibration` |
| `tests/test_scales.c`, `test_settings.c`, `test_pad_engine.c`, `test_pressure.c`, `test_ui.c` | Host tests |
| `docs/checkpoint-2-instrument.md` | Hardware test procedure for the player |

The Makefile picks up the new firmware sources with
`FW_SOURCES += $(wildcard src/scales/*.c src/instrument/*.c)` (`src/pads/*.c`
is already there). Each task appends its test file and sources to
`TEST_SOURCES` (never `io_hal.c`) and its `test_*_all` to
`tests/run_tests.c`. `SIM_SOURCES` gets the same non-test sources plus
`io_hal.c` in Task 8.

---

### Task 1: Scale engine

**Files:**
- Create: `src/scales/scales.h`, `src/scales/scales.c`
- Test: `tests/test_scales.c`
- Modify: `Makefile` (TEST_SOURCES), `tests/run_tests.c`

**Interfaces:**
- Produces:
  ```c
  #define SCALE_COUNT 25
  enum layout { LAYOUT_INKEY_4THS, LAYOUT_INKEY_3RDS, LAYOUT_INKEY_SEQ,
                LAYOUT_CHROM_4THS, LAYOUT_CHROM_GUITAR, LAYOUT_COUNT };
  enum role { ROLE_OFF, ROLE_OUT, ROLE_SCALE, ROLE_HIGHLIGHT, ROLE_ROOT };
  struct scale_def { const char *name; uint8_t n; uint8_t steps[8]; };
  extern const struct scale_def SCALES[SCALE_COUNT];
  struct scale_ctx { uint8_t root, scale, octave, layout, absolute_root; };
  int16_t scale_pad_pitch(const struct scale_ctx *c, uint8_t pad); /* -1: not a grid pad or outside 0..127 */
  enum role scale_pitch_role(const struct scale_ctx *c, int16_t pitch); /* ROLE_OFF for -1 */
  ```

Scale table (index order is the menu order, and stage 3's `X-ScaleList`):

| # | Name | Semitones |
|---|---|---|
| 0 | Major | 0 2 4 5 7 9 11 |
| 1 | Minor | 0 2 3 5 7 8 10 |
| 2 | Dorian | 0 2 3 5 7 9 10 |
| 3 | Mixolydian | 0 2 4 5 7 9 10 |
| 4 | Lydian | 0 2 4 6 7 9 11 |
| 5 | Phrygian | 0 1 3 5 7 8 10 |
| 6 | Locrian | 0 1 3 5 6 8 10 |
| 7 | Diminished | 0 1 3 4 6 7 9 10 |
| 8 | Whole-half | 0 2 3 5 6 8 9 11 |
| 9 | Whole Tone | 0 2 4 6 8 10 |
| 10 | Minor Blues | 0 3 5 6 7 10 |
| 11 | Minor Pentatonic | 0 3 5 7 10 |
| 12 | Major Pentatonic | 0 2 4 7 9 |
| 13 | Harmonic Minor | 0 2 3 5 7 8 11 |
| 14 | Melodic Minor | 0 2 3 5 7 9 11 |
| 15 | Super Locrian | 0 1 3 4 6 8 10 |
| 16 | Bhairav | 0 1 4 5 7 8 11 |
| 17 | Hungarian Minor | 0 2 3 6 7 8 11 |
| 18 | Minor Gypsy | 0 1 4 5 7 8 10 |
| 19 | Hirojoshi | 0 2 3 7 8 |
| 20 | In-Sen | 0 1 5 7 10 |
| 21 | Iwato | 0 1 5 6 10 |
| 22 | Kumoi | 0 2 3 7 9 |
| 23 | Pelog | 0 1 3 7 8 |
| 24 | Spanish | 0 1 3 4 5 6 8 10 |

Mapping rules (row r = pad/10 − 1, col c = pad%10 − 1, both 0..7):
- In-key: degree `d = c + k·r` with k = 3 (4ths), 2 (3rds) or 8
  (sequential). Pitch = `anchor + 12·((d0 + d) / n) + steps[(d0 + d) % n]`.
  Without absolute root, `anchor = 12·(octave+1) + root` and `d0 = 0`. With
  absolute root, `anchor = 12·octave + root` (one octave lower) and `d0` is
  the smallest degree whose pitch is ≥ `12·(octave+1)`.
- Chromatic 4ths: `base + c + 5·r`. Chromatic guitar: `base + c + G[r]`
  with `G = {0,5,10,15,19,24,29,34}`. `base = 12·(octave+1) + (absolute ? 0 : root)`.
- Role of a pitch: pitch class equal to root → ROLE_ROOT. Equal to
  `root + steps[2]` or `root + steps[4]` (mod 12) → ROLE_HIGHLIGHT. In the
  scale → ROLE_SCALE. Otherwise → ROLE_OUT.

- [ ] **Step 1: Write the failing tests** in `tests/test_scales.c`:

```c
static const struct scale_ctx DEF = {0, 0, 4, LAYOUT_INKEY_4THS, 0};
static void test_default_bottom_left_is_c3(void) {
    CHECK_EQ(scale_pad_pitch(&DEF, 11), 60);
    CHECK_EQ(scale_pad_pitch(&DEF, 12), 62);
    CHECK_EQ(scale_pad_pitch(&DEF, 21), 65);   /* +3 degrees = F */
    CHECK_EQ(scale_pad_pitch(&DEF, 18), 72);   /* 8th degree = C4 */
}
static void test_inkey_layouts_only_scale_notes(void);   /* every layout<3 × scales {0,9,11,24} × roots 0..11: every pad pitch (≠ -1) has role != ROLE_OUT */
static void test_inkey_3rds_and_seq_rows(void);  /* 3rds: pad 21 = 64; seq: pad 21 = 74 (degree 8) */
static void test_chromatic_4ths(void);           /* root D: pad 11 = 62, 12 = 63, 21 = 67 */
static void test_chromatic_guitar(void);         /* default octave: pad 51 = 79 (60+19), pad 41 = 75 */
static void test_absolute_root_inkey(void);      /* D major, octave 4, absolute: pad 11 = 61, pad 12 = 62 */
static void test_absolute_root_chromatic(void);  /* D, chrom 4ths, absolute: pad 11 = 60 */
static void test_octave_positions(void);         /* octave 0..7 → pad 11 = 12·(o+1) */
static void test_out_of_range_is_minus1(void);   /* octave 7 chromatic: pad 88 → 96+7+35=138 → -1; non-grid pads 10, 19, 91 → -1 */
static void test_roles(void);                    /* C major: 60 ROOT, 64 HIGHLIGHT, 67 HIGHLIGHT, 62 SCALE, 61 OUT, -1 OFF */
static void test_scale_table_sane(void);         /* each n in 5..8, steps[0]==0, strictly increasing, < 12; names[0]=="Major", [24]=="Spanish" */
```

- [ ] **Step 2: Run `make test`.** Expected: link failure (`scale_pad_pitch` undefined).
- [ ] **Step 3: Implement `src/scales/scales.c`** with the table and rules above.
- [ ] **Step 4: Run `make test`.** Expected: `0 failures`.
- [ ] **Step 5: Commit** `scales: scale table, layouts and colour roles`.

---

### Task 2: Settings model and flash record

**Files:**
- Create: `src/instrument/settings.h`, `src/instrument/settings.c`, `src/instrument/io.h`, `tests/stubs/io_stub.h`, `tests/stubs/io_stub.c`
- Test: `tests/test_settings.c`
- Modify: `Makefile`, `tests/run_tests.c`

**Interfaces:**
- Consumes: `SCALE_COUNT`, `LAYOUT_COUNT` (Task 1).
- Produces `io.h` (used by Tasks 2–7):
  ```c
  void io_note_on(uint8_t port, uint8_t ch, uint8_t note, uint16_t vel);
  void io_note_off(uint8_t port, uint8_t ch, uint8_t note);
  void io_poly_pressure(uint8_t port, uint8_t ch, uint8_t note, uint32_t v);
  void io_channel_pressure(uint8_t port, uint8_t ch, uint32_t v);
  uint8_t io_port_is_midi2(uint8_t port);          /* USBSTANDALONE && midi2_active() */
  void io_led(uint8_t type, uint8_t index, uint8_t r, uint8_t g, uint8_t b);
  void io_flash_read(uint16_t off, uint8_t *d, uint16_t n);
  void io_flash_write(uint16_t off, const uint8_t *d, uint16_t n);
  ```
- Produces `io_stub.h` (recorder): `struct io_ev { char kind; uint8_t port, ch, note; uint32_t v; }`
  with kind 'N' (note on), 'F' (off), 'P' (poly), 'C' (channel); `io_log[256]`, `io_n`;
  `io_midi2[3]` (per-port `io_port_is_midi2` answers); `io_leds[100][3]`;
  `io_setup_led[3]`; a 1024-byte `io_flash[]` initialised to 0xFF;
  `io_flash_writes`; `uint16_t ADC_Max[64]`, `uint16_t PadCalibration[4] = {256,1023,512,769}`;
  `uint16_t io_adc[64]`; `void io_stub_reset(void)`.
- Produces `settings.h`:
  ```c
  enum { VEL_PEAK_WINDOW, VEL_ZERO_LATENCY, VEL_FIXED };
  enum { CURVE_SOFT, CURVE_LINEAR, CURVE_HARD };
  enum { PRESS_POLY, PRESS_CHANNEL, PRESS_OFF };
  enum { THRESH_LOW, THRESH_MED, THRESH_HIGH };
  struct settings { uint8_t root, scale, octave, layout, absolute_root,
                    velocity_mode, velocity_curve, pressure_mode,
                    pressure_threshold, din_mirror, channel; };
  #define SETTINGS_RECORD_LEN 17
  #define SETTINGS_SAVE_QUIET_MS 2000
  void settings_defaults(struct settings *s);
  uint8_t settings_valid(const struct settings *s);
  void settings_encode(const struct settings *s, uint8_t out[SETTINGS_RECORD_LEN]);
  uint8_t settings_decode(const uint8_t in[SETTINGS_RECORD_LEN], struct settings *s); /* 1 ok; 0 leaves *s untouched */
  void settings_load(struct settings *s);       /* flash offset 0; defaults on failure; remembers it as "saved" */
  void settings_touch(uint32_t now);            /* a change happened now */
  void settings_poll(const struct settings *cur, uint32_t now, uint8_t notes_active);
  ```

Record: bytes 0–3 `'L','P','M','2'`, byte 4 version `1`, bytes 5–15 the
fields in struct order, byte 16 = checksum chosen so that the sum of all 17
bytes is 0 mod 256. Decoding requires the magic, the version, the checksum
and `settings_valid`. Ranges: root < 12, scale < 25, octave < 8,
layout < 5, absolute_root and din_mirror ≤ 1, the three enums < 3,
channel < 16.

`settings_poll` writes only when `cur` differs from the last saved copy,
`now − last_touch ≥ SETTINGS_SAVE_QUIET_MS` (wrap-safe subtraction) and
`notes_active == 0`. After a write, the saved copy is updated.

- [ ] **Step 1: Write the failing tests:**
  - `test_defaults_valid`: defaults pass `settings_valid` and match Global Constraints.
  - `test_record_round_trip`: encode a non-default record, then decode it
    and compare memberwise. The encoded record has bytes 0–4 equal to `"LPM2\x01"`.
  - `test_blank_flash_loads_defaults`: all 0xFF gives defaults and no write.
  - `test_corrupt_record_loads_defaults`: one record with a flipped
    checksum, one with version 2 and one with scale 25 (valid checksum)
    each give defaults.
  - `test_save_waits_for_quiet_and_no_notes`: touch at t=100. A poll at
    t=2099 writes nothing; at t=2100 with notes_active=1 it writes nothing;
    at t=2100 with 0 it gives `io_flash_writes == 1`, and reloading gives
    the change.
  - `test_unchanged_or_reverted_skips_write`: change the settings and
    change them back, touch, poll after quiet: `io_flash_writes == 0`.
  - `test_save_quiet_across_wrap`: touch at 0xFFFFFF00. A poll at 0x000006D0
    (2000 ms later, after the wrap) writes once.
- [ ] **Step 2: Run `make test`.** Expected: link failure.
- [ ] **Step 3: Implement** `settings.c`, `io.h` and the `io_stub` recorder.
- [ ] **Step 4: Run `make test`.** Expected: `0 failures`.
- [ ] **Step 5: Commit** `instrument: settings model, flash record and io seam`.

---

### Task 3: Velocity curves and pressure maths

**Files:**
- Modify: `src/pads/pad_velocity.h`, `src/pads/pad_velocity.c`
- Test: `tests/test_pad_velocity.c`

**Interfaces:**
- Produces:
  ```c
  uint8_t  pad_raw_index(uint8_t pad);                  /* adc_buffer index, PAD_NO_SLOT if not a grid pad */
  uint16_t pad_curve(uint16_t v, uint8_t curve);        /* CURVE_* from settings.h; result ≥ 1 */
  uint16_t pad_pressure12(uint16_t raw12, uint8_t threshold); /* 0..4095 */
  uint32_t pad_pressure32(uint16_t p12);                /* (p<<20)|(p<<8)|(p>>4) */
  #define PRESSURE_FULL_RAW 3076
  /* start points, raw 12-bit: LOW 1792, MED 2048, HIGH 2304 */
  ```

`pad_raw_index`: rows 1–4 → `(row−1)·16 + (col−1)·2`, rows 5–8 →
`(row−5)·16 + (col−1)·2 + 1` (the inverse of `ADC_MAP`). Curves on 16-bit
v, using `uint32_t` arithmetic only: Linear = v. Hard = `(v·v + v) >> 16`.
Soft = `65535 − Hard(65535 − v)`. The result is clamped to ≥ 1.
`pad_pressure12`: raw ≤ start → 0, raw ≥ `PRESSURE_FULL_RAW` → 4095,
otherwise `(raw − start)·4095 / (FULL − start)`.

- [ ] **Step 1: Write the failing tests:**
  - `test_raw_index_inverts_adc_map`: for every i in 0..63, `pad_raw_index(ADC_MAP[i]) == i`. Also `pad_raw_index(10) == PAD_NO_SLOT`.
  - `test_curve_endpoints`: for each curve, `pad_curve(1, c) ≥ 1` and `pad_curve(65535, c) == 65535`.
  - `test_curve_shape`: at v = 32768, Soft > 40000, Linear == 32768 and Hard < 20000. Each curve is monotonic over v = 0..65535 in steps of 64.
  - `test_pressure12_thresholds`: MED gives 2048 → 0 and 3076 → 4095. LOW at 2048 is > 0. HIGH at 2304 is 0. Pressure is monotonic in raw.
  - `test_pressure32_endpoints`: 0 → 0, 4095 → 0xFFFFFFFF, 2048 → 0x80080080.
- [ ] **Step 2: Run `make test`.** Expected: link failure.
- [ ] **Step 3: Implement** in `pad_velocity.c` (include `instrument/settings.h` for `CURVE_*` and `THRESH_*`).
- [ ] **Step 4: Run `make test`.** Expected: `0 failures`.
- [ ] **Step 5: Commit** `pads: velocity curves, raw ADC index and 12-bit pressure`.

---

### Task 4: Main-loop hook and firmware I/O

**Files:**
- Modify: `include/midi2.h`, `src/usb/usb_midi2.c`, `tools/sim_midi2.c`
- Create: `src/instrument/io_hal.c`
- Test: `tests/test_usb_data.c` (or `test_usb_prop.c`, whichever already drives `__wrap_midi_parsers_poll`)

**Interfaces:**
- Produces in `midi2.h`:
  ```c
  /* Called once per main-loop pass, from the midi_parsers_poll wrap, in
   * both USB modes, after USB and DIN parsing. NULL disables it. */
  void midi2_set_main_loop_hook(void (*hook)(void));
  ```
- Produces `io_hal.c`: each `io_*` maps onto `hal_send_note_on2` /
  `hal_send_note_off2(…, 0)` / `hal_send_poly_pressure2` /
  `hal_send_channel_pressure2` / `hal_plot_led` / `hal_read_flash` /
  `hal_write_flash`. `io_port_is_midi2(p)` returns
  `p == USBSTANDALONE && midi2_active()`.

`main.o`'s loop calls `usbmidi_poll`, `midi_parsers_poll` and `midi_din_poll`
on every pass (verified by disassembly), so the hook runs at main-loop rate.
In `__wrap_midi_parsers_poll`, call the hook at the end of **both** branches.
The alt 0 branch currently returns early.

- [ ] **Step 1: Write the failing test** `test_main_loop_hook_runs_in_both_alts`.
  Register a counting hook and call `__wrap_midi_parsers_poll()` once in
  alt 0 and once after `usb_midi2_set_alt(1)` + `usb_midi2_service()`. The
  count is 2. After `midi2_set_main_loop_hook(0)`, one more poll leaves it
  at 2.
- [ ] **Step 2: Run `make test`.** Expected: link failure.
- [ ] **Step 3: Implement** the hook, `io_hal.c`, and a `sim_midi2.c` stand-in that stores the hook and exposes `void sim_run_main_loop_hook(void)`.
- [ ] **Step 4: Run `make test` and `make`.** Expected: `0 failures`; the firmware links (io_hal.c is unused until Task 8 but must compile).
- [ ] **Step 5: Commit** `midi2: main-loop hook; instrument io for firmware`.

---

### Task 5: Pad engine — notes and velocity

**Files:**
- Create: `src/pads/pad_engine.h`, `src/pads/pad_engine.c`
- Test: `tests/test_pad_engine.c`

**Interfaces:**
- Consumes: `io.h`, `pad_velocity.h` (Tasks 2–3), `ADC_Max`/`PadCalibration` (`platform/pads.h`).
- Produces:
  ```c
  #define PEAK_WINDOW_MS 3        /* ≥ 2 ms after the press at 1 ms tick resolution */
  typedef void (*pe_sounding_fn)(uint8_t pitch, uint8_t sounding);
  void pe_init(const uint16_t *adc_buffer, pe_sounding_fn on_sounding);
  void pe_configure(uint8_t velocity_mode, uint8_t curve, uint8_t pressure_mode, uint8_t threshold);
  void pe_press(uint8_t pad, int16_t pitch, uint8_t ch, uint8_t din_mirror, uint32_t now); /* pitch -1: ignored */
  void pe_release(uint8_t pad, uint32_t now);
  void pe_poll(uint32_t now);          /* peak windows (this task) and pressure (Task 6) */
  void pe_release_all(void);           /* Note Off for every sounding note; drops captures; every pad idle */
  uint8_t pe_notes_active(void);       /* any pad capturing or sounding */
  ```

Behaviour:
- Per-pad state is indexed by `pad_to_adcmax_slot`: idle, capturing or
  held. It latches pitch, channel and the DIN flag at press, and keeps a
  10-bit peak, the press time and the pressure bookkeeping for Task 6.
  Budget: ≤ 16 bytes per pad.
- Velocity: `peak10` → `pad_velocity16(peak10, PadCalibration[0], PadCalibration[1])`
  → `pad_curve`. Fixed mode always gives 0xFFFF.
  - Zero-latency: the Note On is sent inside `pe_press`, from `ADC_Max[slot]`.
  - Peak-window: `pe_press` stores `ADC_Max[slot]`. Each `pe_poll` raises
    the peak with `(adc[pad_raw_index]+2)>>2`, then sends the Note On once
    `now − t_press ≥ PEAK_WINDOW_MS`. A `pe_release` while capturing takes
    one more sample, sends the Note On, then the Note Off.
- Sharing: a per-pitch count (`uint8_t[128]`). The Note On is sent only
  when a pitch goes 0 → 1 and the Note Off only when it goes 1 → 0, and
  `on_sounding(pitch, 1/0)` is called at those same edges. Note Off is
  sent with `io_note_off`.
- Destinations: `USBSTANDALONE`, plus `DINMIDI` when the latched DIN flag is set.

- [ ] **Step 1: Write the failing tests.** Every test starts from
  `io_stub_reset()` and `pe_init(io_adc, rec)`, where `rec` logs the
  sounding edges.
  - `test_zero_latency_note_on_at_press`: ADC_Max[slot(45)] = 679. A press
    gives one 'N' with velocity in 0x7E00..0x8200, before any poll.
  - `test_peak_window_waits_and_takes_max`: ADC_Max 400, then raw for pad
    45 = 4000 at t+1. There is nothing at t+2. At t+3 one 'N' arrives with
    velocity > 0xF000.
  - `test_fast_tap_inside_window`: press at t, release at t+1. The log is exactly 'N' then 'F'.
  - `test_retrigger_within_window`: press, release, press within 3 ms, then polls to t+10. The log is N, F, N.
  - `test_fixed_velocity`: velocity 0xFFFF.
  - `test_curves_applied`: the same peak sent as Soft > Linear > Hard.
  - `test_duplicate_pitch_shares_note`: pads 11 and 14 both get pitch 60.
    Press 11, press 14, release 11, release 14 gives exactly one N and one
    F; the F comes on the last release, and the sounding edges are (60,1),(60,0).
  - `test_latched_pitch_survives_remap`: press pad 11 with pitch 60 and
    release it. The Note Off is for 60, whatever a later press uses.
  - `test_din_mirror`: din=1 gives each N/F twice, port 0 then port 2.
  - `test_release_all`: two sounding pads → two F. A later `pe_release` of those pads sends nothing.
  - `test_setup_entry_during_capture_sends_nothing`: peak-window press,
    then `pe_release_all()`, then polls and a release. The log is empty.
  - `test_peak_window_across_ms_wrap`: press at 0xFFFFFFFE. Nothing at
    0xFFFFFFFF; a Note On by `now == 1`.
  - `test_pitch_minus1_ignored`: no output and `pe_notes_active() == 0`.
- [ ] **Step 2: Run `make test`.** Expected: link failure.
- [ ] **Step 3: Implement** `pad_engine.c`.
- [ ] **Step 4: Run `make test`.** Expected: `0 failures`.
- [ ] **Step 5: Commit** `pads: pad engine with peak-window, zero-latency and fixed velocity`.

---

### Task 6: Pad engine — pressure

**Files:**
- Modify: `src/pads/pad_engine.c`
- Test: `tests/test_pressure.c`

**Interfaces:**
- Consumes: Task 5's engine, `pad_pressure12` / `pad_pressure32` (Task 3), `io_port_is_midi2`.
- Produces (tunables in `pad_engine.h`): `PRESSURE_HYST 16` (12-bit units),
  `PRESSURE_MIN_INTERVAL_MS 5`.

Behaviour, run in `pe_poll` for held (not capturing) pads:
- Noise filter: each pad keeps `uint16_t filt` (raw × 16). It is seeded
  with `raw × 16` when the note starts, then updated once per millisecond
  (the first `pe_poll` with a new `now`):
  `filt += ((int32_t)raw·16 − filt) / 8`, in signed arithmetic. The
  filtered raw value is `(filt + 8) >> 4`. A deadband big enough to hide
  ±10 raw counts would cost resolution (one raw count is about 4 units of
  `p12`); the filter removes the noise instead.
- `p12 = pad_pressure12(filtered_raw, threshold)`.
- Poly: the value for a pitch is the max `p12` over held pads sharing that
  pitch. For each destination, send when the destination is MIDI 2.0
  (`io_port_is_midi2`), it is ≥ `PRESSURE_MIN_INTERVAL_MS` since the
  pitch's last send, and either |Δ| ≥ `PRESSURE_HYST` or the value just
  reached 0 or 4095 and differs from the last sent value. For a 7-bit
  destination, send only when `p12 >> 5` changes, with the same interval.
  The value is `pad_pressure32(p12)`; the transport reduces it to 7 bits
  where needed.
- Channel: the same rules applied to the max over all held pads, sent with
  `io_channel_pressure`. When the last pad is released, a 0 goes out
  straight away (if the last value sent wasn't 0).
- Off: no pressure messages.
- The library's 7-bit `app_aftertouch_event` is no longer used for output.

- [ ] **Step 1: Write the failing tests:**
  - `test_poly_pressure_follows_raw`: hold 45, ramp raw 2048 → 3200 over
    50 ms, then hold 3200 for 100 ms (polling every 1 ms). The 'P' values
    never fall and the last one is 0xFFFFFFFF.
  - `test_pressure_rate_cap`: raw changes every 1 ms by 100 over 20 ms. There are ≤ 5 'P' messages.
  - `test_pressure_jitter_does_not_flood`: hold at raw 2500 for 50 ms, then
    alternate 2490/2510 every 1 ms for 1000 ms. That second phase sends ≤ 3 'P' messages.
  - `test_seven_bit_dest_sends_on_change_only`: `io_midi2[0] = 0` and a
    raw ramp from 2048 to 3200 over 200 ms. At least 10 'P' messages are
    sent, and no two consecutive ones have the same `v >> 25`.
  - `test_shared_pitch_uses_max`: pads 11 and 14 both on 60, raw 2500 and
    3000. The last 'P' for note 60 equals `pad_pressure32(pad_pressure12(3000, MED))`.
  - `test_channel_mode_max_and_zero`: two pads in Channel mode give 'C'
    messages, and releasing both ends with a 'C' of 0.
  - `test_pressure_off`: no 'P' or 'C' messages.
  - `test_no_pressure_while_capturing`: in peak-window mode there is no 'P' before the 'N'.
  - `test_release_stops_pressure`: after the 'F' for a pitch there is no further 'P' for it.
- [ ] **Step 2: Run `make test`.** Expected: these fail.
- [ ] **Step 3: Implement** pressure in `pe_poll` (and the channel zero in `pe_release` / `pe_release_all`).
- [ ] **Step 4: Run `make test`.** Expected: `0 failures`.
- [ ] **Step 5: Commit** `pads: 12-bit poly and channel pressure with hysteresis and rate cap`.

---

### Task 7: UI — play page, menu, LEDs

**Files:**
- Create: `src/instrument/ui.h`, `src/instrument/ui.c`
- Test: `tests/test_ui.c`

**Interfaces:**
- Consumes: Tasks 1, 2, 5, 6.
- Produces:
  ```c
  #define SETUP_PEEK_MS 300
  void ui_init(uint32_t now);                     /* settings_load, pe_configure, draw the play page */
  void ui_pad(uint8_t index, uint8_t value, uint32_t now);   /* every TYPEPAD event */
  void ui_setup(uint8_t value, uint32_t now);                /* TYPESETUP events */
  void ui_poll(uint32_t now);                     /* pe_poll, Setup LED every 100 ms, settings_poll */
  const struct settings *ui_settings(void);
  void ui_apply_settings(const struct settings *s, uint32_t now); /* for stage 3 PE: validate; on change release notes, reconfigure, redraw, settings_touch */
  uint8_t ui_menu_open(void);
  ```

Behaviour:
- **Play page:** each grid pad is lit by
  `scale_pitch_role(scale_pad_pitch(pad))`. A pad whose pitch is sounding
  is white, and so is every other pad with the same pitch (driven by
  `on_sounding`). Pressing a grid pad calls `pe_press(pad, pitch,
  channel, din_mirror, now)`. Releasing calls `pe_release`. Buttons
  91/92 change the octave ±1, clamped to 0..7. Buttons 93/94 change the
  root −1/+1, wrapping in 0..11. These buttons don't touch held notes, so
  latched notes release correctly. Each change redraws the grid and calls
  `settings_touch`. Buttons 91–94 are lit dim white; the other buttons are off.
- **Setup:** on a press while the menu is closed, the menu opens
  (`pe_release_all()` first) on the Scale page, or on the page last used.
  On a press while open, the menu closes. On release: if this press opened
  the menu and `now − press ≥ SETUP_PEEK_MS`, the menu closes (peek).
  Closing redraws the play page. Pads are not routed to the pad engine
  while the menu is open.
- **Pages:** 95 = Scale, 96 = Options. The selected page's button is bright
  white, the other dim, and 91–94 are off in the menu.
  - Scale page: 81 toggles absolute root. 83–87 = layouts
    CHROM_GUITAR, INKEY_SEQ, INKEY_3RDS, INKEY_4THS, CHROM_4THS. Root
    sharps are 72 C#, 73 D#, 75 F#, 76 G#, 77 A#. Root naturals are 61..67
    = C D E F G A B. Octave is 51..58 = 0..7. Scales are 41..48 = 0..7,
    31..38 = 8..15, 21..28 = 16..23 and 11 = 24.
  - Options page: 81/82/83 = velocity mode PEAK/ZERO/FIXED. 71/72/73 =
    curve SOFT/LINEAR/HARD. 61/62/63 = pressure POLY/CHANNEL/OFF. 51/52/53
    = threshold LOW/MED/HIGH. 41/42 = DIN mirror off/on. 21..28 = channels
    1..8 and 11..18 = channels 9..16.
  - A tap sets the value at once: it is redrawn, `pe_configure` runs for
    play options, and `settings_touch` is called.
- **Colours** (r,g,b, 0..63): ROOT (0,20,63), HIGHLIGHT (8,24,40), SCALE
  (0,4,16), OUT (4,4,4), OFF (0,0,0), sounding (63,63,63). Menu items are
  bright when selected and dim otherwise:
  - root keys green (0,63,0)/(0,8,0);
  - scales blue (0,0,63)/(0,0,8);
  - octave red (63,0,0)/(8,0,0);
  - layouts amber (63,24,0)/(8,3,0);
  - absolute root amber when on, dim when off;
  - option rows white (63,63,63)/(6,6,6).
- **Setup LED:** every 100 ms, green (0,63,0) when
  `io_port_is_midi2(USBSTANDALONE)`, else amber (63,20,0).
- **Save:** `ui_poll` calls `settings_poll(&s, now, pe_notes_active())`.

- [ ] **Step 1: Write the failing tests** (using the `io_stub` LED framebuffer):
  - `test_play_page_colours_default`: pad 11 is ROOT blue, pad 12 is SCALE,
    pad 13 (E, 3rd) is HIGHLIGHT, and button 91 is lit.
  - `test_press_plays_mapped_note`: pressing pad 11 in zero-latency mode
    gives an 'N' for note 60 on channel 0.
  - `test_same_pitch_pads_light_together`: in the default layout, pads 18,
    25 and 32 are all 72 (degree 7). Pressing pad 25 lights all three
    white, and releasing it restores their role colours.
  - `test_octave_arrows_clamp`: 91 pressed 5 times gives octave 7, and 92
    pressed 9 times gives 0.
  - `test_root_arrows_wrap`: 93 from C gives B (11).
  - `test_setup_tap_opens_and_closes`: press then release Setup at +50 ms
    opens the menu; the next press closes it.
  - `test_setup_peek`: press, then release at +400 ms, leaves the menu closed.
  - `test_menu_entry_releases_notes`: a held note gets an 'F' on Setup press.
  - `test_scale_page_select`: in the menu, tapping pad 33 (scale 10) gives
    scale 10 and the pad is bright blue. Tapping 75 gives root F#. Tapping
    84 gives INKEY_SEQ.
  - `test_options_page_select`: 96, then 82 gives ZERO_LATENCY and 13 gives channel 10 (stored 9).
  - `test_pad_held_across_menu_is_silent`: press grid pad 45, then Setup
    (an 'F' is sent), then Setup again (closed), then release pad 45.
    After the 'F' there is no N, F or P.
  - `test_settings_saved_after_quiet`: an octave change, then
    `ui_poll(now + 2000)` with no notes, gives exactly one flash write.
  - `test_apply_settings_rejects_invalid`: scale 30 changes nothing.
  - `test_setup_led_reflects_protocol`: `io_midi2[0]=1` gives green after `ui_poll(+100)`; 0 gives amber.
- [ ] **Step 2: Run `make test`.** Expected: link failure.
- [ ] **Step 3: Implement `ui.c`.** It owns the `struct settings` and
  registers its `on_sounding` callback with `pe_init`. `ui_init` does not
  call `pe_init`; `app.c` does, passing the callback that `ui.h` exports as
  `void ui_on_sounding(uint8_t pitch, uint8_t on)`.
- [ ] **Step 4: Run `make test`.** Expected: `0 failures`.
- [ ] **Step 5: Commit** `instrument: play page, Setup menu, Scale and Options pages`.

---

### Task 8: App glue, simulator, firmware build, checkpoint 2 doc

**Files:**
- Modify: `src/app.c` (rewrite), `tools/simulator.c`, `tools/sim_midi2.c`, `Makefile`
- Create: `docs/checkpoint-2-instrument.md`

**Interfaces:**
- Consumes: everything above.

`src/app.c`:
- `app_init(adc)` does `pe_init(adc, ui_on_sounding)`, `ui_init(g_ms)` and
  `midi2_set_main_loop_hook(main_loop)`, where `main_loop` calls
  `ui_poll(g_ms)`.
- `app_timer_event` does `g_ms++` (a `volatile uint32_t`) and nothing else.
- `app_surface_event`: TYPEPAD goes to `ui_pad`, TYPESETUP to `ui_setup`.
- `app_aftertouch_event` is empty: pressure comes from the raw ADC now.
- `app_midi_event` keeps the USBMIDI ↔ DIN bridge. `app_sysex_event` and
  `app_cable_event` stay empty.

The simulator calls `sim_run_main_loop_hook()` after each timer event, and
`SIM_SOURCES` adds the new sources and `io_hal.c`.

- [ ] **Step 1: Rewrite `app.c` and update the simulator.**
- [ ] **Step 2: Run `make clean && make test && make`.** Expected:
  `0 failures`, the simulator runs, `check_image.sh` passes, and
  `.data` + `.bss` ≤ 13 KiB (`arm-none-eabi-size build/launchpad_pro.elf`).
  Record text/data/bss in the commit message.
- [ ] **Step 3: Write `docs/checkpoint-2-instrument.md`** for the player
  (Windows, MIDI Services console `midi endpoint monitor`). One table row
  each:
  1. The default grid colours, with pad 11 = C3 (60).
  2. The octave arrows and root arrows.
  3. Every layout on the Scale page, and the absolute-root toggle.
  4. Scale selection: Minor Pentatonic shows only 5 pitch classes.
  5. Velocity in Peak-window vs Zero-latency vs Fixed, soft/medium/hard
     hits on two pads, with the curves.
  6. Poly pressure smoothness (values look continuous, not 128 steps).
  7. Channel pressure returns to 0, and Off sends none.
  8. DIN mirror on a DIN monitor.
  9. Channel 10 appears in the monitor.
  10. Settings survive unplugging and replugging.
  11. The menu peek (hold Setup).
  12. Duplicate pitches light together and give one Note Off.
  13. The MIDI 1.0 fallback (a MIDI 1.0 app sees 7-bit velocity and pressure).
  14. The "feel" comparison between the velocity modes.

  It ends with "What to send back": the results table, one monitor
  capture of a pressure sweep, and notes on how the pressure thresholds
  feel (they are tunable constants).
- [ ] **Step 4: Build the deliverable.** Run
  `cp build/launchpad_pro.syx build/launchpad_pro_stage2.syx`.
- [ ] **Step 5: Commit** `app: scale instrument replaces the stage 1 test app; checkpoint 2 doc`.
