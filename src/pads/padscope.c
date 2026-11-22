#include "pads/padscope.h"

/* No dynamic allocation, no 64-bit math, bounded work per tick (64 channels
 * plus 4 neighbours). Runs in the 1 kHz timer interrupt; padscope_dump is the
 * only entry point from the main loop and just sets a flag. */

#define TRIGGER_LEVEL 200
#define RELEASE_LEVEL 100
#define RELEASE_TICKS 50
#define IDLE_LEVEL 400
#define STALE_MAX 0x3FFF
#define MSG_MAX 272
#define SETTLE_MAX_TICKS 1000	/* give up waiting for the pad to release */

enum { CAP_IDLE, CAP_RECORD, CAP_SETTLE, CAP_READY };
enum { SEND_NONE, SEND_CAPTURE, SEND_IDLE };

static u16 g_ring[PADSCOPE_PRE][PAD_COUNT];	/* the last 8 frames, oldest at g_ring_head */
static u8 g_ring_head;
static u8 g_have_prev;				/* the ring holds a real frame */
static u8 g_chan_of[100];			/* pad index -> ADC channel */
static u8 g_init_done;

static u16 g_stale;				/* identical-frame ticks since the last capture dump */

/* idle stats: min starts at 0xFFF and max at 0, so a channel that never read
 * below IDLE_LEVEL decodes as min > max */
static u16 g_min[PAD_COUNT], g_max[PAD_COUNT];

static u8 g_cap_state;
static u8 g_chan;				/* watched ADC channel */
static u8 g_nb[4], g_nb_n;			/* neighbour ADC channels */
static u16 g_samp;				/* samples recorded so far */
static u16 g_cap_pad[PADSCOPE_SAMPLES];
static u16 g_cap_nb[PADSCOPE_SAMPLES];
static u16 g_cap_stale;				/* snapshot sent in the message */
static u16 g_settle;				/* ticks spent in CAP_SETTLE */
static u8 g_low;				/* consecutive ticks below RELEASE_LEVEL */

static u8 g_send;
static u8 g_part;
static u8 g_gap = PADSCOPE_MSG_GAP_TICKS;	/* ticks since the last message */
/* dump requests: the main loop increments g_dump_req, the tick copies it to
 * g_dump_ack when it starts a dump, so a press is never lost */
static volatile u8 g_dump_req;
static u8 g_dump_ack;
static u8 g_msg[MSG_MAX];

u16 padscope_pack12(const u16 *vals, u8 count, u8 *out)
{
	u16 n = 0;
	u8 i;

	for (i = 0; i < count; i++) {
		out[n++] = (u8)((vals[i] >> 7) & 0x1F);
		out[n++] = (u8)(vals[i] & 0x7F);
	}
	return n;
}

static void reset_idle_stats(void)
{
	u8 i;

	for (i = 0; i < PAD_COUNT; i++) {
		g_min[i] = 0xFFF;
		g_max[i] = 0;
	}
}

static void init_once(void)
{
	u8 i;

	for (i = 0; i < 100; i++)
		g_chan_of[i] = 0xFF;
	for (i = 0; i < PAD_COUNT; i++)
		g_chan_of[ADC_MAP[i]] = i;
	reset_idle_stats();
	g_init_done = 1;
}

static void add_nb(u8 pad, u8 row, u8 col)
{
	if (row >= 1 && row <= 8 && col >= 1 && col <= 8)
		g_nb[g_nb_n++] = g_chan_of[pad];
}

/* grid neighbours at index +-1 and +-10 that exist on the 8x8 grid */
static void set_neighbours(u8 chan)
{
	u8 pad = ADC_MAP[chan];
	u8 row = pad / 10, col = pad % 10;

	g_nb_n = 0;
	add_nb((u8)(pad - 1), row, (u8)(col - 1));
	add_nb((u8)(pad + 1), row, (u8)(col + 1));
	add_nb((u8)(pad - 10), (u8)(row - 1), col);
	add_nb((u8)(pad + 10), (u8)(row + 1), col);
}

static u16 nb_max(const u16 *frame)
{
	u16 m = 0;
	u8 i;

	for (i = 0; i < g_nb_n; i++)
		if (frame[g_nb[i]] > m)
			m = frame[g_nb[i]];
	return m;
}

static void start_capture(u8 chan, const u16 *raw)
{
	u8 i;

	g_chan = chan;
	set_neighbours(chan);
	for (i = 0; i < PADSCOPE_PRE; i++) {
		const u16 *fr = g_ring[(u8)((g_ring_head + i) % PADSCOPE_PRE)];
		g_cap_pad[i] = fr[chan];
		g_cap_nb[i] = nb_max(fr);
	}
	/* sample 8 is the trigger sample itself (t = 0), the first of the 120
	 * post-trigger samples */
	g_cap_pad[PADSCOPE_PRE] = raw[chan];
	g_cap_nb[PADSCOPE_PRE] = nb_max(raw);
	g_samp = PADSCOPE_PRE + 1;
	g_low = 0;
	g_cap_state = CAP_RECORD;
}

static void capture_step(const u16 *raw)
{
	u8 c;

	if (g_cap_state == CAP_IDLE) {
		const u16 *prev = g_ring[(u8)((g_ring_head + PADSCOPE_PRE - 1) % PADSCOPE_PRE)];

		/* a dump being requested or sent also holds back a new capture */
		if (!g_have_prev || g_dump_req != g_dump_ack || g_send != SEND_NONE)
			return;
		for (c = 0; c < PAD_COUNT; c++) {
			if (prev[c] < TRIGGER_LEVEL && raw[c] >= TRIGGER_LEVEL) {
				start_capture(c, raw);
				return;
			}
		}
		return;
	}
	if (g_cap_state == CAP_RECORD) {
		g_cap_pad[g_samp] = raw[g_chan];
		g_cap_nb[g_samp] = nb_max(raw);
		if (++g_samp >= PADSCOPE_SAMPLES) {
			g_cap_state = CAP_SETTLE;
			g_low = 0;
			g_settle = 0;
		}
		return;
	}
	if (g_cap_state == CAP_SETTLE) {
		g_low = raw[g_chan] < RELEASE_LEVEL ? (u8)(g_low + 1) : 0;
		/* bounded: a pad held down or a high baseline must not block
		 * captures for ever */
		if (g_low >= RELEASE_TICKS || ++g_settle >= SETTLE_MAX_TICKS) {
			g_cap_stale = g_stale;
			g_stale = 0;
			g_cap_state = CAP_READY;
		}
	}
}

/* g_msg[7..] already holds the payload */
static void send_msg(u8 type, u8 part, u16 payload_len)
{
	g_msg[0] = 0xF0;
	g_msg[1] = 0x00;
	g_msg[2] = 0x20;
	g_msg[3] = 0x29;
	g_msg[4] = 0x7D;
	g_msg[5] = type;
	g_msg[6] = part;
	g_msg[7 + payload_len] = 0xF7;
	hal_send_sysex(USBSTANDALONE, g_msg, (u16)(8 + payload_len));
}

/* sends at most one message per call */
static void send_step(void)
{
	u16 n;

	if (g_gap < PADSCOPE_MSG_GAP_TICKS) {
		g_gap++;
		return;
	}
	if (g_send == SEND_NONE) {
		if (g_cap_state == CAP_READY) {
			g_send = SEND_CAPTURE;
		} else if (g_dump_req != g_dump_ack) {
			g_send = SEND_IDLE;
			g_dump_ack = g_dump_req;
		} else {
			return;
		}
		g_part = 0;
	}
	g_gap = 0;
	if (g_send == SEND_CAPTURE) {
		if (g_part == 0) {
			g_msg[7] = ADC_MAP[g_chan];
			g_msg[8] = (u8)(g_cap_stale & 0x7F);
			g_msg[9] = (u8)(g_cap_stale >> 7);
			n = padscope_pack12(g_cap_pad, PADSCOPE_SAMPLES, g_msg + 10);
			send_msg(1, 0, (u16)(3 + n));
			g_part = 1;
		} else {
			n = padscope_pack12(g_cap_nb, PADSCOPE_SAMPLES, g_msg + 7);
			send_msg(1, 1, n);
			g_send = SEND_NONE;
			g_cap_state = CAP_IDLE;
		}
		return;
	}
	n = padscope_pack12(g_part == 0 ? g_min : g_max, PAD_COUNT, g_msg + 7);
	send_msg(2, g_part, n);
	if (g_part == 0) {
		g_part = 1;
	} else {
		reset_idle_stats();
		g_send = SEND_NONE;
	}
}

void padscope_tick(const u16 *raw)
{
	u8 c;

	if (!raw)
		return;
	if (!g_init_done)
		init_once();

	if (g_have_prev) {
		const u16 *prev = g_ring[(u8)((g_ring_head + PADSCOPE_PRE - 1) % PADSCOPE_PRE)];
		u8 same = 1;

		for (c = 0; c < PAD_COUNT; c++) {
			if (raw[c] != prev[c]) {
				same = 0;
				break;
			}
		}
		if (same && g_stale < STALE_MAX)
			g_stale++;
	}

	/* stats are frozen while an idle dump is being sent, so both halves
	 * come from the same data */
	if (g_send != SEND_IDLE) {
		for (c = 0; c < PAD_COUNT; c++) {
			if (raw[c] < IDLE_LEVEL) {
				if (raw[c] < g_min[c])
					g_min[c] = raw[c];
				if (raw[c] > g_max[c])
					g_max[c] = raw[c];
			}
		}
	}

	capture_step(raw);
	send_step();

	/* this frame becomes the newest history entry */
	for (c = 0; c < PAD_COUNT; c++)
		g_ring[g_ring_head][c] = raw[c];
	g_ring_head = (u8)((g_ring_head + 1) % PADSCOPE_PRE);
	g_have_prev = 1;
}

void padscope_dump(void)
{
	g_dump_req++;
}

#ifdef HOST_TEST
void padscope_test_reset(void)
{
	u8 i, j;

	init_once();
	for (i = 0; i < PAD_COUNT; i++)
		for (j = 0; j < PADSCOPE_PRE; j++)
			g_ring[j][i] = 0;
	g_ring_head = 0;
	g_have_prev = 0;
	g_stale = 0;
	g_cap_state = CAP_IDLE;
	g_send = SEND_NONE;
	g_dump_ack = g_dump_req;
	g_gap = PADSCOPE_MSG_GAP_TICKS;
	g_settle = 0;
	g_samp = 0;
	g_low = 0;
}

u8 padscope_test_state(void) { return g_send == SEND_CAPTURE ? 3 : g_cap_state; }
u8 padscope_test_chan(void) { return g_chan; }
u16 padscope_test_pad(u8 i) { return g_cap_pad[i]; }
u16 padscope_test_nb(u8 i) { return g_cap_nb[i]; }
u16 padscope_test_stale(void) { return g_stale; }
#endif
