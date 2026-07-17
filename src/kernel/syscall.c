#include <kernel/syscall.h>
#include <kernel/process.h>
#include <kernel/env.h>
#include <kernel/argv.h>
#include <kernel/service.h>
#include <kernel/scheduler.h>
#include <kernel/time.h>
#include <kernel/vfs.h>
#include <kernel/vfs_api.h>
#include <kernel/errno.h>
#include <kernel/mm.h>
#include <kernel/module.h>
#include <kernel/mke.h>
#include <kernel/mkdx_api.h>
#include <kernel/netif.h>
#include <kernel/socket.h>
#include <kernel/uaccess.h>
#include <kernel/string.h>
#include <drivers/serial.h>
#include <drivers/driver.h>
#include <drivers/vfs_fs.h>
#include <user/gx.h>

typedef struct {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
} regs_t;

/* Log only setup-ish GX/WM calls (not per-frame YIELD/DAMAGE/INPUT/GET probes). */
static int sys_log_interesting(long n)
{
    return n == SYS_GX_INFO || n == SYS_GX_PRESENT || n == SYS_WM_CREATE ||
           n == SYS_WM_SET || n == SYS_WM_CLOSE ||
           n == SYS_WM_MAP || n == SYS_GX_SET_WALLPAPER || n == SYS_WM_DESTROY ||
           n == SYS_WM_SHOW || n == SYS_WM_FOCUS || n == SYS_WM_FIND ||
           n == SYS_WM_FIND_CLASS ||
           n == SYS_SPAWN || n == SYS_KILL || n == SYS_SERVICE_START ||
           n == SYS_SERVICE_STOP || n == SYS_CONSOLE_SHOW;
}

static const char *fs_proc_name(void)
{
    process_t *p = process_current();
    return (p && p->name[0]) ? p->name : "?";
}

static void fs_log_ret(const char *op, long ret)
{
    if (ret >= 0)
        return;
    /* ENOENT is a normal probe result (optional configs, path_exists, etc.).
     * Logging it every theme poll floods the serial console. */
    if (ret == -ENOENT)
        return;

    klog("[fs] ");
    klog(fs_proc_name());
    klog(" ");
    klog(op);
    klog(" -> err=");
    serial_print_uint((uint32_t)(-ret));
    klog("\n");
}

void syscall_isr_handler(regs_t *r)
{
    static int s_log_left = 64;
    long n = (long)r->eax;
    int log = (s_log_left > 0 && sys_log_interesting(n));

    if (log) {
        process_t *p = process_current();
        klog("[sys] ");
        klog(p && p->name[0] ? p->name : "?");
        klog(" nr=");
        serial_print_uint((uint32_t)n);
        klog(" a1=");
        serial_print_hex((uint32_t)r->ebx);
        klog("\n");
    }

    r->eax = (uint32_t)syscall_dispatch(
        n,
        (long)r->ebx,
        (long)r->ecx,
        (long)r->edx,
        (long)r->esi,
        (long)r->edi);

    if (log) {
        klog("[sys] -> ret=");
        serial_print_hex(r->eax);
        klog("\n");
        s_log_left--;
    }
}

/* Collapse ".", "..", and duplicate slashes into an absolute path. */
static int path_normalize(char *path)
{
    char out[VFS_PATH_MAX];
    char *parts[64];
    int nparts = 0;
    char *p;
    size_t i, len;

    if (!path || path[0] != '/')
        return -1;

    len = strlen(path);
    if (len >= VFS_PATH_MAX)
        return -1;

    {
        char tmp[VFS_PATH_MAX];
        strcpy(tmp, path);
        p = tmp;
        while (*p) {
            char *start;
            while (*p == '/')
                p++;
            if (!*p)
                break;
            start = p;
            while (*p && *p != '/')
                p++;
            if (*p)
                *p++ = 0;
            if (strcmp(start, ".") == 0)
                continue;
            if (strcmp(start, "..") == 0) {
                if (nparts > 0)
                    nparts--;
                continue;
            }
            if (nparts >= (int)(sizeof(parts) / sizeof(parts[0])))
                return -1;
            parts[nparts++] = start;
        }
        out[0] = '/';
        out[1] = 0;
        for (i = 0; i < (size_t)nparts; i++) {
            size_t ol = strlen(out);
            size_t pl = strlen(parts[i]);
            if (ol + (out[1] ? 1u : 0u) + pl + 1 >= sizeof(out))
                return -1;
            if (out[1] != 0) {
                out[ol] = '/';
                memcpy(out + ol + 1, parts[i], pl + 1);
            } else {
                memcpy(out + 1, parts[i], pl + 1);
            }
        }
        strcpy(path, out);
    }
    return 0;
}

static int resolve_path(process_t *p, const char *in, char *out, size_t outsz)
{
    size_t cl, il, n;

    if (!p || !in || !out || outsz < 2)
        return -1;
    if (in[0] == '/') {
        if (strlen(in) >= outsz)
            return -1;
        strcpy(out, in);
    } else {
        cl = strlen(p->cwd);
        il = strlen(in);
        if (strcmp(p->cwd, "/") == 0) {
            n = 1 + il + 1;
            if (n > outsz)
                return -1;
            out[0] = '/';
            memcpy(out + 1, in, il + 1);
        } else {
            n = cl + 1 + il + 1;
            if (n > outsz)
                return -1;
            memcpy(out, p->cwd, cl);
            out[cl] = '/';
            memcpy(out + cl + 1, in, il + 1);
        }
    }
    return path_normalize(out);
}

static long do_exit(long code)
{
    process_exit((int)code);
    return 0;
}

static long do_read(long fd, long buf, long count)
{
    process_t *p = process_current();
    char tmp[256];
    long total = 0;
    int enc;
    long ret;

    if (!p || count < 0) {
        fs_log_ret("read", -1);
        return -1;
    }
    enc = process_lookup_fd(p, (int)fd);
    if (enc < 0) {
        fs_log_ret("read", -1);
        return -1;
    }

    if (PROC_FD_IS_SOCK(enc)) {
        char kbuf[1472];
        ssize_t n;
        size_t want = (size_t)count;
        if (want > sizeof(kbuf))
            want = sizeof(kbuf);
        n = sock_recvfrom(PROC_FD_SOCK_ID(enc), kbuf, want, 0, NULL);
        if (n < 0) {
            fs_log_ret("read", n);
            return n;
        }
        if (copy_to_user((void *)buf, kbuf, (size_t)n) < 0) {
            fs_log_ret("read", -EFAULT);
            return -EFAULT;
        }
        fs_log_ret("read", n);
        return n;
    }

    while (total < count) {
        size_t chunk = (size_t)(count - total);
        ssize_t n;
        if (chunk > sizeof(tmp))
            chunk = sizeof(tmp);
        n = vfs_read(enc, tmp, chunk);
        if (n < 0) {
            ret = total > 0 ? total : -1;
            fs_log_ret("read", ret);
            return ret;
        }
        if (n == 0)
            break;
        if (copy_to_user((void *)(buf + total), tmp, (size_t)n) < 0) {
            fs_log_ret("read", -1);
            return -1;
        }
        total += n;
        if ((size_t)n < chunk)
            break;
    }
    fs_log_ret("read", total);
    return total;
}

static long do_write(long fd, long buf, long count)
{
    process_t *p = process_current();
    char tmp[256];
    long total = 0;
    int enc;
    long ret;

    if (!p || count < 0) {
        fs_log_ret("write", -1);
        return -1;
    }
    enc = process_lookup_fd(p, (int)fd);
    if (enc < 0) {
        fs_log_ret("write", -1);
        return -1;
    }

    if (PROC_FD_IS_SOCK(enc)) {
        char kbuf[1472];
        ssize_t n;
        size_t want = (size_t)count;
        if (want > sizeof(kbuf))
            want = sizeof(kbuf);
        if (copy_from_user(kbuf, (const void *)buf, want) < 0) {
            fs_log_ret("write", -EFAULT);
            return -EFAULT;
        }
        n = sock_sendto(PROC_FD_SOCK_ID(enc), kbuf, want, 0, NULL);
        fs_log_ret("write", n);
        return n;
    }

    while (total < count) {
        size_t chunk = (size_t)(count - total);
        ssize_t n;
        if (chunk > sizeof(tmp))
            chunk = sizeof(tmp);
        if (copy_from_user(tmp, (const void *)(buf + total), chunk) < 0) {
            fs_log_ret("write", -1);
            return -1;
        }
        n = vfs_write(enc, tmp, chunk);
        if (n < 0) {
            ret = total > 0 ? total : -1;
            fs_log_ret("write", ret);
            return ret;
        }
        if (n > 0 && ((int)fd == STDOUT_FILENO || (int)fd == STDERR_FILENO)) {
            const mkdx_api_t *api = mkdx_api_get();
            if (api && api->console_write)
                (void)api->console_write((int)p->pid, tmp, (size_t)n);
        }
        total += n;
        if ((size_t)n < chunk)
            break;
    }
    fs_log_ret("write", total);
    return total;
}

static long do_open(long path, long flags)
{
    process_t *p = process_current();
    char upath[VFS_PATH_MAX];
    char kpath[VFS_PATH_MAX];
    int len;
    int vfd;
    int ufd;

    if (!p) {
        fs_log_ret("open", -1);
        return -1;
    }
    len = user_strlen((const char *)path, sizeof(upath));
    if (len < 0) {
        fs_log_ret("open", -1);
        return -1;
    }
    if (copy_from_user(upath, (const void *)path, (size_t)len + 1) < 0) {
        fs_log_ret("open", -1);
        return -1;
    }
    if (resolve_path(p, upath, kpath, sizeof(kpath)) < 0) {
        fs_log_ret("open", -1);
        return -1;
    }

    vfd = vfs_open(kpath, (int)flags);
    if (vfd < 0) {
        fs_log_ret("open", vfd);
        return -1;
    }

    ufd = process_alloc_fd(p, vfd);
    if (ufd < 0) {
        vfs_close(vfd);
        fs_log_ret("open", -1);
        return -1;
    }
    fs_log_ret("open", ufd);
    return ufd;
}

static long do_close(long fd)
{
    process_t *p = process_current();
    int enc;
    if (!p)
        return -1;

    enc = process_lookup_fd(p, (int)fd);
    if (enc < 0)
        return -1;

    if (PROC_FD_IS_SOCK(enc)) {
        (void)sock_close(PROC_FD_SOCK_ID(enc));
    } else if ((int)fd > STDERR_FILENO) {
        vfs_close(enc);
    }

    process_free_fd(p, (int)fd);
    return 0;
}

static long do_getpid(void)
{
    process_t *p = process_current();
    return p ? (long)p->pid : -1;
}

static long do_getppid(void)
{
    return (long)process_getppid();
}

static long do_yield(void)
{
    const mkdx_api_t *api = mkdx_api_get();

    /* Cooperative scheduler: drain input every yield so PS/2 isn't starved. */
    drivers_poll();
    if (api && api->pump_input)
        api->pump_input();

    /* Keep shared proc snapshot fresh without forcing apps to SYS_PROC_LIST. */
    process_snapshot_publish();

    schedule();
    return 0;
}

static long do_waitpid(long pid, long status_ptr, long options)
{
    int status = 0;
    pid_t waited = process_waitpid((pid_t)pid, status_ptr ? &status : NULL, (int)options);
    if (waited > 0 && status_ptr) {
        if (copy_to_user((void *)status_ptr, &status, sizeof(status)) < 0)
            return -EFAULT;
    }
    return (long)waited;
}

static long do_spawn(long path_ptr, long flags, long argv_ptr, long argc)
{
    process_t *p = process_current();
    char user_path[VFS_PATH_MAX];
    char full_path[VFS_PATH_MAX];
    char argv_storage[PROC_ARGC_MAX][PROC_ARGV_MAX];
    const char *kargv[PROC_ARGC_MAX + 1];
    uint32_t spawn_flags;
    int len;
    int copied_argc = 0;

    if (!p)
        return -ESRCH;

    len = user_strlen((const char *)path_ptr, sizeof(user_path));
    if (len < 0)
        return -EFAULT;
    if (copy_from_user(user_path, (const void *)path_ptr, (size_t)len + 1) < 0)
        return -EFAULT;
    if (resolve_path(p, user_path, full_path, sizeof(full_path)) < 0)
        return -EINVAL;

    if (argv_ptr) {
        copied_argc = argv_copy_from_user(argv_storage, kargv, (int)argc, argv_ptr);
        if (copied_argc < 0)
            return copied_argc;
    }

    spawn_flags = (uint32_t)flags;
    if (spawn_flags == 0)
        spawn_flags = SPAWN_CONSOLE_HIDDEN;
    return (long)mke_spawn_path_flags(full_path, spawn_flags,
                                      copied_argc > 0 ? kargv : NULL, copied_argc);
}

static long do_console_show(long pid, long visible)
{
    const mkdx_api_t *api = mkdx_api_get();
    if (!api || !api->console_show)
        return -ENOSYS;
    return (long)api->console_show((int)pid, visible ? 1 : 0);
}

static long do_kill(long pid)
{
    return (long)process_kill((pid_t)pid);
}

static long do_getargc(void)
{
    process_t *p = process_current();
    return (long)argv_proc_count(p);
}

static long do_getargv(long index, long buf_ptr, long buflen)
{
    process_t *p = process_current();
    char kbuf[PROC_ARGV_MAX];
    int len;

    if (!p)
        return -ESRCH;
    if (buflen <= 0 || buflen > (long)PROC_ARGV_MAX)
        return -EINVAL;

    len = argv_proc_get(p, (int)index, kbuf, (size_t)buflen);
    if (len < 0)
        return len;
    if (copy_to_user((void *)buf_ptr, kbuf, (size_t)len + 1) < 0)
        return -EFAULT;
    return len;
}

static long do_getenv(long name_ptr, long buf_ptr, long buflen)
{
    process_t *p = process_current();
    char key[ENV_KEY_MAX];
    char val[ENV_VAL_MAX];
    int len;
    int rc;

    if (!p)
        return -ESRCH;
    if (buflen <= 0)
        return -EINVAL;

    len = user_strlen((const char *)name_ptr, sizeof(key));
    if (len < 0)
        return -EFAULT;
    if (copy_from_user(key, (const void *)name_ptr, (size_t)len + 1) < 0)
        return -EFAULT;

    rc = env_get(p, key, val, sizeof(val));
    if (rc < 0)
        return rc;
    if ((size_t)rc >= (size_t)buflen)
        return -ERANGE;
    if (copy_to_user((void *)buf_ptr, val, (size_t)rc + 1) < 0)
        return -EFAULT;
    return rc;
}

static long do_setenv(long name_ptr, long val_ptr, long global_flag)
{
    process_t *p = process_current();
    char key[ENV_KEY_MAX];
    char val[ENV_VAL_MAX];
    int len;

    if (!p)
        return -ESRCH;

    len = user_strlen((const char *)name_ptr, sizeof(key));
    if (len < 0)
        return -EFAULT;
    if (copy_from_user(key, (const void *)name_ptr, (size_t)len + 1) < 0)
        return -EFAULT;

    if (!val_ptr)
        return (long)env_unset(p, key, global_flag != 0);

    len = user_strlen((const char *)val_ptr, sizeof(val));
    if (len < 0)
        return -EFAULT;
    if (copy_from_user(val, (const void *)val_ptr, (size_t)len + 1) < 0)
        return -EFAULT;

    return (long)env_set(p, key, val, global_flag != 0);
}

static long do_proc_list(long outp, long max_entries)
{
    /* Prefer shared snapshot — one publish, no per-call table storm. */
    proc_page_t *page;
    proc_list_entry_t chunk[16];
    int total;
    int copied;
    int want;

    if (max_entries < 0)
        return -EINVAL;

    process_snapshot_publish();
    page = process_page_get();
    if (page && page->magic == PROC_PAGE_MAGIC && (page->seq & 1u) == 0) {
        total = (int)page->process_count;
        if (!outp || max_entries == 0)
            return total;
        copied = (int)page->count;
        if (copied > max_entries)
            copied = (int)max_entries;
        if (copied > 0 &&
            copy_to_user((void *)outp, page->entries,
                         (size_t)copied * sizeof(page->entries[0])) < 0)
            return -EFAULT;
        return copied;
    }

    /* Fallback: walk in tiny stack chunks (never PROC_MAX / PROC_PAGE_MAX). */
    total = process_list(NULL, 0);
    if (!outp || max_entries == 0)
        return total;

    want = total;
    if (want > PROC_PAGE_MAX)
        want = PROC_PAGE_MAX;
    if ((long)want > max_entries)
        want = (int)max_entries;

    copied = 0;
    while (copied < want) {
        int batch = want - copied;
        int remain;

        if (batch > 16)
            batch = 16;
        (void)process_list_range(chunk, (size_t)batch, (size_t)copied);
        remain = total - copied;
        if (remain < batch)
            batch = remain;
        if (batch <= 0)
            break;
        if (copy_to_user((void *)((uintptr_t)outp + (size_t)copied * sizeof(chunk[0])),
                         chunk, (size_t)batch * sizeof(chunk[0])) < 0)
            return -EFAULT;
        copied += batch;
    }
    return copied;
}

static long do_proc_stat(long pid, long outp)
{
    proc_stat_t st;
    int rc;

    if (!outp)
        return -EFAULT;
    rc = process_stat((pid_t)pid, &st);
    if (rc < 0)
        return rc;
    if (copy_to_user((void *)outp, &st, sizeof(st)) < 0)
        return -EFAULT;
    return 0;
}

static long do_sysinfo(long outp)
{
    sys_info_t info;
    proc_page_t *page;
    int rc;

    if (!outp)
        return -EFAULT;

    process_snapshot_publish();
    page = process_page_get();
    if (page && page->magic == PROC_PAGE_MAGIC && (page->seq & 1u) == 0) {
        memset(&info, 0, sizeof(info));
        info.uptime_ticks = page->uptime_ticks;
        info.total_cpu_ticks = page->total_cpu_ticks;
        info.total_ram_bytes = page->total_ram_bytes;
        info.used_ram_bytes = page->used_ram_bytes;
        info.free_ram_bytes = page->free_ram_bytes;
        info.process_count = page->process_count;
        if (copy_to_user((void *)outp, &info, sizeof(info)) < 0)
            return -EFAULT;
        return 0;
    }

    rc = process_sysinfo(&info);
    if (rc < 0)
        return rc;
    if (copy_to_user((void *)outp, &info, sizeof(info)) < 0)
        return -EFAULT;
    return 0;
}

static long do_lseek(long fd, long off, long whence)
{
    process_t *p = process_current();
    int enc;
    if (!p)
        return -EBADF;
    enc = process_lookup_fd(p, (int)fd);
    if (enc < 0)
        return -EBADF;
    if (PROC_FD_IS_SOCK(enc))
        return -ESPIPE;
    return (long)vfs_lseek(enc, (off_t)off, (int)whence);
}

static long do_mkdir(long path, long mode)
{
    process_t *p = process_current();
    char upath[VFS_PATH_MAX];
    char kpath[VFS_PATH_MAX];
    int len;
    if (!p)
        return -EFAULT;
    len = user_strlen((const char *)path, sizeof(upath));
    if (len < 0)
        return -EFAULT;
    if (copy_from_user(upath, (const void *)path, (size_t)len + 1) < 0)
        return -EFAULT;
    if (resolve_path(p, upath, kpath, sizeof(kpath)) < 0)
        return -EINVAL;
    return vfs_mkdir(kpath, (int)mode);
}

static long do_chdir(long path)
{
    process_t *p = process_current();
    char upath[VFS_PATH_MAX];
    char kpath[VFS_PATH_MAX];
    int len, vfd;
    if (!p)
        return -EFAULT;
    len = user_strlen((const char *)path, sizeof(upath));
    if (len < 0)
        return -EFAULT;
    if (copy_from_user(upath, (const void *)path, (size_t)len + 1) < 0)
        return -EFAULT;
    if (resolve_path(p, upath, kpath, sizeof(kpath)) < 0)
        return -EINVAL;
    vfd = vfs_open(kpath, O_RDONLY | O_DIRECTORY);
    if (vfd < 0)
        return vfd;
    vfs_close(vfd);
    strncpy(p->cwd, kpath, sizeof(p->cwd) - 1);
    p->cwd[sizeof(p->cwd) - 1] = 0;
    return 0;
}

static long do_getcwd(long buf, long size)
{
    process_t *p = process_current();
    size_t n;
    if (!p || !buf || size <= 0)
        return -EFAULT;
    n = strlen(p->cwd) + 1;
    if ((size_t)size < n)
        return -ERANGE;
    if (copy_to_user((void *)buf, p->cwd, n) < 0)
        return -EFAULT;
    return (long)n;
}

static long do_getdents(long fd, long buf, long count)
{
    process_t *p = process_current();
    vfs_dirent_t ents[32];
    int vfd, n;
    size_t max, bytes;

    if (!p || !buf || count <= 0)
        return -EINVAL;
    vfd = process_lookup_fd(p, (int)fd);
    if (vfd < 0)
        return -EBADF;
    if (PROC_FD_IS_SOCK(vfd))
        return -ENOTDIR;
    max = (size_t)count;
    if (max > sizeof(ents) / sizeof(ents[0]))
        max = sizeof(ents) / sizeof(ents[0]);
    n = vfs_readdir(vfd, ents, max);
    if (n < 0)
        return n;
    bytes = (size_t)n * sizeof(vfs_dirent_t);
    if (copy_to_user((void *)buf, ents, bytes) < 0)
        return -EFAULT;
    return n;
}

static long do_getuid(void)
{
    process_t *p = process_current();
    return p ? (long)p->uid : 0;
}

static long do_geteuid(void)
{
    process_t *p = process_current();
    return p ? (long)p->euid : 0;
}

static long do_mount(long source, long target, long fstype, long flags, long data)
{
    char ksrc[128], ktgt[128], kfs[32];
    int len;

    memset(ksrc, 0, sizeof(ksrc));
    if (source) {
        len = user_strlen((const char *)source, sizeof(ksrc));
        if (len < 0 || copy_from_user(ksrc, (const void *)source, (size_t)len + 1) < 0)
            return -EFAULT;
    }
    len = user_strlen((const char *)target, sizeof(ktgt));
    if (len < 0 || copy_from_user(ktgt, (const void *)target, (size_t)len + 1) < 0)
        return -EFAULT;
    len = user_strlen((const char *)fstype, sizeof(kfs));
    if (len < 0 || copy_from_user(kfs, (const void *)fstype, (size_t)len + 1) < 0)
        return -EFAULT;
    (void)data;
    return vfs_mount(source ? ksrc : NULL, ktgt, kfs, (unsigned long)flags, NULL);
}

static long do_umount(long target, long flags)
{
    char ktgt[128];
    int len;
    len = user_strlen((const char *)target, sizeof(ktgt));
    if (len < 0 || copy_from_user(ktgt, (const void *)target, (size_t)len + 1) < 0)
        return -EFAULT;
    return vfs_umount(ktgt, (int)flags);
}

#define K_AIO_MAX 8
static vfs_aio_t g_kaios[K_AIO_MAX];
static int g_kaio_used[K_AIO_MAX];

static long do_aio_submit(long aiop)
{
    vfs_aio_t tmp;
    const vfs_api_t *api = vfs_api_get();
    process_t *p = process_current();
    int vfd, slot, rc;
    if (!api || !api->aio_submit || !p)
        return -ENOTSUP;
    if (copy_from_user(&tmp, (const void *)aiop, sizeof(tmp)) < 0)
        return -EFAULT;
    vfd = process_lookup_fd(p, tmp.fd);
    if (vfd < 0)
        return -EBADF;
    for (slot = 0; slot < K_AIO_MAX; slot++) {
        if (!g_kaio_used[slot])
            break;
    }
    if (slot >= K_AIO_MAX)
        return -EAGAIN;
    g_kaios[slot] = tmp;
    g_kaios[slot].fd = vfd;
    g_kaios[slot].done = 0;
    g_kaios[slot].complete = NULL;
    g_kaio_used[slot] = 1;
    rc = api->aio_submit(&g_kaios[slot]);
    if (rc < 0) {
        g_kaio_used[slot] = 0;
        return rc;
    }
    return slot;
}

static long do_aio_wait(long slot)
{
    const vfs_api_t *api = vfs_api_get();
    int rc;
    if (!api || !api->aio_wait)
        return -ENOTSUP;
    if (slot < 0 || slot >= K_AIO_MAX || !g_kaio_used[slot])
        return -EINVAL;
    rc = api->aio_wait(&g_kaios[slot]);
    g_kaio_used[slot] = 0;
    return rc;
}

static const mkdx_api_t *mkdx(void)
{
    return mkdx_api_get();
}

static long do_gx_info(long outp)
{
    ugx_info info;
    const mkdx_api_t *api = mkdx();
    if (!outp || !api || !api->info)
        return -1;
    if (api->info(&info.width, &info.height, &info.bpp) < 0)
        return -1;
    if (copy_to_user((void *)outp, &info, sizeof(info)) < 0)
        return -1;
    return 0;
}

static long do_gx_present(void)
{
    const mkdx_api_t *api = mkdx();
    if (!api || !api->present)
        return -1;
    return api->present();
}

static long do_wm_create(long argp)
{
    ugx_window_opts args;
    const mkdx_api_t *api = mkdx();
    process_t *p = process_current();
    if (!api || !api->wm_create)
        return -1;
    if (copy_from_user(&args, (const void *)argp, sizeof(args)) < 0)
        return -1;
    return api->wm_create(&args, p ? (uint32_t)p->pid : 0);
}

static long do_wm_set(long id, long optsp)
{
    ugx_window_opts args;
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_set)
        return -1;
    if (copy_from_user(&args, (const void *)optsp, sizeof(args)) < 0)
        return -1;
    return api->wm_set((int)id, &args);
}

static long do_wm_get(long id, long outp)
{
    ugx_window_opts args;
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_get)
        return -1;
    if (api->wm_get((int)id, &args) < 0)
        return -1;
    if (copy_to_user((void *)outp, &args, sizeof(args)) < 0)
        return -1;
    return 0;
}

static long do_wm_close(long id)
{
    const mkdx_api_t *api = mkdx();
    if (!api)
        return -1;
    if (api->wm_close)
        return api->wm_close((int)id);
    if (api->wm_destroy)
        return api->wm_destroy((int)id);
    return -1;
}

static long do_wm_destroy(long id)
{
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_destroy)
        return -1;
    return api->wm_destroy((int)id);
}

static long do_wm_map(long id, long outp)
{
    ugx_map m;
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_map)
        return -1;
    if (api->wm_map((int)id, &m) < 0)
        return -1;
    if (copy_to_user((void *)outp, &m, sizeof(m)) < 0)
        return -1;
    return 0;
}

static long do_wm_move(long id, long x, long y)
{
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_move)
        return -1;
    return api->wm_move((int)id, (int32_t)x, (int32_t)y);
}

static long do_wm_resize(long id, long w, long h)
{
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_resize)
        return -1;
    return api->wm_resize((int)id, (int32_t)w, (int32_t)h);
}

static long do_wm_focus(long id)
{
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_focus)
        return -1;
    return api->wm_focus((int)id);
}

static long do_wm_show(long id, long vis)
{
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_show)
        return -1;
    return api->wm_show((int)id, (int)vis);
}

static long do_gx_fill(long argp, int rounded)
{
    ugx_fill_args args;
    const mkdx_api_t *api = mkdx();
    if (!api || !api->fill)
        return -1;
    if (copy_from_user(&args, (const void *)argp, sizeof(args)) < 0)
        return -1;
    return api->fill(&args, rounded);
}

static long do_gx_set_wallpaper(long argp)
{
    ugx_wallpaper args;
    ugx_wallpaper kargs;
    uint32_t pixels[64];
    size_t npix;
    const mkdx_api_t *api = mkdx();

    if (!api || !api->set_wallpaper)
        return -1;
    if (copy_from_user(&args, (const void *)argp, sizeof(args)) < 0)
        return -1;

    /* NULL / zero size => load baked default wallpaper from initrd. */
    if (!args.pixels || args.width == 0 || args.height == 0) {
        kargs.pixels = NULL;
        kargs.width = 0;
        kargs.height = 0;
        kargs.stride = 0;
        return api->set_wallpaper(&kargs);
    }

    npix = (size_t)args.width * (size_t)args.height;
    if (npix > sizeof(pixels) / sizeof(pixels[0]))
        return -1;
    if (copy_from_user(pixels, args.pixels, npix * sizeof(uint32_t)) < 0)
        return -1;

    kargs = args;
    kargs.pixels = pixels;
    kargs.stride = args.width;
    return api->set_wallpaper(&kargs);
}

static long do_input_state(long outp)
{
    ugx_input_state st;
    const mkdx_api_t *api = mkdx();
    if (!api || !api->input_state)
        return -1;
    if (api->input_state(&st) < 0)
        return -1;
    if (copy_to_user((void *)outp, &st, sizeof(st)) < 0)
        return -1;
    return 0;
}

static long do_wm_pop_key(long id)
{
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_pop_key)
        return -1;
    return api->wm_pop_key((int)id);
}

static long do_gx_damage(long win_id)
{
    const mkdx_api_t *api = mkdx();
    if (!api || !api->mark_dirty)
        return -1;
    api->mark_dirty((int)win_id);
    return 0;
}

static long do_wm_get_frame(long id, long outp)
{
    ugx_frame fr;
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_get_frame)
        return -1;
    if (api->wm_get_frame((int)id, &fr) < 0)
        return -1;
    if (copy_to_user((void *)outp, &fr, sizeof(fr)) < 0)
        return -1;
    return 0;
}

static long do_wm_find(long titlep)
{
    char title[64];
    int len;
    const mkdx_api_t *api = mkdx();

    if (!api || !api->wm_find)
        return -1;
    len = user_strlen((const char *)titlep, sizeof(title));
    if (len < 0)
        return -1;
    if (copy_from_user(title, (const void *)titlep, (size_t)len + 1) < 0)
        return -1;
    return api->wm_find(title);
}

static long do_wm_find_class(long classp)
{
    char class_name[32];
    int len;
    const mkdx_api_t *api = mkdx();

    if (!api || !api->wm_find_class)
        return -1;
    len = user_strlen((const char *)classp, sizeof(class_name));
    if (len < 0)
        return -1;
    if (copy_from_user(class_name, (const void *)classp, (size_t)len + 1) < 0)
        return -1;
    return api->wm_find_class(class_name);
}

static int copy_netif_name(long namep, char name[NETIF_NAME_MAX])
{
    int len;
    if (!namep) {
        name[0] = 0;
        return 0;
    }
    len = user_strlen((const char *)namep, NETIF_NAME_MAX);
    if (len < 0)
        return -EFAULT;
    if (copy_from_user(name, (const void *)namep, (size_t)len + 1) < 0)
        return -EFAULT;
    return 0;
}

static long do_netif_get(long namep, long outp)
{
    netif_info_t info;
    char name[NETIF_NAME_MAX];
    int rc;

    if (!outp)
        return -EFAULT;
    rc = copy_netif_name(namep, name);
    if (rc < 0)
        return rc;
    rc = netif_get_info(name[0] ? name : NULL, &info);
    if (rc < 0)
        return rc;
    if (copy_to_user((void *)outp, &info, sizeof(info)) < 0)
        return -EFAULT;
    return 0;
}

static long do_netif_set(long namep, long infop)
{
    netif_info_t info;
    char name[NETIF_NAME_MAX];
    int rc;

    if (!infop)
        return -EFAULT;
    rc = copy_netif_name(namep, name);
    if (rc < 0)
        return rc;
    if (copy_from_user(&info, (const void *)infop, sizeof(info)) < 0)
        return -EFAULT;
    return netif_set_info(name[0] ? name : NULL, &info);
}

static long do_dhcp_renew(long namep)
{
    char name[NETIF_NAME_MAX];
    int rc = copy_netif_name(namep, name);
    if (rc < 0)
        return rc;
    return netif_dhcp_renew(name[0] ? name : NULL);
}

static long do_listen(long fd, long backlog)
{
    process_t *p = process_current();
    int enc;
    if (!p)
        return -ESRCH;
    enc = process_lookup_fd(p, (int)fd);
    if (enc < 0 || !PROC_FD_IS_SOCK(enc))
        return -ENOTSOCK;
    return sock_listen(PROC_FD_SOCK_ID(enc), (int)backlog);
}

static long do_accept(long fd, long outp)
{
    process_t *p = process_current();
    sockaddr_in_t addr;
    int enc, sid, ufd;
    if (!p)
        return -ESRCH;
    enc = process_lookup_fd(p, (int)fd);
    if (enc < 0 || !PROC_FD_IS_SOCK(enc))
        return -ENOTSOCK;
    sid = sock_accept(PROC_FD_SOCK_ID(enc), outp ? &addr : NULL);
    if (sid < 0)
        return sid;
    ufd = process_alloc_sock_fd(p, sid);
    if (ufd < 0) {
        sock_close(sid);
        return -EMFILE;
    }
    if (outp && copy_to_user((void *)outp, &addr, sizeof(addr)) < 0) {
        process_free_fd(p, ufd);
        sock_close(sid);
        return -EFAULT;
    }
    return ufd;
}

static long do_send(long fd, long buf, long len)
{
    process_t *p = process_current();
    char kbuf[1024];
    int enc;
    size_t want, sent = 0;
    if (!p)
        return -ESRCH;
    if (len < 0)
        return -EINVAL;
    enc = process_lookup_fd(p, (int)fd);
    if (enc < 0 || !PROC_FD_IS_SOCK(enc))
        return -ENOTSOCK;
    want = (size_t)len;
    while (sent < want) {
        size_t chunk = want - sent;
        ssize_t n;
        if (chunk > sizeof(kbuf))
            chunk = sizeof(kbuf);
        if (copy_from_user(kbuf, (const void *)(buf + sent), chunk) < 0)
            return sent > 0 ? (long)sent : -EFAULT;
        n = sock_send(PROC_FD_SOCK_ID(enc), kbuf, chunk, 0);
        if (n < 0)
            return sent > 0 ? (long)sent : n;
        sent += (size_t)n;
        if ((size_t)n < chunk)
            break;
    }
    return (long)sent;
}

static long do_recv(long fd, long buf, long len)
{
    process_t *p = process_current();
    char kbuf[1024];
    int enc;
    size_t want, got = 0;
    if (!p)
        return -ESRCH;
    if (len < 0)
        return -EINVAL;
    enc = process_lookup_fd(p, (int)fd);
    if (enc < 0 || !PROC_FD_IS_SOCK(enc))
        return -ENOTSOCK;
    want = (size_t)len;
    while (got < want) {
        size_t chunk = want - got;
        ssize_t n;
        if (chunk > sizeof(kbuf))
            chunk = sizeof(kbuf);
        n = sock_recv(PROC_FD_SOCK_ID(enc), kbuf, chunk, 0);
        if (n < 0)
            return got > 0 ? (long)got : n;
        if (n == 0)
            break;
        if (copy_to_user((void *)(buf + got), kbuf, (size_t)n) < 0)
            return got > 0 ? (long)got : -EFAULT;
        got += (size_t)n;
        if ((size_t)n < chunk)
            break;
    }
    return (long)got;
}

static long do_shutdown(long fd, long how)
{
    process_t *p = process_current();
    int enc;
    if (!p)
        return -ESRCH;
    enc = process_lookup_fd(p, (int)fd);
    if (enc < 0 || !PROC_FD_IS_SOCK(enc))
        return -ENOTSOCK;
    return sock_shutdown(PROC_FD_SOCK_ID(enc), (int)how);
}

long syscall_dispatch(long n, long a1, long a2, long a3, long a4, long a5)
{
    switch (n) {
    case SYS_EXIT:   return do_exit(a1);
    case SYS_READ:   return do_read(a1, a2, a3);
    case SYS_WRITE:  return do_write(a1, a2, a3);
    case SYS_OPEN:   return do_open(a1, a2);
    case SYS_CLOSE:  return do_close(a1);
    case SYS_CHDIR:  return do_chdir(a1);
    case SYS_LSEEK:  return do_lseek(a1, a2, a3);
    case SYS_MKDIR:  return do_mkdir(a1, a2);
    case SYS_MOUNT:  return do_mount(a1, a2, a3, a4, a5);
    case SYS_UMOUNT: return do_umount(a1, a2);
    case SYS_GETUID: return do_getuid();
    case SYS_GETEUID: return do_geteuid();
    case SYS_GETDENTS: return do_getdents(a1, a2, a3);
    case SYS_GETCWD: return do_getcwd(a1, a2);
    case SYS_AIO_SUBMIT: return do_aio_submit(a1);
    case SYS_AIO_WAIT:   return do_aio_wait(a1);
    case SYS_UNLINK: {
        char kpath[256];
        int len = user_strlen((const char *)a1, sizeof(kpath));
        const vfs_api_t *api = vfs_api_get();
        if (len < 0 || copy_from_user(kpath, (const void *)a1, (size_t)len + 1) < 0)
            return -EFAULT;
        if (!api || !api->unlink) return -ENOTSUP;
        return api->unlink(kpath);
    }
    case SYS_RMDIR: {
        char kpath[256];
        int len = user_strlen((const char *)a1, sizeof(kpath));
        const vfs_api_t *api = vfs_api_get();
        if (len < 0 || copy_from_user(kpath, (const void *)a1, (size_t)len + 1) < 0)
            return -EFAULT;
        if (!api || !api->rmdir) return -ENOTSUP;
        return api->rmdir(kpath);
    }
    case SYS_RENAME: {
        char a[256], b[256];
        int la, lb;
        const vfs_api_t *api = vfs_api_get();
        la = user_strlen((const char *)a1, sizeof(a));
        lb = user_strlen((const char *)a2, sizeof(b));
        if (la < 0 || lb < 0) return -EFAULT;
        if (copy_from_user(a, (const void *)a1, (size_t)la + 1) < 0) return -EFAULT;
        if (copy_from_user(b, (const void *)a2, (size_t)lb + 1) < 0) return -EFAULT;
        if (!api || !api->rename) return -ENOTSUP;
        return api->rename(a, b);
    }
    case SYS_MMAP: {
        process_t *p = process_current();
        int vfd = -1;
        if (!p) return -EFAULT;
        if ((int)a5 >= 0) {
            vfd = process_lookup_fd(p, (int)a5);
            if (vfd < 0) return -EBADF;
        }
        return mm_mmap(p, (uint32_t)a1, (size_t)a2, (int)a3, (int)a4, vfd, 0);
    }
    case SYS_MUNMAP:
        return mm_munmap(process_current(), (uint32_t)a1, (size_t)a2);
    case SYS_MSYNC:
        return mm_msync(process_current(), (uint32_t)a1, (size_t)a2, (int)a3);
    case SYS_FLOCK: {
        process_t *p = process_current();
        flock_t fl;
        const vfs_api_t *api = vfs_api_get();
        int vfd;
        if (!p || !api || !api->flock) return -ENOTSUP;
        vfd = process_lookup_fd(p, (int)a1);
        if (vfd < 0) return -EBADF;
        if (a3) {
            if (copy_from_user(&fl, (const void *)a3, sizeof(fl)) < 0)
                return -EFAULT;
        } else {
            memset(&fl, 0, sizeof(fl));
            fl.l_type = (int16_t)a2;
        }
        fl.l_pid = p->pid;
        return api->flock(vfd, (int)a2, &fl);
    }
    case SYS_GETXATTR: {
        char path[256], name[64];
        int lp, ln;
        const vfs_api_t *api = vfs_api_get();
        if (!api || !api->getxattr) return -ENOTSUP;
        lp = user_strlen((const char *)a1, sizeof(path));
        ln = user_strlen((const char *)a2, sizeof(name));
        if (lp < 0 || ln < 0) return -EFAULT;
        if (copy_from_user(path, (const void *)a1, (size_t)lp + 1) < 0) return -EFAULT;
        if (copy_from_user(name, (const void *)a2, (size_t)ln + 1) < 0) return -EFAULT;
        {
            char tmp[256];
            int n = api->getxattr(path, name, tmp, sizeof(tmp));
            if (n < 0) return n;
            if (a3 && a4) {
                size_t c = (size_t)n < (size_t)a4 ? (size_t)n : (size_t)a4;
                if (copy_to_user((void *)a3, tmp, c) < 0) return -EFAULT;
            }
            return n;
        }
    }
    case SYS_SETXATTR: {
        char path[256], name[64], val[256];
        int lp, ln;
        const vfs_api_t *api = vfs_api_get();
        if (!api || !api->setxattr) return -ENOTSUP;
        lp = user_strlen((const char *)a1, sizeof(path));
        ln = user_strlen((const char *)a2, sizeof(name));
        if (lp < 0 || ln < 0) return -EFAULT;
        if (copy_from_user(path, (const void *)a1, (size_t)lp + 1) < 0) return -EFAULT;
        if (copy_from_user(name, (const void *)a2, (size_t)ln + 1) < 0) return -EFAULT;
        if (a4 > (long)sizeof(val)) return -EINVAL;
        if (a4 && copy_from_user(val, (const void *)a3, (size_t)a4) < 0) return -EFAULT;
        return api->setxattr(path, name, val, (size_t)a4, (int)a5);
    }
    case SYS_LISTXATTR: {
        char path[256], list[512];
        int lp, n;
        const vfs_api_t *api = vfs_api_get();
        if (!api || !api->listxattr) return -ENOTSUP;
        lp = user_strlen((const char *)a1, sizeof(path));
        if (lp < 0 || copy_from_user(path, (const void *)a1, (size_t)lp + 1) < 0)
            return -EFAULT;
        n = api->listxattr(path, list, sizeof(list));
        if (n < 0) return n;
        if (a2 && a3) {
            size_t c = (size_t)n < (size_t)a3 ? (size_t)n : (size_t)a3;
            if (copy_to_user((void *)a2, list, c) < 0) return -EFAULT;
        }
        return n;
    }
    case SYS_REMOVEXATTR: {
        char path[256], name[64];
        int lp, ln;
        const vfs_api_t *api = vfs_api_get();
        if (!api || !api->removexattr) return -ENOTSUP;
        lp = user_strlen((const char *)a1, sizeof(path));
        ln = user_strlen((const char *)a2, sizeof(name));
        if (lp < 0 || ln < 0) return -EFAULT;
        if (copy_from_user(path, (const void *)a1, (size_t)lp + 1) < 0) return -EFAULT;
        if (copy_from_user(name, (const void *)a2, (size_t)ln + 1) < 0) return -EFAULT;
        return api->removexattr(path, name);
    }
    case SYS_FSNOTIFY_ADD: {
        char path[256];
        int lp;
        const vfs_api_t *api = vfs_api_get();
        if (!api || !api->fsnotify_add_watch) return -ENOTSUP;
        lp = user_strlen((const char *)a1, sizeof(path));
        if (lp < 0 || copy_from_user(path, (const void *)a1, (size_t)lp + 1) < 0)
            return -EFAULT;
        return api->fsnotify_add_watch(path, (uint32_t)a2);
    }
    case SYS_FSNOTIFY_RM: {
        const vfs_api_t *api = vfs_api_get();
        if (!api || !api->fsnotify_rm_watch) return -ENOTSUP;
        return api->fsnotify_rm_watch((int)a1);
    }
    case SYS_FSNOTIFY_READ: {
        fsnotify_event_t ev[16];
        const vfs_api_t *api = vfs_api_get();
        int n;
        size_t max;
        if (!api || !api->fsnotify_read) return -ENOTSUP;
        max = (size_t)a3;
        if (max > 16) max = 16;
        n = api->fsnotify_read((int)a1, ev, max);
        if (n < 0) return n;
        if (n && copy_to_user((void *)a2, ev, (size_t)n * sizeof(ev[0])) < 0)
            return -EFAULT;
        return n;
    }
    case SYS_MODULE_LOAD: {
        char path[256];
        int lp = user_strlen((const char *)a1, sizeof(path));
        if (lp < 0 || copy_from_user(path, (const void *)a1, (size_t)lp + 1) < 0)
            return -EFAULT;
        return module_load_path(path);
    }
    case SYS_SOCKET: {
        process_t *p = process_current();
        int sid, ufd;
        if (!p)
            return -ESRCH;
        sid = sock_create((int)a1, (int)a2, (int)a3);
        if (sid < 0)
            return sid;
        ufd = process_alloc_sock_fd(p, sid);
        if (ufd < 0) {
            sock_close(sid);
            return -EMFILE;
        }
        return ufd;
    }
    case SYS_BIND: {
        process_t *p = process_current();
        sockaddr_in_t addr;
        int enc;
        if (!p)
            return -ESRCH;
        enc = process_lookup_fd(p, (int)a1);
        if (enc < 0 || !PROC_FD_IS_SOCK(enc))
            return -ENOTSOCK;
        if (copy_from_user(&addr, (const void *)a2, sizeof(addr)) < 0)
            return -EFAULT;
        return sock_bind(PROC_FD_SOCK_ID(enc), &addr);
    }
    case SYS_CONNECT: {
        process_t *p = process_current();
        sockaddr_in_t addr;
        int enc;
        if (!p)
            return -ESRCH;
        enc = process_lookup_fd(p, (int)a1);
        if (enc < 0 || !PROC_FD_IS_SOCK(enc))
            return -ENOTSOCK;
        if (copy_from_user(&addr, (const void *)a2, sizeof(addr)) < 0)
            return -EFAULT;
        return sock_connect(PROC_FD_SOCK_ID(enc), &addr);
    }
    case SYS_SENDTO: {
        process_t *p = process_current();
        char kbuf[1472];
        sockaddr_in_t addr, *dstp = NULL;
        int enc;
        size_t len;
        ssize_t n;
        if (!p)
            return -ESRCH;
        enc = process_lookup_fd(p, (int)a1);
        if (enc < 0 || !PROC_FD_IS_SOCK(enc))
            return -ENOTSOCK;
        len = (size_t)a3;
        if (len > sizeof(kbuf))
            return -EMSGSIZE;
        if (len && copy_from_user(kbuf, (const void *)a2, len) < 0)
            return -EFAULT;
        if (a4) {
            if (copy_from_user(&addr, (const void *)a4, sizeof(addr)) < 0)
                return -EFAULT;
            dstp = &addr;
        }
        n = sock_sendto(PROC_FD_SOCK_ID(enc), kbuf, len, 0, dstp);
        return n;
    }
    case SYS_RECVFROM: {
        process_t *p = process_current();
        char kbuf[1472];
        sockaddr_in_t addr;
        int enc;
        size_t len;
        ssize_t n;
        if (!p)
            return -ESRCH;
        enc = process_lookup_fd(p, (int)a1);
        if (enc < 0 || !PROC_FD_IS_SOCK(enc))
            return -ENOTSOCK;
        len = (size_t)a3;
        if (len > sizeof(kbuf))
            len = sizeof(kbuf);
        n = sock_recvfrom(PROC_FD_SOCK_ID(enc), kbuf, len, 0, a4 ? &addr : NULL);
        if (n < 0)
            return n;
        if (n && copy_to_user((void *)a2, kbuf, (size_t)n) < 0)
            return -EFAULT;
        if (a4 && copy_to_user((void *)a4, &addr, sizeof(addr)) < 0)
            return -EFAULT;
        return n;
    }
    case SYS_NETIF_GET:
        return do_netif_get(a1, a2);
    case SYS_NETIF_SET:
        return do_netif_set(a1, a2);
    case SYS_DHCP_RENEW:
        return do_dhcp_renew(a1);
    case SYS_LISTEN:
        return do_listen(a1, a2);
    case SYS_ACCEPT:
        return do_accept(a1, a2);
    case SYS_SEND:
        return do_send(a1, a2, a3);
    case SYS_RECV:
        return do_recv(a1, a2, a3);
    case SYS_SHUTDOWN:
        return do_shutdown(a1, a2);
    case SYS_SPAWN:
        return do_spawn(a1, a2, a3, a4);
    case SYS_WAITPID:
        return do_waitpid(a1, a2, a3);
    case SYS_KILL:
        return do_kill(a1);
    case SYS_PROC_LIST:
        return do_proc_list(a1, a2);
    case SYS_PROC_STAT:
        return do_proc_stat(a1, a2);
    case SYS_SYSINFO:
        return do_sysinfo(a1);
    /* Service supervisor syscalls (Wave M). */
    case SYS_SERVICE_LIST:
        return service_syscall_list(a1, a2, a3);
    case SYS_SERVICE_START:
        return service_syscall_start(a1);
    case SYS_SERVICE_STOP:
        return service_syscall_stop(a1);
    case SYS_SERVICE_STATUS:
        return service_syscall_status(a1, a2);
    case SYS_GETENV:
        return do_getenv(a1, a2, a3);
    case SYS_SETENV:
        return do_setenv(a1, a2, a3);
    case SYS_CONSOLE_SHOW:
        return do_console_show(a1, a2);
    case SYS_GETARGC:
        return do_getargc();
    case SYS_GETARGV:
        return do_getargv(a1, a2, a3);
    case SYS_TIME_MAP: {
        time_page_t *tp = time_page_get();
        if (!tp || tp->magic != TIME_PAGE_MAGIC)
            return 0;
        return (long)(uintptr_t)tp;
    }
    case SYS_PROC_MAP: {
        proc_page_t *pp = process_page_get();
        if (!pp || pp->magic != PROC_PAGE_MAGIC)
            return 0;
        process_snapshot_publish();
        return (long)(uintptr_t)pp;
    }
    case SYS_TIME_GET: {
        time_snapshot_t snap;
        memset(&snap, 0, sizeof(snap));
        snap.utc_nsec = time_utc_nsec_now();
        snap.mono_nsec = time_mono_nsec_now();
        {
            time_page_t *tp = time_page_get();
            if (tp) {
                snap.tz_offset_sec = tp->tz_offset_sec;
                snap.flags = tp->flags;
                memcpy(snap.tz_name, tp->tz_name, sizeof(snap.tz_name));
            }
        }
        if (copy_to_user((void *)a1, &snap, sizeof(snap)) < 0)
            return -1;
        return 0;
    }
    case SYS_TIME_SET: {
        uint64_t utc = ((uint64_t)(uint32_t)a2 << 32) | (uint64_t)(uint32_t)a1;
        return time_set_utc_nsec(utc);
    }
    case SYS_TIME_SETTZ: {
        char name[16];
        memset(name, 0, sizeof(name));
        if (a2) {
            int n = user_strlen((const char *)a2, sizeof(name));
            if (n < 0)
                return -1;
            if (n >= (int)sizeof(name))
                n = (int)sizeof(name) - 1;
            if (copy_from_user(name, (const void *)a2, (size_t)n + 1) < 0)
                return -1;
        }
        return time_set_timezone((int32_t)a1, name);
    }
    case SYS_TIME_SETFLAGS:
        return time_set_flags((uint32_t)a1);
    case SYS_GETPID: return do_getpid();
    case SYS_GETPPID: return do_getppid();
    case SYS_YIELD:  return do_yield();
    case SYS_FORK:   return -1;

    case SYS_GX_INFO:          return do_gx_info(a1);
    case SYS_GX_PRESENT:       return do_gx_present();
    case SYS_WM_CREATE:        return do_wm_create(a1);
    case SYS_WM_SET:           return do_wm_set(a1, a2);
    case SYS_WM_GET:           return do_wm_get(a1, a2);
    case SYS_WM_CLOSE:         return do_wm_close(a1);
    case SYS_WM_DESTROY:       return do_wm_destroy(a1);
    case SYS_WM_MAP:           return do_wm_map(a1, a2);
    case SYS_WM_MOVE:          return do_wm_move(a1, a2, a3);
    case SYS_WM_RESIZE:        return do_wm_resize(a1, a2, a3);
    case SYS_WM_FOCUS:         return do_wm_focus(a1);
    case SYS_WM_SHOW:          return do_wm_show(a1, a2);
    case SYS_GX_FILL:          return do_gx_fill(a1, 0);
    case SYS_GX_FILL_ROUND:    return do_gx_fill(a1, 1);
    case SYS_GX_SET_WALLPAPER: return do_gx_set_wallpaper(a1);
    case SYS_INPUT_STATE:      return do_input_state(a1);
    case SYS_WM_POP_KEY:       return do_wm_pop_key(a1);
    case SYS_GX_DAMAGE:        return do_gx_damage(a1);
    case SYS_WM_GET_FRAME:     return do_wm_get_frame(a1, a2);
    case SYS_WM_FIND:          return do_wm_find(a1);
    case SYS_WM_FIND_CLASS:    return do_wm_find_class(a1);

    default:         return -1;
    }
}

void syscall_init(void)
{
}

static long syscall0(long n)
{
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n) : "memory");
    return ret;
}

static long syscall1(long n, long a1)
{
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "b"(a1) : "memory");
    return ret;
}

static long syscall3(long n, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(n), "b"(a1), "c"(a2), "d"(a3)
                 : "memory");
    return ret;
}

long sys_exit(int code) { return syscall1(SYS_EXIT, code); }
long sys_getpid(void)   { return syscall0(SYS_GETPID); }
long sys_yield(void)    { return syscall0(SYS_YIELD); }

long sys_read(int fd, void *buf, size_t count)
{
    return syscall3(SYS_READ, fd, (long)buf, (long)count);
}

long sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, fd, (long)buf, (long)count);
}

long sys_open(const char *path, int flags)
{
    long ret;
    __asm__ volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(SYS_OPEN), "b"(path), "c"(flags)
                 : "memory");
    return ret;
}

long sys_close(int fd) { return syscall1(SYS_CLOSE, fd); }
