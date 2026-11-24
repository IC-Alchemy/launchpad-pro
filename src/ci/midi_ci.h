#ifndef MIDI_CI_H
#define MIDI_CI_H

#include <stdint.h>

#define MIDI_CI_VERSION 0x02
#define MIDI_CI_REPLY_BYTES 60

struct midi_ci_reply {
	uint8_t data[MIDI_CI_REPLY_BYTES]; /* F0 through F7 */
	uint16_t len;
	uint8_t muid_changed;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Main-loop only. Returns nonzero when the Standalone SysEx belongs to MIDI-CI. */
int midi_ci_receive(const uint8_t *data, uint16_t len, struct midi_ci_reply *reply);

#ifdef __cplusplus
}
#endif

#endif
