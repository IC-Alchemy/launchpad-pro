#ifndef CI_IDENTITY_H
#define CI_IDENTITY_H

/*
 * One identity for everything that announces the Launchpad: the UMP Device
 * Identity Notification (ump_stream.c), the MIDI-CI Discovery Reply and the
 * Identity Reply, and the Property Exchange DeviceInfo resource. MIDI2Sound
 * compares them, so they must never diverge.
 */

#define CI_MANUFACTURER_0 0x00
#define CI_MANUFACTURER_1 0x20
#define CI_MANUFACTURER_2 0x29
#define CI_FAMILY 0x0051u	/* 14-bit, sent LSB first */
#define CI_MODEL 0x0000u	/* 14-bit, sent LSB first */
#define CI_SW_REV_0 0x00
#define CI_SW_REV_1 0x01
#define CI_SW_REV_2 0x00
#define CI_SW_REV_3 0x00

/* UMP Endpoint Name, and the model string in the PE DeviceInfo resource. */
#define CI_ENDPOINT_NAME "Launchpad Pro MIDI 2.0"
#define CI_MANUFACTURER_NAME "Novation"
#define CI_FAMILY_NAME "Launchpad"

/* UMP Stream Device Identity Notification words 1..3 (word 0 is the header). */
#define CI_UMP_IDENTITY_WORD1 \
	(((uint32_t)CI_MANUFACTURER_0 << 16) | ((uint32_t)CI_MANUFACTURER_1 << 8) | CI_MANUFACTURER_2)
#define CI_UMP_IDENTITY_WORD2                                                              \
	(((uint32_t)(CI_FAMILY & 0x7F) << 24) | ((uint32_t)((CI_FAMILY >> 7) & 0x7F) << 16) | \
	 ((uint32_t)(CI_MODEL & 0x7F) << 8) | (uint32_t)((CI_MODEL >> 7) & 0x7F))
#define CI_UMP_IDENTITY_WORD3                                                              \
	(((uint32_t)CI_SW_REV_0 << 24) | ((uint32_t)CI_SW_REV_1 << 16) |                   \
	 ((uint32_t)CI_SW_REV_2 << 8) | (uint32_t)CI_SW_REV_3)

#endif
