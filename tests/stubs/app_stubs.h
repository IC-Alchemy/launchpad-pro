#ifndef APP_STUBS_H
#define APP_STUBS_H

#include "app.h"

/* Recording stand-ins for the hal / midi2 / library calls src/app.c makes.
 * Linked only into the app test binary (build/tests/run_app_tests). */

enum { EV_NOTE_ON, EV_NOTE_OFF, EV_POLY, EV_MIDI };

struct app_ev {
	int kind;
	u8 note;
	u32 value;
};

#define APP_EV_MAX 256
extern struct app_ev app_ev_log[APP_EV_MAX];
extern int app_ev_n;
extern void (*stub_irq_hook)(void);
extern int stub_irq_depth;
extern u8 app_pad_led[100];		/* last level plotted on each TYPEPAD index */
extern int app_pause_rx_calls;
extern u8 app_pause_rx_last;

void app_stubs_reset(void);
/* number of logged events of this kind (note < 0: any note) */
int app_ev_count(int kind, int note);

#endif
