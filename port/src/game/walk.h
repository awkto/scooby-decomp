/* Walking subsystem (GitLab #16): Shaggy/Scooby movement, collision, camera follow.
 * See docs/subsystems/walking.md for the data formats and the pathfinder API. */
#ifndef SCOOBY_WALK_H
#define SCOOBY_WALK_H
#include "game.h"

/* ---- functions (each is also an HLE_FUNC replacing the 68k routine at its address) */
void player_update(hle_ctx *ctx);          /* $005E4A  per-frame Shaggy/Scooby/actor update (VBlank) */
void player_zone_flags(hle_ctx *ctx);      /* $006AE8  tail: Shaggy "behind" flag, then camera ... */
void camera_follow(hle_ctx *ctx);          /* $006B28  tail: scroll camera after Shaggy, then ... */
void verb_menu_open_check(hle_ctx *ctx);   /* $006C28  tail: A/B opens the verb menu, then ... */
void actors_update(hle_ctx *ctx);          /* $006D24  tail: Scooby walking, depth, actors 2-5 */
void walk_blocked_probe(hle_ctx *ctx);     /* $0073D6  Z=0 if WALK_PROBE is on a blocked pixel */
void walk_blocked_line_probe(hle_ctx *ctx);/* $007476  same for LINE_PROBE (no upper bound check) */
void pad_dir8(hle_ctx *ctx);               /* $00750A  D-pad -> direction code 0..7 */

/* ---- actor arrays: index 0 = Shaggy, 1 = Scooby, 2..5 = other actors */
#define ACTOR_ANIM(i)        (0xFF0488 + 2 * (i))  /* w: animation id (bit i of ACTORS_ANIM_NEW = restart) */
#define ACTOR_X(i)           (0xFF04DC + 4 * (i))  /* l: world x, 16.16 (pixel = high word) */
#define ACTOR_Y(i)           (0xFF04F4 + 4 * (i))  /* l: world y, 16.16 (feet) */
#define ACTOR_STEP_X(i)      (0xFF0514 + 4 * (i))  /* l: actors 2-5: |x step| per frame, 16.16 */
#define ACTOR_STEP_Y(i)      (0xFF0528 + 4 * (i))  /* l: actors 2-5: |y step| per frame */
#define ACTOR_DEPTH_X(i)     (0xFF05A0 + 2 * (i))  /* w: depth scale 0 (near) .. ~$100 (far); slows x speed */
#define ACTOR_DEPTH_Y(i)     (0xFF05AC + 2 * (i))  /* w: same value, used for the y speed */
#define ACTOR_TARGET_X(i)    (0xFF05DC + 2 * (i))  /* w: walk target pixel x */
#define ACTOR_TARGET_Y(i)    (0xFF05E8 + 2 * (i))  /* w: walk target pixel y */
#define ACTOR_PATH_PTR(i)    (0xFF05F4 + 4 * (i))  /* l: scripted path table (x,y word pairs, commands < 0) */
#define ACTOR_PATH_POS(i)    (0xFF060C + 2 * (i))  /* w: byte offset of the next path entry */
#define ACTOR_PATH_DELAY(i)  (0xFF0AA5 + (i))      /* b: frames until the next path entry */
#define ACTOR_PATH_RELOAD(i) (0xFF0AAB + (i))      /* b: path playback period */

/* bit sets, bit i = actor i */
#define ACTORS_ACTIVE        0xFF0AB4  /* actors 2-5 that are updated by actors_update */
#define ACTORS_ANIM_NEW      0xFF0AB5  /* animation id changed (consumed by the sprite code) */
#define ACTORS_MOVING        0xFF0AB7  /* walking towards ACTOR_TARGET */
#define ACTORS_DEPTH_ON      0xFF0AB8  /* depth scale follows the room's per-row depth table */
#define ACTORS_BEHIND        0xFF0ABB  /* standing on a zone cell with bit 15 set (drawn behind) */
#define ACTORS_ANIM_DONE     0xFF0ABC  /* current animation reached its end (sprite code) */
#define ACTORS_ANIM_LOOPED   0xFF0ABD  /* (sprite code) turn animation finished */
#define ACTORS_DEPTH_NEW     0xFF0ABE  /* depth changed (sprite code rescales) */
#define ACTORS_ON_PATH       0xFF0AC0  /* playing a scripted path */
#define ACTORS_NO_ZONE       0xFF0AC2  /* don't update ACTORS_BEHIND */
#define ACTORS_PATH_ONCE     0xFF0AC3  /* stop at the $FFFF path terminator instead of looping */

/* ---- Shaggy */
#define SHAGGY_FACING        0xFF05B8  /* w: 0 right, 1 left, 2 up, 3 down */
#define SHAGGY_SPEED         0xFF062A  /* l: D-pad speed, 16.16 = (256-depth)*$155 */
#define SHAGGY_DIR_X         0xFF062E  /* w: auto-walk direction x, 8.8 (+-$100 = full speed) */
#define SHAGGY_DIR_Y         0xFF0630  /* w */
#define SHAGGY_SPEED_X       0xFF0632  /* l: speed*dir (written, not read by this code) */
#define SHAGGY_SPEED_Y       0xFF0636  /* l */
#define WALK_PROBE_X         0xFF0642  /* l: candidate position tested by walk_blocked_probe, 16.16 */
#define WALK_PROBE_Y         0xFF0646  /* l */
#define LINE_PROBE_X         0xFF064A  /* l: point walked along a line by walk_line_clear ($5378) */
#define LINE_PROBE_Y         0xFF064E  /* l */
#define LINE_FROM_X          0xFF0652  /* l: line start, 16.16 (was ram_view_x0) */
#define LINE_FROM_Y          0xFF0656  /* l */
#define SHAGGY_WP_COUNT      0xFF065A  /* w: auto-walk waypoints left (-1/0 = none) */
#define SHAGGY_WP_X          0xFF065C  /* w[2] waypoint x, then y [2], dir x [2], dir y [2], facing [2] */
#define SHAGGY_IDLE_TIMER    0xFF0686  /* w: frames until the "bored" animation (300) */
#define SHAGGY_TURNING       0xFF0ACC  /* b: bit 0 R, 1 L, 2 U, 3 D: turn animation in progress */
#define SHAGGY_TURN_TABLE    0x0FC720  /* w[4][4]: turn animation id [facing][new direction] */

/* ---- Scooby */
#define SCOOBY_FACING        0xFF05BA
#define SCOOBY_ANIM_LOCK     0xFF061A  /* w: set to -1 when Scooby's animation is (re)started here */
#define SHAGGY_ANIM_LOCK     0xFF0618
#define SCOOBY_DIR_X         0xFF063A  /* w: 8.8 */
#define SCOOBY_DIR_Y         0xFF063C
#define SCOOBY_POSE_ANIM     0xFF063E  /* w: idle-spot arrival animation base */
#define SCOOBY_POSE_FACING   0xFF0640  /* w: idle-spot arrival facing */
#define SCOOBY_WP_COUNT      0xFF0670
#define SCOOBY_WP_X          0xFF0672  /* same layout as SHAGGY_WP_X */
#define SCOOBY_WANDER_TIMER  0xFF0688  /* w: frames until Scooby walks to a random idle spot */
#define SCOOBY_TURN_TABLE    0x0FC6E0
#define SCOOBY_POSE_TABLE    0x0FC700

/* ---- camera / room */
#define CAMERA_X             0xFF06CA  /* w: world x of the left screen edge */
#define CAMERA_Y             0xFF06CE  /* w: world y of the top of the room view (screen line 16) */
#define CAMERA_SCROLL_REQ    0xFF0AB3  /* b: bit 0 right, 1 left, 2 up, 3 down: stream a new column/row */
#define MAP_WIDTH            0xFF06C6  /* w: cells */
#define MAP_HEIGHT           0xFF06C8
#define ZONE_MAP             0xFFE000  /* w[width*height]: collision name table */
#define ROOM_DEPTH_ROWS      0xFF0692  /* l: -> w[height] depth per cell row (<0 = interpolate) */
#define COLL_NAMES           0x02AF8E  /* w[150]: zone index -> collision tile | hflip<<11 | vflip<<12 */
#define COLL_TILES           0x02B0BA  /* 4bpp 8x8 tiles, nibble != 0 = blocked */

/* ---- verb menu (owned by the verb bar UI, issue #17; touched here) */
#define MENU_CURSOR_X        0xFF06DC
#define MENU_CURSOR_Y        0xFF06DE
#define MENU_STEP_X          0xFF06E0
#define MENU_STEP_Y          0xFF06E2
#define MENU_VERB_HOVER      0xFF06EA
#define VERB_BAR_SLIDE       0xFF0ACB  /* b: frames left in the verb-bar page slide, <0 idle */

/* ---- misc flags */
#define VBLANK_FLAGS         0xFF09DC  /* bit 3 verb menu open, bit 4 verb page 2, bit 7 script running */
#define GAME_FLAGS_09DE      0xFF09DE
#define GAME_FLAGS_09E8      0xFF09E8
#define GAME_FLAGS_09E9      0xFF09E9
#define GAME_FLAGS_09EA      0xFF09EA
#define GAME_FLAGS_09EB      0xFF09EB
#define GAME_STATE           0xFF2A00

#endif
