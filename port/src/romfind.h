/* Finding the ROM (GitLab #28).  The game needs the user's own ROM; it is never shipped.
 * Order: --rom, the ini's `rom`, a ROM next to the binary, rom/baserom.md in the working
 * directory, then a file picker (zenity / kdialog), then a "drop the ROM on the window"
 * screen.  Whatever is found is verified by sha1 first and remembered in the ini. */
#ifndef SCOOBY_ROMFIND_H
#define SCOOBY_ROMFIND_H
#include <stdbool.h>
#include <stddef.h>

/* is `path` a readable file whose sha1 is the expected one? (err: why not) */
bool rom_verify(const char *path, char *err, size_t errlen);

/* Unattended part: ini path, files next to the binary, working directory.  Returns a verified
 * path in `out`, or false.  `explicit_path` (--rom) is returned as is, without checking: the
 * emulator start refuses a wrong file with its own message. */
bool rom_locate(const char *explicit_path, const char *ini_rom, const char *argv0, char *out, size_t n);

/* Interactive part: picker, then (if SDL video works) a drop window.  Returns a verified path. */
bool rom_ask_user(char *out, size_t n, char *err, size_t errlen);

#endif
