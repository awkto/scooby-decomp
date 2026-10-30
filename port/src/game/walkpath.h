/* Click-to-walk support (GitLab #23): queries on the room's per-pixel collision map,
 * a pathfinder, the room's walk-on (exit/hotspot) rectangles, and a driver that walks
 * Shaggy along a path through the game's own auto-walk state machine (player_update).
 *
 * Nothing here is an HLE replacement: these are host-side helpers that read the game's
 * RAM/ROM between frames and write only the variables the game's own walk-to code
 * ($005002 shaggy_walk_to) writes.  docs/subsystems/walking.md sections 2-9 and
 * docs/mouse.md describe the data and the design.  All coordinates are world (room)
 * pixels unless stated otherwise. */
#ifndef SCOOBY_WALKPATH_H
#define SCOOBY_WALKPATH_H
#include <stdbool.h>
#include <stdint.h>

typedef struct { int x, y; } walk_point;
typedef struct { int x0, y0, x1, y1; int record; int id; } walk_rect;   /* x1/y1 exclusive */

/* ---- collision queries (same answer as walk_blocked_probe $0073D6) */
int  walk_room_w(void);                       /* room size in pixels (0 when no room) */
int  walk_room_h(void);
bool walk_is_blocked(int x, int y);           /* outside the room or collision nibble != 0 */
uint16_t walk_zone_word(int x, int y);        /* raw zone word; bit 15 = "behind" */

/* ---- walk-on rectangles of the objects in the current room (main loop $0012CC) */
int  walk_triggers(walk_rect *out, int max);
int  walk_trigger_at(int x, int y, walk_rect *out);   /* index into walk_triggers or -1 */
bool walk_trigger_of_record(int record, walk_rect *out);
/* can Shaggy, standing at (sx, sy), set foot in the rectangle? */
bool walk_rect_reachable(int sx, int sy, const walk_rect *r);

/* ---- pathfinding
 * Plans a walk for Shaggy from (sx,sy) to the reachable pixel nearest to (gx,gy), or,
 * when `rect` is given, to the reachable pixel nearest to that rectangle (preferring
 * pixels inside it, closest to (gx,gy)).  "Reachable" = the 4-connected component of
 * free pixels around Shaggy (the collision map only draws fences).  A* runs on the
 * free pixels eroded by WALK_MARGIN px, with the time cost of the game's movement
 * (vertical speed is half the horizontal), and the result is string-pulled into
 * straight legs that are clear of the fence.  Returns the number of legs written to
 * out (the start point is not included), 0 when already there, -1 when no room. */
#define WALK_MARGIN 1
int  walk_find_path(int sx, int sy, int gx, int gy, const walk_rect *rect,
                    walk_point *out, int max);
/* every pixel stepped along the line (like walk_line_clear $005378) is free */
bool walk_line_is_clear(int x0, int y0, int x1, int y1);

/* ---- driving Shaggy (non-blocking; the per-frame auto-walk of player_update moves him)
 * walk_start: only when no script runs, pointer mode is closed and no scripted path
 * plays (returns false otherwise).  Legs are fed into the game's 2-entry waypoint queue
 * (SHAGGY_WP_*) as it drains; call walk_tick() once per frame before the frame runs.
 * The walk ends by itself on arrival, and is dropped (Shaggy stops) when a script
 * starts, pointer mode opens, the room changes, or the D-pad takes over. */
bool walk_start(const walk_point *legs, int n);
void walk_tick(void);
void walk_cancel(void);                       /* stop Shaggy where he is */
bool walk_active(void);
/* shaggy_set_dir ($00548E) with speed 1: 8.8 direction and facing (0 R 1 L 2 U 3 D) */
void walk_leg_dir(int fx, int fy, int tx, int ty, int16_t *dir_x, int16_t *dir_y, int *facing);

#endif
