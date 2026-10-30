/* Host options: see options.h. */
#include "options.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

enum { T_BOOL, T_INT, T_ENUM, T_STR, T_KEYS };

typedef struct {
	const char *key;
	int type;
	const char *def;
	const char *choices;      /* enum: "a|b|c"; int: "min..max" */
	int game;                 /* changes the game itself: must default to off */
	const char *desc;
	char val[256];
	int source;
} opt_t;

#define OPT(k, t, d, c, g, desc) { k, t, d, c, g, desc, "", OPT_DEFAULT }
static opt_t opts[] = {
	OPT("filter", T_ENUM, "nearest", "nearest|smooth|crt", 0, "output scaling filter (F3 cycles it in the game)"),
	OPT("scale", T_INT, "3", "1..8", 0, "initial window size as a multiple of 320x224"),
	OPT("fullscreen", T_BOOL, "off", "", 0, "start fullscreen (F11 toggles)"),
	OPT("vsync", T_BOOL, "on", "", 0, "sync presentation to the display"),
	OPT("renderer", T_ENUM, "native", "native|gpgx", 0, "who draws the picture: the C renderer or the emulator core"),
	OPT("mouse", T_BOOL, "on", "", 0, "mouse control (click to walk / use verbs, docs/mouse.md)"),
	OPT("mute", T_BOOL, "off", "", 0, "no audio"),
	OPT("volume", T_INT, "100", "0..100", 0, "audio volume in percent"),
	OPT("rom", T_STR, "", "", 0, "path of the ROM to run (remembered automatically)"),
	/* keybindings: SDL key names, several separated by | */
	OPT("key_up", T_KEYS, "Up", "", 0, "D-pad up"),
	OPT("key_down", T_KEYS, "Down", "", 0, "D-pad down"),
	OPT("key_left", T_KEYS, "Left", "", 0, "D-pad left"),
	OPT("key_right", T_KEYS, "Right", "", 0, "D-pad right"),
	OPT("key_a", T_KEYS, "Z|A", "", 0, "button A"),
	OPT("key_b", T_KEYS, "X|S", "", 0, "button B"),
	OPT("key_c", T_KEYS, "C|D", "", 0, "button C"),
	OPT("key_start", T_KEYS, "Return|Keypad Enter", "", 0, "Start button"),
};
#define NOPTS ((int)(sizeof opts / sizeof opts[0]))

static opt_t *find(const char *key)
{
	for (int i = 0; i < NOPTS; i++)
		if (!strcmp(opts[i].key, key))
			return &opts[i];
	return NULL;
}

void options_init(void)
{
	for (int i = 0; i < NOPTS; i++) {
		snprintf(opts[i].val, sizeof opts[i].val, "%s", opts[i].def);
		opts[i].source = OPT_DEFAULT;
	}
}

static bool parse_bool(const char *v, bool *out)
{
	static const char *yes[] = { "1", "on", "true", "yes" }, *no[] = { "0", "off", "false", "no" };
	for (int i = 0; i < 4; i++) {
		if (!strcasecmp(v, yes[i])) { *out = true; return true; }
		if (!strcasecmp(v, no[i])) { *out = false; return true; }
	}
	return false;
}

bool options_set(const char *key, const char *val, int source, char *err, size_t errlen)
{
	opt_t *o = find(key);
	if (!o) {
		snprintf(err, errlen, "unknown option '%s' (see --options --json)", key);
		return false;
	}
	char norm[256];
	snprintf(norm, sizeof norm, "%s", val);
	switch (o->type) {
	case T_BOOL: {
		bool b;
		if (!parse_bool(val, &b)) {
			snprintf(err, errlen, "%s: '%s' is not on/off", key, val);
			return false;
		}
		snprintf(norm, sizeof norm, "%s", b ? "on" : "off");
		break;
	}
	case T_INT: {
		int lo = 0, hi = 0;
		char *end;
		long n = strtol(val, &end, 10);
		sscanf(o->choices, "%d..%d", &lo, &hi);
		if (end == val || *end || n < lo || n > hi) {
			snprintf(err, errlen, "%s: '%s' is not a number in %s", key, val, o->choices);
			return false;
		}
		snprintf(norm, sizeof norm, "%ld", n);
		break;
	}
	case T_ENUM: {
		char c[128];
		snprintf(c, sizeof c, "%s", o->choices);
		bool ok = false;
		for (char *save, *t = strtok_r(c, "|", &save); t; t = strtok_r(NULL, "|", &save))
			if (!strcasecmp(t, val)) {
				snprintf(norm, sizeof norm, "%s", t);
				ok = true;
			}
		if (!ok) {
			snprintf(err, errlen, "%s: '%s' is not one of %s", key, val, o->choices);
			return false;
		}
		break;
	}
	}
	snprintf(o->val, sizeof o->val, "%s", norm);
	o->source = source;
	return true;
}

bool options_parse_list(const char *list, int source, char *err, size_t errlen)
{
	char buf[1024];
	snprintf(buf, sizeof buf, "%s", list);
	for (char *save, *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
		char *eq = strchr(t, '=');
		if (!eq) {
			snprintf(err, errlen, "'%s' is not key=value", t);
			return false;
		}
		*eq = 0;
		while (isspace((unsigned char)*t))
			t++;
		if (!options_set(t, eq + 1, source, err, errlen))
			return false;
	}
	return true;
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

bool options_load(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	char line[600], err[256];
	int n = 0;
	while (fgets(line, sizeof line, f)) {
		n++;
		char *s = trim(line);
		if (!*s || *s == '#' || *s == ';' || *s == '[')
			continue;
		char *eq = strchr(s, '=');
		if (!eq) {
			fprintf(stderr, "%s:%d: ignored (not key = value)\n", path, n);
			continue;
		}
		*eq = 0;
		if (!options_set(trim(s), trim(eq + 1), OPT_INI, err, sizeof err))
			fprintf(stderr, "%s:%d: ignored (%s)\n", path, n, err);
	}
	fclose(f);
	return true;
}

static void mkdirs_for(const char *file)
{
	char tmp[1024];
	snprintf(tmp, sizeof tmp, "%s", file);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0777);
			*p = '/';
		}
}

bool options_save(const char *path)
{
	mkdirs_for(path);
	FILE *f = fopen(path, "w");
	if (!f)
		return false;
	fprintf(f, "# Scooby-Doo Mystery native port - options.  Edit freely; --options k=v overrides for one run.\n"
	           "# Key names for key_*: SDL names (Up, Z, Return, Left Shift, F1 ...), several joined with |.\n");
	for (int i = 0; i < NOPTS; i++) {
		fprintf(f, "\n# %s", opts[i].desc);
		if (opts[i].choices[0])
			fprintf(f, "  [%s]", opts[i].choices);
		fprintf(f, "\n%s = %s\n", opts[i].key, opts[i].val);
	}
	return fclose(f) == 0;
}

const char *options_str(const char *key)
{
	opt_t *o = find(key);
	return o ? o->val : NULL;
}
int options_int(const char *key) { const char *s = options_str(key); return s ? atoi(s) : 0; }
bool options_bool(const char *key) { const char *s = options_str(key); return s && !strcmp(s, "on"); }
int options_source(const char *key) { opt_t *o = find(key); return o ? o->source : 0; }

static void json_str(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			fputc('\\', f);
		fputc(*s, f);
	}
	fputc('"', f);
}

void options_print(FILE *f, bool json)
{
	static const char *tn[] = { "bool", "int", "enum", "string", "keys" };
	if (json)
		fputs("[\n", f);
	for (int i = 0; i < NOPTS; i++) {
		opt_t *o = &opts[i];
		if (json) {
			fprintf(f, "  {\"key\": \"%s\", \"type\": \"%s\", \"default\": ", o->key, tn[o->type]);
			json_str(f, o->def);
			fputs(", \"value\": ", f);
			json_str(f, o->val);
			fputs(", \"choices\": ", f);
			json_str(f, o->choices);
			fprintf(f, ", \"game_changing\": %s, \"description\": ", o->game ? "true" : "false");
			json_str(f, o->desc);
			fprintf(f, "}%s\n", i + 1 < NOPTS ? "," : "");
		} else {
			fprintf(f, "%-12s %-8s %-10s %s%s%s%s\n", o->key, tn[o->type], o->val, o->desc,
			        o->choices[0] ? "  [" : "", o->choices, o->choices[0] ? "]" : "");
		}
	}
	if (json)
		fputs("]\n", f);
}

void options_default_path(char *buf, size_t n)
{
	const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	if (x && *x)
		snprintf(buf, n, "%s/scooby-decomp/scooby.ini", x);
	else
		snprintf(buf, n, "%s/.config/scooby-decomp/scooby.ini", h ? h : ".");
}

void options_data_dir(char *buf, size_t n)
{
	const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
	if (x && *x)
		snprintf(buf, n, "%s/scooby-decomp", x);
	else
		snprintf(buf, n, "%s/.local/share/scooby-decomp", h ? h : ".");
}
