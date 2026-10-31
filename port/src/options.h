/* Host options (GitLab #28): one table, three sources.  Precedence, lowest first:
 *   built-in default < ini file (~/.config/scooby-decomp/scooby.ini) < command line
 *   (--options k=v,k=v and the dedicated flags such as --scale, which are shorthands).
 * SDL-free so that it can be unit-tested.  Options that change the game itself (not just
 * the picture, sound or controls) are flagged `game` and must default to off. */
#ifndef SCOOBY_OPTIONS_H
#define SCOOBY_OPTIONS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

enum { OPT_DEFAULT = 0, OPT_INI = 1, OPT_CLI = 2 };

void options_init(void);                         /* all defaults */
bool options_load(const char *path);             /* ini; false if unreadable (not an error) */
bool options_save(const char *path);             /* writes every option, with comments */
/* set one option; on error returns false and fills err.  source: OPT_INI / OPT_CLI */
bool options_set(const char *key, const char *val, int source, char *err, size_t errlen);
/* "k=v,k=v" (a value may not contain a comma; key lists use '|') */
bool options_parse_list(const char *list, int source, char *err, size_t errlen);

const char *options_str(const char *key);        /* NULL for an unknown key */
int options_int(const char *key);
bool options_bool(const char *key);
int options_source(const char *key);

/* human or JSON listing of every option: key, type, default, current value, choices, description */
void options_print(FILE *f, bool json);

/* ~/.config/scooby-decomp/scooby.ini (XDG_CONFIG_HOME honoured) */
void options_default_path(char *buf, size_t n);
/* ~/.local/share/scooby-decomp (XDG_DATA_HOME honoured) */
void options_data_dir(char *buf, size_t n);

#endif
