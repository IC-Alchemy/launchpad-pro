/*
 * STM32F103 implementation of platform.h (firmware only; the host tests use
 * the stand-in in tests/stubs/stubs.c).
 */
#include "platform/platform.h"
#include <stdint.h>

/* 96-bit unique device ID (RM0008 §30.2), byte-addressable */
#define UID_BASE 0x1FFFF7E8u
#define UID_BYTES 12

#define SYST_VAL (*(const volatile uint32_t *)0xE000E018u)	/* SysTick current value */
#define USB_FNR (*(const volatile uint32_t *)0x40005C48u)	/* USB frame number (RM0008 §23.5.3) */

/* murmur3 32-bit finalizer */
static uint32_t mix32(uint32_t h)
{
	h ^= h >> 16;
	h *= 0x85EBCA6Bu;
	h ^= h >> 13;
	h *= 0xC2B2AE35u;
	h ^= h >> 16;
	return h;
}

uint32_t platform_entropy32(void)
{
	static uint32_t pool;
	static uint8_t seeded;

	if (!seeded) {
		const volatile uint32_t *uid = (const volatile uint32_t *)UID_BASE;
		pool = mix32(uid[0] ^ mix32(uid[1] ^ mix32(uid[2])));
		seeded = 1;
	}
	pool = mix32(pool ^ SYST_VAL ^ (USB_FNR << 16) ^ 0x9E3779B9u);
	return pool;
}

void platform_product_instance_id(char out[25])
{
	static const char hex[16] = "0123456789ABCDEF";
	const volatile uint8_t *uid = (const volatile uint8_t *)UID_BASE;
	int i;

	for (i = 0; i < UID_BYTES; i++) {
		uint8_t b = uid[i];
		out[2 * i] = hex[b >> 4];
		out[2 * i + 1] = hex[b & 0xF];
	}
	out[2 * UID_BYTES] = 0;
}
