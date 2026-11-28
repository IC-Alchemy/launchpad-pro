# Launchpad Pro MIDI 2.0

An old grid, a finer touch.

Firmware that turns the original 2015 Novation Launchpad Pro into a USB MIDI 2.0 scale instrument. The pads read 12-bit raw sensors, so instead of squeezing that into 7 bits, velocity goes out as 16-bit and pressure as 32-bit.

The target is Windows MIDI Services. On hosts without MIDI 2.0 it's a plain two-port USB MIDI 1.0 device.

Built on Novation's [open Launchpad Pro firmware](https://github.com/dvhdr/launchpad-pro) (BSD licence, see `LICENSE`). The full design is in `docs/superpowers/specs/2026-11-21-midi2-scale-instrument-design.md`.

## Where things stand

Stage 1, the USB MIDI 2.0 transport, is tested on hardware (`docs/checkpoint-1-results.md`). The rest passes host tests and still needs its hardware check:

| Stage | What | Hardware check |
|---|---|---|
| 2 | Scale engine, five layouts, velocity modes, pressure, Setup menu, settings in flash | `docs/checkpoint-2-instrument.md` |
| 2b | Two-stage velocity (thresholds are first guesses) | same doc |
| 2c | 29 tunings, 56 scales, A4 reference, MIDI 2.0 pitch attribute | section 18 |
| 2d | Palettes and split surface | section 19 |
| 3 | MIDI-CI and Property Exchange | pending, with MIDI2Sound |
| 4 | Final hardware pass, plus stage 1 checks 5b, 6a, 6c | not started |

## Playing it

The grid is a scale keyboard. Default is C major, layout "In-key 4ths", bottom-left pad on note 60. Buttons 91/92 move the octave, 93/94 the root.

- **Notes:** on the "Standalone Port" (UMP group 0), 16-bit velocity in the MIDI 2.0 Protocol, 7-bit in MIDI 1.0. Pads that share a pitch share one note.
- **Pressure:** Poly, Channel or off, from the raw pad ADC, rate-capped so a steady hold doesn't flood the port.
- **Velocity:** Peak-window (default, best reading in 3 ms), Zero-latency, Two-stage (rise time between two thresholds, for soft playing) or Fixed, each with a Soft, Linear or Hard curve.
- **Tunings:** 29 from Pico2Seq: equal divisions from 5 to 53, just, historical, Indian, Bohlen-Pierce and the Carlos scales, with A4 from 415.0 to 466.0 Hz. Twelve-note tunings offer the 26 classic scales, the others their own sets (24-EDO has Rast, Bayati, Hijaz, Saba and All Degrees).
- **DIN:** the "MIDI Port" (UMP group 1) bridges to DIN both ways. DIN mirror also copies pad notes and pressure to DIN Out as MIDI 1.0.
- **Setup LED:** green means MIDI 2.0 Protocol, amber means anything else.

### Palettes and split

New, and not checked on hardware yet.

Left round buttons, top down: 80 split X, 70 split Y, 60 split off. 50, 40, 30, 20 pick Ocean, Citrus, Glacier or Contrast for the primary zone. In a split, 10 cycles both halves through Ocean / Contrast, Glacier / Rose and Citrus / Amethyst.

Each palette is one hue family, brightest on the fifth, then root, third, scale, out-of-scale. A sounding pad goes white.

X splits between rows 4 and 5, Y between columns 4 and 5. The second half gets its own palette and channel (Contrast on channel 2 by default). Pitch mapping stays put. Changing the split releases held notes, and a pitch in both halves sends one note per channel.

### Setup menu

Tap Setup to open or close, hold to peek.

- 95: Scale page (root, scale, octave, layout, absolute root)
- 96: Options page (velocity mode and curve, pressure mode and threshold, DIN mirror, MIDI channel)
- 97: Tuning page (tuning and A4)
- 98: Surface page, each half's palette and channel
- 89: MIDI-CI page, scans for peers (pad 11 rescans)

Settings save 2 s after the last change, once you stop playing. The full pad map is in `docs/checkpoint-2-instrument.md`.

### Other tunings

Twelve-note tunings keep 12-EDO note numbers, only the pitch moves. Other tunings map consecutive notes to consecutive degrees from 60 + root, so in 24-EDO note 61 is a quarter tone up. Over MIDI 2.0 each Note On carries the exact pitch as a Pitch 7.9 attribute (type 3), so a MIDI 2.0 synth plays in tune with no setup. DIN and MIDI 1.0 just get the note number, so load the same tuning on the synth.

### USB identity

VID `0x1235`, PID `0x4051` + (bootloader device ID − 1), product "Launchpad Pro MIDI 2.0" (the bootloader may add a suffix such as " 16"). UMP endpoint "Launchpad Pro MIDI 2.0" with two Function Blocks, "Standalone Port" and "MIDI Port".

## MIDI-CI with MIDI2Sound

The Standalone Function Block speaks MIDI-CI 1.2 with Property Exchange (PE 1.2, ASCII JSON). Windows calls UMP group 0 **Group 1**. It can ask a peer about Profiles, per section 2.4 of the [Common Rules for MIDI-CI Profiles v1.1](https://midi.org/common-rules-for-midi-ci-profiles), but has none of its own.

Property Exchange resources, one request at a time, messages up to 320 bytes:

| Resource | Get | Set | Subscribe | What it holds |
|---|---|---|---|---|
| `ResourceList` | yes | | | The resources below, with JSON schemas for the two settings resources |
| `DeviceInfo` | yes | | | Novation IDs, "Launchpad Pro MIDI 2.0", version 0.1.0.0, serial number = Product Instance ID |
| `ChannelList` | yes | | yes | One entry per active MIDI channel. With two distinct channels, X lists "Standalone Lower" and "Standalone Upper", and Y lists "Standalone Left" and "Standalone Right". |
| `X-ScaleSettings` | yes | full and partial | yes | `root` (0 to 11), `scale` (0 to 55), `octave` (0 to 7), `layout`, `absoluteRoot`, `tuning` (a tuning id), `a4Tenths` (A4 in tenths of a hertz, 4150 to 4660) |
| `X-PlayOptions` | yes | full and partial | yes | `velocityMode`, `velocityCurve`, `pressureMode`, `pressureThreshold` (`low`, `med`, `high`, `instant`), `pressureSmoothing`, `dinMirror`, `channel` (1 to 16) |
| `X-ScaleList` | yes | | | 56 objects with a `name` field, in `scale` order |
| `X-TuningList` | yes | | | The 29 tunings: `id`, `name`, `degrees`, and `scales` (the scales it offers, in menu order; left out for twelve-note tunings, which offer scales 0 to 25) |
| `X-SurfaceSettings` | yes | full and partial | yes | `palette` and `upperPalette` (`ocean`, `citrus`, `glacier`, `contrast`, `rose`, `amethyst`), `splitMode` (`off`, `x`, `y`), `upperChannel` (1 to 16). The legacy `split` boolean is also accepted. The primary half's channel is `channel` in `X-PlayOptions` |

Partial Sets use `"setPartial":true` and JSON Pointer keys like `"/root"`. One bad field rejects the whole Set with 400. Each settings resource takes one subscriber. Network MIDI 2.0 UDP needs a host bridge.

To try it, open **MIDI-CI** in MIDI2Sound on the Launchpad endpoint and click **DISCOVER**. You should get one reply on Group 1 with ResourceList and DeviceInfo at status 200. Or from the CLI: `midi_monitor_cli --ci-discover --endpoint Launchpad --all-groups`. MIDI2Sound only reads PE, so for Set and Subscribe use something like the MIDI 2.0 Workbench.

It uses the same [MIT-licensed Microsoft MIDI-CI headers](src/midici/vendor/README.md) as MIDI2Sound.

## Building

You need an ARM GCC toolchain with C++17, a host `gcc`/`g++`, `make`, and the `tools/libintelhex` git submodule.

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

Checked on Ubuntu 24.04 with `arm-none-eabi-gcc` 13.2.1. `make` also runs the host simulator as a smoke test and `tools/check_image.sh`, which fails the build if the image runs past `0x0801F800`. `make test` should end with `0 failures`. `make clean` removes `build/`.

### macOS, Docker, Vagrant

On macOS, install the Arm GNU toolchain (for example `brew install --cask gcc-arm-embedded`), then follow the Linux steps from `git submodule update --init`. `Dockerfile`, `Vagrantfile` and `bootstrap.sh` are from upstream. None of these are tested for this fork.

## Flashing

Firmware goes over USB as SysEx to the bootloader. It sits in protected flash, so a bad image can't brick the unit.

1. Unplug the Launchpad Pro (and any DIN cables).
2. **Hold Setup** while you plug in USB, directly to the computer, not a VM.
3. Send `build/launchpad_pro.syx` to its MIDI port with a SysEx tool:
   - **Windows:** MIDI-OX, *Actions > Send > SysEx File*.
   - **macOS:** SysEx Librarian.
   - **Linux:** `amidi -l` to find the port, then
     `amidi -p hw:1,0,0 -s build/launchpad_pro.syx -i 2` (use your port; `-i`
     is the pause between SysEx messages in ms).
4. The grid scrolls "upgrading...". Leave it alone until it reboots. If the upload fails, raise the SysEx delay and retry.

- **Always use the hold-Setup method.** Updater tools' "reboot to bootloader" SysEx can't reach the library in MIDI 2.0 mode (USB alt 1).
- If MIDI-OX can't reach the bootloader through Windows MIDI Services, switch the device to the in-box legacy USB Audio driver in Device Manager, or flash from a PC without it.
- **Back to factory firmware:** flash `resources/Launchpad Pro-1.0.154.syx` the same way.

To check it on Windows, `midi endpoint list` should show "Launchpad Pro MIDI 2.0", native protocol MIDI 2.0, two Function Blocks. `midi endpoint monitor` shows the 16-bit velocity and 32-bit pressure as you play.

## How it works

The USB stack and low-level pad, LED, DIN and flash code live in Novation's closed `lib/launchpad_pro.a`. The Makefile strips out its `usb_desc.o`, `usb_prop.o` and `usb_endp.o` and links our replacements instead. `include/app.h` is unchanged, so `hal_send_midi` still works in both USB modes. The new API is in `include/midi2.h`.

| Path | What is in it |
|---|---|
| `src/app.c` | Entry points, wired to the library callbacks |
| `src/usb/` | Descriptors (alt 0 and 1), class requests, EP1, Group Terminal Blocks |
| `src/ump/` | UMP codec, transmit queue, receive path, stream discovery |
| `src/midici/` | MIDI-CI responder, initiator and Property Exchange |
| `src/pads/` | Velocity, curves, pad engine, Two-stage detector |
| `src/scales/` | Scales, layouts, colour roles, tunings (`tuning_data.c` generated from Pico2Seq) |
| `src/instrument/` | Settings, flash record, menu and play UI |
| `src/platform/` | Library internals (raw ADC, pad calibration, interrupts) |
| `tests/` | Host unit tests |
| `tools/` | Host simulator, `hextosyx`, image size check |
| `docs/` | Design spec, plans, hardware checkpoints |
| `resources/` | Factory firmware for recovery |

## Hardware

STM32F103RB (Cortex-M3, 72 MHz), 20 KiB RAM, 128 KiB flash. The app occupies `0x08006400`–`0x0801F7FF` (101 KiB); the 1 KiB page at `0x0801F800` holds the saved settings. USB full speed, 64-byte bulk packets on EP1.

No hardware debugger. Opening the unit risks the pad sensor sheet, so debugging is LEDs, MIDI output and host tests.
