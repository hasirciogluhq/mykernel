#include <drivers/vfs_fs.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <kernel/errno.h>

#define WATCH_MAX  32
#define EVENT_MAX  64

typedef struct watch {
    int      wd;
    char     path[VFS_PATH_MAX];
    uint32_t mask;
    int      used;
    size_t   head, tail, count;
    fsnotify_event_t q[EVENT_MAX];
} watch_t;

static watch_t g_watches[WATCH_MAX];
static int g_next_wd = 1;

void vfs_fsnotify_init(void)
{
    memset(g_watches, 0, sizeof(g_watches));
}

int vfs_fsnotify_add_watch(const char *path, uint32_t mask)
{
    size_t i;
    if (!path)
        return -EINVAL;
    for (i = 0; i < WATCH_MAX; i++) {
        if (!g_watches[i].used) {
            g_watches[i].used = 1;
            g_watches[i].wd = g_next_wd++;
            strncpy(g_watches[i].path, path, VFS_PATH_MAX - 1);
            g_watches[i].mask = mask ? mask : (FS_CREATE | FS_DELETE | FS_MODIFY | FS_MOVE | FS_ACCESS);
            g_watches[i].head = g_watches[i].tail = g_watches[i].count = 0;
            return g_watches[i].wd;
        }
    }
    return -ENOSPC;
}

int vfs_fsnotify_rm_watch(int wd)
{
    size_t i;
    for (i = 0; i < WATCH_MAX; i++) {
        if (g_watches[i].used && g_watches[i].wd == wd) {
            g_watches[i].used = 0;
            return 0;
        }
    }
    return -ENOENT;
}

void vfs_fsnotify_event(const char *path, uint32_t mask, const char *name)
{
    size_t i;
    size_t plen;

    if (!path)
        return;
    plen = strlen(path);
    for (i = 0; i < WATCH_MAX; i++) {
        watch_t *w = &g_watches[i];
        size_t wlen;
        if (!w->used || !(w->mask & mask))
            continue;
        wlen = strlen(w->path);
        if (strncmp(path, w->path, wlen) != 0)
            continue;
        if (w->path[wlen - 1] != '/' && path[wlen] != '/' && path[wlen] != 0)
            continue;
        if (w->count >= EVENT_MAX) {
            w->head = (w->head + 1) % EVENT_MAX;
            w->count--;
        }
        {
            fsnotify_event_t *e = &w->q[w->tail];
            memset(e, 0, sizeof(*e));
            e->mask = mask;
            strncpy(e->path, path, sizeof(e->path) - 1);
            if (name)
                strncpy(e->name, name, sizeof(e->name) - 1);
            w->tail = (w->tail + 1) % EVENT_MAX;
            w->count++;
        }
        (void)plen;
    }
}

int vfs_fsnotify_read(int wd, fsnotify_event_t *events, size_t max)
{
    size_t i, n = 0;
    watch_t *w = NULL;
    if (!events || max == 0)
        return -EINVAL;
    for (i = 0; i < WATCH_MAX; i++) {
        if (g_watches[i].used && g_watches[i].wd == wd) {
            w = &g_watches[i];
            break;
        }
    }
    if (!w)
        return -ENOENT;
    while (w->count > 0 && n < max) {
        events[n++] = w->q[w->head];
        w->head = (w->head + 1) % EVENT_MAX;
        w->count--;
    }
    return (int)n;
}
