/* Input/command scripts, compatible with tools/harness scripts:
 *   run N            advance N frames with the held buttons (none by default)
 *   press BTNS N     hold BTNS (letters U D L R A B C S) for N frames, then release
 *   hold BTNS | hold -   set / clear buttons held during `run`
 *   shot NAME        OUTDIR/NAME.ppm          ram NAME   OUTDIR/NAME.ram (68k byte order)
 *   vdp NAME         OUTDIR/NAME.{vram,cram,vsram,reg}
 *   save NAME / load NAME   OUTDIR/NAME.state (NAME containing '/' is used as a path)
 *   hle SPEC         e.g. "hle none", "hle -pad_read", "hle all"
 *   capture ADDR N DIR [EVERY [SKIP]]   start entry-state capture (see capture.h)
 *   regs             print frame number, PC and D0-D7/A0-A7 to stderr
 *   echo TEXT
 * Blank lines and lines starting with '#' are ignored. */
#ifndef SCOOBY_SCRIPT_H
#define SCOOBY_SCRIPT_H
#include <stdbool.h>

typedef struct script script;

script *script_open(const char *path, const char *outdir);
/* Execute commands until the next frame must be emulated; returns true and the pad mask
 * for that frame, or false when the script has ended. */
bool script_step(script *s, unsigned *pad);
int script_errors(const script *s);
void script_close(script *s);

#endif
