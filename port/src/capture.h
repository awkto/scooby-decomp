/* Entry-state capture: whenever the CPU is about to execute ADDR, save the complete
 * machine state (savestate + pad inputs) to DIR/NNNNN.cap, for difftest. */
#ifndef SCOOBY_CAPTURE_H
#define SCOOBY_CAPTURE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t addr, index, frame;
	uint16_t pad[2];          /* GPGX input.pad[0..1] at capture time */
	size_t state_size;
	uint8_t *state;
} cap_state;

/* every: keep one call out of `every`; skip: ignore the first `skip` calls.
 * dir == NULL: only count the calls (capture_calls()). */
bool capture_start(uint32_t addr, int max_states, const char *dir, int every, int skip);
/* add another address to the running capture (capture_start resets the list): one
 * pass over a route captures several functions; capture_done() = all of them full */
bool capture_add(uint32_t addr, int max_states, const char *dir, int every, int skip);
int capture_slot_count(int i);
void capture_stop(void);
int capture_count(void);
unsigned long capture_calls(void);
bool capture_done(void);

bool cap_read(const char *path, cap_state *out);
void cap_free(cap_state *c);
/* load a captured entry state into the emulator (state + inputs) */
bool cap_apply(const cap_state *c);

#endif
