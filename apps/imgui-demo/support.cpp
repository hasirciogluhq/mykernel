#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#include "mykernel_imconfig.h"
#include <user/sdk/syscall.hpp>

namespace {

constexpr size_t kHeapBytes = 2u * 1024u * 1024u;
alignas(16) uint8_t g_heap[kHeapBytes];

/* First-fit freelist: ImGui font atlas / draw lists realloc heavily; a bump
 * allocator (free=no-op) exhausts 2MB and then abort() freezes the OS. */
struct FreeNode {
    size_t size; /* usable payload bytes */
    FreeNode *next;
};

FreeNode *g_free = nullptr;
bool g_heap_ready = false;

void heap_init_once()
{
    if (g_heap_ready)
        return;
    g_free = reinterpret_cast<FreeNode *>(g_heap);
    g_free->size = kHeapBytes - sizeof(FreeNode);
    g_free->next = nullptr;
    g_heap_ready = true;
}

size_t align16(size_t n)
{
    return (n + 15u) & ~15u;
}

void freelist_insert(FreeNode *node)
{
    node->next = g_free;
    g_free = node;
}

void free_merge_insert(FreeNode *node)
{
    /* Merge with any adjacent free blocks, then push. */
    for (;;) {
        bool merged = false;
        FreeNode **pp = &g_free;
        while (*pp) {
            FreeNode *b = *pp;
            uint8_t *node_end =
                reinterpret_cast<uint8_t *>(node) + sizeof(FreeNode) + node->size;
            uint8_t *b_end =
                reinterpret_cast<uint8_t *>(b) + sizeof(FreeNode) + b->size;
            if (reinterpret_cast<uint8_t *>(b) == node_end) {
                *pp = b->next;
                node->size += sizeof(FreeNode) + b->size;
                merged = true;
                break;
            }
            if (reinterpret_cast<uint8_t *>(node) == b_end) {
                *pp = b->next;
                b->size += sizeof(FreeNode) + node->size;
                node = b;
                merged = true;
                break;
            }
            pp = &(*pp)->next;
        }
        if (!merged)
            break;
    }
    freelist_insert(node);
}

float expf_approx(float x)
{
    if (x < -20.0f)
        return 0.0f;
    if (x > 20.0f)
        return 1.0e8f;
    float sum = 1.0f;
    float term = 1.0f;
    for (int i = 1; i < 12; i++) {
        term *= x / (float)i;
        sum += term;
    }
    return sum;
}

void qsort_swap(uint8_t *a, uint8_t *b, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        uint8_t t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

void qsort_rec(uint8_t *base, size_t count, size_t size,
               int (*compar)(const void *, const void *))
{
    if (count < 2)
        return;
    uint8_t *pivot = base + (count / 2) * size;
    size_t i = 0, j = count - 1;
    for (;;) {
        while (compar(base + i * size, pivot) < 0)
            i++;
        while (compar(base + j * size, pivot) > 0)
            j--;
        if (i >= j)
            break;
        qsort_swap(base + i * size, base + j * size, size);
        if (pivot == base + i * size)
            pivot = base + j * size;
        else if (pivot == base + j * size)
            pivot = base + i * size;
        i++;
        if (j == 0)
            break;
        j--;
    }
    qsort_rec(base, i, size, compar);
    qsort_rec(base + i * size, count - i, size, compar);
}

} // namespace

extern "C" {

void *malloc(size_t n)
{
    heap_init_once();
    if (n == 0)
        n = 1;
    n = align16(n);

    FreeNode **pp = &g_free;
    while (*pp) {
        FreeNode *node = *pp;
        if (node->size >= n) {
            *pp = node->next;
            const size_t remain = node->size - n;
            if (remain >= sizeof(FreeNode) + 32u) {
                FreeNode *split = reinterpret_cast<FreeNode *>(
                    reinterpret_cast<uint8_t *>(node) + sizeof(FreeNode) + n);
                split->size = remain - sizeof(FreeNode);
                freelist_insert(split);
                node->size = n;
            }
            return reinterpret_cast<uint8_t *>(node) + sizeof(FreeNode);
        }
        pp = &node->next;
    }
    return nullptr;
}

void free(void *p)
{
    if (!p)
        return;
    heap_init_once();
    FreeNode *node = reinterpret_cast<FreeNode *>(
        static_cast<uint8_t *>(p) - sizeof(FreeNode));
    free_merge_insert(node);
}

void *realloc(void *p, size_t n)
{
    if (!p)
        return malloc(n);
    if (n == 0) {
        free(p);
        return nullptr;
    }
    FreeNode *node = reinterpret_cast<FreeNode *>(
        static_cast<uint8_t *>(p) - sizeof(FreeNode));
    if (align16(n) <= node->size)
        return p;
    void *q = malloc(n);
    if (!q)
        return nullptr;
    uint8_t *dst = static_cast<uint8_t *>(q);
    uint8_t *src = static_cast<uint8_t *>(p);
    for (size_t i = 0; i < node->size && i < n; i++)
        dst[i] = src[i];
    free(p);
    return q;
}

void abort(void)
{
    /* Blocked sleep — bare yield(0) keeps Ready forever and burns CPU. */
    for (;;)
        hsrc::sdk::yield(32u);
}

char *strchr(const char *s, int c)
{
    char ch = (char)c;
    if (!s)
        return nullptr;
    while (*s) {
        if (*s == ch)
            return const_cast<char *>(s);
        s++;
    }
    return ch == 0 ? const_cast<char *>(s) : nullptr;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!haystack || !needle)
        return nullptr;
    if (!needle[0])
        return const_cast<char *>(haystack);
    for (const char *h = haystack; *h; h++) {
        const char *a = h;
        const char *b = needle;
        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (!*b)
            return const_cast<char *>(h);
    }
    return nullptr;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = static_cast<const uint8_t *>(s);
    uint8_t ch = (uint8_t)c;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == ch)
            return const_cast<uint8_t *>(p + i);
    }
    return nullptr;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = static_cast<const uint8_t *>(a);
    const uint8_t *y = static_cast<const uint8_t *>(b);
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return (int)x[i] - (int)y[i];
    }
    return 0;
}

void qsort(void *base, size_t count, size_t size,
           int (*compar)(const void *, const void *))
{
    if (!base || count < 2 || size == 0 || !compar)
        return;
    qsort_rec(static_cast<uint8_t *>(base), count, size, compar);
}

int atoi(const char *s)
{
    if (!s)
        return 0;
    while (*s == ' ' || *s == '\t')
        s++;
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v * sign;
}

double atof(const char *s)
{
    return imgui_atof(s);
}

int sscanf(const char *str, const char *fmt, ...)
{
    (void)str;
    (void)fmt;
    return 0;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    (void)fmt;
    (void)ap;
    if (!buf || size == 0)
        return 0;
    buf[0] = 0;
    return 0;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    (void)fmt;
    if (!buf || size == 0)
        return 0;
    buf[0] = 0;
    return 0;
}

float fabsf(float x) { return imgui_fabsf(x); }
float sqrtf(float x) { return imgui_sqrtf(x); }
float fmodf(float x, float y) { return imgui_fmodf(x, y); }
float cosf(float x) { return imgui_cosf(x); }
float sinf(float x) { return imgui_sinf(x); }
float acosf(float x) { return imgui_acosf(x); }
float atan2f(float y, float x) { return imgui_atan2f(y, x); }
float powf(float x, float y) { return imgui_powf(x, y); }
float logf(float x) { return imgui_logf(x); }
float ceilf(float x) { return imgui_ceilf(x); }
float floorf(float x) { return imgui_floorf(x); }

double fabs(double x) { return (double)imgui_fabsf((float)x); }
double sqrt(double x) { return (double)imgui_sqrtf((float)x); }
double fmod(double x, double y) { return (double)imgui_fmodf((float)x, (float)y); }
double cos(double x) { return (double)imgui_cosf((float)x); }
double sin(double x) { return (double)imgui_sinf((float)x); }
double acos(double x) { return (double)imgui_acosf((float)x); }
double atan2(double y, double x) { return (double)imgui_atan2f((float)y, (float)x); }
double pow(double x, double y) { return (double)imgui_powf((float)x, (float)y); }
double log(double x) { return (double)imgui_logf((float)x); }
double ceil(double x) { return (double)imgui_ceilf((float)x); }
double floor(double x) { return (double)imgui_floorf((float)x); }

} // extern "C"

float imgui_fabsf(float x)
{
    return x < 0.0f ? -x : x;
}

float imgui_sqrtf(float x)
{
    if (x <= 0.0f)
        return 0.0f;
    float g = x;
    for (int i = 0; i < 8; i++)
        g = 0.5f * (g + x / g);
    return g;
}

float imgui_fmodf(float x, float y)
{
    if (y == 0.0f)
        return 0.0f;
    int q = (int)(x / y);
    return x - (float)q * y;
}

float imgui_cosf(float x)
{
    constexpr float kPi = 3.14159265f;
    constexpr float kTwoPi = 6.2831853f;
    /* Avoid infinite loops on NaN / huge values (font bake soft-float). */
    if (!(x == x))
        return 1.0f;
    if (x > 1.0e6f || x < -1.0e6f)
        return 1.0f;
    x = x - kTwoPi * (float)(int)(x / kTwoPi);
    if (x > kPi)
        x -= kTwoPi;
    if (x < -kPi)
        x += kTwoPi;
    float x2 = x * x;
    return 1.0f - x2 / 2.0f + x2 * x2 / 24.0f - x2 * x2 * x2 / 720.0f;
}

float imgui_sinf(float x)
{
    constexpr float kHalfPi = 1.5707963f;
    return imgui_cosf(x - kHalfPi);
}

float imgui_atan2f(float y, float x)
{
    constexpr float kPi = 3.14159265f;
    if (x == 0.0f) {
        if (y > 0.0f)
            return kPi * 0.5f;
        if (y < 0.0f)
            return -kPi * 0.5f;
        return 0.0f;
    }
    float a = y / x;
    float a2 = a * a;
    float at = a / (1.0f + 0.28f * a2);
    if (x < 0.0f)
        return (y >= 0.0f) ? (at + kPi) : (at - kPi);
    return at;
}

float imgui_acosf(float x)
{
    if (x <= -1.0f)
        return 3.14159265f;
    if (x >= 1.0f)
        return 0.0f;
    return imgui_atan2f(imgui_sqrtf(1.0f - x * x), x);
}

float imgui_logf(float x)
{
    if (x <= 0.0f)
        return -100.0f;
    float y = 0.0f;
    while (x > 1.5f) {
        x *= 0.5f;
        y += 0.693147f;
    }
    while (x < 0.7f) {
        x *= 2.0f;
        y -= 0.693147f;
    }
    float z = x - 1.0f;
    float z2 = z * z;
    return y + z - z2 / 2.0f + z2 * z / 3.0f - z2 * z2 / 4.0f;
}

float imgui_powf(float x, float y)
{
    if (x <= 0.0f)
        return 0.0f;
    return expf_approx(y * imgui_logf(x));
}

float imgui_ceilf(float x)
{
    int i = (int)x;
    if ((float)i == x)
        return x;
    return x > 0.0f ? (float)(i + 1) : (float)i;
}

float imgui_floorf(float x)
{
    int i = (int)x;
    if ((float)i == x)
        return x;
    return x >= 0.0f ? (float)i : (float)(i - 1);
}

double imgui_atof(const char *s)
{
    if (!s)
        return 0.0;
    while (*s == ' ' || *s == '\t')
        s++;
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    double v = 0.0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10.0 + (double)(*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
        double place = 0.1;
        while (*s >= '0' && *s <= '9') {
            v += place * (double)(*s - '0');
            place *= 0.1;
            s++;
        }
    }
    return v * (double)sign;
}

void *operator new(size_t n)
{
    void *p = malloc(n);
    if (!p)
        abort();
    return p;
}

void *operator new[](size_t n)
{
    return operator new(n);
}

void operator delete(void *p) noexcept
{
    free(p);
}

void operator delete[](void *p) noexcept
{
    free(p);
}

void operator delete(void *p, size_t) noexcept
{
    free(p);
}

void operator delete[](void *p, size_t) noexcept
{
    free(p);
}
