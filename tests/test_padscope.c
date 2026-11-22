#include "test.h"
#include "pads/padscope.h"

/* hal_send_sysex stand-in: the test link has no real one */
#define MAXMSG 8
static u8 g_msg[MAXMSG][320];
static u16 g_len[MAXMSG];
static int g_n;
static int g_calls_this_tick;
static int g_max_per_tick;
static int g_gap_min;		/* fewest ticks seen between two messages */
static int g_since;

void hal_send_sysex(u8 port, const u8 *data, u16 len)
{
	CHECK_EQ(port, USBSTANDALONE);
	if (g_n < MAXMSG && len <= 320) {
		memcpy(g_msg[g_n], data, len);
		g_len[g_n] = len;
	}
	if (g_n > 0 && g_since < g_gap_min)
		g_gap_min = g_since;
	g_since = 0;
	g_n++;
	g_calls_this_tick++;
}

static void reset(void)
{
	padscope_test_reset();
	g_n = 0;
	g_max_per_tick = 0;
	g_gap_min = 1000000;
	g_since = 0;
}

static void tick(const u16 *f)
{
	g_calls_this_tick = 0;
	g_since++;
	padscope_tick(f);
	if (g_calls_this_tick > g_max_per_tick)
		g_max_per_tick = g_calls_this_tick;
}

static void ticks(const u16 *f, int n)
{
	while (n-- > 0)
		tick(f);
}

static u16 unpack(const u8 *p)
{
	return (u16)((p[0] << 7) | p[1]);
}

static void test_pack12(void)
{
	const u16 v[3] = { 0x000, 0xFFF, 0x0A5 };
	const u8 want[6] = { 0x00, 0x00, 0x1F, 0x7F, 0x01, 0x25 };
	u8 out[6];

	CHECK_EQ(padscope_pack12(v, 3, out), 6);
	CHECK_MEM(out, want, 6);
}

static void test_capture_trigger(void)
{
	u16 f[64];
	int k;

	reset();
	memset(f, 0, sizeof f);
	f[2] = 77;			/* pad 12: neighbour of pad 11 */
	for (k = 0; k <= 10; k++) {	/* pad 11 (ADC channel 0) ramp 0..1000 */
		f[0] = (u16)(k * 100);
		tick(f);
	}
	CHECK_EQ(padscope_test_state(), 1);	/* triggered at 200, recording */
	CHECK_EQ(padscope_test_chan(), 0);
	CHECK_EQ(padscope_test_pad(6), 0);	/* history before the first frame */
	CHECK_EQ(padscope_test_pad(7), 100);
	CHECK_EQ(padscope_test_pad(8), 200);	/* trigger sample is t = 0 */
	CHECK_EQ(padscope_test_pad(9), 300);
	CHECK_EQ(padscope_test_pad(8 + 8), 1000);
	CHECK_EQ(padscope_test_nb(8), 77);
	CHECK_EQ(padscope_test_nb(7), 77);	/* pre-trigger neighbours too */
	CHECK_EQ(g_n, 0);
}

static void test_stale_count(void)
{
	u16 f[64], g[64];

	reset();
	memset(f, 0, sizeof f);
	memcpy(g, f, sizeof g);
	g[5] = 9;
	tick(f);
	CHECK_EQ(padscope_test_stale(), 0);	/* first frame only stored */
	tick(f);
	CHECK_EQ(padscope_test_stale(), 1);
	tick(g);
	CHECK_EQ(padscope_test_stale(), 1);
}

static void test_capture_dump_paced(void)
{
	u16 f[64];
	int k;

	reset();
	memset(f, 0, sizeof f);
	tick(f);
	tick(f);			/* one stale tick */
	for (k = 0; k < 130; k++) {
		f[0] = (u16)(k == 0 ? 300 : 4000);	/* crosses 200, stays high */
		f[40] = (u16)(k + 1);		/* keeps the frames distinct (not stale) */
		tick(f);
	}
	CHECK_EQ(padscope_test_state(), 2);	/* complete, waiting for release */
	f[0] = 50;
	for (k = 0; k < 49; k++) {
		f[40]++;
		tick(f);
	}
	CHECK_EQ(g_n, 0);
	f[40]++;
	tick(f);			/* 50th tick below 100 */
	CHECK_EQ(g_n, 1);
	for (k = 0; k < PADSCOPE_MSG_GAP_TICKS - 1; k++) {
		f[40]++;
		tick(f);
	}
	CHECK_EQ(g_n, 1);		/* part 1 held back for the gap */
	for (k = 0; k < 3 && g_n < 2; k++) {
		f[40]++;
		tick(f);
	}
	CHECK_EQ(g_n, 2);
	CHECK(g_gap_min >= PADSCOPE_MSG_GAP_TICKS);
	for (k = 0; k < 30; k++)
		tick(f);
	CHECK_EQ(g_n, 2);
	CHECK_EQ(g_max_per_tick, 1);

	CHECK(g_len[0] < 320 && g_len[1] < 320);
	CHECK_EQ(g_msg[0][0], 0xF0);
	CHECK_EQ(g_msg[0][1], 0x00);
	CHECK_EQ(g_msg[0][2], 0x20);
	CHECK_EQ(g_msg[0][3], 0x29);
	CHECK_EQ(g_msg[0][4], 0x7D);
	CHECK_EQ(g_msg[0][5], 1);
	CHECK_EQ(g_msg[0][6], 0);
	CHECK_EQ(g_msg[0][7], 11);		/* pad index */
	CHECK_EQ(g_msg[0][8], 1);		/* stale lo */
	CHECK_EQ(g_msg[0][9], 0);		/* stale hi */
	CHECK_EQ(g_len[0], 10 + 256 + 1);
	CHECK_EQ(g_msg[0][g_len[0] - 1], 0xF7);
	CHECK_EQ(unpack(g_msg[0] + 10 + 2 * 8), 300);	/* t = 0 */
	CHECK_EQ(unpack(g_msg[0] + 10 + 2 * 9), 4000);
	CHECK_EQ(g_msg[1][5], 1);
	CHECK_EQ(g_msg[1][6], 1);
	CHECK_EQ(g_len[1], 7 + 256 + 1);
	CHECK_EQ(g_msg[1][g_len[1] - 1], 0xF7);
	CHECK_EQ(padscope_test_state(), 0);
}

static void test_idle_dump(void)
{
	u16 f[64];
	reset();
	memset(f, 0, sizeof f);
	f[3] = 30;
	tick(f);
	f[3] = 10;
	tick(f);
	f[3] = 399;
	tick(f);
	f[3] = 400;			/* not idle: ignored */
	tick(f);
	padscope_dump();
	CHECK_EQ(g_n, 0);		/* only a request */
	tick(f);
	CHECK_EQ(g_n, 1);
	ticks(f, PADSCOPE_MSG_GAP_TICKS - 1);
	CHECK_EQ(g_n, 1);
	ticks(f, 2);
	CHECK_EQ(g_n, 2);
	ticks(f, 30);
	CHECK_EQ(g_n, 2);
	CHECK_EQ(g_max_per_tick, 1);
	CHECK_EQ(g_msg[0][5], 2);
	CHECK_EQ(g_msg[0][6], 0);
	CHECK_EQ(g_len[0], 7 + 128 + 1);
	CHECK_EQ(g_msg[1][5], 2);
	CHECK_EQ(g_msg[1][6], 1);
	CHECK_EQ(unpack(g_msg[0] + 7 + 2 * 3), 10);	/* min */
	CHECK_EQ(unpack(g_msg[1] + 7 + 2 * 3), 399);	/* max */
	CHECK_EQ(unpack(g_msg[0] + 7), 0);

	/* stats were reset after the dump: a new dump shows only new data */
	f[3] = 55;
	tick(f);
	padscope_dump();
	ticks(f, 2 * PADSCOPE_MSG_GAP_TICKS + 4);
	CHECK_EQ(g_n, 4);
	CHECK_EQ(unpack(g_msg[2] + 7 + 2 * 3), 55);
	CHECK_EQ(unpack(g_msg[3] + 7 + 2 * 3), 55);
}

static void test_settle_timeout(void)
{
	u16 f[64];
	int k;

	reset();
	memset(f, 0, sizeof f);
	tick(f);
	f[0] = 4000;			/* pad goes down and stays down */
	for (k = 0; k < 130; k++) {
		f[40]++;
		tick(f);
	}
	CHECK_EQ(padscope_test_state(), 2);
	for (k = 0; k < 900; k++) {
		f[40]++;
		tick(f);
	}
	CHECK_EQ(g_n, 0);
	for (k = 0; k < 100 && g_n == 0; k++) {
		f[40]++;
		tick(f);
	}
	CHECK_EQ(g_n, 1);		/* sent anyway, so new captures can start */
}

static void test_dump_during_send_kept(void)
{
	u16 f[64];

	reset();
	memset(f, 0, sizeof f);
	tick(f);
	padscope_dump();
	tick(f);			/* part 0 out, dump in progress */
	padscope_dump();		/* second press while sending */
	ticks(f, 4 * PADSCOPE_MSG_GAP_TICKS);
	CHECK_EQ(g_n, 4);		/* both dumps went out */
}

static void test_null_frame(void)
{
	reset();
	padscope_tick(0);
	CHECK_EQ(g_n, 0);
}

void test_padscope_all(void)
{
	RUN(test_pack12);
	RUN(test_capture_trigger);
	RUN(test_stale_count);
	RUN(test_capture_dump_paced);
	RUN(test_idle_dump);
	RUN(test_settle_timeout);
	RUN(test_dump_during_send_kept);
	RUN(test_null_frame);
}
