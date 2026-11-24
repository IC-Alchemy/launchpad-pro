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
| 3 | MIDI-CI with Property Exchange (DAW/editor discovery, edit settings from the PC) | **Not started.** `src/ci/` does not exist yet; `src/ump/ump_rx.c` has a placeholder hook for it. |
| 4 | Final hardware pass, including MIDI-CI and the deferred stage 1 checks | Not started. |

Current image size: about 30 KB of the 101 KB app flash region (about 70 KB
free), and about 12.3 KB of the 20 KB RAM. `make` prints the exact flash margin.

## What the firmware does now

- **Play page.** The 8×8 grid is a scale keyboard. Default: C major, octave 4,
  layout "In-key 4ths", bottom-left pad = note 60. Roots are bright blue, the
  3rd and 5th are light blue, other scale notes are dark blue, a sounding pad is
  white. Buttons 91/92 change the octave, 93/94 change the root.
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
- **Setup menu.** Tap Setup to open or close, hold it to peek. Button 95 opens
  the Scale page (root, scale, octave, layout, absolute root), button 96 opens
  the Options page (velocity mode and curve, pressure mode and threshold, DIN
  mirror, MIDI channel). Settings are saved to flash 2 s after the last change,
  once no pad is being played.
- **DIN.** The "MIDI Port" (UMP group 1) is bridged to the DIN MIDI ports in
  both directions. With DIN mirror on, pad notes and pressure also go to DIN
  Out as MIDI 1.0.
- **Setup LED.** Green = MIDI 2.0 Protocol active. Amber = anything else
  (MIDI 1.0 Protocol, or USB alternate setting 0).
- **USB identity.** VID `0x1235`, PID `0x4051` + (bootloader device ID − 1),
  product "Launchpad Pro MIDI 2.0" (the bootloader may add a suffix such as
  " 16"). UMP endpoint name "Launchpad Pro MIDI 2.0" with two Function Blocks,
  "Standalone Port" and "MIDI Port".

The full pad map of the menu and every check to run on the hardware are in
`docs/checkpoint-2-instrument.md`.

## Building

You need the ARM GCC toolchain, a host `gcc`/`g++`, `make`, and the
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
the host unit tests (208 at present) and should end with `0 failures`.
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
| `src/scales/` | Scale table, layouts, pad colour roles |
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
