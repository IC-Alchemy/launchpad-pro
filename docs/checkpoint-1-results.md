# Checkpoint 1 (transport) — hardware results

Firmware: `launchpad_pro_stage1_velfix2.syx` (stage 1 plus the pad velocity
fix: ADC_Max read in library slot order, 10-bit; held pads lit at a minimum
brightness).

Host: Windows 10.0.28020 (26H1). Driver `usb.inf` / `wdma_usb.inf`
10.0.28000.3531. Windows MIDI Services Console SDK RC4 1.0.17-rc.4.25.
Device ID 16: `USB\VID_1235&PID_0060\LAUNCHPAD_PRO_16`.

## Outcome

The player reports every step of `docs/checkpoint-1-transport.md` as
expected, except three that were **not tested** (deferred, not failed):

| Step | What | Status |
|---|---|---|
| 5b | Stream Configuration Request to MIDI 2.0 (`0xF0050200`) reply and held-note release | not tested |
| 6a | DIN input continues across alt 1, 0, 1 | not tested |
| 6c | EP1 framing in alt 1 | not tested |

All three are added to the final hardware pass (stage 4).

## Endpoint (Windows MIDI Services)

- Transport "Kernel Streaming MIDI for UMP Drivers (KS)", native data
  format **Native UMP**.
- The Product Instance ID is reported. The serial number is "Launchpad Open
  16" (the library's serial string, kept so Windows does not see a new device).
- MIDI 1.0 ports: group 1 "Launchpad Pro MIDI 2.0 16", group 2
  "MIDIIN2 (Launchpad Pro MIDI 2.0" / "MIDIOUT2 (…)". These are Windows'
  classic-compatible names. The function block names are "Standalone Port"
  and "MIDI Port".

## Pad data (`midi-capture3.txt`, Zero-latency velocity)

- 124 Note On / 124 Note Off as MIDI 2.0 (MT 4) on group 1, channel 1.
- Velocity follows the hit: min 1, median 50 558, max 64 676, with 96
  distinct values in 124 hits.
- Poly pressure arrives as MIDI 2.0 32-bit, but still from the library's
  7-bit aftertouch (bit-replicated 7 → 32). Stage 2 reads pressure from the
  raw 12-bit ADC instead.
- The 125 bpm clock test traffic ran at 20.00 ms ± 0.06 ms.
