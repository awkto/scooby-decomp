/* Glue between the C blocks (GitLab #24, docs/native.md "Glue", glue.h).
 *
 * Part 1: the calls, branches and returns the C ranges of the earlier waves stop at (they leave
 * the jsr/bsr of a routine that can wait for VBlank, the bra back to a loop head and the rts to
 * the 68k). One line per instruction; the comment names the callee. Every callee and every
 * return address is itself a C block.
 *
 * Part 2: the few instructions around such calls that are more than a call: pushes and pops
 * that straddle a wait, computed calls through a register (the script interpreter's opcode
 * dispatch), and the soft reset.
 *
 * The list is the measured residue of docs/remaining68k.md (tools/py/interp_sites.py): every
 * instruction any gate route executed interpreted with --hle all. A block that no route reaches
 * stays 68k-only, and the native build stops with "no C block" if it is ever reached.
 */
#include "game.h"
#include "cc68k.h"
#include "glue.h"

/* ================================================================== part 1 */
/* Reset */
GLUE_CALL(0x000690, 6, 0x00069A)   /* jsr factory_test */
GLUE_GOTO(0x000696, 0x0008CE)
GLUE_CALL(0x000916, 6, 0x00A248)   /* jsr game_init */
GLUE_CALL(0x0009AC, 6, 0x009E7E)   /* jsr palette_fade */
GLUE_CALL(0x0009B6, 6, 0x009742)   /* jsr wait_vblanks */
GLUE_CALL(0x0009DA, 6, 0x00974C)   /* jsr wait_vblank */
GLUE_CALL(0x0009F4, 6, 0x009742)   /* jsr wait_vblanks */
GLUE_CALL(0x000A02, 6, 0x009742)   /* jsr wait_vblanks */
GLUE_CALL(0x000A08, 6, 0x009DE0)   /* jsr palette_read_cram */
GLUE_CALL(0x000A62, 6, 0x009E7E)   /* jsr palette_fade */
GLUE_CALL(0x000A6C, 6, 0x009742)   /* jsr wait_vblanks */
GLUE_CALL(0x000A72, 6, 0x009DE0)   /* jsr palette_read_cram */
GLUE_CALL(0x000B08, 6, 0x009E7E)   /* jsr palette_fade */
GLUE_CALL(0x000B12, 6, 0x00974C)   /* jsr wait_vblank */
GLUE_CALL(0x000B2C, 6, 0x009742)   /* jsr wait_vblanks */
GLUE_CALL(0x000B32, 6, 0x009DE0)   /* jsr palette_read_cram */
GLUE_CALL(0x000BA0, 6, 0x009E7E)   /* jsr palette_fade */
GLUE_CALL(0x000BAA, 6, 0x009742)   /* jsr wait_vblanks */
GLUE_CALL(0x000BB0, 6, 0x009DE0)   /* jsr palette_read_cram */
GLUE_CALL(0x000C48, 6, 0x009E7E)   /* jsr palette_fade */
GLUE_CALL(0x000C54, 6, 0x009742)   /* jsr wait_vblanks */
GLUE_CALL(0x000C90, 6, 0x009742)   /* jsr wait_vblanks */
GLUE_CALL(0x000C96, 6, 0x009DE0)   /* jsr palette_read_cram */
GLUE_CALL(0x000CB4, 4, 0x008196)   /* bsr title_screen */
GLUE_CALL(0x000CC8, 4, 0x007F66)   /* bsr mystery_load */
GLUE_CALL(0x000CCC, 4, 0x00801A)   /* bsr actors_init */
GLUE_CALL(0x000CE8, 4, 0x002406)   /* bsr script_run_frame */
GLUE_CALL(0x000D04, 4, 0x002406)   /* bsr script_run_frame */
GLUE_CALL(0x000D2C, 4, 0x002138)   /* bsr room_load */
GLUE_CALL(0x000D30, 4, 0x00219A)   /* bsr room_fade_in */
GLUE_CALL(0x000D34, 4, 0x004862)   /* bsr script_subop_1C_bar_show */
GLUE_CALL(0x000D38, 6, 0x00AFDA)   /* jsr run_object_events */
GLUE_CALL(0x000D4A, 4, 0x0019B0)   /* bsr action_execute */
GLUE_CALL(0x000D54, 6, 0x00974C)   /* jsr wait_vblank */
GLUE_CALL(0x000D62, 6, 0x00974C)   /* jsr wait_vblank */
GLUE_GOTO(0x000D68, 0x000D38)
GLUE_CALL(0x000D88, 6, 0x009DE0)   /* jsr palette_read_cram */
GLUE_CALL(0x000D8E, 4, 0x008196)   /* bsr title_screen */
GLUE_CALL(0x000D92, 4, 0x002196)   /* bsr room_reload */
GLUE_CALL(0x000D9E, 6, 0x00974C)   /* jsr wait_vblank */
GLUE_GOTO(0x000DA4, 0x000D38)
GLUE_CALL(0x000E86, 4, 0x0056B0)   /* bsr scooby_walk_to */
GLUE_CALL(0x00116C, 4, 0x005C5A)   /* bsr inventory_arrow_up_wait */
GLUE_CALL(0x001170, 4, 0x005CF6)   /* bsr inventory_refresh */
GLUE_CALL(0x001174, 4, 0x005B92)   /* bsr inventory_arrows_update */
GLUE_CALL(0x0011A8, 4, 0x005C74)   /* bsr inventory_arrow_down_wait */
GLUE_CALL(0x0011AC, 4, 0x005CF6)   /* bsr inventory_refresh */
GLUE_CALL(0x0011B0, 4, 0x005B92)   /* bsr inventory_arrows_update */
GLUE_CALL(0x001228, 6, 0x00974C)   /* jsr wait_vblank */
GLUE_CALL(0x001392, 4, 0x0019B0)   /* bsr action_execute */
GLUE_CALL(0x0013A6, 6, 0x00974C)   /* jsr wait_vblank */
GLUE_GOTO(0x0013AC, 0x000D38)
GLUE_CALL(0x0013B0, 6, 0x00B0B8)   /* jsr actor_animate */
GLUE_GOTO(0x0013B6, 0x000D38)

/* action_execute */
GLUE_CALL(0x001A7C, 4, 0x002406)   /* bsr script_run_frame */
GLUE_CALL(0x001B9A, 6, 0x00A130)   /* jsr text_show */
GLUE_RTS(0x001BD6)
GLUE_RTS(0x001BD8)

/* sub_001C0C */
GLUE_CALL(0x001C0C, 4, 0x002320)   /* bsr script_run_nested */
GLUE_CALL(0x001C98, 6, 0x00B0B8)   /* jsr actor_animate */
GLUE_CALL(0x001CD4, 4, 0x002320)   /* bsr script_run_nested */
GLUE_CALL(0x001DB2, 6, 0x00B0B8)   /* jsr actor_animate */
GLUE_CALL(0x001ECE, 6, 0x00A18C)   /* jsr text_wait */
GLUE_CALL(0x001FBC, 6, 0x00A130)   /* jsr text_show */
GLUE_CALL(0x001FE0, 4, 0x002406)   /* bsr script_run_frame */
GLUE_CALL(0x00207E, 6, 0x00B0B8)   /* jsr actor_animate */
GLUE_CALL(0x0020C4, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_CALL(0x0020C8, 4, 0x005CF6)   /* bsr inventory_refresh */
GLUE_CALL(0x0020CC, 4, 0x005B92)   /* bsr inventory_arrows_update */
GLUE_RTS(0x0020D0)

/* run_load_script */
GLUE_CALL(0x002260, 4, 0x002406)   /* bsr script_run_frame */
GLUE_RTS(0x002276)

/* run_arrival_script */
GLUE_CALL(0x0022BE, 4, 0x002406)   /* bsr script_run_frame */
GLUE_RTS(0x0022D4)

/* script_run_nested */
GLUE_CALL(0x002344, 4, 0x0023DC)   /* bsr script_run */
GLUE_RTS(0x002352)

/* script_run */
GLUE_RTS(0x002404)

/* script_run_frame */
GLUE_RTS(0x002444)

/* script_op_1E_face */
GLUE_CALL(0x0026D4, 6, 0x00B0B8)   /* jsr actor_animate */
GLUE_CALL(0x002716, 6, 0x00B0B8)   /* jsr actor_animate */
GLUE_RTS(0x002730)

/* script_op_04_if_else */
GLUE_RTS(0x00277A)
GLUE_RTS(0x00278A)
GLUE_RTS(0x002794)

/* sub_002796 */
GLUE_GOTO(0x00281C, 0x00285C)
GLUE_RTS(0x00289E)

/* script_op_05_move_obj */
GLUE_CALL(0x0029E4, 6, 0x00B0B8)   /* jsr actor_animate */
GLUE_CALL(0x002A28, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_CALL(0x002AC0, 4, 0x005CF6)   /* bsr inventory_refresh */
GLUE_CALL(0x002AC4, 4, 0x005B92)   /* bsr inventory_arrows_update */
GLUE_RTS(0x002ACE)

/* script_op_19_unfreeze_wait */
GLUE_CALL(0x002B3E, 6, 0x00B0B8)   /* jsr actor_animate */
GLUE_RTS(0x002B58)

/* script_op_06_anim_wait */
GLUE_CALL(0x002B8C, 6, 0x00B0B8)   /* jsr actor_animate */
GLUE_CALL(0x002BDC, 6, 0x00B0B8)   /* jsr actor_animate */
GLUE_RTS(0x002BF6)

/* script_op_07_show_obj */
GLUE_CALL(0x002C4C, 4, 0x005CF6)   /* bsr inventory_refresh */
GLUE_RTS(0x002C54)

/* script_op_0A_set_icon */
GLUE_CALL(0x002DE4, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_CALL(0x002DE8, 4, 0x005CF6)   /* bsr inventory_refresh */
GLUE_RTS(0x002DF0)

/* script_op_0B_set_state */
GLUE_RTS(0x002E7C)

/* script_op_0C_sys */
GLUE_RTS(0x002EB2)

/* script_subop_0F_camera_pan_right */
GLUE_CALL(0x004418, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_RTS(0x004426)

/* script_subop_10_camera_pan_left */
GLUE_CALL(0x004470, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_RTS(0x004480)

/* script_subop_02_room_fade_in */
GLUE_CALL(0x004548, 4, 0x00219A)   /* bsr room_fade_in */
GLUE_RTS(0x00454C)

/* script_subop_03_fade_out */
GLUE_CALL(0x00455C, 4, 0x009DE0)   /* bsr palette_read_cram */
GLUE_RTS(0x004566)

/* script_subop_05_fullscreen_picture */
GLUE_CALL(0x0045E0, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_CALL(0x0046F4, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_CALL(0x00476C, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_CALL(0x00477E, 4, 0x009742)   /* bsr wait_vblanks */
GLUE_CALL(0x004796, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_CALL(0x0047A6, 4, 0x00482E)   /* bsr script_subop_1D_bar_hide */
GLUE_RTS(0x0047AA)

/* script_subop_06_bar_show_if_hidden */
GLUE_RTS(0x004826)
GLUE_CALL(0x004828, 4, 0x004862)   /* bsr script_subop_1C_bar_show */
GLUE_RTS(0x00482C)

/* script_subop_1D_bar_hide */
GLUE_CALL(0x004840, 4, 0x0047AC)   /* bsr sub_0047AC */
GLUE_CALL(0x00484C, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_RTS(0x004860)

/* script_subop_1C_bar_show */
GLUE_CALL(0x004870, 4, 0x00B0B8)   /* bsr actor_animate */

/* script_op_15_wait */
GLUE_CALL(0x0048E4, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_RTS(0x0048F6)

/* script_op_0E_walk_spawn */
GLUE_CALL(0x004A30, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_CALL(0x004A48, 4, 0x005002)   /* bsr shaggy_walk_to */
GLUE_CALL(0x004AAE, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_CALL(0x004AC6, 4, 0x00566A)   /* bsr scooby_walk_wait */
GLUE_CALL(0x004B0A, 4, 0x004B24)   /* bsr actor_walk_to */
GLUE_RTS(0x004B1C)
GLUE_RTS(0x004B22)

/* actor_walk_to */
GLUE_RTS(0x004B4A)
GLUE_CALL(0x004B9C, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_RTS(0x004BAA)
GLUE_CALL(0x004C10, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_RTS(0x004C1E)

/* script_op_0F_walk_obj */
GLUE_CALL(0x004D18, 4, 0x005002)   /* bsr shaggy_walk_to */

/* sub_004D7A */
GLUE_RTS(0x004D7E)

/* script_op_10_say */
GLUE_CALL(0x004DDA, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_CALL(0x004DEA, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_CALL(0x004E50, 4, 0x00A130)   /* bsr text_show */
GLUE_CALL(0x004EA4, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_CALL(0x004EB4, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_RTS(0x004EE2)

/* script_op_18_goto_room_cut */
GLUE_CALL(0x004F04, 4, 0x002138)   /* bsr room_load */
GLUE_RTS(0x004F0C)

/* script_op_11_goto_room */
GLUE_CALL(0x004F30, 4, 0x009DE0)   /* bsr palette_read_cram */
GLUE_CALL(0x004F46, 4, 0x002196)   /* bsr room_reload */
GLUE_RTS(0x004F4E)

/* script_op_1F_wait_stop */
GLUE_CALL(0x004FA0, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_RTS(0x004FBC)

/* script_op_17_wait_anim */
GLUE_CALL(0x004FEE, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_RTS(0x005000)

/* shaggy_walk_to */
GLUE_RTS(0x00501A)
GLUE_CALL(0x0055F0, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_CALL(0x00561A, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_CALL(0x005644, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_RTS(0x005668)

/* scooby_walk_wait */
GLUE_CALL(0x00566A, 4, 0x0056B0)   /* bsr scooby_walk_to */
GLUE_CALL(0x005674, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_CALL(0x00569E, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_RTS(0x0056AE)

/* scooby_walk_to */
GLUE_RTS(0x0056D0)

/* scooby_walk_start */
GLUE_CALL(0x005B30, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_RTS(0x005B54)

/* inventory_arrows_update */
GLUE_CALL(0x005B9E, 4, 0x005C5A)   /* bsr inventory_arrow_up_wait */
GLUE_CALL(0x005BD6, 4, 0x005C74)   /* bsr inventory_arrow_down_wait */
GLUE_RTS(0x005BDA)

/* inventory_arrow_up_wait */
GLUE_RTS(0x005C72)

/* inventory_arrow_down_wait */
GLUE_RTS(0x005C8C)

/* inventory_refresh */
GLUE_RTS(0x005D1A)

/* mystery_load */
GLUE_CALL(0x00800A, 4, 0x0013BA)   /* bsr mystery_intro */
GLUE_CALL(0x008014, 4, 0x009776)   /* bsr sound_stop */
GLUE_RTS(0x008018)

/* actors_init */
GLUE_CALL(0x008190, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_RTS(0x008194)

/* title_screen */
GLUE_CALL(0x008196, 4, 0x009DE0)   /* bsr palette_read_cram */
GLUE_CALL(0x0083E2, 6, 0x009E7E)   /* jsr palette_fade */
GLUE_CALL(0x00840A, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_CALL(0x0084AA, 4, 0x00974C)   /* bsr wait_vblank */

/* menu_open */
GLUE_CALL(0x0085CE, 4, 0x008D6A)   /* bsr menu_select */

/* debug_rooms */
GLUE_CALL(0x0088A8, 4, 0x0096F4)   /* bsr menu_load_screen */
GLUE_CALL(0x0088B2, 4, 0x0088D8)   /* bsr rooms_browser */

/* rooms_browser */
GLUE_CALL(0x008928, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_CALL(0x008972, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_RTS(0x008A9A)

/* objects_browser */
GLUE_CALL(0x008AFC, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_CALL(0x008B16, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_RTS(0x008C3E)

/* debug_objects */
GLUE_CALL(0x008C58, 4, 0x0096F4)   /* bsr menu_load_screen */
GLUE_CALL(0x008C5C, 4, 0x008AB4)   /* bsr objects_browser */
GLUE_CALL(0x008C74, 4, 0x0088D8)   /* bsr rooms_browser */

/* menu_start_game */
GLUE_CALL(0x008CD6, 4, 0x009DE0)   /* bsr palette_read_cram */
GLUE_RTS(0x008D34)

/* menu_select */
GLUE_CALL(0x008E74, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_RTS(0x008F12)

/* quit_confirm */
GLUE_CALL(0x008F90, 4, 0x008D36)   /* bsr menu_choose */

/* soft_reset */
GLUE_CALL(0x008FA2, 4, 0x009DE0)   /* bsr palette_read_cram */

/* mystery_menu */
GLUE_CALL(0x008FE6, 4, 0x008D36)   /* bsr menu_choose */
GLUE_CALL(0x0091D4, 4, 0x00974C)   /* bsr wait_vblank */

/* password_cancel */
GLUE_CALL(0x00927A, 4, 0x00974C)   /* bsr wait_vblank */

/* password_check */
GLUE_RTS(0x009326)
GLUE_CALL(0x00933A, 4, 0x009742)   /* bsr wait_vblanks */

/* sound_test */
GLUE_CALL(0x0096A8, 4, 0x00974C)   /* bsr wait_vblank */

/* menu_load_screen */
GLUE_CALL(0x00970C, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_RTS(0x009714)

/* palette_read_cram */
GLUE_CALL(0x009DEE, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_CALL(0x009E5C, 4, 0x009742)   /* bsr wait_vblanks */
GLUE_RTS(0x009E7C)

/* palette_fade */
GLUE_CALL(0x009F06, 4, 0x009742)   /* bsr wait_vblanks */
GLUE_RTS(0x009F2E)

/* text_show */
GLUE_CALL(0x00A148, 4, 0x00A18C)   /* bsr text_wait */
GLUE_RTS(0x00A170)

/* text_wait */
GLUE_CALL(0x00A1AA, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_CALL(0x00A1AE, 4, 0x00AFDA)   /* bsr run_object_events */
GLUE_CALL(0x00A1E2, 6, 0x0020DA)   /* jsr talk_gesture */
GLUE_CALL(0x00A1E8, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_RTS(0x00A234)

/* run_object_events */
GLUE_RTS(0x00B0B6)


/* ---- the minigames, Shaggy's poses, the close-up picture and the credits (long routes) */
/* script_subop_35_shaggy_pose_45 */
GLUE_CALL(0x003038, 6, 0x00B0B8)   /* jsr actor_animate */
GLUE_CALL(0x00304E, 6, 0x00B0B8)   /* jsr actor_animate */
GLUE_CALL(0x003096, 6, 0x00B0B8)   /* jsr actor_animate */
GLUE_RTS(0x0030A4)

/* script_subop_32_minigame_tug */
GLUE_CALL(0x00331A, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_CALL(0x003346, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_CALL(0x00334A, 4, 0x004548)   /* bsr script_subop_02_room_fade_in */
GLUE_CALL(0x00335C, 4, 0x009742)   /* bsr wait_vblanks */
GLUE_CALL(0x00336E, 4, 0x009742)   /* bsr wait_vblanks */

/* sub_003372 (the tug minigame's frame loop) */
GLUE_CALL(0x003372, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_CALL(0x003376, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_CALL(0x0038DC, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_CALL(0x0038E8, 4, 0x00454E)   /* bsr script_subop_03_fade_out */
GLUE_RTS(0x003938)

/* script_subop_2B_shaggy_alt_sprite_on */
GLUE_CALL(0x003EFC, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_RTS(0x003F14)

/* script_subop_27_minigame_throw */
GLUE_CALL(0x003F82, 4, 0x00B0B8)   /* bsr actor_animate */

/* sub_003FE0 (the throw minigame) */
GLUE_CALL(0x004014, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_CALL(0x004086, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_CALL(0x0040AE, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_CALL(0x0040CA, 4, 0x009DE0)   /* bsr palette_read_cram */
GLUE_RTS(0x0040DE)

/* script_subop_25_wait_shaggy_path, _22_shaggy_anim_by_x, _21_wait_dpad_choice */
GLUE_CALL(0x0040FC, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_RTS(0x00410A)
GLUE_CALL(0x004148, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_RTS(0x004158)
GLUE_CALL(0x00415A, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_RTS(0x0041A0)

/* script_subop_1E_ending_credits */
GLUE_CALL(0x0041BA, 4, 0x009742)   /* bsr wait_vblanks */
GLUE_CALL(0x004222, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_CALL(0x004226, 4, 0x00974C)   /* bsr wait_vblank */
GLUE_CALL(0x004258, 4, 0x00B0B8)   /* bsr actor_animate */
GLUE_CALL(0x00426C, 4, 0x009DE0)   /* bsr palette_read_cram */
GLUE_GOTO(0x004270, 0x008F9A)      /* the end: soft_reset */

/* script_subop_07_closeup_picture */
GLUE_RTS(0x004532)

/* script_op_0F_walk_obj, Scooby's part */
GLUE_CALL(0x004D44, 4, 0x005AD8)   /* bsr scooby_walk_direct */


/* ================================================================== part 2 */

/* move.b X,-(sp): the 68000 moves SP by 2 and writes the high byte */
static void push_byte(hle_ctx *ctx, uint8_t v)
{
	ctx->a[7] -= 2;
	bus_w8(ctx->a[7], v);
	cc_logic8(ctx, v);
}

static uint8_t pop_byte(hle_ctx *ctx)
{
	uint8_t v = bus_r8(ctx->a[7]);
	ctx->a[7] += 2;
	cc_logic8(ctx, v);
	return v;
}

#define RAM_SCRIPT_CB     0xFF0878   /* long: the script runner the nested bodies use */
#define RAM_SCRIPT_FLAGS  0xFF0AC9   /* b: bit 0 = the script ended / broke out */
#define RAM_VERB_STATE    0xFF09DE

/* ------------------------------------------------------------------ action_execute ($0019B0) */
/* $001A0E: movem.l a5-a6,-(sp) / bsr script_run_nested (the target's script) */
HLE_RANGE(0x001A0E, action_run_target_script, 50, HLE_EXIT_SP(0x002320, -12))
{
	movem_push_l(ctx, MOVEM_A(5) | MOVEM_A(6));
	glue_call(ctx, 0x002320, 0x001A16);
}

/* $001A3A: move.b (VERB_STATE),-(sp) / bsr script_run: the verb's script, VERB_STATE saved */
HLE_RANGE(0x001A3A, action_run_verb_script, 36, HLE_EXIT_SP(0x0023DC, -6))
{
	push_byte(ctx, RAM8(RAM_VERB_STATE));
	glue_call(ctx, 0x0023DC, 0x001A44);
}

/* $001A44: move.b (sp)+,d0 */
HLE_RANGE(0x001A44, action_verb_script_done, 8, HLE_EXIT_SP(0x001A46, 2))
{
	set_b(&ctx->d[0], pop_byte(ctx));
	hle_jump(ctx, 0x001A46);
}

/* $001A80: movem.l (sp)+,a5-a6 */
HLE_RANGE(0x001A80, action_target_script_done, 28, HLE_EXIT_SP(0x001A84, 8))
{
	movem_pop_l(ctx, MOVEM_A(5) | MOVEM_A(6));
	hle_jump(ctx, 0x001A84);
}

/* $001A8C: move.b (RAM_SCRIPT_FLAGS),-(sp) / bsr talk ($001C0C) */
HLE_RANGE(0x001A8C, action_talk, 36, HLE_EXIT_SP(0x001C0C, -6))
{
	push_byte(ctx, RAM8(RAM_SCRIPT_FLAGS));
	glue_call(ctx, 0x001C0C, 0x001A96);
}

/* $001A96: move.b (sp)+,(RAM_SCRIPT_FLAGS) */
HLE_RANGE(0x001A96, action_talk_done, 20, HLE_EXIT_SP(0x001A9C, 2))
{
	RAM8(RAM_SCRIPT_FLAGS) = pop_byte(ctx);
	hle_jump(ctx, 0x001A9C);
}

/* ------------------------------------------------------------------ the TALK dialog ($001C0C) */
#define RAM_TALK_DEPTH    0xFF0810   /* w: nesting level of the TALK menus */
#define RAM_TALK_A5       0xFF0812
#define RAM_TALK_A6       0xFF0816
#define RAM_TALK_USED     0xFF0874   /* w: bit n = choice n already taken on this level */
#define RAM_POINTER_HIDE  0xFF06EE
#define RAM_BAR_SLIDE     0xFF0ACB
#define RAM_FONT_FLAGS    0xFF068C   /* w: font_tiles_load parameters */

/* $001CEA: the level has choices: load the menu font (font_tiles_load 6, 0), registers saved */
HLE_RANGE(0x001CEA, talk_menu_font, 80, HLE_EXIT_SP(0x007A2A, -64))
{
	movem_push_l(ctx, MOVEM_ALL);
	ctx->d[0] = 6;
	ctx->d[1] = 0;
	set_w(&ctx->d[2], RAM16(RAM_FONT_FLAGS));
	set_w(&ctx->d[2], ctx->d[2] & 0x9FFF);
	RAM16(RAM_FONT_FLAGS) = (uint16_t)ctx->d[2];
	cc_logic16(ctx, ctx->d[2]);
	glue_call(ctx, 0x007A2A, 0x001D06);
}

/* $001D06: movem.l (sp)+,d0-d7/a0-a6, then the wait for A and B released ($1D0A, waits.c) */
HLE_RANGE(0x001D06, talk_menu_font_done, 130, HLE_EXIT_SP(0x001D0A, 60))
{
	movem_pop_l(ctx, MOVEM_ALL);
	hle_jump(ctx, 0x001D0A);
}

/* $00204A: a level without choices: back to the parent level (pop what talk_choice_after
 * pushed) or, at the top, leave: blank the bar and slide it back in ($207E) */
HLE_RANGE(0x00204A, talk_level_empty, 60, HLE_EXIT_SP(0x001CC4, 10), 0x00207E)
{
	uint16_t lv = (uint16_t)cc_sub(ctx, RAM16(RAM_TALK_DEPTH), 1, 16, true);
	RAM16(RAM_TALK_DEPTH) = lv;
	if (!(lv & 0x8000)) {
		uint32_t v = pop32(ctx);
		RAM32_W(RAM_TALK_A6, v);
		v = pop32(ctx);
		RAM32_W(RAM_TALK_A5, v);
		uint16_t used = pop16(ctx);
		RAM16(RAM_TALK_USED) = used;
		cc_logic16(ctx, used);
		hle_jump(ctx, 0x001CC4);
		return;
	}
	RAM16(RAM_POINTER_HIDE) = 0xFFFF;
	hle_call68k(ctx, 0x0022F2);                        /* talk_bar_blank */
	RAM8(RAM_BAR_SLIDE) = 0x10;
	cc_logic8(ctx, 0x10);
	hle_jump(ctx, 0x00207E);
}

/* ------------------------------------------------------------------ room scripts ($00221A, $002278) */
/* run_load_script / run_arrival_script save the script runner and VBLANK_FLAGS around the run:
 *     move.l (RAM_SCRIPT_CB),-(sp) / move.b (VBLANK_FLAGS),-(sp)      ... C ...
 *     bsr script_run_frame / move.b (sp)+,d0                           ... C ...
 *     move.l (sp)+,(RAM_SCRIPT_CB) / rts */
static void save_script_state(hle_ctx *ctx)
{
	push32(ctx, RAM32(RAM_SCRIPT_CB));
	push_byte(ctx, RAM8(VBLANK_FLAGS));
}

static void restore_script_cb(hle_ctx *ctx)
{
	uint32_t v = pop32(ctx);
	RAM32_W(RAM_SCRIPT_CB, v);
	cc_logic32(ctx, v);
}

HLE_RANGE(0x00221A, run_load_script_save, 40, HLE_EXIT_SP(0x002226, -6))
{
	save_script_state(ctx);
	hle_jump(ctx, 0x002226);
}
HLE_RANGE(0x002264, run_load_script_flags, 8, HLE_EXIT_SP(0x002266, 2))
{
	set_b(&ctx->d[0], pop_byte(ctx));
	hle_jump(ctx, 0x002266);
}
HLE_FUNC(0x002270, run_load_script_end, 44) { restore_script_cb(ctx); }

HLE_RANGE(0x002278, run_arrival_script_save, 40, HLE_EXIT_SP(0x002284, -6))
{
	save_script_state(ctx);
	hle_jump(ctx, 0x002284);
}
HLE_RANGE(0x0022C2, run_arrival_script_flags, 8, HLE_EXIT_SP(0x0022C4, 2))
{
	set_b(&ctx->d[0], pop_byte(ctx));
	hle_jump(ctx, 0x0022C4);
}
HLE_FUNC(0x0022CE, run_arrival_script_end, 44) { restore_script_cb(ctx); }

/* ------------------------------------------------------------------ script interpreter */
/* The opcode handlers are called through a0 (jsr (a0)): push the return address, go to the
 * handler. Each handler is a C block (or a chain of them, when it waits). */
HLE_FUNC(0x0023F2, script_run_call_op, 16) { glue_call(ctx, ctx->a[0], 0x0023F4); }
HLE_FUNC(0x002430, script_run_frame_call_op, 16) { glue_call(ctx, ctx->a[0], 0x002432); }
HLE_FUNC(0x002EAC, script_op_sys_call, 16) { glue_call(ctx, ctx->a[0], 0x002EAE); }

/* $002752 op $04 if_else, condition true: run the then-body nested through the runner in
 * RAM_SCRIPT_CB, with the caller's a5/a6 saved:
 *     movem.l a5-a6,-(sp) / move.w 2(a5),d0 / lea (a5,d0.w),a6 / lea $10(a5),a5 /
 *     movea.l (RAM_SCRIPT_CB),a0 / jsr (a0) */
HLE_FUNC(0x002752, script_op_if_else_then, 60)
{
	movem_push_l(ctx, MOVEM_A(5) | MOVEM_A(6));
	set_w(&ctx->d[0], bus_r16(ctx->a[5] + 2));
	cc_logic16(ctx, ctx->d[0]);
	ctx->a[6] = ctx->a[5] + (uint32_t)(int16_t)ctx->d[0];
	ctx->a[5] += 0x10;
	ctx->a[0] = RAM32(RAM_SCRIPT_CB);
	glue_call(ctx, ctx->a[0], 0x00276A);
}

/* $00276A: the body ended the script (RAM_SCRIPT_FLAGS bit 0): drop the saved a5/a6 and return
 * with the body's position; else restore them and skip the else-body */
HLE_FUNC(0x00276A, script_op_if_else_then_done, 50)
{
	cc_btst(ctx, RAM8(RAM_SCRIPT_FLAGS), 0);
	if (RAM8(RAM_SCRIPT_FLAGS) & 1) {
		ctx->a[7] += 8;                                /* lea 8(sp),sp / rts */
		return;
	}
	ctx->a[4] = ctx->a[5];
	movem_pop_l(ctx, MOVEM_A(5) | MOVEM_A(6));
	set_w(&ctx->d[0], bus_r16(ctx->a[5] + 4));
	cc_logic16(ctx, ctx->d[0]);
	ctx->a[5] = ctx->a[4] + (uint32_t)(int16_t)ctx->d[0];
}

/* $002ABA op $05 move_obj, an inventory change: move.l a5,-(sp) / bsr wait_vblank, then
 * inventory_refresh and inventory_arrows_update (glue above), then $2AC8 */
HLE_RANGE(0x002ABA, script_op_move_obj_inventory, 30, HLE_EXIT_SP(0x00974C, -8))
{
	push32(ctx, ctx->a[5]);
	cc_logic32(ctx, ctx->a[5]);
	glue_call(ctx, 0x00974C, 0x002AC0);
}

/* $002AC8: movea.l (sp)+,a5 / lea 6(a5),a5 / rts ($2ACE) */
HLE_RANGE(0x002AC8, script_op_move_obj_done, 16, HLE_EXIT_SP(0x002ACE, 4))
{
	ctx->a[5] = pop32(ctx) + 6;
	hle_jump(ctx, 0x002ACE);
}

/* $00487E script_subop_1C_bar_show, the bar is in: put back the 6 rows of the bar saved at
 * $FF2C00 (vdp_dma_vram, 32 words per row from d4), reload the bar font, clear the "bar hidden"
 * story flag; rts */
HLE_FUNC(0x00487E, bar_show_restore, 400)
{
	set_w(&ctx->d[0], ctx->d[4]);
	cc_logic16(ctx, ctx->d[0]);
	ctx->a[0] = 0xFF2C00;
	ctx->d[2] = 5;
	set_w(&ctx->d[1], 0x20);
	cc_logic16(ctx, 0x20);
	for (;;) {
		hle_call68k(ctx, 0x0099F6);                    /* vdp_dma_vram */
		set_w(&ctx->d[0], cc_add(ctx, ctx->d[0], 0x80, 16));
		set_w(&ctx->d[2], ctx->d[2] - 1);
		if ((ctx->d[2] & 0xFFFF) == 0xFFFF)
			break;
	}
	set_w(&ctx->d[0], 0xF);
	ctx->d[1] = 1;
	set_w(&ctx->d[2], RAM16(RAM_FONT_FLAGS));
	set_w(&ctx->d[2], ctx->d[2] & 0x9FFF);
	cc_logic16(ctx, ctx->d[2]);
	hle_call68k(ctx, 0x007A2A);                        /* font_tiles_load */
	cc_btst(ctx, RAM8(0xFF2A00), 1);
	RAM8(0xFF2A00) &= (uint8_t)~2;
}

/* $004EDE op $10 say, end: lea 8(a5),a5, then the rts at $4EE2 */
HLE_RANGE(0x004EDE, script_op_say_skip, 8, 0x004EE2)
{
	ctx->a[5] += 8;
	hle_jump(ctx, 0x004EE2);
}

/* ------------------------------------------------------------------ menus, palette, text */
/* $00800E mystery_load after the intro card: move.l #$1E,d0 / bsr sound_stop ($8014) */
HLE_RANGE(0x00800E, mystery_load_after_intro, 12, 0x008014)
{
	ctx->d[0] = 0x1E;
	cc_logic32(ctx, ctx->d[0]);
	hle_jump(ctx, 0x008014);
}

/* $008FA6 soft_reset, after the fade: move.w #$2700,sr / movea.l (0).w,sp / movea.l (4).w,a0 /
 * jmp (a0): the reset vector, with the stack pointer from the vector table */
HLE_FUNC(0x008FA6, soft_reset_jump, 40)
{
	ctx->sr = 0x2700;
	ctx->a[7] = ROM32(0);
	ctx->a[0] = ROM32(4);
	hle_jump(ctx, ctx->a[0]);
}

/* $009E7E palette_fade: movem.l d0-d4/a1,-(sp) ... $9F2A movem.l (sp)+,d0-d4/a1 / rts */
#define PALETTE_FADE_SAVED (0x1Fu | MOVEM_A(1))
HLE_RANGE(0x009E7E, palette_fade_save, 56, HLE_EXIT_SP(0x009E82, -24))
{
	movem_push_l(ctx, PALETTE_FADE_SAVED);
	hle_jump(ctx, 0x009E82);
}
HLE_FUNC(0x009F2A, palette_fade_end, 76) { movem_pop_l(ctx, PALETTE_FADE_SAVED); }

/* $00A13A text_show: move.w d0,-(sp) (the pointer-mode bit) ... $A160 move.w (sp)+,d0 */
HLE_RANGE(0x00A13A, text_show_save, 8, HLE_EXIT_SP(0x00A13C, -2))
{
	push16(ctx, (uint16_t)ctx->d[0]);
	cc_logic16(ctx, ctx->d[0]);
	hle_jump(ctx, 0x00A13C);
}
HLE_RANGE(0x00A160, text_show_restore, 8, HLE_EXIT_SP(0x00A162, 2))
{
	set_w(&ctx->d[0], pop16(ctx));
	cc_logic16(ctx, ctx->d[0]);
	hle_jump(ctx, 0x00A162);
}

/* ------------------------------------------------------------------ run_object_events ($00AFDA) */
#define ROE_SAVED (MOVEM_D(0) | MOVEM_A(0) | MOVEM_A(1) | MOVEM_A(2))

/* $00B050: an object's event script: movem.l d0/a0-a2,-(sp) / jsr script_run */
HLE_RANGE(0x00B050, roe_run_event, 52, HLE_EXIT_SP(0x0023DC, -20))
{
	movem_push_l(ctx, ROE_SAVED);
	glue_call(ctx, 0x0023DC, 0x00B05A);
}

/* $00B078: it ended: run its "done" part as a frame script:
 *     movem.l d0/a0-a2,-(sp) / move.w 2(a5),d0 / lea (a5,d0.w),a6 / tst.l (a5)+ /
 *     move.l #script_run_frame,(RAM_SCRIPT_CB) / jsr script_run_frame */
HLE_RANGE(0x00B078, roe_run_done, 90, HLE_EXIT_SP(0x002406, -20))
{
	movem_push_l(ctx, ROE_SAVED);
	set_w(&ctx->d[0], bus_r16(ctx->a[5] + 2));
	ctx->a[6] = ctx->a[5] + (uint32_t)(int16_t)ctx->d[0];
	ctx->a[5] += 4;
	RAM32_W(RAM_SCRIPT_CB, 0x002406);
	cc_logic32(ctx, 0x002406);
	glue_call(ctx, 0x002406, 0x00B096);
}

/* $00B05A / $00B096: movem.l (sp)+,d0/a0-a2 */
HLE_RANGE(0x00B05A, roe_event_done, 44, HLE_EXIT_SP(0x00B05E, 16))
{
	movem_pop_l(ctx, ROE_SAVED);
	hle_jump(ctx, 0x00B05E);
}
HLE_RANGE(0x00B096, roe_done_done, 44, HLE_EXIT_SP(0x00B09A, 16))
{
	movem_pop_l(ctx, ROE_SAVED);
	hle_jump(ctx, 0x00B09A);
}
