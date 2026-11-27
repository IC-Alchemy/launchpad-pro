# Checkpoint 2: the scale instrument, hardware procedure

This is the stage 2 hardware checkpoint. The firmware is now a playable scale
instrument on top of the stage 1 USB MIDI 2.0 transport. You flash it, play it
and run the checks below on Windows, fill in the results table and send it
back with one monitor capture.

Download the firmware file `launchpad_pro_stage2.syx` that you were sent (it
is the `build/launchpad_pro.syx` of this commit). It is not stored in the
repository.

## What the firmware does

- The 8 x 8 grid is a scale keyboard. Default: C major, octave 4, layout
  "In-key 4ths", so the bottom-left pad (pad 11) is C3 (note 60) and the grid
  holds only notes of the scale.
- Pad press: a Note On on the Standalone port (UMP group 0). It is a 16-bit
  velocity in the MIDI 2.0 Protocol and 7-bit (top 7 bits, at least 1) in the
  MIDI 1.0 Protocol. Pad release: Note Off.
- Pad pressure while held: Poly Pressure (default), Channel Pressure or none.
  The value comes from the raw 12-bit pad ADC, so it is 32-bit in the MIDI 2.0
  Protocol and 7-bit otherwise.
- Pads that map to the same pitch light together and share one Note On and
  one Note Off (the Off comes when the last of them is released).
- Settings (root, scale, octave, layout, absolute root, velocity mode, curve,
  pressure mode, pressure threshold, DIN mirror, channel) are kept in flash.
  They are saved once 2 s have passed since the last change and since you
  last pressed or released a grid pad, with no note held (the save stalls
  the device for a few tens of ms, so it waits for a pause in playing).
  Unplugging sooner may lose the last change.
- The "MIDI" port (UMP group 1) is still bridged to the DIN port in both
  directions. The stage 1 MIDI clock and the "Setup pauses receive" behaviour
  are gone.
- The Setup LED shows the USB MIDI state: green = MIDI 2.0 Protocol active,
  amber = anything else (MIDI 1.0 Protocol, or alternate setting 0).

## The user interface

Play page (the menu is closed):

- Select the primary palette with a left round button. The Surface page
  (button 98) also lets you set and preview both palettes.
  Every palette gives each role its own colour (red, green, blue on the 0 to
  63 LED scale); the default, Ocean:
  - root note: blue (0, 16, 63), the palette's own hue, used by no other role;
  - the 5th step of the scale, counting the root as the 1st (G in C major; Bb
    in C Minor Pentatonic): azure (0, 40, 58), a neighbour of the root's hue
    and as bright, so root and fifth read as a pair and stand out from the rest;
  - the 3rd step (E in C major; F in C Minor Pentatonic): dim amber (13, 6, 0),
    a contrasting hue, kept dim;
  - every other note of the scale: faint blue (0, 4, 15);
  - notes outside the scale: faint grey (2, 2, 2) in every palette, only in
    the chromatic layouts;
  - a pad whose note would be above 127: unlit, and it plays nothing;
  - a sounding pad: white (63, 63, 63) in every palette.

  The four palettes, in the order of left buttons 50, 40, 30 and 20.
  The buttons use the root colour, with the active palette brightest.

  | Palette | Root | Fifth | Third | Scale |
  |---|---|---|---|---|
  | Ocean | blue (0, 16, 63) | azure (0, 40, 58) | amber (13, 6, 0) | (0, 4, 15) |
  | Citrus | gold (63, 34, 0) | yellow (63, 56, 0) | blue (0, 7, 26) | (6, 4, 0) |
  | Glacier | cyan (0, 52, 63) | aquamarine (0, 63, 34) | rose (29, 0, 14) | (0, 5, 5) |
  | Contrast | vermilion (63, 16, 0) | amber (63, 40, 0) | sky blue (2, 8, 21) | (10, 3, 0) |

  The same rules hold in each, so a colour means the same thing whatever the
  palette: lightness first, hue second (so the roles still read with colour
  blindness or in bright light). Treating the LED values as linear light, the
  CIE lightness L* is about 21 for out-of-scale, 30 for scale notes, 37 to 42
  for the third, and 52 to 94 for the root and fifth, at least 12 above the
  third. Contrast keeps to the orange/blue axis (Okabe-Ito), which stays apart
  under every common colour blindness. Chromatic and All Degrees have no third
  or fifth: only the root stands out.
- Buttons 91 and 92 are octave up and down, 93 and 94 are root down and up
  (one semitone, wrapping B to C). They are dim white. Octave is 0 to 7. Held
  notes keep their pitch when you change these.
- The bottom-left pad is `12 x (octave + 1) + root`, or with "absolute root"
  on, `12 x (octave + 1)` (the grid then starts on C whatever the root).

Setup button: tap it to open the menu, tap it again to close. If you hold it
for 300 ms or more and then release, it is a peek: the menu closes on release.
Opening the menu releases every note. Menu LEDs: the selected value of each
row is bright, the others dim. The page buttons (95 to 98 and 89) are bright
white for the active page and dim white otherwise.

Scale page (button 95, the default menu page):

| Row | Pads | What | Colour |
|---|---|---|---|
| 8 | 81 | Absolute root toggle | amber |
| 8 | 83 to 87 | Layout: Chromatic guitar, In-key sequential, In-key 3rds, In-key 4ths, Chromatic 4ths | amber |
| 7 | 72, 73, 75, 76, 77 | Root C#, D#, F#, G#, A# | green |
| 6 | 61 to 67 | Root C, D, E, F, G, A, B | green |
| 5 | 51 to 58 | Octave 0 to 7 | red |
| 4, 3, 2, 1 | 41 to 48, 31 to 38, 21 to 28, 11 to 17 | The scales the current tuning offers, in order. In the twelve-note tunings these are scales 1 to 31: Major, Minor, Dorian, Mixolydian, Lydian, Phrygian, Locrian, Diminished; Whole-half, Whole Tone, Minor Blues, Minor Pentatonic, Major Pentatonic, Harmonic Minor, Melodic Minor, Super Locrian; Bhairav, Hungarian Minor, Minor Gypsy, Hirojoshi, In-Sen, Iwato, Kumoi, Pelog; Spanish, Phrygian Dominant, Lydian Dominant, Marwa Thaat, Poorvi Thaat, Todi Thaat, Chromatic. Other tunings show their own few scales from pad 41 on. | blue |

Surface page (button 98). The second half's settings are on top and the
primary half's below. The left round buttons work here and on the play page.

| Row | Pads | What | Colour |
|---|---|---|---|
| 8 | 81 to 84 | Second palette: Ocean, Citrus, Glacier, Contrast | root colour, brightest when selected |
| 7 | 71 to 78 | Upper channel 1 to 8 | soft green |
| 6 | 61 to 68 | Upper channel 9 to 16 | soft green |
| 5 | 51 to 57 | Preview of the upper palette: a major scale from the root (root, 2, 3rd, 4, 5th, 6, 7) | the palette |
| 4 | 41 to 47 | Preview of the lower palette | the palette |
| 3 | 31 to 38 | Lower channel 1 to 8 (the Options page's MIDI channel) | soft green |
| 2 | 21 to 28 | Lower channel 9 to 16 | soft green |
| 1 | 11 to 14 | Primary palette, same order as row 8 | as row 8 |

Use left button 80 for an X split between rows 4 and 5, button 70 for a
Y split between columns 4 and 5, and button 60 for the full grid. The second
palette and channel apply to rows 5 to 8 in X mode or columns 5 to 8 in Y mode
(Contrast on channel 2 by default). Changing the split releases any held notes.
On two channels a pitch in both halves is two notes, and each half lights only
its own sounding pads. With both halves on one channel they share notes.
Channel pressure is sent per channel. DIN mirror, velocity, pressure and tuning
apply to both halves.

Tuning page (button 97):

| Row | Pads | What | Colour |
|---|---|---|---|
| 8 | 81 to 88 | 12-EDO, 24, 19, 31, 22, 17, 15, 10-EDO | cyan |
| 7 | 71 to 74 | 7-EDO, 5-EDO, 41-EDO, 53-EDO | cyan |
| 7 | 75 to 78 | Just Intonation, Just: Harmonic 7, Pythagorean, Overtone 16-31 | yellow |
| 6 | 61, 62 | Undertone 32-17, Partch 43-Tone | yellow |
| 6 | 63 to 66 | 1/4-Comma Meantone, Werckmeister III, Kirnberger III, Vallotti | orange |
| 6 | 67, 68 | 22 Shruti, 12 Svara JI | magenta |
| 5 | 51 | Pythagorean Svara | magenta |
| 5 | 52 to 55 | Bohlen-Pierce 13, Carlos Alpha, Carlos Beta, Carlos Gamma | violet |
| 2 | 21, 22, 27, 28 | A4 −1 Hz, −0.1 Hz, +0.1 Hz, +1 Hz (415.0 to 466.0) | dim white |
| 1 | 13 to 16 | A4 = 415.0 (Baroque), 432.0, 440.0, 442.0 Hz; the one in use is bright | white |

Choosing a tuning keeps the scale if the tuning offers it, and otherwise moves
to the tuning's first scale (Bhairav for the Indian twelve-note tunings).

Options page (button 96), mostly white with a light hue per section (selected
bright, others dim): velocity rows 7 and 8 warm, aftertouch rows 3, 5 and 6 cool
blue, MIDI routing rows 1, 2 and 4 soft green; the page buttons stay pure white:

| Row | Pads | What |
|---|---|---|
| 5 | 51, 52, 53, 54 | Pressure threshold: Low, Med, High, Instant (aftertouch from first contact) |
| 6 | 61, 62, 63 | Pressure: Poly, Channel, Off |
| 7 | 71, 72, 73 | Velocity curve: Soft, Linear, Hard |
| 8 | 81, 82, 83, 84 | Velocity mode: Peak-window, Zero-latency, Two-stage, Fixed |
| 3 | 31, 32, 33, 34 | Aftertouch smoothing: None, Light, Medium, Heavy |
| 4 | 41, 42 | DIN mirror: Off, On (also sends the pad notes and pressure to DIN) |
| 2 | 21 to 28 | MIDI channel 1 to 8 |
| 1 | 11 to 18 | MIDI channel 9 to 16 |

Aftertouch smoothing (row 3) is a low-pass on each held pad's pressure before
it is sent, to cut jitter, for poly and channel pressure alike: None barely
filters (about 6 ms to settle), Light is the previous behaviour and the
default (about 30 ms), Medium about 125 ms, Heavy about 250 ms to settle to
within 2% of a step. More smoothing is steadier but follows quick pressure
changes later. It is saved with the other settings (settings record version 2;
a version 1 record from the earlier image still loads, with Light).

Velocity modes: Peak-window waits 3 ms after the strike and sends the highest
raw 12-bit reading in that window (the most accurate, full sensor resolution,
at 3 ms latency). Zero-latency sends the Note On at once
from the first reading (fastest, less accurate). Fixed always sends full
velocity. Two-stage (pad 83) does its own press detection from the raw pad
reading and takes the velocity from how fast the pad rises between a low and a
high threshold: a slow, gentle press is soft, a quick one loud. In Two-stage, after the
timeout a touch sounds only if it rises above raw 120 and stays at or above 120
for 80 ms; a short tap that never reaches 480 and falls back is silent. A touch
that peaks between raw 120 and about 452 and is held sounds only in Two-stage
(softly, after up to 80 ms); a brief brush below raw 120 is silent. A touch peaking between about 452 and
480 sounds at once in Zero-latency but, if held, only after the timeout in Two-stage
(its high threshold, 480, is above the library trigger of about 452 raw).
It may be weaker at the loud end.

MIDI-CI page (button 89, the uppermost right-side round button): opening the
page starts a three-second USB UMP discovery scan. Pad 11 rescans and is bright
cyan while scanning. Pads 21 to 28 show peers: amber while follow-up queries
are pending, green once complete for peers that advertise Property Exchange,
and cyan once complete for other peers.

## Two-stage and the tuning logs

Select it: Setup, Options page (button 96), row 8, third pad (pad 83). In
Two-stage the note starts when the pad has risen past the high threshold (or
after 80 ms above the low one), so expect a brief brush to stay silent
and a slow press to sound a little late. The library's press threshold is also
lowered in this image, so Zero-latency and Peak-window play softer than at
checkpoint 2's first build. The thresholds are first guesses. I tune them from
your logs, so please capture these in the Windows MIDI Services monitor, in
each of the four velocity modes (Linear curve, a few different pads):

1. The softest notes you can possibly play, a few on each of several pads.
2. A sweep from soft to hard in even steps, a few notes per step.
3. Slow, firm pushes (press down over about half a second).
4. A light rest on the pad, then a strike.
5. Hard hits, watching the neighbouring pads for ghost notes (the monitor
   should show notes only for the pads you hit).

Export each monitor log as text, with the 16-bit velocities (not the 7-bit
ones), and add a line of feel notes per mode: which notes did not sound,
which were too loud or soft, any late or doubled notes.

## First guesses to be tuned

These are my first guesses, not measured values. I could not try them on the
hardware, so I need your feel for them:

- Pressure thresholds, in raw 12-bit ADC counts: Low 1792, Med 2048, High
  2304. Pressure reads 0 at or below the threshold and reaches full at 3076.
  Instant starts where velocity sensing starts, (trigger + 80) x 4 from the
  pad calibration (400 with the app's trigger of 20), so aftertouch follows
  the finger from first contact with no dead zone. It never starts above Low.
- Noise filter and hysteresis: a MIDI 2.0 destination gets a new pressure
  value only when it moved by 32 counts of 4096 (`PRESSURE_HYST`). A 7-bit
  destination (DIN, and USB in MIDI 1.0) gets a new value only when the
  pressure left its last 7-bit step by more than half a step, 16 counts of
  4096 (`PRESSURE_HYST7`), so a pad resting on a step boundary does not
  toggle. Every destination gets at most one message per pitch every 5 ms.
- In Zero-latency mode the pressure filter is seeded at the strike, so the
  first pressure messages right after the hit may start high and then fall.

All of these are constants in the source and easy to change once I know what
you feel.

Two-stage defaults are untested on hardware. A held note ends only after 3
readings below raw 80 (`TS_REL`), so a pad that rests between raw 80 and 120
may hang its note, and a pad that rests at or above raw 120 (`TS_T1`) never
plays in this mode. If a note hangs, opening Setup releases it. Please report
which pads hang or stay silent in Two-stage, and include a short log with the
pads untouched and with them resting.

## Notes on the console commands

As in checkpoint 1: I could not run the Windows MIDI Services console, so the
syntax is my best understanding. If a command is rejected, run `midi --help`
or `midi endpoint --help` and use the equivalent, and report what differed.
Do not guess at output: paste what you actually see.

## Your setup

Same as checkpoint 1: Windows 11 build 10.0.28020 (26H1), Windows MIDI
Services Console SDK Release Candidate 4 (1.0.17-rc.4.25), monitor with
`midi endpoint monitor`. For the DIN checks you need a DIN MIDI monitor or
sound module on the DIN MIDI Out.

Checkpoint 1 left steps 5b, 6a and 6c untested. They are not part of this
checkpoint: they move to the stage 4 final pass.

## 1. Flashing, and recovery

Follow section 1 of `docs/checkpoint-1-transport.md` (hold Setup while
plugging in, send the file, wait for the reboot), using
`launchpad_pro_stage2.syx`. The recovery steps there apply unchanged.

Result: flashed OK? (yes/no), notes.

## 2. Default grid colours

After the reboot, with the monitor running: look at the grid. Expected: C
major in the Ocean palette, the C pads blue, the G pads azure, the E pads dim
amber, the other pads (D, F, A, B) faint blue; in this layout every pad is in
scale. (A unit with settings from an older image keeps them and starts in
Ocean with split off.) Press pad 11: expected Note On for note 60 (C3 in the numbering where
60 is C3; the monitor may call it C4 or C3), white pad while held, Note Off on
release. Check the top-right pad is higher in pitch than the bottom-left.

## 3. Octave and root arrows

Press button 91, then 92, several times: the notes of pad 11 move by 12 and
stop at octave 0 and 7. At octaves 6 and 7 the top pads would be above note
127: they stay unlit and send nothing. That is by design, not dead pads. Press 94 and 93: pad 11 moves by one semitone per
press, wrapping B to C, and the grid recolours to the new key. Hold a pad
while pressing an arrow: the held note must keep its pitch and stop on
release.

## 4. Layouts and absolute root

Tap Setup (the menu opens on the Scale page; press 95 if not). Press each
layout pad 83 to 87 in turn, close the menu, and check the grid pitches
change as named (Chromatic guitar, In-key sequential, In-key 3rds, In-key
4ths, Chromatic 4ths). Then turn on absolute root (pad 81), choose root G, and
check that pad 11 is still C (note 60) and the root pads are bright blue
wherever G is. Turn it off again: pad 11 follows the root.

## 5. Scale selection

Pick Minor Pentatonic (pad 34). Close the menu. Expected: only 5 pitch classes
are in scale. Play a few pads, record the distinct pitch classes seen in the
monitor (expected C, Eb, F, G, Bb for root C). Try one more scale, for
example Hirojoshi (pad 24).

## 6. Velocity modes and curves

Open the menu, Options page (button 96). Pick Peak-window (pad 81), Linear
(pad 72). On two pads (for example 11 and 88) hit soft, medium and hard, a few
times each, and record the typical 16-bit velocity. Repeat with Zero-latency
(pad 82), Two-stage (pad 83), then Fixed (pad 84; expect 0xFFFF every time). Then repeat
Peak-window with Soft (pad 71) and Hard (pad 73) curves. Expected: soft
curve gives higher values for the same hit, hard gives lower values, and every
pad gives a similar range.

## 7. Poly pressure smoothness

Pressure Poly (pad 61), threshold Med (pad 52). Press a pad and slowly
increase and decrease the pressure, then do a slow sweep. Expected: Poly
Pressure values (32-bit) that look continuous and finely stepped, not 128
steps, rising from near zero to near full, with no flood of messages while
you hold a steady pressure. Capture one sweep for "What to send back".
Also try Low (pad 51), High (pad 53) and Instant (pad 54): with Instant,
aftertouch should move as soon as the pad is down, before you lean on it.

## 8. Channel pressure

Pressure Channel (pad 62). Press two pads and vary the pressure: Channel
Pressure follows the harder pad. Release all pads: a final Channel Pressure of
0 is expected. Then Off (pad 63): no pressure messages at all.

## 9. DIN mirror

Turn DIN mirror on (pad 42). Connect a DIN monitor. Expected: pad notes and
pressure appear on the DIN MIDI Out as MIDI 1.0 messages (7-bit), as well as
on USB. Steady hold: hold one pad with steady pressure for 5 s. The DIN
monitor should show only an occasional pressure message, not a stream (a
stream here would overload the DIN port). Turn it off (pad 41): DIN goes
quiet. Check the USB "MIDI" port to
DIN bridge still works (as in checkpoint 1 section 4).

## 10. Channel 10

Select channel 10 (pad 12, row 1). Close the menu and play. Expected: the
monitor shows channel 10 on notes and pressure. Return to channel 1 (pad 21).

## 11. Settings survive unplugging

Change several settings (scale, root, octave, layout, velocity mode,
channel). Wait at least 5 s without touching the pads. Unplug and replug
USB. Expected: all of them come back. Then change one more and unplug
within 1 s: record whether it was lost (expected possible, and not a fault).

## 12. Menu peek

Hold Setup for longer than 300 ms: the menu shows while held. Release it: the
play page returns. A short tap opens the menu and keeps it open until the
next tap.

Held pads and the menu, two cases:

- Hold a pad (its Note On shows), open the menu, release the pad, close the
  menu. Expected: exactly one Note Off, at the moment the menu opens, and
  nothing when you release the pad or close the menu.
- Hold a pad, open the menu, close the menu, then release the pad.
  Expected: again exactly one Note Off when the menu opens, and nothing at
  the close or the release (no new note, no pressure while still held).

## 13. Duplicate pitches

Choose the Chromatic 4ths or Chromatic guitar layout so the same pitch
appears on two pads. Press both pads: both light white together. Expected: one
Note On when the first is pressed, no second Note On for the second, and a
single Note Off when the last one is released. Poly pressure is the larger
of the two pads.

## 14. MIDI 1.0 fallback

As in checkpoint 1 section 7 (switch the driver to the in-box one, Setup LED
amber: not red now). Expected: velocity and pressure are 7-bit and the
instrument still plays. Steady hold: hold one pad with steady pressure for
5 s, once with DIN mirror off and once with it on. The USB monitor (and the
DIN monitor) should show only an occasional pressure message, not a stream.
Also try the MIDI 1.0 Protocol request from checkpoint 1 section 5 to put the
MIDI 2.0 connection into MIDI 1.0 Protocol.

If you hold a pad while the connection changes (driver switch, or the
protocol request), the connection ends its note: the pad then stays
silent, its release sends nothing and the next press plays normally.

## 15. Feel

Play a few passages on each velocity mode. Which one feels best? Does
Zero-latency feel clearly tighter than Peak-window? Does it start too loud
(the first pressure messages start high and then fall)? Does Peak-window's
3 ms feel like delay at all? How does the pressure onset feel at Low, Med and
High? Does resting pressure on a held pad jitter?

## 16. Results table

Fill in "Result" and "Notes". "Expected" is what I predict.

| # | Check | Expected (verify) | Result | Notes |
|---|---|---|---|---|
| 1 | Flash the stage 2 image | Flashes and reboots | | |
| 2 | Default grid colours, pad 11 = C3 (60) | Blue roots, azure G, dim amber E, faint blue others; pad 11 sends 60 | | |
| 3 | Octave and root arrows | Octave 0 to 7, root wraps B to C, held notes keep pitch; top pads unlit and silent above note 127 | | |
| 4 | Every layout, absolute root | Five layouts as named, pad 11 stays C with absolute root | | |
| 5 | Minor Pentatonic | Only 5 pitch classes | | |
| 6 | Velocity: peak-window, zero-latency, two-stage, fixed, curves | Soft, medium, hard separate on both pads; Fixed = 0xFFFF | | |
| 7 | Poly pressure smoothness | Continuous, not 128 steps | | |
| 7b | Aftertouch smoothing (Options row 3) | Heavy: slow smooth swell (about 250 ms to settle), None: fast but jittery; Light is the default; survives unplug | | |
| 8 | Channel pressure and Off | Returns to 0 on release; Off sends none | | |
| 9 | DIN mirror | Notes and pressure on DIN, 7-bit; steady hold sends only occasional pressure | | |
| 10 | Channel 10 | Seen in the monitor | | |
| 11 | Settings survive unplug | All restored | | |
| 12 | Menu peek | Hold shows, release closes; held pad: one Note Off when the menu opens, then nothing | | |
| 13 | Duplicate pitches | Light together, one Note On, one Note Off | | |
| 14 | MIDI 1.0 fallback | 7-bit velocity and pressure; steady hold sends only occasional pressure (USB and DIN) | | |
| 15 | Feel of the velocity modes, Two-stage included (see Two-stage and the tuning logs) | (describe) | | |
| 16 | Tuning logs captured per mode; Two-stage hangs or silent pads reported | Logs attached | | |

## 17. What to send back

- The filled results table (section 16).
- One monitor capture (text copy) of a pressure sweep (section 7).
- Notes on how the pressure thresholds feel (Low, Med, High), how the pressure
  onset feels, and whether resting pressure jitters. Also whether the first
  pressure messages in Zero-latency mode start high and then fall.
- Any console output from the `midi` commands, including any command whose
  syntax differed from this document.
- Anything odd: stuck notes, wrong colours, lost settings, hangs.

## 18. Intonation (stage 2c)

Use a MIDI 2.0 monitor that shows Note On attributes (MIDI2Sound's Live MIDI,
or the Windows MIDI Services monitor), with the Launchpad in the MIDI 2.0
Protocol (green Setup LED).

1. **Default stays plain.** With 12-EDO and A4 = 440.0 (pad 15 bright on the
   Tuning page), play a few pads. Expected: no attribute on any Note On.
2. **A4 only.** Press pad 13 (415.0 Hz). Play pad 11 (C, note 60). Expected:
   note 60 with attribute type 3, Pitch 7.9 0x75FA (58.99 semitones, about a semitone low).
   Press pad 15 to go back to 440.0.
3. **24-EDO.** Press pad 82. Close the menu. Expected: the Scale page now shows
   five scales on row 4 (Maqam Rast first). With All Degrees (pad 45) and the
   In-key sequential layout (pad 84), root C, octave 4, pad 11 sends note 60 with pitch 0x7800 and pad 12
   sends note 61 with pitch 0x7900 (60.5 semitones).
4. **A just tuning.** Press pad 75 (Just Intonation). Play E (pad 13 in Major,
   In-key sequential, root C). Expected: note 64 with pitch 0x7FBA
   (63.86 semitones, 14 cents flat).
5. **Bohlen-Pierce.** Press pad 52. Expected: the scale moves to Bohlen-Pierce
   Lambda. In-key sequential, root C: pad 11 sends note 60 (0x7800), pad 12
   sends note 62 with pitch 0x7DDA (62.93 semitones, two Bohlen-Pierce steps of
   about 146 cents).
6. **Saved.** Unplug and plug in again. Expected: the tuning and A4 are kept.
7. **MIDI 1.0 and DIN.** With DIN mirror on, or in the MIDI 1.0 Protocol,
   expected: plain note numbers and no attribute.
8. **From the PC.** In Alembic's Intonation page (or a PE editor), pick a
   tuning. Expected: the Launchpad's Tuning page follows within a second, and
   a change on the Launchpad shows in Alembic.

| # | Check | Expected | Result | Notes |
|---|---|---|---|---|
| 18.1 | 12-EDO at 440 | No attribute | | |
| 18.2 | A4 415.0 | Attribute 0x75FA on note 60 | | |
| 18.3 | 24-EDO | Five scales offered; 60 → 0x7800, 61 → 0x7900 | | |
| 18.4 | Just Intonation | E = note 64, 0x7FBA | | |
| 18.5 | Bohlen-Pierce | Pad 12 = note 62, 0x7DDA | | |
| 18.6 | Survives unplug | Tuning and A4 restored | | |
| 18.7 | MIDI 1.0 and DIN | Plain notes | | |
| 18.8 | Two-way with Alembic | Changes follow both ways | | |

## 19. Palettes and split

1. **Every palette.** On the play page, press left buttons 50, 40, 30 and 20
   in turn. The chosen button should brighten in its palette's root colour.
   For each palette, the roots, fifths, thirds and other scale notes should be
   four clearly different colours. Root and fifth are the bright pair and
   differ in hue, the third is dim and in a contrasting hue, and the scale
   notes are faint. Note any palette where the root and fifth look the same
   from playing distance, where the third looks like a scale note, or where
   something looks white.
2. **Out of scale.** Choose the Chromatic 4ths layout (Scale page pad 87).
   Expected in each palette: out-of-scale pads a faint grey, visibly dimmer
   than the scale notes and not tinted.
3. **Previews.** On the Surface page, rows 5 and 4 show the upper and lower
   palettes as root, 2, 3rd, 4, 5th, 6, 7. Expected: they match the play page.
4. **Split modes.** With Ocean selected and the second palette set to
   Contrast, press left button 80. Expect Ocean below and Contrast above.
   Press 70. Expect Ocean left and Contrast right. Press 60. Expect Ocean
   throughout. The selected mode button is bright amber. Select X again for
   the next check.
5. **Two channels.** Play pad 44 and then pad 51 (both note 81). Expected: a
   Note On on channel 1, then a second one on channel 2. Only the pressed pads
   light white. Release them: a Note Off on each channel.
6. **Upper channel.** On the Surface page press pad 63 (upper channel 11) and
   pad 34 (lower channel 4). Expected: rows 1 to 4 play on channel 4, rows 5 to
   8 on channel 11. With Channel pressure (Options pad 62), pressure from each
   half goes to its own channel, and releasing one half's last pad sends that
   channel a 0.
7. **One channel.** Set the upper channel to the lower one. Expected: pads 44
   and 51 together send one Note On, as without split.
8. **Saved.** Unplug and plug in again. Expected: palettes, split and both
   channels are kept.
9. **From the PC.** In a PE editor (MIDI 2.0 Workbench), read
   `X-SurfaceSettings`, Set `{"splitMode":"y","upperPalette":"glacier"}` and
   subscribe. Expected: the grid follows at once, a change on the Surface page
   shows in the editor, and `ChannelList` lists "Standalone Left" and
   "Standalone Right" with their channels.

| # | Check | Expected | Result | Notes |
|---|---|---|---|---|
| 19.1 | Four palettes | Root, fifth, third and scale apart in each; nothing white | | |
| 19.2 | Out of scale | Faint grey, dimmer than scale notes | | |
| 19.3 | Previews | Match the play page | | |
| 19.4 | X, Y and Off | Horizontal seam, vertical seam, full grid | | |
| 19.5 | Same pitch, two channels | Two notes, own LEDs | | |
| 19.6 | Upper channel, channel pressure | Per half; 0 per channel on release | | |
| 19.7 | One channel | Shared note | | |
| 19.8 | Survives unplug | All restored | | |
| 19.9 | X-SurfaceSettings | Get, Set, notify, ChannelList | | |
