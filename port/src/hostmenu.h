/* The host's GAME SAVES overlay (GitLab #28): 8 slots, keyboard and mouse, drawn into the
 * game image like the debug overlay.  It only reports what the player asked for; main.c does
 * the saving / loading (saveslot.c). */
#ifndef SCOOBY_HOSTMENU_H
#define SCOOBY_HOSTMENU_H
#include <stdbool.h>
#include <stdint.h>

typedef enum { HM_NONE, HM_SAVE, HM_LOAD, HM_ERASE } hm_action;

void hostmenu_open(void);
void hostmenu_close(void);
bool hostmenu_active(void);

/* SDL_Keycode; returns an action for slot *slot (1..8) or HM_NONE */
hm_action hostmenu_key(int keycode, int *slot);
/* pointer in game pixels; button 0 = just moved, 1 = left click, 3 = right click */
hm_action hostmenu_pointer(int gx, int gy, int button, int *slot);

/* draw into an RGBA game frame of w x h; `status` is shown at the bottom (may be NULL) */
void hostmenu_draw(uint32_t *buf, int w, int h, const char *status);

#endif
