#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes a NUL-terminated product instance id (at most 24 characters) into out. */
void platform_product_instance_id(char out[25]);

void platform_start_muid_entropy(void);
uint32_t platform_muid_entropy(void);

#ifdef __cplusplus
}
#endif

#endif
