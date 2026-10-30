/* Click-to-walk support: collision queries, walk-on rectangles, A* pathfinder and the
 * auto-walk driver.  See walkpath.h for the API and docs/mouse.md for the design. */
#include "walkpath.h"
#include "walk.h"

#include <stdlib.h>
#include <string.h>

#define ROOM_INDEX    0xFF06AC   /* w: current room */
#define MYSTERY_INDEX 0xFF06AA   /* w: 0 hotel, 1 carnival */
#define MYSTERY_HDR   0xFF068E   /* l: active mystery header */
#define OBJECTS       0xFF1200   /* object records, $1A bytes each */
#define OBJECT_SIZE   0x1A
#define OBJECT_LAST   0xFF06F4   /* w: index of the last record (the loops use <=) */

static inline bool bit8(uint32_t a, int b) { return (RAM8(a) >> b) & 1; }
static inline void set8(uint32_t a, int b) { RAM8(a) |= (uint8_t)(1u << b); }
static inline void clr8(uint32_t a, int b) { RAM8(a) &= (uint8_t)~(1u << b); }

/* ------------------------------------------------------------------ collision */
int walk_room_w(void) { return RAM16(MAP_WIDTH) * 8; }
int walk_room_h(void) { return RAM16(MAP_HEIGHT) * 8; }

uint16_t walk_zone_word(int x, int y)
{
	uint32_t i = (uint32_t)((x >> 3) + (y >> 3) * RAM16(MAP_WIDTH));
	return RAM16(ZONE_MAP + 2 * i);
}

/* zone index -> COLL_NAMES[index] (bits 0-10 tile, 11 hflip, 12 vflip) -> nibble of the
 * 4bpp collision tile; the same lookup as coll_pixel() in walk.c */
bool walk_is_blocked(int x, int y)
{
	if (x < 0 || y < 0 || x >= walk_room_w() || y >= walk_room_h())
		return true;
	uint16_t z = walk_zone_word(x, y) & 0x7FFF;
	if (!z)
		return false;
	uint16_t name = ROM16(COLL_NAMES + 2u * z);
	int px = x & 7, py = y & 7;
	if (!(name & 0x0800))          /* pixel 0 is the top nibble of the tile row */
		px ^= 7;
	if (name & 0x1000)
		py ^= 7;
	uint32_t row = ROM32(COLL_TILES + 32u * (name & 0x7FF) + 4u * py);
	return (row >> (4 * px)) & 0xF;
}

/* ------------------------------------------------------------------ walk-on rectangles */
/* The rectangle of object record `rec` (shape id at +2) from the mystery header:
 * header+$1C base, header+$24 table of longs (offset per id-1); x, y, w, h in cells. */
int walk_triggers(walk_rect *out, int max)
{
	uint32_t hdr = RAM32(MYSTERY_HDR);
	if (!hdr || !RAM16(MAP_WIDTH))
		return 0;
	uint32_t base = bus_r32(hdr + 0x1C), offs = bus_r32(hdr + 0x24);
	uint16_t room = RAM16(ROOM_INDEX);
	int n = 0, last = (int16_t)RAM16(OBJECT_LAST);
	for (int i = 0; i <= last && i < 0x800 / OBJECT_SIZE && n < max; i++) {
		uint32_t rec = OBJECTS + OBJECT_SIZE * (uint32_t)i;
		uint16_t id = RAM16(rec + 2);
		if (RAM16(rec + 6) != room || (RAM8(rec + 0x18) & 2) || !id)
			continue;
		uint32_t r = base + bus_r32(offs + 4u * (uint16_t)(id - 1));
		int x = (int16_t)bus_r16(r), y = (int16_t)bus_r16(r + 2);
		int w = (int16_t)bus_r16(r + 4), h = (int16_t)bus_r16(r + 6);
		out[n++] = (walk_rect){ x * 8, y * 8, (x + w) * 8, (y + h) * 8, i, id };
	}
	return n;
}

bool walk_trigger_of_record(int record, walk_rect *out)
{
	walk_rect r[64];
	int n = walk_triggers(r, 64);
	for (int i = 0; i < n; i++)
		if (r[i].record == record) {
			*out = r[i];
			return true;
		}
	return false;
}

int walk_trigger_at(int x, int y, walk_rect *out)
{
	walk_rect r[64];
	int n = walk_triggers(r, 64), hit = -1;
	for (int i = 0; i < n; i++)          /* the main loop keeps the last hit */
		if (x >= r[i].x0 && x < r[i].x1 && y >= r[i].y0 && y < r[i].y1)
			hit = i;
	if (hit >= 0 && out)
		*out = r[hit];
	return hit;
}

/* ------------------------------------------------------------------ grids */
typedef struct {
	int w, h;
	uint8_t *ok;      /* 1 = may be stepped on */
} grid;

static inline bool g_ok(const grid *g, int x, int y)
{
	return x >= 0 && y >= 0 && x < g->w && y < g->h && g->ok[y * g->w + x];
}

/* every pixel touched while stepping from (x0,y0) to (x1,y1) one pixel along the major
 * axis is ok; the minor axis checks both pixels it lies between (conservative) */
static bool g_line(const grid *g, int x0, int y0, int x1, int y1)
{
	int dx = x1 - x0, dy = y1 - y0;
	int n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
	if (n == 0)
		return g_ok(g, x0, y0);
	for (int k = 0; k <= n; k++) {
		/* 16.16 position of step k */
		int64_t fx = ((int64_t)x0 << 16) + (int64_t)dx * 65536 * k / n;
		int64_t fy = ((int64_t)y0 << 16) + (int64_t)dy * 65536 * k / n;
		int ix = (int)(fx >> 16), iy = (int)(fy >> 16);
		if (!g_ok(g, ix, iy))
			return false;
		if ((fx & 0xFFFF) && !g_ok(g, ix + 1, iy))
			return false;
		if ((fy & 0xFFFF) && !g_ok(g, ix, iy + 1))
			return false;
	}
	return true;
}

bool walk_line_is_clear(int x0, int y0, int x1, int y1)
{
	int dx = x1 - x0, dy = y1 - y0;
	int n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
	for (int k = 0; k <= n; k++) {
		int x = n ? x0 + (int)((int64_t)dx * k / n) : x0;
		int y = n ? y0 + (int)((int64_t)dy * k / n) : y0;
		if (walk_is_blocked(x, y))
			return false;
	}
	return true;
}

/* nearest ok pixel to (x,y) (euclidean, ties: first in scan order); false if none */
static bool g_nearest(const grid *g, int x, int y, int *ox, int *oy)
{
	if (g_ok(g, x, y)) {
		*ox = x;
		*oy = y;
		return true;
	}
	long best = -1;
	for (int yy = 0; yy < g->h; yy++)
		for (int xx = 0; xx < g->w; xx++) {
			if (!g->ok[yy * g->w + xx])
				continue;
			long d = (long)(xx - x) * (xx - x) + (long)(yy - y) * (yy - y);
			if (best < 0 || d < best) {
				best = d;
				*ox = xx;
				*oy = yy;
			}
		}
	return best >= 0;
}

/* ------------------------------------------------------------------ A* */
/* Movement cost = frames: the auto-walk covers max(|dx|, 2|dy|) "x pixels" per leg
 * (y speed is half of x).  Costs x10: horizontal 10, vertical 20, diagonal 21 (a hair
 * more than vertical so that straight runs win ties).  h = 10*max(|dx|, 2|dy|) is
 * admissible and consistent for these costs. */
typedef struct { int32_t f; int32_t i; } heap_ent;
typedef struct { heap_ent *a; int n, cap; } heap;

static void heap_push(heap *h, int32_t f, int32_t i)
{
	if (h->n == h->cap) {
		h->cap = h->cap ? h->cap * 2 : 4096;
		h->a = realloc(h->a, sizeof *h->a * (size_t)h->cap);
	}
	int k = h->n++;
	while (k > 0) {
		int p = (k - 1) / 2;
		if (h->a[p].f <= f)
			break;
		h->a[k] = h->a[p];
		k = p;
	}
	h->a[k] = (heap_ent){ f, i };
}

static heap_ent heap_pop(heap *h)
{
	heap_ent top = h->a[0], last = h->a[--h->n];
	int k = 0;
	for (;;) {
		int c = 2 * k + 1;
		if (c >= h->n)
			break;
		if (c + 1 < h->n && h->a[c + 1].f < h->a[c].f)
			c++;
		if (h->a[c].f >= last.f)
			break;
		h->a[k] = h->a[c];
		k = c;
	}
	if (h->n)
		h->a[k] = last;
	return top;
}

static int32_t heur(int x, int y, int gx, int gy)
{
	int dx = abs(gx - x), dy = 2 * abs(gy - y);
	return 10 * (dx > dy ? dx : dy);
}

/* fills path (start..goal, as pixel indices) and returns its length, 0 if unreachable */
static int astar(const grid *g, int sx, int sy, int gx, int gy, int32_t **path_out)
{
	int n = g->w * g->h;
	int32_t *cost = malloc(sizeof *cost * (size_t)n);
	int32_t *from = malloc(sizeof *from * (size_t)n);
	for (int i = 0; i < n; i++)
		cost[i] = INT32_MAX;
	heap h = { 0 };
	int s = sy * g->w + sx, goal = gy * g->w + gx, len = 0;
	cost[s] = 0;
	from[s] = -1;
	heap_push(&h, heur(sx, sy, gx, gy), s);
	static const int ddx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
	static const int ddy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
	static const int dc[8] = { 10, 10, 20, 20, 21, 21, 21, 21 };
	bool found = false;
	while (h.n) {
		heap_ent e = heap_pop(&h);
		int i = e.i, x = i % g->w, y = i / g->w;
		if (e.f - heur(x, y, gx, gy) > cost[i])
			continue;                       /* stale entry */
		if (i == goal) {
			found = true;
			break;
		}
		for (int k = 0; k < 8; k++) {
			int nx = x + ddx[k], ny = y + ddy[k];
			if (!g_ok(g, nx, ny))
				continue;
			/* no corner cutting: a diagonal step needs both orthogonal neighbours */
			if (k >= 4 && (!g_ok(g, nx, y) || !g_ok(g, x, ny)))
				continue;
			int j = ny * g->w + nx;
			int32_t c = cost[i] + dc[k];
			if (c < cost[j]) {
				cost[j] = c;
				from[j] = i;
				heap_push(&h, c + heur(nx, ny, gx, gy), j);
			}
		}
	}
	if (found) {
		for (int i = goal; i >= 0; i = from[i])
			len++;
		int32_t *p = malloc(sizeof *p * (size_t)len);
		int k = len;
		for (int i = goal; i >= 0; i = from[i])
			p[--k] = i;
		*path_out = p;
	}
	free(h.a);
	free(cost);
	free(from);
	return len;
}

/* ------------------------------------------------------------------ planning */
static long rect_dist2(const walk_rect *r, int x, int y)
{
	long dx = x < r->x0 ? r->x0 - x : x >= r->x1 ? x - (r->x1 - 1) : 0;
	long dy = y < r->y0 ? r->y0 - y : y >= r->y1 ? y - (r->y1 - 1) : 0;
	return dx * dx + dy * dy;
}

/* the walkable area around (sx, sy): fr = free pixels, comp = the 4-connected component
 * of free pixels that contains (sx, sy), or the nearest free pixel when Shaggy stands on
 * a fence pixel (wall slide, some spawn points) */
typedef struct {
	int w, h;
	uint8_t *fr, *comp;
	int seedx, seedy;
} area;

static void area_free(area *a)
{
	free(a->fr);
	free(a->comp);
	a->fr = a->comp = NULL;
}

static bool area_build(int sx, int sy, area *a)
{
	int w = walk_room_w(), h = walk_room_h();
	memset(a, 0, sizeof *a);
	if (w <= 0 || h <= 0)
		return false;
	int n = w * h;
	a->w = w;
	a->h = h;
	a->fr = malloc((size_t)n);
	a->comp = calloc((size_t)n, 1);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			a->fr[y * w + x] = !walk_is_blocked(x, y);
	grid gfree = { w, h, a->fr };
	if (!g_nearest(&gfree, sx, sy, &a->seedx, &a->seedy)) {
		area_free(a);
		return false;
	}
	int32_t *stack = malloc(sizeof *stack * (size_t)n);
	int sp = 0;
	a->comp[a->seedy * w + a->seedx] = 1;
	stack[sp++] = a->seedy * w + a->seedx;
	while (sp) {
		int i = stack[--sp], x = i % w, y = i / w;
		static const int nx4[4] = { 1, -1, 0, 0 }, ny4[4] = { 0, 0, 1, -1 };
		for (int k = 0; k < 4; k++) {
			int xx = x + nx4[k], yy = y + ny4[k];
			if (xx < 0 || yy < 0 || xx >= w || yy >= h)
				continue;
			int j = yy * w + xx;
			if (a->fr[j] && !a->comp[j]) {
				a->comp[j] = 1;
				stack[sp++] = j;
			}
		}
	}
	free(stack);
	return true;
}

bool walk_rect_reachable(int sx, int sy, const walk_rect *r)
{
	area a;
	if (!area_build(sx, sy, &a))
		return false;
	bool any = false;
	for (int y = r->y0 < 0 ? 0 : r->y0; y < r->y1 && y < a.h && !any; y++)
		for (int x = r->x0 < 0 ? 0 : r->x0; x < r->x1 && x < a.w && !any; x++)
			any = a.comp[y * a.w + x];
	area_free(&a);
	return any;
}

int walk_find_path(int sx, int sy, int gx, int gy, const walk_rect *rect, walk_point *out, int max)
{
	area a;
	if (max <= 0 || !area_build(sx, sy, &a))
		return -1;
	int w = a.w, h = a.h, n = w * h;
	uint8_t *fr = a.fr, *comp = a.comp, *safe = calloc((size_t)n, 1);
	int32_t *path = NULL;
	int legs = -1;
	grid gfree = { w, h, fr }, gcomp = { w, h, comp }, gsafe = { w, h, safe };
	int seedx = a.seedx, seedy = a.seedy;

	/* goal: the reachable pixel nearest to the click (or to the rectangle) */
	int tx = -1, ty = -1;
	long best1 = -1, best2 = -1;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			if (!comp[y * w + x])
				continue;
			long d1 = rect ? rect_dist2(rect, x, y) : 0;
			long d2 = (long)(x - gx) * (x - gx) + (long)(y - gy) * (y - gy);
			if (best1 < 0 || d1 < best1 || (d1 == best1 && d2 < best2)) {
				best1 = d1;
				best2 = d2;
				tx = x;
				ty = y;
			}
		}
	if (tx < 0)
		goto done;

	/* safe area: reachable pixels at least WALK_MARGIN px away from any blocked pixel,
	 * so that the straight legs (no collision test during an auto-walk) never touch
	 * the fence even with the game's rounding */
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			if (!comp[y * w + x])
				continue;
			bool ok = true;
			for (int dy = -WALK_MARGIN; dy <= WALK_MARGIN && ok; dy++)
				for (int dx = -WALK_MARGIN; dx <= WALK_MARGIN && ok; dx++)
					ok = g_ok(&gfree, x + dx, y + dy);
			safe[y * w + x] = ok;
		}

	int s2x, s2y, t2x, t2y, len = 0;
	const grid *g = &gsafe;
	if (g_nearest(&gsafe, sx, sy, &s2x, &s2y) && g_nearest(&gsafe, tx, ty, &t2x, &t2y))
		len = astar(&gsafe, s2x, s2y, t2x, t2y, &path);
	if (!len) {                                /* erosion cut the area: plan on the raw area */
		g = &gcomp;
		s2x = seedx;
		s2y = seedy;
		t2x = tx;
		t2y = ty;
		len = astar(&gcomp, s2x, s2y, t2x, t2y, &path);
		if (!len)
			goto done;
	}

	/* string pulling: from each corner, jump to the farthest path pixel in sight */
	walk_point pts[256];
	int np = 0;
	if (s2x != sx || s2y != sy)
		pts[np++] = (walk_point){ s2x, s2y };
	for (int i = 0; i < len - 1 && np < 254;) {
		int ix = path[i] % w, iy = path[i] / w, j = len - 1;
		while (j > i + 1 && !g_line(g, ix, iy, path[j] % w, path[j] / w))
			j--;
		pts[np++] = (walk_point){ path[j] % w, path[j] / w };
		i = j;
	}
	/* last step from the safe area onto the goal (e.g. into an exit rectangle) */
	if ((t2x != tx || t2y != ty) && g_line(&gcomp, t2x, t2y, tx, ty))
		pts[np++] = (walk_point){ tx, ty };

	/* drop zero-length legs */
	legs = 0;
	int px = sx, py = sy;
	for (int i = 0; i < np && legs < max; i++) {
		if (pts[i].x == px && pts[i].y == py)
			continue;
		out[legs++] = pts[i];
		px = pts[i].x;
		py = pts[i].y;
	}
done:
	free(path);
	free(safe);
	area_free(&a);
	return legs;
}

/* ------------------------------------------------------------------ driving Shaggy */
#define MAX_LEGS 64
static struct {
	bool active;
	int n, cur;                 /* legs; index of the leg Shaggy is walking now */
	walk_point leg[MAX_LEGS];
	int16_t dx[MAX_LEGS], dy[MAX_LEGS];
	int face[MAX_LEGS];
	uint16_t room, mystery;
} wd;

/* shaggy_set_dir ($00548E) with speed 1: the axis that takes longer (y counts double,
 * it is walked at half speed) gets $100, the other the proportional fraction (8.8). */
void walk_leg_dir(int fx, int fy, int tx, int ty, int16_t *dir_x, int16_t *dir_y, int *facing)
{
	int16_t d0 = (int16_t)(tx - fx), d1 = (int16_t)((ty - fy) * 2);
	uint16_t ax = (uint16_t)(d0 < 0 ? -d0 : d0), ay = (uint16_t)(d1 < 0 ? -d1 : d1);
	*facing = ay >= ax ? (d1 < 0 ? 2 : 3) : (d0 < 0 ? 1 : 0);
	int x, y;
	if (ay == 0) {
		x = 0x100;
		y = 0;
	} else if (ax == 0) {
		x = 0;
		y = 0x100;
	} else if (ay > ax) {
		x = (int)(((uint32_t)ax << 8) / ay);
		y = 0x100;
	} else {
		x = 0x100;
		y = (int)(((uint32_t)ay << 8) / ax);
	}
	*dir_x = (int16_t)(d0 < 0 ? -x : x);
	*dir_y = (int16_t)(d1 < 0 ? -y : y);
}

bool walk_active(void) { return wd.active; }

/* the game consumes SHAGGY_WP_* from the end (count-1 first): keep up to two of our
 * next legs queued behind the current one */
static void wp_write(int slot, int leg)
{
	RAM16(SHAGGY_WP_X + 2 * slot) = (uint16_t)wd.leg[leg].x;
	RAM16(SHAGGY_WP_X + 4 + 2 * slot) = (uint16_t)wd.leg[leg].y;
	RAM16(SHAGGY_WP_X + 8 + 2 * slot) = (uint16_t)wd.dx[leg];
	RAM16(SHAGGY_WP_X + 12 + 2 * slot) = (uint16_t)wd.dy[leg];
	RAM16(SHAGGY_WP_X + 16 + 2 * slot) = (uint16_t)wd.face[leg];
}

static void wp_refill(void)
{
	int queued = (int16_t)RAM16(SHAGGY_WP_COUNT);
	if (queued < 0)
		queued = 0;
	while (queued < 2 && wd.cur + 1 + queued < wd.n) {
		if (queued == 1) {             /* move the pending entry up, ours goes behind it */
			for (int k = 0; k < 10; k += 2)
				RAM16(SHAGGY_WP_X + 2 + 2 * k) = RAM16(SHAGGY_WP_X + 2 * k);
		}
		wp_write(0, wd.cur + 1 + queued);
		queued++;
		RAM16(SHAGGY_WP_COUNT) = (uint16_t)queued;
	}
}

static void drop(bool stop)
{
	if (stop)
		clr8(ACTORS_MOVING, 0);
	if ((int16_t)RAM16(SHAGGY_WP_COUNT) > 0)
		RAM16(SHAGGY_WP_COUNT) = 0;   /* stale entries would be popped by a later wall slide */
	wd.active = false;
}

bool walk_start(const walk_point *legs, int n)
{
	if (bit8(VBLANK_FLAGS, 7) || bit8(VBLANK_FLAGS, 3) || bit8(ACTORS_ON_PATH, 0) || n <= 0)
		return false;
	if (n > MAX_LEGS)
		n = MAX_LEGS;
	int fx = (int16_t)RAM16(ACTOR_X(0)), fy = (int16_t)RAM16(ACTOR_Y(0));
	for (int i = 0; i < n; i++) {
		wd.leg[i] = legs[i];
		walk_leg_dir(fx, fy, legs[i].x, legs[i].y, &wd.dx[i], &wd.dy[i], &wd.face[i]);
		fx = legs[i].x;
		fy = legs[i].y;
	}
	wd.n = n;
	wd.cur = 0;
	wd.active = true;
	wd.room = RAM16(ROOM_INDEX);
	wd.mystery = RAM16(MYSTERY_INDEX);

	/* first leg: what shaggy_walk_to's $005592 does, without its wait for the turn
	 * animation (the walk starts at once; the turn animation plays while he walks) */
	RAM16(SHAGGY_WP_COUNT) = 0;
	RAM16(ACTOR_TARGET_X(0)) = (uint16_t)legs[0].x;
	RAM16(ACTOR_TARGET_Y(0)) = (uint16_t)legs[0].y;
	RAM16(SHAGGY_DIR_X) = (uint16_t)wd.dx[0];
	RAM16(SHAGGY_DIR_Y) = (uint16_t)wd.dy[0];
	RAM8(SHAGGY_TURNING) = 0;        /* a D-pad turn in progress would steal the frame */
	uint16_t facing = RAM16(SHAGGY_FACING), anim = RAM16(ACTOR_ANIM(0));
	if (anim != (uint16_t)wd.face[0]) {
		RAM16(ACTOR_ANIM(0)) = ROM16(SHAGGY_TURN_TABLE + 8u * (facing & 3) + 2u * (unsigned)wd.face[0]);
		RAM16(SHAGGY_ANIM_LOCK) = 0xFFFF;
		set8(ACTORS_ANIM_NEW, 0);
	}
	RAM16(SHAGGY_FACING) = (uint16_t)wd.face[0];
	wp_refill();
	set8(ACTORS_MOVING, 0);
	return true;
}

void walk_cancel(void)
{
	if (wd.active)
		drop(bit8(ACTORS_MOVING, 0));
}

void walk_tick(void)
{
	if (!wd.active)
		return;
	if (RAM16(ROOM_INDEX) != wd.room || RAM16(MYSTERY_INDEX) != wd.mystery) {
		wd.active = false;                  /* new room: the room loader owns everything */
		return;
	}
	/* which of our legs is Shaggy walking?  (the game pops the queue on arrival) */
	int tx = (int16_t)RAM16(ACTOR_TARGET_X(0)), ty = (int16_t)RAM16(ACTOR_TARGET_Y(0));
	int k = wd.cur;
	while (k < wd.n && !(wd.leg[k].x == tx && wd.leg[k].y == ty))
		k++;
	bool ours = k < wd.n;
	if (!bit8(ACTORS_MOVING, 0) || !ours) {
		drop(false);                        /* arrived, or the D-pad / a slide took over */
		return;
	}
	if (bit8(VBLANK_FLAGS, 7) || bit8(VBLANK_FLAGS, 3)) {
		drop(true);                         /* a script (e.g. a walk-on trigger) or the
		                                     * pointer took over: stand still, like the D-pad */
		return;
	}
	wd.cur = k;
	wp_refill();
}
