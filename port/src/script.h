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
 *   echo TEXT        stderr and "FRAME TEXT" in OUTDIR/peek.log (live drivers sync on it)
 *   trace/untrace    ignored (harness only)
 *   peek LABEL ADDR [W]   append "frame LABEL value" (W bytes, hex) to OUTDIR/peek.log
 *   poke ADDR W VAL  write W (1/2/4) bytes of work RAM, big-endian hex VAL (cheat; #19 script routes)
 * Mouse (port/src/mouse, docs/mouse.md; game screen pixels 0..255 x 0..223):
 *   mouse X Y        move the mouse      mouse leave   move it off the game image
 *   click L|R|M [X Y]   click a button (optionally moving there first)
 *   wheel N          wheel steps (+ up, - down)
 * The script FILE may be a FIFO: commands are then executed as they arrive (tools/py/session.py live control).
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
