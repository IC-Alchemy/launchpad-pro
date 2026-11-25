#ifndef TUNING_H
#define TUNING_H

#include <stdint.h>

/* Intonation: what pitch each degree of the pad ladder sounds. A tuning is a
 * table of pitches for one period (usually an octave); a degree is an index
 * on the ladder that table repeats into, counted from the tonic (the root).
 * The library is Pico2Seq's (src/pico2seq-core/tuning), with the same ids so
 * the two can name a tuning to each other.
 *
 * Note numbers: a pad plays note 60 + root_degree + D, where D is its ladder
 * degree from the tonic in octave 4 and root_degree is the degree the root's
 * semitone maps to. With a twelve-note tuning that is the 12-EDO note number
 * the pad always had; with any other tuning consecutive notes are
 * consecutive degrees, the usual keyboard mapping for microtonal synths.
 *
 * Pitch: 1/32768 of a 12-EDO semitone ("fine" units), integer only. */

#define TUNING_COUNT 29
#define TUNING_DEFAULT_ID 0      /* 12-EDO */
#define TUNING_FINE_PER_SEMI 32768
#define A4_TENTHS_MIN 4150       /* 415.0 Hz */
#define A4_TENTHS_MAX 4660       /* 466.0 Hz */
#define A4_TENTHS_DEFAULT 4400   /* 440.0 Hz */

enum tuning_family { TUNING_EQUAL, TUNING_JUST, TUNING_TEMPERAMENT, TUNING_INDIAN, TUNING_XENO };

struct tuning_def {
	uint8_t id;            /* stable: Pico2Seq's id, family * 32 + index */
	uint8_t family;        /* enum tuning_family */
	uint8_t degrees;       /* N: degrees per period, 1..53 */
	uint8_t nscales;       /* entries in scales; 0: every twelve-note scale */
	uint8_t default_scale; /* what a change to this tuning picks when the
	                        * current scale isn't offered */
	uint32_t period;       /* fine units: 393216 for an octave */
	uint8_t period_frac;   /* and 1/256ths of one: a non-octave period
	                        * repeats, so its rounding must not add up */
	const uint32_t *pitch; /* N entries in fine units, ascending from 0; 0 for
	                        * an equal division (computed) */
	const uint8_t *chroma; /* 12 entries: the degree each semitone slot
	                        * plays; 0 for the nearest degree */
	const uint8_t *scales; /* the scales offered (SCALES indexes), page order */
	const char *name;
};

extern const struct tuning_def TUNINGS[TUNING_COUNT];

/* The tuning with this id, or 0. */
const struct tuning_def *tuning_find(uint8_t id);
/* The tuning with this id, or 12-EDO for an unknown id. */
const struct tuning_def *tuning_get(uint8_t id);
/* Library position (the pad on the Tuning page), or -1. */
int8_t tuning_index(uint8_t id);

/* floor(v / d) for d > 0 */
int32_t tuning_floor_div(int32_t v, int32_t d);
/* Fine units of ladder degree D above the tonic (negative D allowed). */
int32_t tuning_degree_fine(const struct tuning_def *t, int32_t degree);
/* The ladder degree a 12-EDO semitone slot (a classic scale step) plays:
 * itself in a twelve-note tuning, the chroma map where there is one, else
 * the nearest degree (ties to the lower one). Pico2Seq's slotToDegree. */
int32_t tuning_slot_to_degree(const struct tuning_def *t, int32_t slot);

/* Whether the tuning offers this scale, and the scale on page slot i (or
 * 0xFF past the end). */
uint8_t tuning_offers(const struct tuning_def *t, uint8_t scale);
uint8_t tuning_scale_at(const struct tuning_def *t, uint8_t i);
uint8_t tuning_scale_slots(const struct tuning_def *t);

/* Fine units of 12 * log2(a4 / 440). */
int32_t tuning_a4_fine(uint16_t a4_tenths);
/* 12-EDO at A4 = 440: notes need no pitch attribute. */
uint8_t tuning_is_standard(uint8_t id, uint16_t a4_tenths);
/* The MIDI 2.0 Pitch 7.9 attribute (semitones * 512) of note n played with
 * this tuning, root (0..11) and A4, clamped to 0..0xFFFF. */
uint16_t tuning_pitch79(uint8_t id, uint8_t root, uint16_t a4_tenths, uint8_t note);
/* 60 + root_degree: the note number of degree 0 below octave shifts. */
int16_t tuning_tonic_note(const struct tuning_def *t, uint8_t root);

#endif
