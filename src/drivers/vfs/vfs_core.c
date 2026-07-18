#include <kernel/vfs_api.h>
#include <kernel/vfs.h>
#include <kernel/block_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <kernel/module.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/sync.h>
#include <arch/x86/irq.h>
#include <drivers/vfs_fs.h>
#include <drivers/driver.h>
#include <drivers/vga.h>
#include "vfs_internal.h"

#define VFS_MAX_MOUNTS   16

/* Allocated once in vfs_drv_init — avoids huge .bss prealloc. */
static file_t *g_files;
static vfsmount_t g_mounts[VFS_MAX_MOUNTS];
static file_system_type_t *g_fs_types;
static vfsmount_t *g_root_mnt;
static int g_next_ino = 1;

static inode_t *inode_alloc(super_block_t *sb, uint32_t mode)
{
    inode_t *ino;
    if (sb && sb->s_op && sb->s_op->alloc_inode)
        ino = sb->s_op->alloc_inode(sb);
    else
        ino = (inode_t *)kmalloc(sizeof(*ino));
    if (!ino)
        return NULL;
    memset(ino, 0, sizeof(*ino));
    ino->i_ino = (uint32_t)g_next_ino++;
    ino->i_mode = mode;
    ino->i_nlink = 1;
    ino->i_sb = sb;
    ino->i_ref = 1;
    return ino;
}

static dentry_t *dentry_alloc(const char *name, dentry_t *parent, inode_t *inode)
{
    dentry_t *d = (dentry_t *)kmalloc(sizeof(*d));
    if (!d)
        return NULL;
    memset(d, 0, sizeof(*d));
    if (name)
        strncpy(d->d_name, name, sizeof(d->d_name) - 1);
    d->d_parent = parent;
    d->d_inode = inode;
    d->d_sb = inode ? inode->i_sb : (parent ? parent->d_sb : NULL);
    d->d_ref = 1;
    return d;
}

static int alloc_fd(void)
{
    int i;
    if (!g_files)
        return -ENOMEM;
    for (i = 0; i < VFS_MAX_OPEN; i++) {
        if (!g_files[i].used)
            return i;
    }
    return -EMFILE;
}

static file_system_type_t *find_fs(const char *name)
{
    file_system_type_t *t;
    if (!name)
        return NULL;
    for (t = g_fs_types; t; t = t->next) {
        if (strcmp(t->name, name) == 0)
            return t;
    }
    return NULL;
}

static int register_filesystem(const void *fs_type_void)
{
    file_system_type_t *fs = (file_system_type_t *)fs_type_void;
    if (!fs || !fs->name || !fs->mount)
        return -EINVAL;
    if (find_fs(fs->name))
        return -EEXIST;
    fs->next = g_fs_types;
    g_fs_types = fs;
    vga_print("vfs: fs ");
    vga_print(fs->name);
    vga_print("\n");
    return 0;
}

static int unregister_filesystem(const char *name)
{
    file_system_type_t **pp = &g_fs_types;
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            *pp = (*pp)->next;
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -ENOENT;
}

/* Resolve absolute path; returns dentry (referenced) or NULL + sets *err */
static dentry_t *path_walk(const char *path, int *err)
{
    dentry_t *cur;
    char tmp[VFS_PATH_MAX];
    char *save;
    char *tok;
    size_t n;

    if (!path || !g_root_mnt || !g_root_mnt->mnt_root) {
        if (err)
            *err = -ENOENT;
        return NULL;
    }

    if (path[0] != '/') {
        if (err)
            *err = -EINVAL;
        return NULL;
    }

    n = strlen(path);
    if (n >= sizeof(tmp)) {
        if (err)
            *err = -ENAMETOOLONG;
        return NULL;
    }
    memcpy(tmp, path, n + 1);

    cur = g_root_mnt->mnt_root;
    cur->d_ref++;

    /* skip leading slashes */
    save = tmp;
    while (*save == '/')
        save++;
    if (!*save)
        return cur;

    for (tok = save; *tok; ) {
        char *slash;
        dentry_t *next;
        size_t i;

        while (*tok == '/')
            tok++;
        if (!*tok)
            break;
        slash = tok;
        while (*slash && *slash != '/')
            slash++;
        if (*slash) {
            *slash = 0;
            slash++;
        } else {
            slash = tok + strlen(tok);
        }

        /* mount crossing: if cur is a mountpoint covered by another mount */
        for (i = 0; i < VFS_MAX_MOUNTS; i++) {
            if (g_mounts[i].used && g_mounts[i].mnt_mountpoint == cur &&
                g_mounts[i].mnt_root) {
                cur->d_ref--;
                cur = g_mounts[i].mnt_root;
                cur->d_ref++;
                break;
            }
        }

        if (!cur->d_inode || !cur->d_inode->i_op || !cur->d_inode->i_op->lookup) {
            cur->d_ref--;
            if (err)
                *err = -ENOTSUP;
            return NULL;
        }

        next = vfs_dcache_lookup(cur, tok);
        if (!next) {
            dentry_t probe;
            memset(&probe, 0, sizeof(probe));
            strncpy(probe.d_name, tok, sizeof(probe.d_name) - 1);
            probe.d_parent = cur;
            next = cur->d_inode->i_op->lookup(cur->d_inode, &probe);
            if (next)
                vfs_dcache_add(next);
        }
        cur->d_ref--;
        if (!next || !next->d_inode) {
            if (err)
                *err = -ENOENT;
            return NULL;
        }
        cur = next;
        tok = slash;
    }

    /* Enter filesystems mounted on the final dentry (/root -> fat, etc.). */
    for (;;) {
        int crossed = 0;
        size_t i;
        for (i = 0; i < VFS_MAX_MOUNTS; i++) {
            if (g_mounts[i].used && g_mounts[i].mnt_mountpoint == cur &&
                g_mounts[i].mnt_root) {
                cur->d_ref--;
                cur = g_mounts[i].mnt_root;
                cur->d_ref++;
                crossed = 1;
                break;
            }
        }
        if (!crossed)
            break;
    }
    return cur;
}

static int split_parent_leaf(const char *path, char *parent, size_t psz, char *leaf, size_t lsz)
{
    const char *slash;
    size_t plen;
    if (!path || path[0] != '/')
        return -EINVAL;
    slash = path + strlen(path);
    while (slash > path && slash[-1] != '/')
        slash--;
    plen = (size_t)(slash - path);
    if (plen == 0 || plen >= psz)
        return -ENAMETOOLONG;
    memcpy(parent, path, plen);
    parent[plen] = 0;
    if (plen > 1 && parent[plen - 1] == '/')
        parent[plen - 1] = 0;
    if (parent[0] == 0)
        strcpy(parent, "/");
    strncpy(leaf, slash, lsz - 1);
    leaf[lsz - 1] = 0;
    if (!leaf[0])
        return -EINVAL;
    return 0;
}

static int vfs_do_mount(const char *source, const char *target,
                        const char *fstype, unsigned long flags, const void *data)
{
    file_system_type_t *fs;
    super_block_t *sb = NULL;
    vfsmount_t *mnt = NULL;
    dentry_t *mp = NULL;
    int err = 0;
    int i;
    int rc;

    (void)flags;
    if (!fstype)
        return -EINVAL;
    fs = find_fs(fstype);
    if (!fs)
        return -ENODEV;

    rc = fs->mount(fs, (int)flags, source, (void *)data, &sb);
    if (rc < 0)
        return rc;
    if (!sb || !sb->s_root)
        return -EIO;

    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].used) {
            mnt = &g_mounts[i];
            break;
        }
    }
    if (!mnt)
        return -ENOSPC;

    memset(mnt, 0, sizeof(*mnt));
    if (source)
        strncpy(mnt->mnt_devname, source, sizeof(mnt->mnt_devname) - 1);
    if (target)
        strncpy(mnt->mnt_path, target, sizeof(mnt->mnt_path) - 1);
    mnt->mnt_sb = sb;
    mnt->mnt_root = sb->s_root;
    mnt->mnt_flags = flags;
    mnt->used = 1;

    if (!g_root_mnt || !target || strcmp(target, "/") == 0) {
        mnt->mnt_parent = NULL;
        mnt->mnt_mountpoint = NULL;
        g_root_mnt = mnt;
        vga_print("vfs: mounted root ");
        vga_print(fstype);
        vga_print("\n");
        return 0;
    }

    mp = path_walk(target, &err);
    if (!mp)
        return err ? err : -ENOENT;
    mnt->mnt_mountpoint = mp;
    mnt->mnt_parent = g_root_mnt;
    vga_print("vfs: mount ");
    vga_print(fstype);
    vga_print(" -> ");
    vga_print(target);
    vga_print("\n");
    return 0;
}

static int vfs_do_umount(const char *target, int flags)
{
    int i;
    (void)flags;
    if (!target)
        return -EINVAL;
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].used)
            continue;
        if (strcmp(g_mounts[i].mnt_path, target) != 0)
            continue;
        if (&g_mounts[i] == g_root_mnt && strcmp(target, "/") == 0)
            return -EBUSY;
        if (g_mounts[i].mnt_sb && g_mounts[i].mnt_sb->s_type &&
            g_mounts[i].mnt_sb->s_type->kill_sb)
            g_mounts[i].mnt_sb->s_type->kill_sb(g_mounts[i].mnt_sb);
        memset(&g_mounts[i], 0, sizeof(g_mounts[i]));
        return 0;
    }
    return -ENOENT;
}

static int vfs_create_leaf(const char *path, int mode, dentry_t **out)
{
    char parent[VFS_PATH_MAX];
    char leaf[64];
    const char *slash;
    dentry_t *dir;
    dentry_t probe;
    int err = 0;
    size_t plen;

    if (!path || path[0] != '/')
        return -EINVAL;
    slash = path + strlen(path);
    while (slash > path && slash[-1] != '/')
        slash--;
    plen = (size_t)(slash - path);
    if (plen == 0 || strlen(slash) == 0 || strlen(slash) >= sizeof(leaf))
        return -EINVAL;
    if (plen >= sizeof(parent))
        return -ENAMETOOLONG;
    memcpy(parent, path, plen);
    parent[plen] = 0;
    if (plen > 1 && parent[plen - 1] == '/')
        parent[plen - 1] = 0;
    if (parent[0] == 0)
        strcpy(parent, "/");
    strncpy(leaf, slash, sizeof(leaf) - 1);
    leaf[sizeof(leaf) - 1] = 0;

    dir = path_walk(parent, &err);
    if (!dir)
        return err ? err : -ENOENT;
    if (!dir->d_inode || !dir->d_inode->i_op || !dir->d_inode->i_op->create) {
        dir->d_ref--;
        return -ENOTSUP;
    }
    memset(&probe, 0, sizeof(probe));
    strncpy(probe.d_name, leaf, sizeof(probe.d_name) - 1);
    probe.d_parent = dir;
    err = dir->d_inode->i_op->create(dir->d_inode, &probe, mode | S_IFREG);
    if (err < 0) {
        dir->d_ref--;
        return err;
    }
    dir->d_ref--;
    /* Re-walk full path so we get a referenced dentry */
    *out = path_walk(path, &err);
    return (*out) ? 0 : (err ? err : -ENOENT);
}

static int vfs_do_open(const char *path, int flags)
{
    dentry_t *d;
    file_t *f;
    int err = 0;
    int fd;
    int rc;

    d = path_walk(path, &err);
    if (!d) {
        if ((flags & O_CREAT) && err == -ENOENT) {
            err = vfs_create_leaf(path, 0644, &d);
            if (err < 0)
                return err;
        } else {
            return err ? err : -ENOENT;
        }
    }
    if (!d->d_inode) {
        d->d_ref--;
        return -ENOENT;
    }
    if ((flags & O_DIRECTORY) && !S_ISDIR(d->d_inode->i_mode)) {
        d->d_ref--;
        return -ENOTDIR;
    }

    fd = alloc_fd();
    if (fd < 0) {
        d->d_ref--;
        return fd;
    }
    f = &g_files[fd];
    memset(f, 0, sizeof(*f));
    f->f_dentry = d;
    f->f_inode = d->d_inode;
    f->f_op = d->d_inode->i_fop;
    f->f_flags = flags;
    f->f_pos = 0;
    f->used = 1;

    if (f->f_op && f->f_op->open) {
        rc = f->f_op->open(d->d_inode, f);
        if (rc < 0) {
            f->used = 0;
            d->d_ref--;
            return rc;
        }
    }
    return fd;
}

static ssize_t vfs_do_read(int fd, void *buf, size_t count)
{
    file_t *f;
    if (!g_files || fd < 0 || fd >= VFS_MAX_OPEN || !g_files[fd].used)
        return -EBADF;
    f = &g_files[fd];
    if (f->f_flags == O_WRONLY)
        return -EBADF;
    if (!f->f_op || !f->f_op->read)
        return -ENOTSUP;
    return f->f_op->read(f, buf, count, &f->f_pos);
}

static ssize_t vfs_do_write(int fd, const void *buf, size_t count)
{
    file_t *f;
    if (!g_files || fd < 0 || fd >= VFS_MAX_OPEN || !g_files[fd].used)
        return -EBADF;
    f = &g_files[fd];
    if (f->f_flags == O_RDONLY)
        return -EBADF;
    if (!f->f_op || !f->f_op->write)
        return -ENOTSUP;
    return f->f_op->write(f, buf, count, &f->f_pos);
}

static int vfs_do_close(int fd)
{
    file_t *f;
    if (!g_files || fd < 0 || fd >= VFS_MAX_OPEN || !g_files[fd].used)
        return -EBADF;
    f = &g_files[fd];
    if (f->f_op && f->f_op->release)
        f->f_op->release(f->f_inode, f);
    if (f->f_dentry)
        f->f_dentry->d_ref--;
    memset(f, 0, sizeof(*f));
    return 0;
}

static off_t vfs_do_lseek(int fd, off_t off, int whence)
{
    file_t *f;
    off_t base;
    if (!g_files || fd < 0 || fd >= VFS_MAX_OPEN || !g_files[fd].used)
        return -EBADF;
    f = &g_files[fd];
    if (f->f_op && f->f_op->llseek)
        return f->f_op->llseek(f, off, whence);
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = f->f_pos;
    else if (whence == SEEK_END)
        base = (off_t)(f->f_inode ? f->f_inode->i_size : 0);
    else
        return -EINVAL;
    if (base + off < 0)
        return -EINVAL;
    f->f_pos = base + off;
    return f->f_pos;
}

static int vfs_do_mkdir(const char *path, int mode)
{
    char parent[VFS_PATH_MAX];
    char leaf[64];
    const char *slash;
    dentry_t *dir;
    dentry_t probe;
    int err = 0;
    size_t plen;

    if (!path || path[0] != '/')
        return -EINVAL;
    slash = path + strlen(path);
    while (slash > path && slash[-1] != '/')
        slash--;
    plen = (size_t)(slash - path);
    if (plen == 0)
        return -EINVAL;
    if (plen >= sizeof(parent))
        return -ENAMETOOLONG;
    memcpy(parent, path, plen);
    parent[plen] = 0;
    if (plen > 1 && parent[plen - 1] == '/')
        parent[plen - 1] = 0;
    if (parent[0] == 0)
        strcpy(parent, "/");
    strncpy(leaf, slash, sizeof(leaf) - 1);

    dir = path_walk(parent, &err);
    if (!dir)
        return err ? err : -ENOENT;
    if (!dir->d_inode || !dir->d_inode->i_op || !dir->d_inode->i_op->mkdir) {
        dir->d_ref--;
        return -ENOTSUP;
    }
    memset(&probe, 0, sizeof(probe));
    strncpy(probe.d_name, leaf, sizeof(probe.d_name) - 1);
    probe.d_parent = dir;
    err = dir->d_inode->i_op->mkdir(dir->d_inode, &probe, mode | S_IFDIR);
    dir->d_ref--;
    return err;
}

static int vfs_do_rmdir(const char *path)
{
    char parent[VFS_PATH_MAX], leaf[64];
    dentry_t *dir, *child, probe;
    int err = 0, rc;
    if (split_parent_leaf(path, parent, sizeof(parent), leaf, sizeof(leaf)) < 0)
        return -EINVAL;
    dir = path_walk(parent, &err);
    if (!dir)
        return err ? err : -ENOENT;
    if (!dir->d_inode || !dir->d_inode->i_op || !dir->d_inode->i_op->rmdir) {
        dir->d_ref--;
        return -ENOTSUP;
    }
    memset(&probe, 0, sizeof(probe));
    strncpy(probe.d_name, leaf, sizeof(probe.d_name) - 1);
    probe.d_parent = dir;
    child = dir->d_inode->i_op->lookup(dir->d_inode, &probe);
    if (!child) {
        dir->d_ref--;
        return -ENOENT;
    }
    rc = dir->d_inode->i_op->rmdir(dir->d_inode, child);
    vfs_dcache_remove(child);
    vfs_fsnotify_event(path, FS_DELETE, leaf);
    dir->d_ref--;
    return rc;
}

static int vfs_do_unlink(const char *path)
{
    char parent[VFS_PATH_MAX], leaf[64];
    dentry_t *dir, *child, probe;
    int err = 0, rc;
    if (split_parent_leaf(path, parent, sizeof(parent), leaf, sizeof(leaf)) < 0)
        return -EINVAL;
    dir = path_walk(parent, &err);
    if (!dir)
        return err ? err : -ENOENT;
    if (!dir->d_inode || !dir->d_inode->i_op || !dir->d_inode->i_op->unlink) {
        dir->d_ref--;
        return -ENOTSUP;
    }
    memset(&probe, 0, sizeof(probe));
    strncpy(probe.d_name, leaf, sizeof(probe.d_name) - 1);
    probe.d_parent = dir;
    child = dir->d_inode->i_op->lookup(dir->d_inode, &probe);
    if (!child) {
        dir->d_ref--;
        return -ENOENT;
    }
    rc = dir->d_inode->i_op->unlink(dir->d_inode, child);
    vfs_dcache_remove(child);
    vfs_fsnotify_event(path, FS_DELETE, leaf);
    dir->d_ref--;
    return rc;
}

static int vfs_do_rename(const char *oldpath, const char *newpath)
{
    char op[VFS_PATH_MAX], ol[64], np[VFS_PATH_MAX], nl[64];
    dentry_t *odir, *ndir, oprobe, nprobe, *old_d, *new_d;
    int err = 0, rc;
    if (split_parent_leaf(oldpath, op, sizeof(op), ol, sizeof(ol)) < 0)
        return -EINVAL;
    if (split_parent_leaf(newpath, np, sizeof(np), nl, sizeof(nl)) < 0)
        return -EINVAL;
    odir = path_walk(op, &err);
    if (!odir)
        return err ? err : -ENOENT;
    ndir = path_walk(np, &err);
    if (!ndir) {
        odir->d_ref--;
        return err ? err : -ENOENT;
    }
    if (!odir->d_inode || !odir->d_inode->i_op || !odir->d_inode->i_op->rename) {
        odir->d_ref--;
        ndir->d_ref--;
        return -ENOTSUP;
    }
    memset(&oprobe, 0, sizeof(oprobe));
    strncpy(oprobe.d_name, ol, sizeof(oprobe.d_name) - 1);
    oprobe.d_parent = odir;
    old_d = odir->d_inode->i_op->lookup(odir->d_inode, &oprobe);
    if (!old_d) {
        odir->d_ref--;
        ndir->d_ref--;
        return -ENOENT;
    }
    memset(&nprobe, 0, sizeof(nprobe));
    strncpy(nprobe.d_name, nl, sizeof(nprobe.d_name) - 1);
    nprobe.d_parent = ndir;
    new_d = &nprobe;
    rc = odir->d_inode->i_op->rename(odir->d_inode, old_d, ndir->d_inode, new_d);
    vfs_dcache_remove(old_d);
    vfs_fsnotify_event(oldpath, FS_MOVE, ol);
    odir->d_ref--;
    ndir->d_ref--;
    return rc;
}

static int vfs_do_stat(const char *path, void *statbuf)
{
    dentry_t *d;
    vfs_stat_t *st = (vfs_stat_t *)statbuf;
    int err = 0;
    if (!st)
        return -EINVAL;
    d = path_walk(path, &err);
    if (!d)
        return err ? err : -ENOENT;
    if (d->d_inode && d->d_inode->i_op && d->d_inode->i_op->getattr) {
        err = d->d_inode->i_op->getattr(d, st);
        d->d_ref--;
        return err;
    }
    if (!d->d_inode) {
        d->d_ref--;
        return -ENOENT;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = d->d_inode->i_mode;
    st->st_uid = d->d_inode->i_uid;
    st->st_gid = d->d_inode->i_gid;
    st->st_nlink = d->d_inode->i_nlink;
    st->st_size = d->d_inode->i_size;
    st->st_ino = d->d_inode->i_ino;
    d->d_ref--;
    return 0;
}

static int vfs_do_fstat(int fd, void *statbuf)
{
    file_t *f;
    vfs_stat_t *st = (vfs_stat_t *)statbuf;
    if (!g_files || fd < 0 || fd >= VFS_MAX_OPEN || !g_files[fd].used)
        return -EBADF;
    if (!st)
        return -EINVAL;
    f = &g_files[fd];
    memset(st, 0, sizeof(*st));
    if (!f->f_inode)
        return -ENOENT;
    st->st_mode = f->f_inode->i_mode;
    st->st_size = f->f_inode->i_size;
    st->st_ino = f->f_inode->i_ino;
    return 0;
}

static int vfs_do_ioctl(int fd, unsigned long cmd, void *arg)
{
    file_t *f;
    if (!g_files || fd < 0 || fd >= VFS_MAX_OPEN || !g_files[fd].used)
        return -EBADF;
    f = &g_files[fd];
    if (!f->f_op || !f->f_op->ioctl)
        return -ENOTSUP;
    return f->f_op->ioctl(f, cmd, arg);
}

static int vfs_do_fsync(int fd)
{
    file_t *f;
    if (!g_files || fd < 0 || fd >= VFS_MAX_OPEN || !g_files[fd].used)
        return -EBADF;
    f = &g_files[fd];
    if (!f->f_op || !f->f_op->fsync)
        return -ENOTSUP;
    return f->f_op->fsync(f);
}

static int vfs_do_readdir(int fd, void *dirent, size_t max)
{
    file_t *f;
    if (!g_files || fd < 0 || fd >= VFS_MAX_OPEN || !g_files[fd].used)
        return -EBADF;
    f = &g_files[fd];
    if (!f->f_op || !f->f_op->readdir)
        return -ENOTSUP;
    return f->f_op->readdir(f, dirent, max);
}

/* ---- async I/O ---- */

typedef struct aio_ctx {
    vfs_aio_t *aio;
} aio_ctx_t;

static void aio_done(void *ctx, ssize_t n)
{
    aio_ctx_t *c = (aio_ctx_t *)ctx;
    process_t *p;
    if (!c || !c->aio)
        return;
    c->aio->result = n;
    c->aio->done = 1;
    if (c->aio->waiter_tid > 0) {
        p = process_by_tid((pid_t)c->aio->waiter_tid);
        c->aio->waiter_tid = -1;
        if (p)
            process_wake(p);
    }
    if (c->aio->complete)
        c->aio->complete(c->aio);
}

static int vfs_do_aio_submit(vfs_aio_t *aio)
{
    file_t *f;
    aio_ctx_t *ctx;
    int rc;

    if (!aio)
        return -EINVAL;
    if (!g_files || aio->fd < 0 || aio->fd >= VFS_MAX_OPEN || !g_files[aio->fd].used)
        return -EBADF;
    f = &g_files[aio->fd];
    aio->done = 0;
    aio->result = 0;
    aio->waiter_tid = -1;

    ctx = (aio_ctx_t *)kmalloc(sizeof(*ctx));
    if (!ctx)
        return -ENOMEM;
    ctx->aio = aio;

    if (aio->write) {
        if (f->f_op && f->f_op->aio_write) {
            rc = f->f_op->aio_write(f, aio->buf, aio->count, aio->pos, aio_done, ctx);
            if (rc < 0) {
                aio->result = rc;
                aio->done = 1;
            }
            return rc < 0 ? rc : 0;
        }
        /* Fallback: sync write then complete */
        if (f->f_op && f->f_op->write) {
            off_t pos = aio->pos;
            ssize_t n = f->f_op->write(f, aio->buf, aio->count, &pos);
            aio_done(ctx, n);
            return 0;
        }
        return -ENOTSUP;
    }

    if (f->f_op && f->f_op->aio_read) {
        rc = f->f_op->aio_read(f, aio->buf, aio->count, aio->pos, aio_done, ctx);
        if (rc < 0) {
            aio->result = rc;
            aio->done = 1;
        }
        return rc < 0 ? rc : 0;
    }
    if (f->f_op && f->f_op->read) {
        off_t pos = aio->pos;
        ssize_t n = f->f_op->read(f, aio->buf, aio->count, &pos);
        aio_done(ctx, n);
        return 0;
    }
    return -ENOTSUP;
}

static int vfs_do_aio_wait(vfs_aio_t *aio)
{
    const block_api_t *blk;
    process_t *cur;
    if (!aio)
        return -EINVAL;
    blk = block_api_get();
    cur = process_current();
    while (!aio->done) {
        if (blk && blk->poll)
            blk->poll();
        if (aio->done)
            break;
        if (!cur)
            return -EIO;
        /* Completion wake preferred; short tick backup for poll-driven I/O. */
        aio->waiter_tid = (int)(cur->tid > 0 ? cur->tid : cur->pid);
        process_block(irq_timer_ticks() + 2);
        schedule();
    }
    return (int)aio->result;
}

static int vfs_do_aio_poll(void)
{
    const block_api_t *blk = block_api_get();
    if (blk && blk->poll)
        return blk->poll();
    return 0;
}

static void *api_alloc_inode(void *sb, uint32_t mode)
{
    return inode_alloc((super_block_t *)sb, mode);
}

static void *api_alloc_dentry(const char *name, void *parent, void *inode)
{
    return dentry_alloc(name, (dentry_t *)parent, (inode_t *)inode);
}

static int vfs_do_getxattr(const char *path, const char *name, void *value, size_t size)
{
    dentry_t *d;
    int err = 0, rc;
    d = path_walk(path, &err);
    if (!d)
        return err ? err : -ENOENT;
    rc = vfs_xattr_get(d->d_inode, name, value, size);
    d->d_ref--;
    return rc;
}

static int vfs_do_setxattr(const char *path, const char *name, const void *value, size_t size, int flags)
{
    dentry_t *d;
    int err = 0, rc;
    d = path_walk(path, &err);
    if (!d)
        return err ? err : -ENOENT;
    rc = vfs_xattr_set(d->d_inode, name, value, size, flags);
    vfs_fsnotify_event(path, FS_MODIFY, NULL);
    d->d_ref--;
    return rc;
}

static int vfs_do_listxattr(const char *path, char *list, size_t size)
{
    dentry_t *d;
    int err = 0, rc;
    d = path_walk(path, &err);
    if (!d)
        return err ? err : -ENOENT;
    rc = vfs_xattr_list(d->d_inode, list, size);
    d->d_ref--;
    return rc;
}

static int vfs_do_removexattr(const char *path, const char *name)
{
    dentry_t *d;
    int err = 0, rc;
    d = path_walk(path, &err);
    if (!d)
        return err ? err : -ENOENT;
    rc = vfs_xattr_remove(d->d_inode, name);
    d->d_ref--;
    return rc;
}

static int vfs_do_flock(int fd, int cmd, void *fl)
{
    if (!g_files || fd < 0 || fd >= VFS_MAX_OPEN || !g_files[fd].used)
        return -EBADF;
    return vfs_flock(&g_files[fd], cmd, (flock_t *)fl);
}

static ssize_t api_cached_read(void *inode, void *buf, size_t count, off_t pos)
{
    return vfs_cached_read((inode_t *)inode, buf, count, pos);
}

static ssize_t api_cached_write(void *inode, const void *buf, size_t count, off_t pos)
{
    return vfs_cached_write((inode_t *)inode, buf, count, pos);
}

static int api_cache_invalidate(void *inode)
{
    return vfs_cache_invalidate((inode_t *)inode);
}

static int vfs_do_module_load_path(const char *path)
{
    int fd;
    uint8_t *buf;
    ssize_t n, total = 0;
    off_t size;
    int rc;

    if (!path)
        return -EINVAL;
    fd = vfs_do_open(path, O_RDONLY);
    if (fd < 0)
        return fd;
    size = vfs_do_lseek(fd, 0, SEEK_END);
    if (size <= 0) {
        vfs_do_close(fd);
        return -EINVAL;
    }
    vfs_do_lseek(fd, 0, SEEK_SET);
    buf = (uint8_t *)kmalloc((size_t)size);
    if (!buf) {
        vfs_do_close(fd);
        return -ENOMEM;
    }
    while (total < size) {
        n = vfs_do_read(fd, buf + total, (size_t)(size - total));
        if (n <= 0)
            break;
        total += n;
    }
    vfs_do_close(fd);
    if (total <= 0) {
        return -EIO;
    }
    {
        const char *base = path;
        const char *p = path;
        while (*p) {
            if (*p == '/')
                base = p + 1;
            p++;
        }
        rc = modules_load_blob(base, buf, (size_t)total);
    }
    return rc;
}

static const vfs_api_t g_vfs_api = {
    .open = vfs_do_open,
    .read = vfs_do_read,
    .write = vfs_do_write,
    .close = vfs_do_close,
    .lseek = vfs_do_lseek,
    .mkdir = vfs_do_mkdir,
    .rmdir = vfs_do_rmdir,
    .unlink = vfs_do_unlink,
    .rename = vfs_do_rename,
    .stat = vfs_do_stat,
    .fstat = vfs_do_fstat,
    .mount = vfs_do_mount,
    .umount = vfs_do_umount,
    .ioctl = vfs_do_ioctl,
    .fsync = vfs_do_fsync,
    .readdir = vfs_do_readdir,
    .aio_submit = vfs_do_aio_submit,
    .aio_wait = vfs_do_aio_wait,
    .aio_poll = vfs_do_aio_poll,
    .register_filesystem = register_filesystem,
    .unregister_filesystem = unregister_filesystem,
    .alloc_inode = api_alloc_inode,
    .alloc_dentry = api_alloc_dentry,
    .getxattr = vfs_do_getxattr,
    .setxattr = vfs_do_setxattr,
    .listxattr = vfs_do_listxattr,
    .removexattr = vfs_do_removexattr,
    .flock = vfs_do_flock,
    .fsnotify_add_watch = vfs_fsnotify_add_watch,
    .fsnotify_rm_watch = vfs_fsnotify_rm_watch,
    .fsnotify_read = (int (*)(int, void *, size_t))vfs_fsnotify_read,
    .cached_read = api_cached_read,
    .cached_write = api_cached_write,
    .cache_invalidate = api_cache_invalidate,
    .module_load_path = vfs_do_module_load_path,
};

static int vfs_drv_init(driver_t *drv, void *ctx)
{
    (void)drv;
    (void)ctx;
    g_files = (file_t *)kmalloc(sizeof(file_t) * VFS_MAX_OPEN);
    if (!g_files) {
        vga_print("vfs: file table alloc failed\n");
        return -ENOMEM;
    }
    memset(g_files, 0, sizeof(file_t) * VFS_MAX_OPEN);
    memset(g_mounts, 0, sizeof(g_mounts));
    g_fs_types = NULL;
    g_root_mnt = NULL;
    vfs_dcache_init();
    vfs_pcache_init();
    vfs_flock_init();
    vfs_fsnotify_init();
    vfs_api_register(&g_vfs_api);
    vga_print("vfs: core ready\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "vfs", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "1.0", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_MISC;
    d.priority = 20;
    d.init = vfs_drv_init;
    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("vfs", NULL) < 0)
        return -1;
    return 0;
}
