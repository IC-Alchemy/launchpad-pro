# Launchpad Pro MIDI 2.0

Custom firmware that turns a Novation Launchpad Pro (original, 2015 model) into
a **USB MIDI 2.0 scale instrument**. Pad velocity goes out as 16-bit values and
pad pressure as 32-bit values, read from the pads' raw 12-bit sensors, instead
of being squeezed into MIDI 1.0's 7 bits. The target host is Windows with
Windows MIDI Services. The device still works as a plain two-port USB MIDI 1.0
device on hosts without MIDI 2.0 support.

Built on Novation's [open Launchpad Pro firmware](https://github.com/dvhdr/launchpad-pro)
(BSD licence, see `LICENSE`). The full design is in
`docs/superpowers/specs/2026-11-21-midi2-scale-instrument-design.md`.

## Where things stand

| Stage | What | Status |
|---|---|---|
| 1 | USB MIDI 2.0 transport: new descriptors (alt 0 MIDI 1.0, alt 1 UMP), Group Terminal Blocks, UMP stream discovery, DIN bridge | **Done, tested on hardware.** Results in `docs/checkpoint-1-results.md`. Three checks (5b, 6a, 6c) were deferred to stage 4. |
| 2 | Scale instrument: scale engine, five layouts, 25 scales, velocity modes and curves, poly/channel pressure from the raw ADC, Setup menu, settings saved to flash | **Done and host-tested. Hardware checkpoint pending.** Procedure in `docs/checkpoint-2-instrument.md`. |
| 2b | Two-stage velocity (a fourth velocity mode) and a lower library press threshold | **Done and host-tested.** The thresholds are first guesses, to be tuned from the player's MIDI logs. |
| 2c | Intonation: Pico2Seq's 29 tunings (equal, just, historical temperaments, Indian, xenharmonic), 61 scales, Tuning page, A4 reference, MIDI 2.0 pitch attribute on notes | **Done and host-tested.** Hardware check pending (section 18 of `docs/checkpoint-2-instrument.md`). |
| 2d | Four colour palettes (root, 3rd and 5th each in their own colour) and the split surface (two halves, two palettes, two MIDI channels), all in Property Exchange too | **Done and host-tested.** Hardware check pending (section 19 of `docs/checkpoint-2-instrument.md`). |
| 3 | MIDI-CI with Property Exchange (DAW/editor discovery, edit settings from the PC) | **Discovery, Endpoint Inquiry and Property Exchange (Get, Set, Subscribe) implemented and host-tested.** Hardware check with MIDI2Sound pending. |
| 4 | Final hardware pass, including MIDI-CI and the deferred stage 1 checks | Not started. |

The app has 101 KB of flash and 20 KB of RAM. `make` prints the exact flash margin.

## What the firmware does now

- **Play page.** The 8×8 grid is a scale keyboard. Default: C major, octave 4,
  layout "In-key 4ths", bottom-left pad = note 60. Buttons 91/92 change the
  octave, 93/94 change the root.
- **Left round buttons.** From the top, 80 selects X, 70 selects Y, and 60
  turns the split off. Buttons 50, 40, 30, and 20 select Ocean (blue), Citrus
  (gold), Glacier (cyan), and Contrast (vermilion). The selected mode and
  palette are brighter. The bottom button (10) is unused.
- **Palettes.** Ocean is the default. Each colour role works the same way
  across all four palettes. Root and fifth are the bright
  pair: the root in the palette's own hue (no other role uses it), the fifth
  in a neighbouring hue. The third is a dim contrasting hue. Other scale notes
  are a faint tint of the root's hue, out-of-scale notes a faint grey. A
  sounding pad is white. Lightness carries the meaning and hue backs it up,
  so the roles still read with colour blindness, and Contrast keeps to the
  orange/blue axis. Palette tables are in flash and use no RAM.
- **Split.** X divides the grid between rows 4 and 5. Y divides it between
  columns 4 and 5. The primary palette and MIDI channel apply to the whole
  grid with Off selected, the lower half with X, or the left half with Y.
  The other half uses its own palette and channel (Contrast on channel 2 by
  default). Each pad keeps its pitch mapping. Changing the split releases
  held notes. A pitch sounding in two halves on different channels sends one
  note per channel. Channel pressure is sent per channel.
- **Notes.** Pads send Note On/Off on the "Standalone Port" (UMP group 0) with
  16-bit velocity in the MIDI 2.0 Protocol and 7-bit velocity in the MIDI 1.0
  Protocol. Pads that share a pitch share one note.
- **Pressure.** Poly Pressure, Channel Pressure, or off, from the raw 12-bit
  pad ADC, with hysteresis and a rate cap so a steady hold does not flood the
  port.
- **Velocity modes.** Peak-window (default; highest raw reading in a 3 ms
  window), Zero-latency (the library's peak at the press event), Two-stage
  (time to rise between two thresholds; aimed at the softest playing) and
  Fixed. Each has a Soft, Linear or Hard curve.
- **Intonation.** 29 tunings from the Pico2Seq library: 12-EDO and 11 other
  equal divisions (24, 19, 31, 22, 17, 15, 10, 7, 5, 41, 53), 6 just tunings,
  4 historical temperaments, 3 Indian tunings, Bohlen-Pierce and the three
  Carlos scales, plus an A4 reference from 415.0 to 466.0 Hz. Each tuning
  offers its own scales: the twelve-note tunings offer the 31 classic scales,
  the others their own sets (for example 24-EDO offers Rast, Bayati, Hijaz,
  Saba and All Degrees). See "Notes in other tunings" below.
- **Setup menu.** Tap Setup to open or close, hold it to peek. Button 95 opens
  the Scale page (root, scale, octave, layout, absolute root), button 96 opens
  the Options page (velocity mode and curve, pressure mode and threshold, DIN
  mirror, MIDI channel), button 97 opens the Tuning page (tuning and A4),
  button 98 opens the Surface page for setting and previewing each half's
  palette and selecting its channel. The left round buttons also work on
  that page. The uppermost right-side round button (89) opens the MIDI-CI
  page and scans for peers; pad 11 rescans, and pads 21–28 show peer status.
  Settings are saved to flash 2 s after the last change, once no pad is
  being played.
- **DIN.** The "MIDI Port" (UMP group 1) is bridged to the DIN MIDI ports in
  both directions. With DIN mirror on, pad notes and pressure also go to DIN
  Out as MIDI 1.0.
- **Setup LED.** Green = MIDI 2.0 Protocol active. Amber = anything else
  (MIDI 1.0 Protocol, or USB alternate setting 0).
- **USB identity.** VID `0x1235`, PID `0x4051` + (bootloader device ID − 1),
  product "Launchpad Pro MIDI 2.0" (the bootloader may add a suffix such as
  " 16"). UMP endpoint name "Launchpad Pro MIDI 2.0" with two Function Blocks,
  "Standalone Port" and "MIDI Port".

### Notes in other tunings

In a twelve-note tuning a pad plays the same note number as in 12-EDO, and
only the pitch changes. In a tuning with another number of degrees,
consecutive note numbers are consecutive degrees, counted from note
60 + root (the usual keyboard mapping for microtonal synths): in 24-EDO note
61 is a quarter tone above 60. In the MIDI 2.0 Protocol every Note On then
carries the exact pitch as a Pitch 7.9 attribute (attribute type 3), so a
MIDI 2.0 synth plays it in tune with no setup. The attribute is left off in
12-EDO at A4 = 440.0 Hz. DIN and the MIDI 1.0 Protocol get the plain note
number; a MIDI 1.0 synth needs the same tuning loaded to follow.

The full pad map of the menu and every check to run on the hardware are in
`docs/checkpoint-2-instrument.md`.

## MIDI-CI with MIDI2Sound

The Standalone Function Block supports MIDI-CI 1.2 over USB UMP. It answers
Discovery and Product Instance ID inquiries, handles MUID invalidation,
collisions, and negative acknowledgments, and supports Property Exchange
(Common Rules for PE 1.1). Discovery advertises Property Exchange only; Profiles
and Process Inquiry are not supported. The Windows UI calls UMP group 0
**Group 1**.

Property Exchange resources, one request at a time, messages up to 320 bytes:

| Resource | Get | Set | Subscribe | What it holds |
|---|---|---|---|---|
| `ResourceList` | yes | | | The resources below, with JSON schemas for the two settings resources |
| `DeviceInfo` | yes | | | Novation IDs, "Launchpad Pro MIDI 2.0", version 0.1.0.0, serial number = Product Instance ID |
| `ChannelList` | yes | | | The Standalone Port and its MIDI channel. X lists "Standalone Lower" and "Standalone Upper". Y lists "Standalone Left" and "Standalone Right". |
| `X-ScaleSettings` | yes | full | yes | `root` (0 to 11), `scale` (0 to 60), `octave` (0 to 7), `layout`, `absoluteRoot`, `tuning` (a tuning id), `a4Tenths` (A4 in tenths of a hertz, 4150 to 4660) |
| `X-PlayOptions` | yes | full | yes | `velocityMode`, `velocityCurve`, `pressureMode`, `pressureThreshold` (`low`, `med`, `high`, `instant`), `pressureSmoothing`, `dinMirror`, `channel` (1 to 16) |
| `X-ScaleList` | yes | | | The 61 scale names, in `scale` order |
| `X-TuningList` | yes | | | The 29 tunings: `id`, `name`, `degrees`, and `scales` (the scales it offers, in menu order; left out for twelve-note tunings, which offer scales 0 to 30) |
| `X-SurfaceSettings` | yes | full | yes | `palette` and `upperPalette` (`ocean`, `citrus`, `glacier`, `contrast`), `splitMode` (`off`, `x`, `y`), `upperChannel` (1 to 16). The legacy `split` boolean is also accepted. The primary half's channel is `channel` in `X-PlayOptions` |

A Set changes only the fields it gives and is applied like a change in the
Setup menu (saved to flash 2 s later). One bad field rejects the whole Set with
status 400, and so does a `scale` the tuning doesn't offer. A Set that changes
`tuning` without a `scale` keeps the scale if the new tuning offers it, and
otherwise moves to the tuning's default scale. Each settings resource has one subscriber (the latest wins), which
gets a full notify when the settings change from the menu or from another
initiator. Subscriptions end on USB reset, an alternate-setting change, or MUID
invalidation.

On Windows, select the Launchpad endpoint in MIDI2Sound using Windows MIDI
Services, open **MIDI-CI**, and click **DISCOVER**. Expect one reply on Group 1
with Function Block 0, MIDI-CI format 2, Property Exchange in its categories,
a Product Instance ID matching the UMP endpoint, and ResourceList and
DeviceInfo with status 200. In Live MIDI, press and release a pad and vary its
pressure to check MIDI 2.0 note and pressure messages. The MIDI2Sound CLI can
also run `midi_monitor_cli --ci-discover --endpoint Launchpad --all-groups`.
MIDI2Sound only reads Property Exchange; to try Set and Subscribe, use a PE
editor such as the MIDI 2.0 Workbench.

The firmware reuses the same [MIT-licensed Microsoft MIDI-CI headers](src/midici/vendor/README.md)
as MIDI2Sound.

## Building

You need an ARM GCC toolchain with C++17, a host `gcc`/`g++`, `make`, and the
`tools/libintelhex` git submodule.

### Linux (Debian / Ubuntu), or Windows via WSL

```sh
git clone --recursive https://github.com/IC-Alchemy/launchpad-pro-MIDI2.git
cd launchpad-pro-MIDI2
# if you cloned without --recursive:
git submodule update --init

sudo apt-get install build-essential gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi

make         # builds build/launchpad_pro.syx
make test    # builds and runs the host unit tests
```

This is the setup that is checked: Ubuntu 24.04 with `arm-none-eabi-gcc`
13.2.1. `make` builds the firmware ELF, runs the host simulator as a smoke
test, converts the image to `build/launchpad_pro.syx`, and runs
`tools/check_image.sh` to confirm the image fits below `0x0801F800` (the end of
the app flash region). It fails if the image does not fit. `make test` runs
the host unit tests and should end with `0 failures`.
`make clean` removes `build/`.

### macOS

Install the Arm GNU toolchain (for example `brew install --cask gcc-arm-embedded`),
then follow the Linux steps from `git submodule update --init`. Not tested
for this fork.

### Docker and Vagrant

`Dockerfile`, `Vagrantfile` and `bootstrap.sh` are from the upstream project
and have not been updated or tested for this fork. Prefer the Linux steps above
(WSL on Windows works the same way).

## Flashing

Firmware is uploaded over USB as SysEx to the Launchpad Pro's bootloader. The
bootloader lives in protected flash, so a bad image cannot brick the unit.

1. Unplug the Launchpad Pro (and any DIN cables).
2. **Hold the Setup button** and, while holding it, plug in the USB cable.
   Plug it into the computer itself, not a virtual machine. The unit starts in
   bootloader mode.
3. Send `build/launchpad_pro.syx` to the Launchpad Pro's MIDI port with a SysEx
   tool:
   - **Windows:** MIDI-OX, *Actions > Send > SysEx File*.
   - **macOS:** SysEx Librarian.
   - **Linux:** `amidi -l` to find the port, then
     `amidi -p hw:1,0,0 -s build/launchpad_pro.syx -i 2` (use your port; `-i`
     is the pause between SysEx messages in ms).
4. The grid scrolls "upgrading...". Do not unplug or close the sender while it
   runs. Wait for the unit to reboot by itself.

A short delay between SysEx messages keeps the upload quick. If the upload
fails, raise the delay and try again.

Notes:

- **Always enter the bootloader by holding Setup while plugging in.** Updater
  tools that send a "reboot to bootloader" SysEx do not work while this
  firmware runs in MIDI 2.0 mode (USB alt 1), because that SysEx no longer
  reaches the library.
- On Windows, if MIDI-OX cannot reach the bootloader through Windows MIDI
  Services, switch the device to the in-box legacy USB Audio driver in Device
  Manager, or flash from a PC without Windows MIDI Services.
- **Back to factory firmware:** flash `resources/Launchpad Pro-1.0.154.syx`
  the same way.

## Checking it on Windows

With Windows MIDI Services installed, `midi endpoint list` should show an
endpoint named "Launchpad Pro MIDI 2.0" with native protocol MIDI 2.0 and two
Function Blocks. `midi endpoint monitor` shows the 16-bit velocities and 32-bit
pressure as you play. The checkpoint documents in `docs/` give the full test
procedures.

## How it works

The USB stack and the low-level pad, LED, DIN and flash code live in Novation's
closed `lib/launchpad_pro.a`. The Makefile builds a trimmed copy of that
archive without its USB descriptor, property and endpoint objects
(`usb_desc.o`, `usb_prop.o`, `usb_endp.o`) and links this project's
replacements in their place. Everything else in the library is used as is.
`include/app.h` and its ABI are unchanged, so `hal_send_midi` and friends still
work in both USB modes. The new API is in `include/midi2.h`.

| Path | What is in it |
|---|---|
| `src/app.c` | App entry points; wires the instrument to the library callbacks |
| `src/usb/` | USB descriptors (alt 0 and alt 1), class requests, EP1 endpoint handling, Group Terminal Blocks |
| `src/ump/` | UMP encode/decode, transmit queue, receive path, stream discovery replies |
| `src/pads/` | Velocity math and curves, the pad engine (notes, pressure), Two-stage detector |
| `src/scales/` | Scale table, layouts, pad colour roles, the tuning library (`tuning_data.c` is generated from Pico2Seq) |
| `src/instrument/` | Settings model and flash record, the menu/play UI |
| `src/platform/` | Access to library internals (raw ADC, pad calibration, interrupts) |
| `tests/` | Host unit tests (`make test`) |
| `tools/` | Host simulator, `hextosyx` converter, image size check |
| `docs/` | Design spec, implementation plans, hardware checkpoint procedures and results |
| `resources/` | Factory firmware image for recovery |

## Hardware

STM32F103RB (Cortex-M3, 72 MHz), 20 KiB RAM, 128 KiB flash. The app occupies
`0x08006400`–`0x0801F7FF` (101 KiB); the 1 KiB page at `0x0801F800` holds the
saved settings. USB full speed, 64-byte bulk packets on EP1.

There is no supported hardware debugger (opening the unit risks damaging the
pad sensor sheet). Debugging is by LEDs, MIDI output and the host tests.
