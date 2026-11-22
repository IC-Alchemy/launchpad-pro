# Checkpoint 1b: velocity measurement and feel test, hardware procedure

This checkpoint has two rounds. Round 1 measures the raw pad signal with a
measurement firmware (`PADSCOPE=1`) so the Two-stage velocity constants can be
set from real data. Round 2 plays the tuned default firmware in both velocity
modes and records how it feels.

The point of the project is dynamic playing, and extremely soft playing
matters most. Three modes (Zero-latency, Peak-window, Two-stage) will have
three different feels, each with its own quirks. That is intended.

Firmware files are not stored in the repository. The builder sends you the
`build/launchpad_pro.syx` of each round.

## Flashing

Same as section 1 of `docs/checkpoint-1-transport.md`: unplug, hold Setup
while plugging in USB, send the `.syx` file to the bootloader, wait for the
unit to reboot. The Makefile tracks the `PADSCOPE` value and rebuilds the firmware objects
when it changes, so switching between the `PADSCOPE=1` image and the default
image needs no `make clean` (running it is harmless).

## What the measurement build does (round 1)

- It samples the raw 12-bit pad ADC every 1 ms (values 0 to 4095).
- A pad strike is captured when a pad's raw reading rises through 200. The
  capture holds 8 ms before the trigger and 120 ms from the trigger on, for
  the struck pad and for the maximum of its four grid neighbours (crosstalk).
- After recording, it waits until the pad reads below 100 for 50 ms, or gives
  up after 1000 ms (a pad held down), then sends the capture.
- Each capture is two SysEx messages on the Standalone port, sent at least
  20 ms apart. Wait for both before the next strike; a new strike is not
  captured while a dump is pending or being sent.
- Pressing Setup sends an idle dump: the min and max raw reading of every pad
  since the last dump (only readings below 400 count), also two SysEx
  messages. The statistics restart after each dump.
- The SysEx framing is `F0 00 20 29 7D <type> <part> <payload> F7`.
- The PADSCOPE image sends no notes at all: the pads are silent and the only
  output is the SysEx dumps (and the Setup LED/clock traffic of the transport).
- In this build Setup does nothing else (no mode switch, no RX pause).

A strike that stays below 200 raw is not captured. If a feather-light strike
gives no capture, write down which pad and how it was played, and carry on.

## Capturing the SysEx

How you capture SysEx from the device on your host is up to you. Any MIDI
monitor that shows the raw bytes in hex works (for example the Windows MIDI
Services console, or MIDI-OX, listening to the Standalone port of the
Launchpad Pro endpoint). Save the text as one hex SysEx message per line,
for example:

    F0 00 20 29 7D 01 00 0B 00 00 ... F7

Rules for the files:

- One file per capture or dump, named for what it is (for example
  `pad44_soft_2.txt`, `idle.txt`).
- Every capture file has exactly two lines (type 1 part 0 and part 1). Every
  idle dump file has exactly two lines (type 2 part 0 and part 1).
- Spaces between bytes are optional. Text after `#` on a line is
  ignored by the decoder.

To decode one file:

    python3 tools/padscope_decode.py < pad44_soft_2.txt > pad44_soft_2.csv

A capture decodes to a header line (`# capture pad=44 stale_ticks=N
samples=128`) and CSV `t_ms,pad,neighbour_max`, t = -8 to 119 ms with 0 the
trigger sample. An idle dump decodes to a table `channel,pad,min,max`; `-`
means that pad never read below 400.

## Round 1: measure (PADSCOPE=1 image)

1. Flash the `PADSCOPE=1` image. Leave the unit untouched on the table for
   10 seconds, then press Setup once and save the idle dump (`idle.txt`).
2. For each of pads 11, 44 and 88, strike and save one capture file per
   strike:
   - 5 feather-light strikes (the softest touch you can still make register),
   - 5 soft strikes,
   - 5 medium strikes,
   - 5 hard strikes,
   - 3 slow firm pushes (press down firmly over about half a second).
   That is 23 captures per pad, 69 in all, plus the idle dump. Wait for both
   messages of a capture to arrive before the next strike. If you hit a
   neighbour by accident, keep the capture and note it in the file name.
3. Decode each file with `tools/padscope_decode.py` as above and send the CSVs
   (and the raw `.txt` files) back.

Optional: repeat the idle dump after playing for a while (the unit warms up),
and say so.

## Round 1b: builder analysis (not for the player)

The builder works from the CSVs. Units matter here: `TS_*` constants and the
padscope data are 12-bit raw (0 to 4095); `PAD_LIB_THRESHOLD` is 10-bit.

1. Noise floor = the highest idle max over all pads (12-bit).
2. Crosstalk = the highest neighbour reading during hard hits (12-bit).
3. The builder sets `TS_REL` above both, with margin. `TS_T1` = `TS_REL` + 40.
   `TS_T2` is set low enough that the feather-light captures reach it.
   `TS_TIMEOUT_MS` comes from the slowest soft press T1-to-T2 time. The order
   `TS_REL < TS_T1 < TS_T2` must hold. Defaults before tuning (12-bit):
   `TS_T1` 120, `TS_T2` 480, `TS_REL` 80, `TS_TIMEOUT_MS` 80,
   `TS_REL_TICKS` 3 (in `src/pads/twostage.h`).
4. Check `PAD_LIB_THRESHOLD` (`src/platform/pads.h`, now 20, 10-bit). With
   threshold 20 the library trigger is about 113 in 10-bit, which is about 452
   in 12-bit. That level must clear the 12-bit noise floor and crosstalk from
   steps 1 and 2; if it does not, raise `PAD_LIB_THRESHOLD`.

## Round 2: feel test (default image, tuned constants)

Flash the default image the builder sends . Setup LED:
green = MIDI 2.0 active. Open a MIDI monitor on the Standalone port that shows
the raw 16-bit velocity values.

- Hold Setup and press pad 11 for Zero-latency, or pad 12 for Two-stage. The
  selected pad is bright and the other dim while Setup is held. Setup plus a
  pad only selects a mode and does not sound a note. Two-stage is the default
  after power-up.
- Holding Setup also pauses receiving of UMP from the host (as in checkpoint
  1), so release Setup before playing.
- Pads send Note On and Note Off on the Standalone port, channel 1, note =
  pad index (pad 11 = note 11).

For each mode (Zero-latency and Two-stage), on several pads (at least 11, 44
and 88):

1. Softest playable note: play as softly as you can and still get a note.
   Record the lowest velocity value seen on the monitor, and how reliable it
   is (out of 10 tries, how many sound).
2. Ghost notes: hit pads hard and watch the neighbours. Record any note that
   sounds on a pad you did not hit.
3. Stuck notes: play fast passages, slow pushes, and switch mode while
   holding a pad. Record any note that does not stop (Note On with no Note
   Off).
4. Feel: describe the soft end, the loud end, how even the steps are, any
   delay. A short free-text note is enough.

### Results table

Fill in the Result and Notes columns. Use "n/a" for anything not tested.

| # | Check | Expected | Result | Notes |
|---|---|---|---|---|
| 1a | PADSCOPE image: idle dump after 10 s untouched | Two messages, idle max low on all pads | | |
| 1b | PADSCOPE image: captures for pads 11, 44, 88 | Two messages each, all 23 strikes per pad | | |
| 1c | Feather-light strikes captured | Reach raw 200 and are captured; else note which | | |
| 2a | Default image, Setup + pad 12 (Two-stage) | Pad 12 bright, 11 dim, no note sounds | | |
| 2b | Default image, Setup + pad 11 (Zero-latency) | Pad 11 bright, 12 dim, no note sounds | | |
| 2c | Mode after power-up | Two-stage | | |
| 3a | Two-stage: softest playable velocity | Low value, note sounds reliably (record value and hit rate) | | |
| 3b | Zero-latency: softest playable velocity | (record value and hit rate) | | |
| 3c | Two-stage: loud end | Reaches high velocity; may be weaker than Zero-latency (accepted) | | |
| 3d | Two-stage: ghost notes on neighbours | None | | |
| 3e | Zero-latency: ghost notes on neighbours | None | | |
| 3f | Stuck notes, either mode | None | | |
| 3g | Mode switch while holding a pad | Note Off sent, no stuck note, no extra Note On | | |
| 3h | Slow firm push, Two-stage | Sounds once, soft to medium velocity | | |
| 3i | Brush a pad lightly, Two-stage | No note | | |
| 3j | Feel notes, Zero-latency | Free text | | |
| 3k | Feel notes, Two-stage | Free text | | |

## What to send back

- The idle dump and capture files from round 1 (raw `.txt` and decoded
  `.csv`), with pad and strike type in the file names.
- The filled results table from round 2.
- Anything odd: stuck notes, wrong LED colour, hangs, resets, strikes that
  gave no capture or no note.
