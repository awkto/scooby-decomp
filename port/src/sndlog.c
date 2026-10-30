/* Z80 mailbox logger: SCOOBY_SNDLOG=file.  Records every 68k write into the sound mailbox ring
 * (Z80 RAM $1B40-$1B7F) as "FRAME RINGSLOT VALUE" so a --hle all run can be compared with
 * --hle none (tools/py/sndcmp.py parses the byte stream into messages).  Works for writes made
 * by emulated 68k code and by C (bus_w8). */
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>

static FILE *g_log;

static void on_access(int type, int width, unsigned addr, unsigned value)
{
	if (type != EMU_HOOK_WRITE || !g_log)
		return;
	addr &= 0xFFFFFF;
	if (addr >= 0xA01B40 && addr < 0xA01B80) {
		fprintf(g_log, "%ld %u %02X\n", emu_frame_count(), addr - 0xA01B40, value & 0xFF);
		fflush(g_log);
	}
}

void sndlog_init(void)
{
	const char *p = getenv("SCOOBY_SNDLOG");
	if (!p || !*p)
		return;
	g_log = fopen(p, "w");
	if (g_log)
		emu_add_hook(on_access);
}
