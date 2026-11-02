/* apps/sdk/libc/stdlib.h — number parsing; an app has no heap of the OS's to ask for. */
#pragma once
#include <stddef.h>

double strtod(const char *s, char **end);
long   strtol(const char *s, char **end, int base);
int    atoi(const char *s);
static inline int abs(int v) { return v < 0 ? -v : v; }
