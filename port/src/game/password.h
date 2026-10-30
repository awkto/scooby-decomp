/* Password codec (GitLab #21, docs/passwords.md, docs/subsystems/menus.md).
 *
 * Pure C, no emulator dependencies, so the later save-slot feature (#28) can call it as is.
 * It is the exact algorithm of the ROM's password encoder ($008660 + $00873E) and checker
 * ($009298 + $00876C), and is unit-tested against tools/py/password.py (port/tools/password_test.sh).
 *
 * A password is 50 characters of the alphabet "ABCDEFGHIJKLMNOPQRSTUVWXYZ!+#%*?" (5 bits each).
 * It carries the start room and the first 29 bytes of the story-state block ($FF2A00: story
 * flags + inventory) behind a checksum.
 */
#ifndef SCOOBY_PASSWORD_H
#define SCOOBY_PASSWORD_H
#include <stdbool.h>
#include <stdint.h>

#define PASSWORD_STATE_BYTES 29   /* bytes of $FF2A00.. that are saved */
#define PASSWORD_CHARS       50   /* characters of a password */
#define PASSWORD_ALPHABET    "ABCDEFGHIJKLMNOPQRSTUVWXYZ!+#%*?"

typedef struct password_state {
	uint8_t room;                         /* start room number ($FF06AC; 2..22 Blake, 2..31 Carnival) */
	uint8_t state[PASSWORD_STATE_BYTES];  /* story flags / inventory, $FF2A00..+28 */
} password_state;

/* The 32 raw bytes r[0..31] a password is made of (50 symbols * 5 bits, MSB first, r[31] and
 * the last two bits unused).  Exposed because the menu code keeps them in scratch RAM. */
#define PASSWORD_RAW_BYTES 32

/* Checksum over (room, state): c = room; rotate left through X twice (X starts 0); then for
 * each state byte: rotate left through X, xor the byte. */
uint8_t password_checksum(uint8_t room, const uint8_t state[PASSWORD_STATE_BYTES]);

/* st -> 50 characters + NUL.  Only rooms 0..31 survive a round trip (the room is stored in
 * 5 bits); a larger room is encoded the way the ROM does it (checksum over the full byte,
 * low 5 bits stored) and will fail to decode. */
void password_encode(const password_state *st, char out[PASSWORD_CHARS + 1]);

/* 50 characters (no separators; see password_decode_display) -> st.  Returns false on a wrong
 * length, a character outside the alphabet, or a bad checksum; *st is then unspecified. */
bool password_decode(const char *pw, password_state *st);

/* Symbol values (0..31) <-> raw bytes, the bit packing shared by the ROM's encoder/checker. */
void password_pack(const uint8_t sym[PASSWORD_CHARS], uint8_t raw[PASSWORD_RAW_BYTES]);
void password_unpack(const uint8_t raw[PASSWORD_RAW_BYTES], uint8_t sym[PASSWORD_CHARS]);

/* raw bytes <-> (room, state).  password_state_from_raw returns the checksum verdict.
 * They are the xor-chain scramble of $00873E (encode) / $00876C (decode) plus the checksum. */
void password_raw_from_state(const password_state *st, uint8_t raw[PASSWORD_RAW_BYTES]);
bool password_state_from_raw(const uint8_t raw[PASSWORD_RAW_BYTES], password_state *st);

/* The on-screen form of the 68k menu ($FF08BC): two rows, each 5 groups of 5 characters
 * followed by a space, then a NUL: 2 * 31 bytes.  The first 25 characters are row 0. */
#define PASSWORD_DISPLAY_BYTES 62
void password_to_display(const char pw[PASSWORD_CHARS], char out[PASSWORD_DISPLAY_BYTES]);
/* inverse (the characters are taken from the group positions; the separators are ignored) */
bool password_decode_display(const char buf[PASSWORD_DISPLAY_BYTES], password_state *st);

#endif
