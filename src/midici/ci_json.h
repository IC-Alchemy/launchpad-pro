#ifndef CI_JSON_H
#define CI_JSON_H

#include <stdint.h>

/*
 * The small, bounded JSON the Property Exchange code needs. No heap, no
 * recursion, nothing copied that the caller did not size.
 *
 * Writer: every byte is "written" at a running position, but only the bytes
 * whose position falls inside [win_start, win_start + win_len) land in buf.
 * Rendering a resource once with win_len 0 measures it; rendering it again
 * with a window fills one PE chunk. So a reply never needs a body buffer.
 *
 * Reader: walks the members of one flat object. Values are strings (escapes
 * are skipped over, not decoded), integers, booleans and null; nested objects
 * and arrays are skipped. Malformed input stops the walk with an error.
 */

struct ci_json_out {
	char *buf;
	uint16_t win_start;
	uint16_t win_len;
	uint16_t pos;	/* bytes written so far, saturating at 0xFFFF */
};

void ci_json_out_init(struct ci_json_out *o, char *buf, uint16_t win_start, uint16_t win_len);
void ci_json_raw(struct ci_json_out *o, const char *s);
void ci_json_rawn(struct ci_json_out *o, const char *s, uint16_t n);
/* A quoted string; " and \ are escaped, control and non-ASCII bytes become '?'. */
void ci_json_str(struct ci_json_out *o, const char *s);
void ci_json_int(struct ci_json_out *o, int32_t v);
void ci_json_bool(struct ci_json_out *o, uint8_t v);
/* "key": (the quoted key and the colon) */
void ci_json_key(struct ci_json_out *o, const char *key);

enum ci_json_type { CI_JSON_STR, CI_JSON_INT, CI_JSON_BOOL, CI_JSON_NULL, CI_JSON_OTHER };

struct ci_json_member {
	const char *key;	/* not terminated */
	uint16_t key_len;
	uint8_t type;		/* enum ci_json_type */
	const char *str;	/* CI_JSON_STR: the raw text between the quotes */
	uint16_t str_len;
	int32_t num;		/* CI_JSON_INT value, CI_JSON_BOOL 0/1 */
};

struct ci_json_it {
	const char *p;
	const char *end;
	uint8_t state;	/* 0 before '{', 1 inside, 2 done, 3 error */
};

void ci_json_it_init(struct ci_json_it *it, const char *json, uint16_t len);
/* 1 and *m filled, 0 at the end of the object, -1 if the input is malformed
 * (not an object, bad token, nesting deeper than 8, trailing garbage). */
int ci_json_next(struct ci_json_it *it, struct ci_json_member *m);

/* Whole-object checks, for headers. */
uint8_t ci_json_key_is(const struct ci_json_member *m, const char *key);
uint8_t ci_json_str_is(const struct ci_json_member *m, const char *s);

#endif
