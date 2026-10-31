/* Game saves (GitLab #28): 8 slots that hold the game's own password state (start room + the
 * 29 story/inventory bytes at $FF2A00, docs/passwords.md) plus a little metadata.
 *
 * A slot file is a few lines of text in the data dir (~/.local/share/scooby-decomp/slotN.sav);
 * `password=` is the 50-character password of the moment, so a slot can also be typed into the
 * game by hand.  Loading does exactly that, by pressing the game's own buttons: reset, title,
 * mystery menu, CONTINUE, the typed password (the text is written into the entry buffer rather
 * than walked in key by key), END.  The game's own checker and start code do the rest, so the
 * result is the password-entered state by construction (port/tools/slottest.sh proves it).
 * The raw emulator states on F5/F9 ("quick states") are a different, separate feature. */
#ifndef SCOOBY_SAVESLOT_H
#define SCOOBY_SAVESLOT_H
#include <stdbool.h>
#include <stddef.h>

#define SLOT_COUNT 8

typedef struct {
	bool used;
	int mystery;               /* 0 Blake's Hotel, 1 Ha Ha Carnival */
	int room;
	char roomname[40];
	char date[24];             /* local time, "YYYY-MM-DD HH:MM" */
	long playtime;             /* emulated frames in the game */
	char password[51];
} slot_info;

void slots_init(const char *dir);
bool slot_read(int n, slot_info *out);                      /* n = 1..SLOT_COUNT */
bool slot_erase(int n);
/* the same on any file path (the tests store many saves along a walkthrough) */
bool slot_read_file(const char *path, slot_info *out);
bool slot_save_file(const char *path, long playtime, char *err, size_t errlen);
bool slot_load_begin_file(const char *path, char *err, size_t errlen);

/* a mystery is running (the pause menu exists, a room is loaded)? */
bool slot_in_game(void);
/* ... and the game's pause menu could be opened now (no cut scene running): saving is allowed */
bool slot_can_save(void);
/* store the current game in slot n (fails during a cut scene) */
bool slot_save_current(int n, long playtime, char *err, size_t errlen);

/* Load: resets the emulator and starts feeding the game the buttons that enter the slot's
 * password.  Call slot_loader_step once per frame in place of the player's pad until it
 * returns false.  slot_loader_result() then says whether the game state matches the slot. */
bool slot_load_begin(int n, char *err, size_t errlen);
bool slot_loader_active(void);
bool slot_loader_step(unsigned *pad);
bool slot_loader_result(char *msg, size_t n);              /* true: loaded and verified */
long slot_loaded_playtime(void);

/* the unpacked slot password as (room, 29 bytes); used by the tests */
#endif
