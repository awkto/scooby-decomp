/* Game-side hooks for mouse control (GitLab #23): what the host-side mouse module
 * (port/src/mouse/) needs to know about, and write into, the game's UI state.
 * Everything goes through the same RAM the original pad input path uses
 * (docs/subsystems/verbs.md section 6); no 68k code is replaced. */
#ifndef SCOOBY_MOUSEHOOK_H
#define SCOOBY_MOUSEHOOK_H
#include <stdbool.h>
#include <stdint.h>

/* game screen layout (256x224, H32) */
#define MG_SCREEN_W   256
#define MG_SCREEN_H   224
#define MG_ROOM_TOP   16     /* lines 0-15: status line (object names) */
#define MG_BAR_TOP    168    /* lines 168-223: verb bar / inventory */

typedef enum {
	MG_NONE,      /* not in the room game loop (title, intro, cut scenes before a room) */
	MG_FREE,      /* walking state: pointer mode closed, nothing scripted -> it may open */
	MG_POINTER,   /* pointer mode open (crosshair, verb bar live) */
	MG_BUSY,      /* a script / action runs or a special mode is on: buttons only */
} mg_mode;

mg_mode mg_mode_get(void);
bool mg_in_room(void);                 /* a room is loaded and the in-game VBlank runs */

/* pointer (crosshair) RAM: $FF06DC/$FF06DE.  Place it so that its hot spot is screen
 * pixel (sx, sy): centre of the crosshair in the room, cell under (sx,sy) on the bar. */
void mg_pointer_place(int sx, int sy);
void mg_pointer_get(int *x, int *y);   /* raw RAM values */

/* UI state */
int  mg_target(void);                  /* $FF06AE object under the pointer (0 none) */
int  mg_target2(void);                 /* $FF06B6 second object */
bool mg_second_object(void);           /* $FF09DE bit 0: "Use X with ..." waits for object 2 */
int  mg_verb(void);                    /* $FF06C0 selected verb (0 none) */
int  mg_default_verb(void);            /* $FF06C4 default verb of the hovered object */
bool mg_inventory_view(void);          /* $FF09DC bit 4 */
bool mg_bar_sliding(void);             /* $FF0ACB != $FF */
int  mg_room(void);                    /* $FF06AC */
/* has pad_read latched these buttons (emu.h PAD_* mask) as pressed in $FF09E0?
 * _any: at least one of them; plain: all of them */
bool mg_pad_latched(unsigned pad_mask);
bool mg_pad_latched_any(unsigned pad_mask);

/* The main loop's pointer hit test ($000E94-$001066, room area only), redone in C on
 * screen pixel (sx, sy) as if the crosshair were placed there with mg_pointer_place:
 * 0 none, 2 Scooby, n >= 3 object record n-3.  Read-only. */
int  mg_hit_test(int sx, int sy);
int  mg_object_default_verb(int target);   /* record+$16 (10 for Shaggy/Scooby) */

/* screen pixel -> world pixel of the room view (false outside lines 16-167) */
bool mg_screen_to_world(int sx, int sy, int *wx, int *wy);
void mg_shaggy_pos(int *x, int *y);

/* verb bar geometry (screen px): centre of the cell of verb `id` (1-10) in the verb view,
 * and what a click at (sx, sy) on the bar means */
bool mg_verb_cell_center(int verb_id, int *sx, int *sy);
typedef enum { BAR_NONE, BAR_VERB, BAR_TOGGLE, BAR_ITEM, BAR_PAGE_UP, BAR_PAGE_DOWN } mg_bar_hit;
mg_bar_hit mg_bar_at(int sx, int sy);
int  mg_verb_at(int sx, int sy);           /* verb id of the verb-view cell at (sx, sy) */
int  mg_inv_page(void);                    /* $FF06EC inventory row */
/* inventory view: the target the main loop ($0011B8) sets for the item cell at (sx, sy):
 * carried object + 3, or 0 past the end of the list */
int  mg_inventory_item_at(int sx, int sy);
void mg_page_arrow_center(bool down, int *sx, int *sy);

#endif
