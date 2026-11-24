/*
 * STM32F103 implementation of platform.h (firmware only; the host tests use
 * the stand-in in tests/stubs/stubs.c).
 */
#include "platform/platform.h"
#include <stdint.h>

/* 96-bit unique device ID (RM0008 §30.2), byte-addressable */
#define UID_BASE 0x1FFFF7E8u
#define UID_BYTES 12

/* Cortex-M3 registers. USB arrival timing is asynchronous to the CPU clock. */
#define CORE_REG(addr) (*(volatile uint32_t *)(addr))
#define DEMCR CORE_REG(0xE000EDFCu)
#define DWT_CTRL CORE_REG(0xE0001000u)
#define DWT_CYCCNT CORE_REG(0xE0001004u)
#define SYST_CVR CORE_REG(0xE000E018u)

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

void platform_start_muid_entropy(void)
{
	DEMCR |= 1u << 24; /* TRCENA */
	DWT_CTRL |= 1u; /* CYCCNTENA */
}

uint32_t platform_muid_entropy(void)
{
	const volatile uint8_t *uid = (const volatile uint8_t *)UID_BASE;
	uint32_t seed = DWT_CYCCNT ^ (SYST_CVR << 16);
	for (int i = 0; i < UID_BYTES; i++)
		seed = (seed ^ uid[i]) * 16777619u;
	return seed;
}
