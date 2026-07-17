#ifndef IMGUI_DEMO_STDLIB_H
#define IMGUI_DEMO_STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void  *malloc(size_t n);
void   free(void *p);
void  *realloc(void *p, size_t n);
void   qsort(void *base, size_t count, size_t size,
             int (*compar)(const void *, const void *));
int    atoi(const char *s);
double atof(const char *s);
void   abort(void);

#ifdef __cplusplus
}
#endif

#endif
