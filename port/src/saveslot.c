/* Game saves: see saveslot.h. */
#include "saveslot.h"

#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "emu.h"
#include "hle.h"
#include "password.h"

/* RAM the saves read and the loader writes (docs/passwords.md, data/ram.txt) */
#define RAM_MYSTERY    0xFF06AA   /* word: 0 Blake's Hotel, 1 Ha Ha Carnival */
#define RAM_ROOM       0xFF06AC   /* word: current room */
#define RAM_ROOM_NAME  0xFF06D2   /* long: absolute pointer to the room name string (ROM) */
#define RAM_START_ROOM 0xFF0A90   /* word: start room, set by the password accept code */
#define RAM_MENU_FLAGS 0xFF09ED   /* byte: bit0 game started, bit2 started from a password */
#define RAM_STATE      0xFF2A00   /* the story block; its first 29 bytes are in a password */
#define RAM_TEXT_BUF   0xFF08BC   /* the password entry screen's text, 2 x 31 bytes */
#define RAM_VBLANK_FLAGS 0xFF09DC /* byte: bit 7 a script is running */
#define RAM_09E9       0xFF09E9   /* byte: bit 4 the main loop does not open the pause menu */

static char g_dir[1024];

void slots_init(const char *dir) { snprintf(g_dir, sizeof g_dir, "%s", dir); }

static void slot_path(int n, char *p, size_t len) { snprintf(p, len, "%s/slot%d.sav", g_dir, n); }

static uint16_t ram16(uint32_t a) { return (uint16_t)((RAM8(a) << 8) | RAM8(a + 1)); }

bool slot_read(int n, slot_info *out)
{
	memset(out, 0, sizeof *out);
	if (n < 1 || n > SLOT_COUNT)
		return false;
	char p[1200];
	slot_path(n, p, sizeof p);
	return slot_read_file(p, out);
}

bool slot_read_file(const char *p, slot_info *out)
{
	char line[256];
	memset(out, 0, sizeof *out);
	FILE *f = fopen(p, "r");
	if (!f)
		return false;
	bool magic = false, pw = false;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *eq = strchr(line, '=');
		if (!strcmp(line, "scooby-save 1"))
			magic = true;
		if (!eq)
			continue;
		*eq++ = 0;
		if (!strcmp(line, "mystery")) out->mystery = atoi(eq);
		else if (!strcmp(line, "room")) out->room = atoi(eq);
		else if (!strcmp(line, "roomname")) snprintf(out->roomname, sizeof out->roomname, "%s", eq);
		else if (!strcmp(line, "date")) snprintf(out->date, sizeof out->date, "%s", eq);
		else if (!strcmp(line, "playtime")) out->playtime = atol(eq);
		else if (!strcmp(line, "password")) { snprintf(out->password, sizeof out->password, "%s", eq); pw = true; }
	}
	fclose(f);
	password_state st;
	out->used = magic && pw && out->mystery >= 0 && out->mystery <= 1 && password_decode(out->password, &st);
	if (out->used)
		out->room = st.room;
	return out->used;
}

bool slot_erase(int n)
{
	char p[1200];
	slot_path(n, p, sizeof p);
	return unlink(p) == 0;
}

bool slot_in_game(void)
{
	unsigned room = ram16(RAM_ROOM);
	return (RAM8(RAM_MENU_FLAGS) & 1) && room >= 2 && room <= 31 && ram16(RAM_MYSTERY) <= 1;
}

/* The game itself shows a password only in its pause menu, and the main loop opens that only between
 * scripts ($000D38: not while ram_vblank_flags bit 7 "script running" is set, not with $FF09E9 bit 4).
 * A save taken in the middle of a cut scene (e.g. the intro in The Mystery Machine, a room the player
 * never stands in) can be a password the game would never show, and may continue into a dead end
 * (#35).  So saves are taken only where the pause menu could be opened. */
bool slot_can_save(void)
{
	return slot_in_game() && !(RAM8(RAM_VBLANK_FLAGS) & 0x80) && !(RAM8(RAM_09E9) & 0x10);
}

static void room_name(char *out, size_t n)
{
	uint32_t p = ((uint32_t)ram16(RAM_ROOM_NAME) << 16) | ram16(RAM_ROOM_NAME + 2);
	size_t i = 0;
	if (p >= 0x200 && p < 0x3FFFF0)
		for (; i < n - 1 && i < 32; i++) {
			uint8_t c = ROM8(p + (uint32_t)i);
			if (c < 0x20 || c > 0x7E)
				break;
			out[i] = (char)c;
		}
	out[i] = 0;
}

bool slot_save_current(int n, long playtime, char *err, size_t errlen)
{
	if (n < 1 || n > SLOT_COUNT) {
		snprintf(err, errlen, "slot must be 1..%d", SLOT_COUNT);
		return false;
	}
	char path[1200];
	mkdir(g_dir, 0777);
	slot_path(n, path, sizeof path);
	return slot_save_file(path, playtime, err, errlen);
}

bool slot_save_file(const char *path, long playtime, char *err, size_t errlen)
{
	if (!slot_in_game()) {
		snprintf(err, errlen, "no mystery is running");
		return false;
	}
	if (!slot_can_save()) {
		snprintf(err, errlen, "not during a cut scene");
		return false;
	}
	password_state st;
	st.room = (uint8_t)ram16(RAM_ROOM);
	for (int i = 0; i < PASSWORD_STATE_BYTES; i++)
		st.state[i] = RAM8(RAM_STATE + (uint32_t)i);
	char pw[PASSWORD_CHARS + 1], name[40], date[24], tmp[1210];
	password_encode(&st, pw);
	room_name(name, sizeof name);
	time_t t = time(NULL);
	strftime(date, sizeof date, "%Y-%m-%d %H:%M", localtime(&t));
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		snprintf(err, errlen, "cannot write %s: %s", tmp, strerror(errno));
		return false;
	}
	fprintf(f, "scooby-save 1\nmystery=%u\nroom=%u\nroomname=%s\ndate=%s\nplaytime=%ld\npassword=%s\n",
	        ram16(RAM_MYSTERY), st.room, name, date, playtime, pw);
	if (fclose(f) || rename(tmp, path)) {
		snprintf(err, errlen, "cannot write %s: %s", path, strerror(errno));
		return false;
	}
	return true;
}

/* ------------------------------------------------------------------ loader */
/* The button script.  Waits are generous (title and menu transitions differ a little between
 * the pure 68k and the C menus); a failed attempt is repeated with longer waits. */
typedef struct { unsigned pad; int frames; int act; } step_t;
enum { ACT_NONE, ACT_POKE_TEXT, ACT_ARM };

#define MAX_STEPS 64
static struct {
	bool active, ok;
	slot_info slot;
	password_state st;
	step_t steps[MAX_STEPS];
	int nsteps, cur, left, attempt, waited;
	bool armed;
	long playtime;
	char msg[160];
} L;

static void add(unsigned pad, int frames, int act)
{
	L.steps[L.nsteps++] = (step_t){ pad, frames, act };
}

static void build(int slack)
{
	L.nsteps = 0;
	add(0, 1500 * slack, 0);                       /* power-on, logos, title */
	for (int i = 0; i < 3; i++) {                  /* three Starts: out of the title into the mystery menu */
		add(PAD_START, 6, 0);
		add(0, 150 * slack, 0);
	}
	if (L.slot.mystery) {                          /* PLAY HA HA CARNIVAL is the second entry */
		add(PAD_DOWN, 6, 0);
		add(0, 20 * slack, 0);
	}
	add(PAD_START, 6, 0);                          /* choose it: START / CONTINUE / CANCEL */
	add(0, 150 * slack, 0);
	add(PAD_DOWN, 6, 0);                           /* CONTINUE */
	add(0, 20 * slack, 0);
	add(PAD_START, 6, 0);                          /* ENTER PASSWORD */
	add(0, 300 * slack, 0);                        /* the screen comes up */
	add(0, 10, ACT_POKE_TEXT);                     /* type the whole password at once */
	add(PAD_UP, 6, 0);                             /* grid cursor (0,0) -> row 4 (the icon row) */
	add(0, 12, 0);
	for (int i = 0; i < 3; i++) {                  /* column 0 -> 5 = END, going left (wraps) */
		add(PAD_LEFT, 6, 0);
		add(0, 12, 0);
	}
	add(PAD_A, 6, ACT_ARM);                        /* END: the game's own checker */
	add(0, 600 * slack, 0);                        /* success jingle, 61-frame wait, game start */
}

static void begin_attempt(void)
{
	build(L.attempt + 1);
	L.cur = 0;
	L.left = L.steps[0].frames;
	L.armed = false;
	L.waited = 0;
	emu_reset();
}

bool slot_load_begin(int n, char *err, size_t errlen)
{
	char p[1200];
	if (n < 1 || n > SLOT_COUNT) {
		snprintf(err, errlen, "slot must be 1..%d", SLOT_COUNT);
		return false;
	}
	slot_path(n, p, sizeof p);
	if (!slot_load_begin_file(p, err, errlen)) {
		snprintf(err, errlen, "slot %d is empty or damaged", n);
		return false;
	}
	return true;
}

bool slot_load_begin_file(const char *path, char *err, size_t errlen)
{
	memset(&L, 0, sizeof L);
	if (!slot_read_file(path, &L.slot) || !password_decode(L.slot.password, &L.st)) {
		snprintf(err, errlen, "%s: empty or damaged game save", path);
		return false;
	}
	L.active = true;
	L.playtime = L.slot.playtime;
	begin_attempt();
	return true;
}

bool slot_loader_active(void) { return L.active; }
long slot_loaded_playtime(void) { return L.playtime; }

static bool state_matches(void)
{
	if (!(RAM8(RAM_MENU_FLAGS) & 4) || ram16(RAM_START_ROOM) != L.st.room)
		return false;
	for (int i = 0; i < PASSWORD_STATE_BYTES; i++)
		if (RAM8(RAM_STATE + (uint32_t)i) != L.st.state[i])
			return false;
	return true;
}

/* The cartridge bug of GitLab #35 (docs/passwords.md, "Cold-start password bug").  game_start
 * ($000CB8) runs the mystery's second start-up script with script_run_frame directly, without
 * setting ram_script_cb, the loop that if/else ($00274A) calls for a true then-body.  Only the
 * verb code (action_execute $1A2A/$1A66) leaves a non-zero value there; everything else saves and
 * restores it.  After power-on (the boot code clears work RAM) the pointer is still 0, and the
 * Ha Ha Carnival's start2 has one if/else (`if F07.0 == 1: ... else ...`, ROM $1BB488, bumper car
 * engine) whose then-body then jumps to $000000: the game hangs in the bus error loop at $892.
 * On a real console the same password works if anything was played since power-on (the pointer
 * then holds script_run_frame or script_run).  The loader gives the game that "warm" value:
 * script_run_frame, the execute-mode loop the body was written for and the one a played
 * game leaves after any verb that ran a script.  It is not part of the game state (no save,
 * no password holds it) and is written only when it is still 0. */
#define RAM_SCRIPT_CB    0xFF0878   /* long: ram_script_cb */
#define SCRIPT_RUN_FRAME 0x002406   /* script_run_frame: execute-mode script loop */
static void warm_script_cb(void)
{
	if (ram16(RAM_SCRIPT_CB) == 0 && ram16(RAM_SCRIPT_CB + 2) == 0) {
		RAM8(RAM_SCRIPT_CB + 2) = (uint8_t)(SCRIPT_RUN_FRAME >> 8);
		RAM8(RAM_SCRIPT_CB + 3) = (uint8_t)SCRIPT_RUN_FRAME;
	}
}

static void poke_text(void)
{
	char disp[PASSWORD_DISPLAY_BYTES];
	password_to_display(L.slot.password, disp);
	for (int i = 0; i < PASSWORD_DISPLAY_BYTES; i++)
		RAM8(RAM_TEXT_BUF + (uint32_t)i) = (uint8_t)disp[i];
	warm_script_cb();
}

bool slot_loader_step(unsigned *pad)
{
	if (!L.active)
		return false;
	/* done as soon as the game has started from the password (then a little run-in) */
	if (L.armed && state_matches()) {
		if (++L.waited > 30) {
			L.active = false;
			L.ok = true;
			snprintf(L.msg, sizeof L.msg, "LOADED SLOT");
			return false;
		}
		*pad = 0;
		return true;
	}
	while (L.left <= 0) {
		if (++L.cur >= L.nsteps) {
			/* script over without the game having taken the password: try again, slower */
			if (++L.attempt >= 3) {
				L.active = false;
				L.ok = false;
				snprintf(L.msg, sizeof L.msg, "LOAD FAILED: THE GAME DID NOT ACCEPT THE PASSWORD");
				return false;
			}
			begin_attempt();
			continue;
		}
		L.left = L.steps[L.cur].frames;
	}
	const step_t *s = &L.steps[L.cur];
	if (L.left == s->frames) {
		if (s->act == ACT_POKE_TEXT)
			poke_text();
		else if (s->act == ACT_ARM)
			L.armed = true;
	}
	*pad = s->pad;
	L.left--;
	return true;
}

bool slot_loader_result(char *msg, size_t n)
{
	snprintf(msg, n, "%s", L.msg);
	return L.ok;
}
