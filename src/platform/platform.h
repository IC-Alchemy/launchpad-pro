#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

/* Writes a NUL-terminated product instance id (at most 24 characters) into out. */
void platform_product_instance_id(char out[25]);

/* 32 bits of boot-varying randomness. The STM32F103 has no hardware TRNG, so
 * the firmware mixes the 96-bit unique ID with timer jitter (SysTick, USB
 * frame number) sampled whenever it is called; successive calls differ. */
uint32_t platform_entropy32(void);

#endif
