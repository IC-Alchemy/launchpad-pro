# Vendored: microsoft/MIDI MIDI-CI headers

- Source repo: https://github.com/microsoft/MIDI
- Commit: `183460a604e82f4ec47cba2b213d970710b4d999` (183460a)
- License: MIT, Copyright (c) Microsoft Corporation (see `LICENSE-MIT.txt`)
- Upstream path: `src/in-box/Inc/`
- Files:
  - `MidiCiMessage.h` - MIDI-CI message parse/build (`Parse`, `Build*`)
  - `MidiCiResponder.h` - allocation-free `Responder` state machine
- Local modifications: none. Do not edit these two files; re-vendor from upstream instead.
- Reply version: upstream `Responder` answers in the requester's message version, but M2-101
  says a MUID uses one outgoing format. Rather than patching `ReplyVersionFor`,
  `../responder.cpp` sets `ParsedMessage::VersionFormat` to `MIDI_CI_VERSION` (0x02) after
  parsing and before `ProcessMessage`. NAKs for an unsupported version are built there too.

## Support files (ours, not upstream)

- `sal_shim.h` - defines the MSVC SAL annotations the headers use (`_In_`, `_Out_`,
  `_In_reads_`, `_In_reads_opt_`, `_Out_writes_`, `_Out_writes_to_opt_`,
  `_Out_writes_bytes_to_opt_`, plus a few defensive extras) as empty macros, each guarded with
  `#ifndef` so it is a no-op on MSVC. Include it before the headers, or force-include it with
  `-include src/midici/vendor/sal_shim.h`.
- `ci_headers_check.cpp` - compile-only check, not part of the firmware build:

      arm-none-eabi-g++ -std=c++17 -mthumb -mcpu=cortex-m3 -Os -fno-exceptions -fno-rtti \
          -Wall -Wextra -include src/midici/vendor/sal_shim.h \
          -c src/midici/vendor/ci_headers_check.cpp -o ci_check.o

## Freestanding notes

Only `<stdint.h>` and `<stddef.h>` are included; no `std::` use. The compiler may emit calls to
`memcpy`/`memset` for struct copy and zero-initialisation (`ParsedMessage{}`, `ResponderConfig`),
so the firmware must provide them (newlib/libc or own implementation).
