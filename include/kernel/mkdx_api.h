#ifndef MYKERNEL_KERNEL_MKDX_API_H
#define MYKERNEL_KERNEL_MKDX_API_H

#include <kernel/types.h>

/*
 * Thin ABI between kernel core (syscalls) and mkdx.kmod.
 * mkdx registers this table from kmod_init; core never links gfx objects.
 */
typedef struct mkdx_api {
    int  (*info)(uint32_t *w, uint32_t *h, uint32_t *bpp);
    int  (*present)(void);
    /* win_id > 0 → damage that window's frame; else full-screen dirty. */
    void (*mark_dirty)(int win_id);
    /* Optional: window-local damage rect (x,y,w,h). NULL = unsupported. */
    void (*mark_dirty_rect)(int win_id, int32_t x, int32_t y, int32_t w, int32_t h);

    long (*wm_create)(const void *args, uint32_t owner_pid);
    int  (*wm_set)(int id, const void *opts);
    int  (*wm_get)(int id, void *out);
    int  (*wm_close)(int id);
    int  (*wm_destroy)(int id);
    void (*wm_destroy_by_pid)(int pid);
    int  (*wm_map)(int id, void *out);
    int  (*wm_move)(int id, int32_t x, int32_t y);
    int  (*wm_resize)(int id, int32_t w, int32_t h);
    int  (*wm_focus)(int id);
    int  (*wm_show)(int id, int vis);
    int  (*wm_get_frame)(int id, void *out);
    int  (*wm_pop_key)(int id);
    int  (*wm_focused_id)(void);
    int  (*wm_find)(const char *title);
    /* Optional: find by class_name; NULL = unsupported. */
    int  (*wm_find_class)(const char *class_name);

    int  (*fill)(const void *args, int rounded);
    int  (*set_wallpaper)(const void *args);
    int  (*input_state)(void *out);
    /* Poll input + cursor; also advances compositor frame if dirty (C10). */
    void (*pump_input)(void);

    /* Per-process console (Wave O). */
    int     (*console_alloc)(int pid, const char *name, int visible);
    void    (*console_free)(int pid);
    ssize_t (*console_write)(int pid, const void *buf, size_t len);
    int     (*console_show)(int pid, int visible);
} mkdx_api_t;

void              mkdx_api_register(const mkdx_api_t *api);
const mkdx_api_t *mkdx_api_get(void);

#endif
