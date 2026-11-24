// Compile-only check that the vendored MIDI-CI headers build freestanding for the target.
// Built with the firmware (Makefile FW_CXX_SOURCES) so the headers are always checked against the
// real flags; emits no code. See README.md.
#include "sal_shim.h"
#include "MidiCiMessage.h"
#include "MidiCiResponder.h"
