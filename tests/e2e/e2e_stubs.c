#include "e2e_stubs.h"

#include "app.h"
#include "midici/midici.h"
#include "platform/irq.h"
#include "platform/lib_private.h"
#include "platform/pads.h"
#include "platform/platform.h"
#include "ump/ump.h"
#include "usb/usb_compat.h"
#include "usb/usb_midi2.h"

void EP1_OUT_Callback(void);

#include <string.h>

#define E2E_FLASH_SIZE 1024u
#define E2E_ENTROPY_MAX 64

/* ---- Observables ---- */
uint8_t  e2e_leds[100][3];
uint8_t  e2e_setup_led[3];
uint8_t  e2e_flash[E2E_FLASH_SIZE];
int      e2e_flash_writes;
uint16_t e2e_adc[64];

struct e2e_midi_ev e2e_real_midi_log[64];
int e2e_real_midi_n;
int e2e_real_sysex_calls;
uint8_t e2e_real_sysex_port;
uint8_t e2e_real_sysex_buf[512];
uint16_t e2e_real_sysex_len;

/* ---- Platform / IRQ ---- */
void (*stub_irq_hook)(void) = 0;
int stub_irq_depth = 0;

static char e2e_serial[25] = "TEST0001";
static uint32_t e2e_entropy_seq[E2E_ENTROPY_MAX];
static int e2e_entropy_n = 0;
static int e2e_entropy_idx = 0;

/* ---- Pads / ADC ---- */
u16 ADC_Max[64];
u16 PadCalibration[4] = {256, 1023, 512, 769};

/* ---- Closed-lib & ST USB-FS state ---- */
DEVICE_INFO Device_Info;
DEVICE_INFO *pInformation = &Device_Info;
volatile uint32_t bDeviceState = 0;
uint8_t PowerMode = 0;
cx_fifo MIDIToDev, MIDIDinIn;
struct cx_fifo1k {
	uint32_t a, b;
	uint8_t buf[1024];
};
cx_fifo1k MIDIToHst, MIDIDinOut;
uint8_t DinRx[24], USBParserToDev[24];

struct e2e_usb_state {
	uint16_t ep_type[8];
	uint16_t ep_tx_status[8];
	uint16_t ep_rx_status[8];
	uint16_t ep_tx_addr[8];
	uint16_t ep_rx_addr[8];
	uint16_t ep_tx_count[8];
	uint16_t ep_rx_count[8];
	uint8_t  rx_packet[64];
	uint16_t rx_len;
	uint8_t  device_address;
	uint16_t btable;
};
static struct e2e_usb_state usb_st;

/* ---- Multi-packet TX UMP FIFO (decoded from USB_SIL_Write(0x81, ...)) ---- */
static uint32_t tx_fifo_words[E2E_TX_FIFO_WORDS];
static uint16_t tx_fifo_head = 0;
static uint16_t tx_fifo_tail = 0;
static uint16_t tx_fifo_count = 0;
static uint32_t tx_fifo_overflows = 0;

static void tx_fifo_push_word(uint32_t w)
{
	if (tx_fifo_count >= E2E_TX_FIFO_WORDS) {
		tx_fifo_overflows++;
		return;
	}
	tx_fifo_words[tx_fifo_head] = w;
	tx_fifo_head = (uint16_t)((tx_fifo_head + 1u) % E2E_TX_FIFO_WORDS);
	tx_fifo_count++;
}

void e2e_stubs_reset(void)
{
	memset(e2e_leds, 0, sizeof e2e_leds);
	memset(e2e_setup_led, 0, sizeof e2e_setup_led);
	memset(e2e_flash, 0xFF, sizeof e2e_flash);
	e2e_flash_writes = 0;
	memset(e2e_adc, 0, sizeof e2e_adc);

	memset(e2e_real_midi_log, 0, sizeof e2e_real_midi_log);
	e2e_real_midi_n = 0;
	e2e_real_sysex_calls = 0;
	e2e_real_sysex_port = 0;
	memset(e2e_real_sysex_buf, 0, sizeof e2e_real_sysex_buf);
	e2e_real_sysex_len = 0;

	stub_irq_hook = 0;
	stub_irq_depth = 0;

	e2e_set_serial(0);
	e2e_entropy_n = 0;
	e2e_entropy_idx = 0;
	memset(e2e_entropy_seq, 0, sizeof e2e_entropy_seq);

	memset(ADC_Max, 0, sizeof ADC_Max);
	PadCalibration[0] = 256;
	PadCalibration[1] = 1023;
	PadCalibration[2] = 512;
	PadCalibration[3] = 769;

	memset(&Device_Info, 0, sizeof Device_Info);
	pInformation = &Device_Info;
	bDeviceState = 0;
	PowerMode = 0;
	memset(&MIDIToDev, 0, sizeof MIDIToDev);
	memset(&MIDIDinIn, 0, sizeof MIDIDinIn);
	memset(&MIDIToHst, 0, sizeof MIDIToHst);
	memset(&MIDIDinOut, 0, sizeof MIDIDinOut);
	memset(DinRx, 0, sizeof DinRx);
	memset(USBParserToDev, 0, sizeof USBParserToDev);
	memset(&usb_st, 0, sizeof usb_st);

	tx_fifo_head = 0;
	tx_fifo_tail = 0;
	tx_fifo_count = 0;
	tx_fifo_overflows = 0;
}

void e2e_set_serial(const char *serial)
{
	size_t i = 0;
	const char *src = serial ? serial : "TEST0001";
	while (i < 24u && src[i] != '\0') {
		e2e_serial[i] = src[i];
		i++;
	}
	e2e_serial[i] = '\0';
}

void e2e_set_entropy(const uint32_t *seq, int count)
{
	if (!seq || count <= 0) {
		e2e_entropy_n = 0;
		e2e_entropy_idx = 0;
		return;
	}
	if (count > E2E_ENTROPY_MAX)
		count = E2E_ENTROPY_MAX;
	for (int i = 0; i < count; i++)
		e2e_entropy_seq[i] = seq[i];
	e2e_entropy_n = count;
	e2e_entropy_idx = 0;
}

void e2e_host_send_ep1_raw(const uint8_t *bytes, uint16_t len)
{
	uint16_t n = len > 64u ? 64u : len;
	if (!bytes)
		n = 0;
	if (n > 0)
		memcpy(usb_st.rx_packet, bytes, n);
	usb_st.rx_len = n;
	EP1_OUT_Callback();
}

void e2e_host_send_ump(const uint32_t *words, uint16_t nwords)
{
	uint16_t off = 0;
	if (!words || nwords == 0)
		return;
	while (off < nwords) {
		if (usb_midi2_alt() &&
		    (ump_rx_free_words() < 16u || midici_rx_free_packets() < 8u)) {
			ump_rx_poll();
		}
		if (usb_midi2_alt() &&
		    (midici_tx_free_packets() < 8u ||
		     (nwords > 16u && midici_tx_free_packets() < 80u))) {
			for (uint16_t k = 0; k < 32u && midici_tx_pending() > 0u; k++) {
				uint16_t pending = midici_tx_pending();
				if (e2e_ep1_tx_is_valid())
					e2e_ep1_tx_ack();
				__wrap_usbmidi_poll();
				if (midici_tx_pending() >= pending)
					break;
				if (midici_tx_free_packets() >= 80u)
					break;
			}
		}
		uint16_t chunk = (uint16_t)(nwords - off);
		if (chunk > 16u)
			chunk = 16u;
		uint8_t pkt[64];
		for (uint16_t i = 0; i < chunk; i++) {
			uint32_t w = words[off + i];
			pkt[4u * i + 0u] = (uint8_t)(w & 0xFFu);
			pkt[4u * i + 1u] = (uint8_t)((w >> 8) & 0xFFu);
			pkt[4u * i + 2u] = (uint8_t)((w >> 16) & 0xFFu);
			pkt[4u * i + 3u] = (uint8_t)((w >> 24) & 0xFFu);
		}
		e2e_host_send_ep1_raw(pkt, (uint16_t)(chunk * 4u));
		off = (uint16_t)(off + chunk);
	}
}

void e2e_set_device_state(uint32_t state)
{
	bDeviceState = state;
}

uint8_t e2e_ep1_rx_is_valid(void)
{
	return usb_st.ep_rx_status[1] == EP_RX_VALID ? 1u : 0u;
}

uint8_t e2e_ep1_tx_is_valid(void)
{
	return usb_st.ep_tx_status[1] == EP_TX_VALID ? 1u : 0u;
}

void e2e_ep1_tx_ack(void)
{
	usb_st.ep_tx_status[1] = EP_TX_NAK;
}

void e2e_host_consume_in(void)
{
	e2e_ep1_tx_ack();
}

uint16_t e2e_tx_count(void)
{
	return tx_fifo_count;
}

uint16_t e2e_tx_pop_ump(uint32_t *out_words, uint16_t max_words)
{
	uint16_t n = 0;
	if (!out_words || max_words == 0)
		return 0;
	while (n < max_words && tx_fifo_count > 0) {
		out_words[n++] = tx_fifo_words[tx_fifo_tail];
		tx_fifo_tail = (uint16_t)((tx_fifo_tail + 1u) % E2E_TX_FIFO_WORDS);
		tx_fifo_count--;
	}
	return n;
}

uint32_t e2e_tx_overflows(void)
{
	return tx_fifo_overflows;
}

/* ---- Platform HAL ---- */

void platform_product_instance_id(char out[25])
{
	size_t i = 0;
	while (i < 24u && e2e_serial[i] != '\0') {
		out[i] = e2e_serial[i];
		i++;
	}
	out[i] = '\0';
}

uint32_t platform_entropy32(void)
{
	if (e2e_entropy_idx < e2e_entropy_n)
		return e2e_entropy_seq[e2e_entropy_idx++];
	return 0x12345678u + (uint32_t)e2e_entropy_idx++ * 0x01010101u;
}

/* ---- Firmware HAL (app.h / pads.h) ---- */

void hal_plot_led(u8 type, u8 index, u8 r, u8 g, u8 b)
{
	if (type == TYPESETUP) {
		e2e_setup_led[0] = r;
		e2e_setup_led[1] = g;
		e2e_setup_led[2] = b;
	} else if (type == TYPEPAD && index < 100u) {
		e2e_leds[index][0] = r;
		e2e_leds[index][1] = g;
		e2e_leds[index][2] = b;
	}
}

void hal_read_led(u8 type, u8 index, u8 *r, u8 *g, u8 *b)
{
	if (type == TYPESETUP) {
		*r = e2e_setup_led[0];
		*g = e2e_setup_led[1];
		*b = e2e_setup_led[2];
	} else if (type == TYPEPAD && index < 100u) {
		*r = e2e_leds[index][0];
		*g = e2e_leds[index][1];
		*b = e2e_leds[index][2];
	} else {
		*r = 0;
		*g = 0;
		*b = 0;
	}
}

void hal_read_flash(u32 offset, u8 *data, u32 length)
{
	if (!data || offset >= E2E_FLASH_SIZE || length > E2E_FLASH_SIZE - offset)
		return;
	memcpy(data, &e2e_flash[offset], length);
}

void hal_write_flash(u32 offset, const u8 *data, u32 length)
{
	if (!data || offset >= E2E_FLASH_SIZE || length > E2E_FLASH_SIZE - offset)
		return;
	memcpy(&e2e_flash[offset], data, length);
	e2e_flash_writes++;
}

u8 hal_read_device_id(void)
{
	return 0;
}

u8 hal_read_layout_text(void)
{
	return 0;
}

void hal_send_midi(u8 port, u8 status, u8 data1, u8 data2)
{
	__wrap_hal_send_midi(port, status, data1, data2);
}

void hal_send_sysex(u8 port, const u8 *data, u16 length)
{
	__wrap_hal_send_sysex(port, data, length);
}

void pad_setcalibration(u16 threshold, u16 c1, u16 aftertouch_lo, u16 aftertouch_hi)
{
	PadCalibration[0] = threshold;
	PadCalibration[1] = c1;
	PadCalibration[2] = aftertouch_lo;
	PadCalibration[3] = aftertouch_hi;
}

/* ---- ST USB-FS library stand-ins ---- */

void SetBTABLE(uint16_t addr)
{
	usb_st.btable = addr;
}

void SetEPType(uint8_t ep, uint16_t type)
{
	usb_st.ep_type[ep & 7u] = type;
}

void SetEPTxStatus(uint8_t ep, uint16_t state)
{
	usb_st.ep_tx_status[ep & 7u] = state;
}

void SetEPRxStatus(uint8_t ep, uint16_t state)
{
	usb_st.ep_rx_status[ep & 7u] = state;
}

void SetEPRxValid(uint8_t ep)
{
	usb_st.ep_rx_status[ep & 7u] = EP_RX_VALID;
}

void SetEPTxAddr(uint8_t ep, uint16_t addr)
{
	usb_st.ep_tx_addr[ep & 7u] = addr;
}

void SetEPRxAddr(uint8_t ep, uint16_t addr)
{
	usb_st.ep_rx_addr[ep & 7u] = addr;
}

void SetEPTxCount(uint8_t ep, uint16_t count)
{
	usb_st.ep_tx_count[ep & 7u] = count;
}

void SetEPRxCount(uint8_t ep, uint16_t count)
{
	usb_st.ep_rx_count[ep & 7u] = count;
}

void ClearDTOG_RX(uint8_t ep)
{
	(void)ep;
}

void ClearDTOG_TX(uint8_t ep)
{
	(void)ep;
}

uint16_t GetEPTxStatus(uint8_t ep)
{
	return usb_st.ep_tx_status[ep & 7u];
}

uint16_t GetEPRxStatus(uint8_t ep)
{
	return usb_st.ep_rx_status[ep & 7u];
}

void Clear_Status_Out(uint8_t ep)
{
	(void)ep;
}

uint32_t USB_SIL_Init(void)
{
	return 0;
}

uint32_t USB_SIL_Read(uint8_t ep_addr, uint8_t *buf)
{
	if (usb_st.rx_len > 0 && buf)
		memcpy(buf, usb_st.rx_packet, usb_st.rx_len);
	usb_st.ep_rx_status[ep_addr & 7u] = EP_RX_NAK;
	return usb_st.rx_len;
}

uint32_t USB_SIL_Write(uint8_t ep_addr, uint8_t *buf, uint32_t n)
{
	if (!buf)
		return n;
	if (ep_addr == 0x81u) {
		for (uint32_t i = 0; i <= n && (n - i) >= 4u; i += 4u) {
			uint32_t w = (uint32_t)buf[i] |
			             ((uint32_t)buf[i + 1u] << 8) |
			             ((uint32_t)buf[i + 2u] << 16) |
			             ((uint32_t)buf[i + 3u] << 24);
			tx_fifo_push_word(w);
		}
	}
	return n;
}

uint8_t *Standard_GetDescriptorData(uint16_t len, ONE_DESCRIPTOR *desc)
{
	uint16_t off = pInformation->Ctrl_Info.Usb_wOffset;
	if (len == 0) {
		pInformation->Ctrl_Info.Usb_wLength = (uint16_t)(desc->Descriptor_Size - off);
		return 0;
	}
	return desc->Descriptor + off;
}

void SetDeviceAddress(uint8_t addr)
{
	usb_st.device_address = addr;
}

void NOP_Process(void)
{
}

RESULT PowerOn(void)
{
	return USB_SUCCESS;
}

/* ---- Closed-lib c_midi / fifo / __real_* stand-ins ---- */

void USBConfigured(void)
{
}

void USBUnconfigured(void)
{
}

void fifo_flush(cx_fifo *f)
{
	f->head = 0;
	f->tail = 0;
}

void fifo1k_flush(cx_fifo1k *f)
{
	f->b = f->a;
}

uint8_t fifo1k_unempty(cx_fifo1k *f)
{
	return ((f->a ^ f->b) & 0x3FFu) != 0 ? 1u : 0u;
}

uint8_t fifo_unempty(cx_fifo *f)
{
	return f->head != f->tail ? 1u : 0u;
}

void fifo_push1(cx_fifo *f, uint8_t b)
{
	f->buf[f->tail++] = b;
}

void midi_parse_rx_byte_stream(cx_fifo *f, void *parser, uint8_t port)
{
	(void)f;
	(void)parser;
	(void)port;
}

void __real_midi_parsers_init(void)
{
	for (int i = 0; i < 24; i++)
		USBParserToDev[i] = (uint8_t)(0xA0u + i);
}

void __real_midi_parsers_poll(void)
{
}

void __real_usbmidi_poll(void)
{
}

void __real_midi_flush_windows_buffer(void)
{
}

void __real_hal_send_midi(uint8_t port, uint8_t status, uint8_t d1, uint8_t d2)
{
	if (e2e_real_midi_n < 64) {
		e2e_real_midi_log[e2e_real_midi_n].port = port;
		e2e_real_midi_log[e2e_real_midi_n].status = status;
		e2e_real_midi_log[e2e_real_midi_n].d1 = d1;
		e2e_real_midi_log[e2e_real_midi_n].d2 = d2;
		e2e_real_midi_n++;
	}
}

void __real_hal_send_sysex(uint8_t port, const uint8_t *data, uint16_t len)
{
	e2e_real_sysex_calls++;
	e2e_real_sysex_port = port;
	uint16_t copy_len = len > sizeof e2e_real_sysex_buf ? (uint16_t)sizeof e2e_real_sysex_buf : len;
	e2e_real_sysex_len = copy_len;
	if (copy_len > 0 && data)
		memcpy(e2e_real_sysex_buf, data, copy_len);
}
