#include "midici/ci_json.h"

/* ---- writer ---- */

void ci_json_out_init(struct ci_json_out *o, char *buf, uint16_t win_start, uint16_t win_len)
{
	o->buf = buf;
	o->win_start = win_start;
	o->win_len = buf ? win_len : 0;
	o->pos = 0;
}

static void put(struct ci_json_out *o, char c)
{
	if (o->pos >= o->win_start && (uint16_t)(o->pos - o->win_start) < o->win_len)
		o->buf[o->pos - o->win_start] = c;
	if (o->pos != 0xFFFF)
		o->pos++;
}

void ci_json_rawn(struct ci_json_out *o, const char *s, uint16_t n)
{
	while (n--)
		put(o, *s++);
}

void ci_json_raw(struct ci_json_out *o, const char *s)
{
	while (*s)
		put(o, *s++);
}

void ci_json_str(struct ci_json_out *o, const char *s)
{
	put(o, '"');
	for (; *s; s++) {
		char c = *s;
		if (c == '"' || c == '\\')
			put(o, '\\');
		else if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E)
			c = '?';
		put(o, c);
	}
	put(o, '"');
}

void ci_json_int(struct ci_json_out *o, int32_t v)
{
	char d[11];
	uint8_t n = 0;
	uint32_t u;

	if (v < 0) {
		put(o, '-');
		u = (uint32_t)(-(v + 1)) + 1u;
	} else {
		u = (uint32_t)v;
	}
	do {
		d[n++] = (char)('0' + u % 10u);
		u /= 10u;
	} while (u);
	while (n)
		put(o, d[--n]);
}

void ci_json_bool(struct ci_json_out *o, uint8_t v)
{
	ci_json_raw(o, v ? "true" : "false");
}

void ci_json_key(struct ci_json_out *o, const char *key)
{
	ci_json_str(o, key);
	put(o, ':');
}

/* ---- reader ---- */

#define MAX_DEPTH 8

void ci_json_it_init(struct ci_json_it *it, const char *json, uint16_t len)
{
	it->p = json;
	it->end = json + len;
	it->state = 0;
}

static void ws(struct ci_json_it *it)
{
	while (it->p < it->end && (*it->p == ' ' || *it->p == '\t' || *it->p == '\r' || *it->p == '\n'))
		it->p++;
}

/* past a string whose opening quote is at it->p; the content is [*s, *s + *n) */
static int string(struct ci_json_it *it, const char **s, uint16_t *n)
{
	const char *start;

	if (it->p >= it->end || *it->p != '"')
		return -1;
	start = ++it->p;
	while (it->p < it->end && *it->p != '"') {
		if ((unsigned char)*it->p < 0x20)
			return -1;
		if (*it->p == '\\') {
			it->p++;
			if (it->p >= it->end)
				return -1;
		}
		it->p++;
	}
	if (it->p >= it->end)
		return -1;
	*s = start;
	*n = (uint16_t)(it->p - start);
	it->p++;
	return 0;
}

static int literal(struct ci_json_it *it, const char *word)
{
	while (*word) {
		if (it->p >= it->end || *it->p != *word)
			return -1;
		it->p++;
		word++;
	}
	return 0;
}

static int is_digit(char c)
{
	return c >= '0' && c <= '9';
}

/* any number; *m gets CI_JSON_INT when it is a plain integer of <= 9 digits */
static int number(struct ci_json_it *it, struct ci_json_member *m)
{
	uint8_t neg = 0, digits = 0, plain = 1;
	int32_t v = 0;

	if (it->p < it->end && *it->p == '-') {
		neg = 1;
		it->p++;
	}
	if (it->p >= it->end || !is_digit(*it->p))
		return -1;
	while (it->p < it->end && is_digit(*it->p)) {
		if (digits < 9)
			v = v * 10 + (*it->p - '0');
		else
			plain = 0;
		digits++;
		it->p++;
	}
	if (it->p < it->end && *it->p == '.') {
		plain = 0;
		it->p++;
		if (it->p >= it->end || !is_digit(*it->p))
			return -1;
		while (it->p < it->end && is_digit(*it->p))
			it->p++;
	}
	if (it->p < it->end && (*it->p == 'e' || *it->p == 'E')) {
		plain = 0;
		it->p++;
		if (it->p < it->end && (*it->p == '+' || *it->p == '-'))
			it->p++;
		if (it->p >= it->end || !is_digit(*it->p))
			return -1;
		while (it->p < it->end && is_digit(*it->p))
			it->p++;
	}
	m->type = plain ? CI_JSON_INT : CI_JSON_OTHER;
	m->num = neg ? -v : v;
	return 0;
}

static int scalar(struct ci_json_it *it, struct ci_json_member *m)
{
	if (it->p >= it->end)
		return -1;
	switch (*it->p) {
	case '"':
		m->type = CI_JSON_STR;
		return string(it, &m->str, &m->str_len);
	case 't':
		m->type = CI_JSON_BOOL;
		m->num = 1;
		return literal(it, "true");
	case 'f':
		m->type = CI_JSON_BOOL;
		m->num = 0;
		return literal(it, "false");
	case 'n':
		m->type = CI_JSON_NULL;
		return literal(it, "null");
	default:
		return number(it, m);
	}
}

/* skips one nested object or array starting at it->p; brackets are matched */
static int nested(struct ci_json_it *it)
{
	char stack[MAX_DEPTH];
	uint8_t depth = 0;
	uint8_t expect_value = 1;	/* after '[', ',' in an array, ':' */
	struct ci_json_member dummy;

	for (;;) {
		ws(it);
		if (it->p >= it->end)
			return -1;
		char c = *it->p;
		if (c == '{' || c == '[') {
			if (!expect_value || depth >= MAX_DEPTH)
				return -1;
			stack[depth++] = (char)(c == '{' ? '}' : ']');
			it->p++;
			ws(it);
			if (it->p < it->end && *it->p == stack[depth - 1]) {
				it->p++;
				depth--;
				expect_value = 0;
				if (depth == 0)
					return 0;
				continue;
			}
			if (c == '{') {
				const char *s;
				uint16_t n;
				if (string(it, &s, &n) != 0)
					return -1;
				ws(it);
				if (it->p >= it->end || *it->p != ':')
					return -1;
				it->p++;
			}
			expect_value = 1;
			continue;
		}
		if (expect_value) {
			if (scalar(it, &dummy) != 0)
				return -1;
			expect_value = 0;
			continue;
		}
		if (depth == 0)
			return -1;
		if (c == stack[depth - 1]) {
			it->p++;
			depth--;
			if (depth == 0)
				return 0;
			continue;
		}
		if (c != ',')
			return -1;
		it->p++;
		if (stack[depth - 1] == '}') {
			const char *s;
			uint16_t n;
			ws(it);
			if (string(it, &s, &n) != 0)
				return -1;
			ws(it);
			if (it->p >= it->end || *it->p != ':')
				return -1;
			it->p++;
		}
		expect_value = 1;
	}
}

static int fail(struct ci_json_it *it)
{
	it->state = 3;
	return -1;
}

int ci_json_next(struct ci_json_it *it, struct ci_json_member *m)
{
	if (it->state == 2)
		return 0;
	if (it->state == 3)
		return -1;
	ws(it);
	if (it->state == 0) {
		if (it->p >= it->end || *it->p != '{')
			return fail(it);
		it->p++;
		ws(it);
		if (it->p < it->end && *it->p == '}') {
			it->p++;
			goto done;
		}
		it->state = 1;
	} else {
		if (it->p >= it->end)
			return fail(it);
		if (*it->p == '}') {
			it->p++;
			goto done;
		}
		if (*it->p != ',')
			return fail(it);
		it->p++;
		ws(it);
	}
	if (string(it, &m->key, &m->key_len) != 0)
		return fail(it);
	ws(it);
	if (it->p >= it->end || *it->p != ':')
		return fail(it);
	it->p++;
	ws(it);
	m->str = 0;
	m->str_len = 0;
	m->num = 0;
	if (it->p < it->end && (*it->p == '{' || *it->p == '[')) {
		m->type = CI_JSON_OTHER;
		if (nested(it) != 0)
			return fail(it);
	} else if (scalar(it, m) != 0) {
		return fail(it);
	}
	ws(it);
	return 1;
done:
	ws(it);
	if (it->p != it->end)
		return fail(it);
	it->state = 2;
	return 0;
}

static uint8_t equals(const char *a, uint16_t n, const char *s)
{
	uint16_t i;
	for (i = 0; i < n; i++)
		if (s[i] != a[i])
			return 0;
	return s[n] == 0;
}

uint8_t ci_json_key_is(const struct ci_json_member *m, const char *key)
{
	return equals(m->key, m->key_len, key);
}

uint8_t ci_json_str_is(const struct ci_json_member *m, const char *s)
{
	return m->type == CI_JSON_STR && equals(m->str, m->str_len, s);
}
