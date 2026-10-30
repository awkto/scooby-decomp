/* Password codec: see password.h.  Formats are documented in docs/passwords.md. */
#include "password.h"
#include <string.h>

static const char alphabet[33] = PASSWORD_ALPHABET;

/* rotate-left-through-X by one bit (roxl.b #1): returns the new byte, updates *x */
static uint8_t roxl8(uint8_t v, int *x)
{
	int out = v >> 7;
	v = (uint8_t)((v << 1) | *x);
	*x = out;
	return v;
}

uint8_t password_checksum(uint8_t room, const uint8_t state[PASSWORD_STATE_BYTES])
{
	int x = 0;
	uint8_t c = room;
	c = roxl8(c, &x);
	c = roxl8(c, &x);
	for (int i = 0; i < PASSWORD_STATE_BYTES; i++) {
		c = roxl8(c, &x);
		c ^= state[i];
	}
	return c;
}

/* raw[0] = (check ^ room) & $1F, raw[1] = check, raw[2+i] = raw[1+i] ^ state[i] with raw[0]
 * standing in for raw[1] as the first link: a running xor chain, so that a changed state byte
 * changes every symbol after it. */
void password_raw_from_state(const password_state *st, uint8_t raw[PASSWORD_RAW_BYTES])
{
	uint8_t check = password_checksum(st->room, st->state);
	uint8_t prev = (uint8_t)((check ^ st->room) & 0x1F);
	raw[0] = prev;
	raw[1] = check;
	for (int i = 0; i < PASSWORD_STATE_BYTES; i++) {
		prev ^= st->state[i];
		raw[2 + i] = prev;
	}
	raw[31] = 0;
}

bool password_state_from_raw(const uint8_t raw[PASSWORD_RAW_BYTES], password_state *st)
{
	st->room = (uint8_t)((raw[1] ^ raw[0]) & 0x1F);
	uint8_t prev = raw[0];
	for (int i = 0; i < PASSWORD_STATE_BYTES; i++) {
		st->state[i] = (uint8_t)(raw[2 + i] ^ prev);
		prev = raw[2 + i];
	}
	return password_checksum(st->room, st->state) == raw[1];
}

void password_pack(const uint8_t sym[PASSWORD_CHARS], uint8_t raw[PASSWORD_RAW_BYTES])
{
	memset(raw, 0, PASSWORD_RAW_BYTES);
	for (int i = 0; i < PASSWORD_CHARS; i++)
		for (int b = 0; b < 5; b++) {
			int bit = i * 5 + b;
			if ((sym[i] >> (4 - b)) & 1)
				raw[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));
		}
}

void password_unpack(const uint8_t raw[PASSWORD_RAW_BYTES], uint8_t sym[PASSWORD_CHARS])
{
	for (int i = 0; i < PASSWORD_CHARS; i++) {
		int v = 0;
		for (int b = 0; b < 5; b++) {
			int bit = i * 5 + b;
			v = (v << 1) | ((raw[bit >> 3] >> (7 - (bit & 7))) & 1);
		}
		sym[i] = (uint8_t)v;
	}
}

void password_encode(const password_state *st, char out[PASSWORD_CHARS + 1])
{
	uint8_t raw[PASSWORD_RAW_BYTES], sym[PASSWORD_CHARS];
	password_raw_from_state(st, raw);
	password_unpack(raw, sym);
	for (int i = 0; i < PASSWORD_CHARS; i++)
		out[i] = alphabet[sym[i]];
	out[PASSWORD_CHARS] = 0;
}

bool password_decode(const char *pw, password_state *st)
{
	uint8_t sym[PASSWORD_CHARS], raw[PASSWORD_RAW_BYTES];
	if (strlen(pw) != PASSWORD_CHARS)
		return false;
	for (int i = 0; i < PASSWORD_CHARS; i++) {
		const char *p = strchr(alphabet, pw[i]);   /* pw[i] != 0 here: the length is checked */
		if (!p)
			return false;
		sym[i] = (uint8_t)(p - alphabet);
	}
	password_pack(sym, raw);
	return password_state_from_raw(raw, st);
}

void password_to_display(const char pw[PASSWORD_CHARS], char out[PASSWORD_DISPLAY_BYTES])
{
	char *o = out;
	for (int row = 0; row < 2; row++) {
		for (int group = 0; group < 5; group++) {
			for (int k = 0; k < 5; k++)
				*o++ = *pw++;
			*o++ = ' ';
		}
		*o++ = 0;
	}
}

bool password_decode_display(const char buf[PASSWORD_DISPLAY_BYTES], password_state *st)
{
	char pw[PASSWORD_CHARS + 1];
	char *o = pw;
	for (int row = 0; row < 2; row++)
		for (int group = 0; group < 5; group++)
			for (int k = 0; k < 5; k++)
				*o++ = buf[row * 31 + group * 6 + k];
	*o = 0;
	return password_decode(pw, st);
}
