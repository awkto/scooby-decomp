/* Sound API for C code and future host features (docs/subsystems/sound.md).
 *
 * The Z80 sound driver stays emulated.  The 68k talks to it through a 64-byte mailbox ring
 * in Z80 RAM; this module is the C replacement of the 68k side (the $1F6D7A stub).  Nothing
 * here needs an hle_ctx: call these from any C code, including host code.
 *
 * All commands are asynchronous: they are queued for the driver, which consumes them the
 * next time its main loop polls the mailbox. */
#ifndef SCOOBY_SOUND_H
#define SCOOBY_SOUND_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOUND_ID_SCOOBY_THEME  0x2F   /* title music; the boot "current music" */
#define SOUND_ID_STOP_ALL      0xFF   /* sound_stop() argument: stop everything */

/* Driver command bytes (second byte of a mailbox message, after $FF). */
enum {
	SNDCMD_INIT       = 0x0B,   /* 4 x 24-bit data pointers (sound_driver_init) */
	SNDCMD_PLAY       = 0x10,   /* id */
	SNDCMD_STOP       = 0x12,   /* id ($FF = all) */
	SNDCMD_PAUSE      = 0x0C,   /* no args: silence, remember what runs */
	SNDCMD_RESUME     = 0x0D,   /* no args */
	SNDCMD_STOP_ALL   = 0x16,   /* no args: reset every channel record */
};

/* Start sound `id` (music or effect; only the low byte is sent).  Ids: docs/sound.md. */
void sound_play(uint32_t id);
/* Stop sound `id` (SOUND_ID_STOP_ALL stops all, like the stop-all command does). */
void sound_stop(uint32_t id);
/* In-game pause: silence everything, keeping the channel state. */
void sound_pause(void);
/* Undo sound_pause(). */
void sound_resume(void);
/* Stop and forget every sound (the game does this on every screen change). */
void sound_stop_all(void);
/* The id of the background music the game wants (long at $FF06A6, set by the game on
 * room/scene changes and replayed after the Sound Test).  This is game state, not driver
 * state: it says what the game last asked for as music. */
uint32_t music_current(void);

/* Load the Z80 driver and the 4 data pointers (what the boot does once).  ptr[0..3] are
 * the ROM addresses of driver data tables 0..3. */
void sound_driver_init(const uint32_t ptr[4]);

/* Low-level: queue one message ($FF, cmd, args...) in a single bus transaction. */
void sound_send(uint8_t cmd, const uint8_t *args, int nargs);

/* Host hook, called before a message from sound_play/stop/pause/resume/stop_all is queued.
 * Return true to swallow it (the driver never sees it): lets a host replace the music or
 * change volume.  NULL (default) = everything reaches the Z80. */
typedef bool (*sound_hook_fn)(uint8_t cmd, const uint8_t *args, int nargs);
void sound_set_hook(sound_hook_fn fn);

#endif
