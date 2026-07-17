#ifndef IMGUI_DEMO_CTYPE_H
#define IMGUI_DEMO_CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

static inline int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static inline int isdigit(int c)
{
    return c >= '0' && c <= '9';
}

static inline int isxdigit(int c)
{
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static inline int toupper(int c)
{
    return (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
}

#ifdef __cplusplus
}
#endif

#endif
