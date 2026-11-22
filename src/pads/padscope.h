#ifndef PADSCOPE_H
#define PADSCOPE_H

/* Measurement build (make PADSCOPE=1): captures raw 12-bit pad attacks and
 * idle noise and dumps them as SysEx on USBSTANDALONE, to be decoded with
 * tools/padscope_decode.py.
 *
 * SysEx: F0 00 20 29 7D <type> <part> <payload> F7, every message < 320 bytes.
 *   type 1 capture, part 0: pad index, stale count lo 7 bits, hi 7 bits,
 *                           128 pad samples packed (pack12)
 *                  part 1: 128 neighbour-max samples packed
 *   type 2 idle stats, part 0: 64 min values packed (ADC order)
 *                      part 1: 64 max values packed
 * Samples: index 0..7 are the 8 samples before the trigger (t = -8..-1 ms),
 * index 8..127 the 120 from the trigger sample on (t = 0..119 ms).
 * At most one message is sent per padscope_tick. */

#include "app.h"

#define PADSCOPE_PRE 8
#define PADSCOPE_POST 120		/* includes the trigger sample */
#define PADSCOPE_SAMPLES (PADSCOPE_PRE + PADSCOPE_POST)

/* each 12-bit value as two 7-bit bytes: (v >> 7) & 0x1F, v & 0x7F.
 * Returns bytes written (2 * count; u16 because 128 values give 256). */
u16 padscope_pack12(const u16 *vals, u8 count, u8 *out);

/* call every app_timer_event (1 kHz, interrupt context) with the 64-value
 * raw ADC frame; ignored when raw is null */
void padscope_tick(const u16 *raw);

/* request an idle stats dump (main-loop safe: only sets a flag, the message
 * goes out from padscope_tick) */
void padscope_dump(void);

#ifdef HOST_TEST
void padscope_test_reset(void);
u8 padscope_test_state(void);			/* 0 idle 1 recording 2 settling 3 sending */
u8 padscope_test_chan(void);			/* watched ADC channel */
u16 padscope_test_pad(u8 i);
u16 padscope_test_nb(u8 i);
u16 padscope_test_stale(void);
#endif

#endif
