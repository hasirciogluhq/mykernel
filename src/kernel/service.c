#include <kernel/service.h>
#include <kernel/errno.h>
#include <kernel/mke.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/uaccess.h>
#include <drivers/serial.h>

typedef struct service_entry {
    service_info_t info;
    int            registered;
    int            manual_stop;
} service_entry_t;

static service_entry_t g_services[SERVICE_MAX];

static int service_proc_alive(const process_t *p)
{
    return p && (p->state == PROC_READY || p->state == PROC_RUNNING || p->state == PROC_BLOCKED);
}

static int service_path_exists(const char *path)
{
    int fd;

    if (!path || !path[0])
        return 0;
    fd = vfs_open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    (void)vfs_close(fd);
    return 1;
}

static int service_name_eq(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    return strcmp(a, b) == 0;
}

static service_entry_t *service_find_slot(const char *name)
{
    int i;

    if (!name || !name[0])
        return NULL;
    for (i = 0; i < SERVICE_MAX; i++) {
        if (g_services[i].registered && service_name_eq(g_services[i].info.name, name))
            return &g_services[i];
    }
    return NULL;
}

static service_entry_t *service_alloc_slot(void)
{
    int i;

    for (i = 0; i < SERVICE_MAX; i++) {
        if (!g_services[i].registered)
            return &g_services[i];
    }
    return NULL;
}

static void service_refresh_entry(service_entry_t *svc)
{
    process_t *p;

    if (!svc || !svc->registered || svc->info.pid <= 0)
        return;

    p = process_get(svc->info.pid);
    if (service_proc_alive(p)) {
        svc->info.running = 1;
        return;
    }

    if (p && p->state == PROC_ZOMBIE)
        svc->info.last_exit_code = p->exit_code;
    svc->info.pid = 0;
    svc->info.running = 0;
}

static void service_bind_existing_entry(service_entry_t *svc)
{
    process_t **table;
    int i;

    if (!svc || !svc->registered)
        return;
    service_refresh_entry(svc);
    if (svc->info.pid > 0)
        return;

    table = process_table();
    for (i = 0; i < PROC_MAX; i++) {
        if (!service_proc_alive(table[i]))
            continue;
        if (!service_name_eq(table[i]->name, svc->info.name))
            continue;
        svc->info.pid = table[i]->pid;
        svc->info.running = 1;
        return;
    }
}

static int service_spawn_entry(service_entry_t *svc)
{
    int pid;

    if (!svc || !svc->registered)
        return -EINVAL;

    service_refresh_entry(svc);
    if (svc->info.pid > 0)
        return svc->info.pid;
    if (!service_path_exists(svc->info.path))
        return -ENOENT;

    pid = mke_spawn_path(svc->info.path);
    if (pid < 0)
        return pid;

    svc->info.pid = (pid_t)pid;
    svc->info.running = 1;
    svc->manual_stop = 0;
    svc->info.last_exit_code = 0;
    klog("[service] started ");
    klog(svc->info.name);
    klog(" pid=");
    serial_print_uint((uint32_t)svc->info.pid);
    klog("\n");
    return pid;
}

static int service_copy_name_from_user(char *dst, long name_ptr)
{
    int len;

    if (!dst || !name_ptr)
        return -EFAULT;
    len = user_strlen((const char *)name_ptr, SERVICE_NAME_MAX);
    if (len < 0)
        return -EFAULT;
    if (copy_from_user(dst, (const void *)name_ptr, (size_t)len + 1) < 0)
        return -EFAULT;
    return 0;
}

void service_init(void)
{
    memset(g_services, 0, sizeof(g_services));
}

int service_register(const char *name, const char *path, int respawn, int critical)
{
    service_entry_t *svc;

    if (!name || !name[0] || !path || !path[0])
        return -EINVAL;
    if (strlen(name) >= SERVICE_NAME_MAX || strlen(path) >= VFS_PATH_MAX)
        return -ENAMETOOLONG;

    svc = service_find_slot(name);
    if (!svc)
        svc = service_alloc_slot();
    if (!svc)
        return -ENOSPC;

    if (!svc->registered) {
        memset(svc, 0, sizeof(*svc));
        svc->registered = 1;
    }
    strcpy(svc->info.name, name);
    strcpy(svc->info.path, path);
    svc->info.respawn = respawn ? 1 : 0;
    svc->info.critical = critical ? 1 : 0;
    return 0;
}

void service_register_builtin_defaults(void)
{
    (void)service_register("os-ui", "/applications/os-ui.mke", 1, 1);
}

void service_bind_existing_processes(void)
{
    int i;

    for (i = 0; i < SERVICE_MAX; i++) {
        if (!g_services[i].registered)
            continue;
        service_bind_existing_entry(&g_services[i]);
    }
}

void service_start_critical(void)
{
    int i;

    for (i = 0; i < SERVICE_MAX; i++) {
        if (!g_services[i].registered)
            continue;
        service_bind_existing_entry(&g_services[i]);
        if (!g_services[i].info.critical)
            continue;
        if (g_services[i].info.pid > 0 || g_services[i].manual_stop)
            continue;
        (void)service_spawn_entry(&g_services[i]);
    }
}

void service_reap_dead(void)
{
    int i;

    for (i = 0; i < SERVICE_MAX; i++) {
        if (!g_services[i].registered)
            continue;
        service_refresh_entry(&g_services[i]);
        if (g_services[i].info.pid > 0)
            continue;
        if (g_services[i].manual_stop)
            continue;
        if (!g_services[i].info.critical || !g_services[i].info.respawn)
            continue;
        (void)service_spawn_entry(&g_services[i]);
    }
}

int service_list(service_info_t *out, size_t max, size_t *total_out)
{
    size_t total = 0;
    size_t written = 0;
    int i;

    for (i = 0; i < SERVICE_MAX; i++) {
        if (!g_services[i].registered)
            continue;
        service_refresh_entry(&g_services[i]);
        if (out && written < max)
            out[written] = g_services[i].info;
        written += written < max ? 1u : 0u;
        total++;
    }
    if (total_out)
        *total_out = total;
    return (int)written;
}

int service_status(const char *name, service_info_t *out)
{
    service_entry_t *svc = service_find_slot(name);

    if (!svc)
        return -ENOENT;
    service_bind_existing_entry(svc);
    service_refresh_entry(svc);
    if (out)
        *out = svc->info;
    return 0;
}

int service_start(const char *name)
{
    service_entry_t *svc = service_find_slot(name);

    if (!svc)
        return -ENOENT;
    svc->manual_stop = 0;
    service_bind_existing_entry(svc);
    return service_spawn_entry(svc);
}

int service_stop(const char *name)
{
    service_entry_t *svc = service_find_slot(name);
    pid_t pid;

    if (!svc)
        return -ENOENT;
    svc->manual_stop = 1;
    service_refresh_entry(svc);
    pid = svc->info.pid;
    if (pid <= 0) {
        svc->info.running = 0;
        svc->info.pid = 0;
        return 0;
    }
    if (process_kill(pid) < 0)
        return -ESRCH;
    svc->info.last_exit_code = 137;
    svc->info.running = 0;
    svc->info.pid = 0;
    klog("[service] stopped ");
    klog(svc->info.name);
    klog("\n");
    return 0;
}

long service_syscall_list(long buf_ptr, long max_entries, long total_ptr)
{
    service_info_t infos[SERVICE_MAX];
    size_t total = 0;
    int copied;

    if (max_entries < 0)
        return -EINVAL;
    copied = service_list(infos, (size_t)((uint32_t)max_entries > SERVICE_MAX ? SERVICE_MAX : (uint32_t)max_entries), &total);
    if (copied < 0)
        return copied;
    if (copied > 0 && (!buf_ptr || copy_to_user((void *)buf_ptr, infos, (size_t)copied * sizeof(infos[0])) < 0))
        return -EFAULT;
    if (total_ptr && copy_to_user((void *)total_ptr, &total, sizeof(total)) < 0)
        return -EFAULT;
    return copied;
}

long service_syscall_start(long name_ptr)
{
    char name[SERVICE_NAME_MAX];
    int rc = service_copy_name_from_user(name, name_ptr);

    if (rc < 0)
        return rc;
    return service_start(name);
}

long service_syscall_stop(long name_ptr)
{
    char name[SERVICE_NAME_MAX];
    int rc = service_copy_name_from_user(name, name_ptr);

    if (rc < 0)
        return rc;
    return service_stop(name);
}

long service_syscall_status(long name_ptr, long out_ptr)
{
    char name[SERVICE_NAME_MAX];
    service_info_t info;
    int rc;

    if (!out_ptr)
        return -EFAULT;
    rc = service_copy_name_from_user(name, name_ptr);
    if (rc < 0)
        return rc;
    rc = service_status(name, &info);
    if (rc < 0)
        return rc;
    if (copy_to_user((void *)out_ptr, &info, sizeof(info)) < 0)
        return -EFAULT;
    return 0;
}
