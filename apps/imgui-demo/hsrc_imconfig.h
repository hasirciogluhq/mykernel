#pragma once

#define IM_ASSERT(_EXPR)                        ((void)(_EXPR))
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DISABLE_WIN32_FUNCTIONS
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
#define IMGUI_DISABLE_FILE_FUNCTIONS
#define IMGUI_DISABLE_DEFAULT_ALLOCATORS
#define IMGUI_DISABLE_DEMO_WINDOWS
#define IMGUI_DISABLE_DEBUG_TOOLS
#define IMGUI_DISABLE_SSE
#define IMGUI_USE_STB_SPRINTF
#define IMGUI_DISABLE_DEFAULT_MATH_FUNCTIONS
#define IMGUI_DEFINE_MATH_OPERATORS

/* Soft-float helpers (implemented in support.cpp). */
float imgui_fabsf(float x);
float imgui_sqrtf(float x);
float imgui_fmodf(float x, float y);
float imgui_cosf(float x);
float imgui_sinf(float x);
float imgui_acosf(float x);
float imgui_atan2f(float y, float x);
float imgui_powf(float x, float y);
float imgui_logf(float x);
float imgui_ceilf(float x);
float imgui_floorf(float x);
double imgui_atof(const char *s);

#define ImFabs(X)           imgui_fabsf(X)
#define ImSqrt(X)           imgui_sqrtf(X)
#define ImFmod(X, Y)        imgui_fmodf((X), (Y))
#define ImCos(X)            imgui_cosf(X)
#define ImSin(X)            imgui_sinf(X)
#define ImAcos(X)           imgui_acosf(X)
#define ImAtan2(Y, X)       imgui_atan2f((Y), (X))
#define ImAtof(STR)         imgui_atof(STR)
#define ImCeil(X)           imgui_ceilf(X)

static inline float  ImPow(float x, float y)    { return imgui_powf(x, y); }
static inline double ImPow(double x, double y)  { return (double)imgui_powf((float)x, (float)y); }
static inline float  ImLog(float x)             { return imgui_logf(x); }
static inline double ImLog(double x)            { return (double)imgui_logf((float)x); }
static inline int    ImAbs(int x)               { return x < 0 ? -x : x; }
static inline float  ImAbs(float x)             { return imgui_fabsf(x); }
static inline double ImAbs(double x)            { return (double)imgui_fabsf((float)x); }
static inline float  ImSign(float x)            { return (x < 0.0f) ? -1.0f : (x > 0.0f) ? 1.0f : 0.0f; }
static inline double ImSign(double x)           { return (x < 0.0) ? -1.0 : (x > 0.0) ? 1.0 : 0.0; }
static inline float  ImRsqrt(float x)           { return 1.0f / imgui_sqrtf(x); }
static inline double ImRsqrt(double x)          { return 1.0 / (double)imgui_sqrtf((float)x); }
