#ifndef IMGUI_DEMO_STDIO_H
#define IMGUI_DEMO_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FILE FILE;

int  sscanf(const char *str, const char *fmt, ...);
int  vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int  snprintf(char *buf, size_t size, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
