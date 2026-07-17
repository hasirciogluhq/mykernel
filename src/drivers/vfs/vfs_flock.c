#include <drivers/vfs_fs.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/types.h>

#define FLOCK_MAX 64

typedef struct flock_slot {
    inode_t *inode;
    flock_t  fl;
    int      used;
} flock_slot_t;

static flock_slot_t g_locks[FLOCK_MAX];

void vfs_flock_init(void)
{
    memset(g_locks, 0, sizeof(g_locks));
}

static int overlaps(const flock_t *a, const flock_t *b)
{
    off_t a_end = a->l_len ? a->l_start + a->l_len : 0x7fffffff;
    off_t b_end = b->l_len ? b->l_start + b->l_len : 0x7fffffff;
    return !(a_end <= b->l_start || b_end <= a->l_start);
}

static int conflicts(inode_t *inode, const flock_t *fl)
{
    size_t i;
    for (i = 0; i < FLOCK_MAX; i++) {
        if (!g_locks[i].used || g_locks[i].inode != inode)
            continue;
        if (g_locks[i].fl.l_pid == fl->l_pid)
            continue;
        if (!overlaps(&g_locks[i].fl, fl))
            continue;
        if (g_locks[i].fl.l_type == F_WRLCK || fl->l_type == F_WRLCK)
            return 1;
    }
    return 0;
}

int vfs_flock(file_t *file, int cmd, flock_t *fl)
{
    size_t i;
    inode_t *inode;

    if (!file || !file->f_inode || !fl)
        return -EINVAL;
    inode = file->f_inode;

    if (file->f_op && file->f_op->lock)
        return file->f_op->lock(file, cmd, fl);

    if (cmd == F_GETLK) {
        for (i = 0; i < FLOCK_MAX; i++) {
            if (g_locks[i].used && g_locks[i].inode == inode &&
                overlaps(&g_locks[i].fl, fl) &&
                (g_locks[i].fl.l_type == F_WRLCK || fl->l_type == F_WRLCK)) {
                *fl = g_locks[i].fl;
                return 0;
            }
        }
        fl->l_type = F_UNLCK;
        return 0;
    }

    if (fl->l_type == F_UNLCK) {
        for (i = 0; i < FLOCK_MAX; i++) {
            if (g_locks[i].used && g_locks[i].inode == inode &&
                g_locks[i].fl.l_pid == fl->l_pid) {
                g_locks[i].used = 0;
            }
        }
        return 0;
    }

    if (conflicts(inode, fl))
        return -EAGAIN;

    for (i = 0; i < FLOCK_MAX; i++) {
        if (g_locks[i].used && g_locks[i].inode == inode &&
            g_locks[i].fl.l_pid == fl->l_pid) {
            g_locks[i].fl = *fl;
            return 0;
        }
    }
    for (i = 0; i < FLOCK_MAX; i++) {
        if (!g_locks[i].used) {
            g_locks[i].used = 1;
            g_locks[i].inode = inode;
            g_locks[i].fl = *fl;
            return 0;
        }
    }
    return -ENOSPC;
}
