#ifndef IMGUI_DEMO_MATH_H
#define IMGUI_DEMO_MATH_H

/* Declarations only — ImGui uses ImFabs/ImSqrt wrappers from imconfig.h. */

#ifdef __cplusplus
extern "C" {
#endif

float fabsf(float x);
float sqrtf(float x);
float fmodf(float x, float y);
float cosf(float x);
float sinf(float x);
float acosf(float x);
float atan2f(float y, float x);
float powf(float x, float y);
float logf(float x);
float ceilf(float x);
float floorf(float x);

double fabs(double x);
double sqrt(double x);
double fmod(double x, double y);
double cos(double x);
double sin(double x);
double acos(double x);
double atan2(double y, double x);
double pow(double x, double y);
double log(double x);
double ceil(double x);
double floor(double x);

#ifdef __cplusplus
}
#endif

#endif
