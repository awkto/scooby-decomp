/* Unit test of the password codec against tools/py/password.py.
 *   password_test VECTORS     VECTORS: lines "ROOM HEX29 PASSWORD50 [bad]" made by
 *                             tools/py/password.py (see port/tools/password_test.sh)
 * Checks, per line: encode(room, state) == PASSWORD; decode(PASSWORD) round-trips with a valid
 * checksum; a "bad" line (one character altered by the generator so the checksum fails) must
 * be rejected.  Also the display (2 x 31 byte) form round-trips. */
#include "password.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: password_test VECTORS\n"); return 2; }
	FILE *f = fopen(argv[1], "r");
	if (!f) { perror(argv[1]); return 2; }
	char line[512];
	int n = 0, bad = 0, fails = 0;
	while (fgets(line, sizeof line, f)) {
		int room;
		char hex[80], pw[80], tag[16] = "";
		if (sscanf(line, "%d %79s %79s %15s", &room, hex, pw, tag) < 3)
			continue;
		password_state st = { .room = (uint8_t)room }, back;
		for (int i = 0; i < PASSWORD_STATE_BYTES; i++) {
			unsigned v;
			sscanf(hex + 2 * i, "%2x", &v);
			st.state[i] = (uint8_t)v;
		}
		n++;
		if (!strcmp(tag, "bad")) {
			bad++;
			if (password_decode(pw, &back)) { printf("FAIL accepted bad password %s\n", pw); fails++; }
			continue;
		}
		char out[PASSWORD_CHARS + 1];
		password_encode(&st, out);
		if (strcmp(out, pw)) { printf("FAIL encode room %d: %s != %s\n", room, out, pw); fails++; }
		if (!password_decode(pw, &back) || back.room != st.room ||
		    memcmp(back.state, st.state, sizeof st.state)) {
			printf("FAIL decode %s\n", pw);
			fails++;
		}
		char disp[PASSWORD_DISPLAY_BYTES];
		password_to_display(pw, disp);
		if (!password_decode_display(disp, &back) || back.room != st.room) {
			printf("FAIL display %s\n", pw);
			fails++;
		}
	}
	fclose(f);
	printf("password_test: %d vectors (%d bad), %d failures\n", n, bad, fails);
	return fails ? 1 : 0;
}
