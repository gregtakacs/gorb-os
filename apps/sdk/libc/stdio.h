/* apps/sdk/libc/stdio.h — formatting only; an app has no files and no stdout. */
#pragma once
#include <stdarg.h>
#include <stddef.h>

int snprintf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
