/* Thin wrapper around the statically linked Genesis Plus GX core (libretro API, called
 * directly - no dlopen). Everything outside emu.c / hle/ talks to the emulator through this. */
#ifndef SCOOBY_EMU_H
#define SCOOBY_EMU_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCOOBY_ROM_SHA1 "ccc9542d964de5bdc1e9101620cae40dd98e7127"

/* Genesis pad buttons (bitmask for emu_set_pad) */
enum {
	PAD_UP = 1 << 0, PAD_DOWN = 1 << 1, PAD_LEFT = 1 << 2, PAD_RIGHT = 1 << 3,
	PAD_A = 1 << 4, PAD_B = 1 << 5, PAD_C = 1 << 6, PAD_START = 1 << 7,
};

/* Locate the ROM: explicit path, else rom/baserom.md relative to cwd, the repo root the
 * binary was built from, or the executable's parent dirs. Returns NULL if not found. */
const char *emu_find_rom(const char *explicit_path, const char *argv0);

/* Verify sha1 and boot the core. Returns false (with message in err) on failure;
 * a ROM with the wrong sha1 is refused. */
bool emu_init(const char *rom_path, const char *workdir, char *err, size_t errlen);
void emu_shutdown(void);

void emu_set_pad(int port, unsigned buttons);
unsigned emu_get_pad(int port);
unsigned emu_pad_from_letters(const char *s);   /* "UDLRABCS" -> mask */

void emu_run_frame(void);
void emu_reset(void);                     /* back to the power-on state, frame count 0 */
long emu_frame_count(void);

/* last video frame, RGB565 */
const uint16_t *emu_video(unsigned *w, unsigned *h, size_t *pitch_bytes);
/* interleaved stereo int16 samples produced by the last emu_run_frame() */
const int16_t *emu_audio(size_t *frames);
double emu_fps(void);
double emu_sample_rate(void);

size_t emu_state_size(void);
bool emu_state_save(void *buf, size_t size);
bool emu_state_load(const void *buf, size_t size);
bool emu_state_save_file(const char *path);
bool emu_state_load_file(const char *path);

bool emu_write_ppm(const char *path);         /* current frame */
bool emu_write_ram(const char *path);         /* 64 KiB work RAM, 68k byte order */
bool emu_write_vdp(const char *prefix);       /* prefix.{vram,cram,vsram,reg} like the harness */

/* 68k bus-event hook fan-out (cpu_hook of the patched core). type: EMU_HOOK_* */
enum { EMU_HOOK_EXEC = 1, EMU_HOOK_READ = 2, EMU_HOOK_WRITE = 4,
       EMU_HOOK_VRAM_W = 16, EMU_HOOK_CRAM_W = 64, EMU_HOOK_VSRAM_W = 256 };
typedef void (*emu_hook_fn)(int type, int width, unsigned addr, unsigned value);
void emu_add_hook(emu_hook_fn fn);
void emu_remove_hook(emu_hook_fn fn);

/* misc state for overlays/tools */
uint8_t emu_ram8(uint32_t addr);
uint16_t emu_ram16(uint32_t addr);
uint32_t emu_ram32(uint32_t addr);

#endif
