/* Interpreted-instruction profile (GitLab #24): which 68k instructions still run on the emulated
 * CPU when C replacements are enabled, split into top level and nested (68k called from a C function
 * through hle_call68k / hle_run68k_until).  Enabled by the environment variable
 *     SCOOBY_IPPROF=FILE      ("%d" in FILE is replaced by the pid)
 * and written when the program exits (normally or via exit()).  Used by scoobyport and frameprof.
 * Format and the aggregation script: tools/py/remaining68k.py, docs/remaining68k.md. */
#ifndef SCOOBY_IPPROF_H
#define SCOOBY_IPPROF_H
void ipprof_init(const char *hle_spec);   /* no-op unless SCOOBY_IPPROF is set */
#endif
