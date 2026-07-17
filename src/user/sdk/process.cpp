#include <user/sdk/process.hpp>
#include <kernel/argv.h>
#include <user/string.h>

namespace hsrc::sdk::process {
namespace {

const proc_page_t *g_page = nullptr;

bool read_page(proc_page_t *out)
{
    if (!map_proc_page() || !g_page || !out)
        return false;

    uint32_t s1, s2;
    do {
        s1 = g_page->seq;
        __asm__ volatile("" ::: "memory");
        memcpy(out, g_page, sizeof(*out));
        __asm__ volatile("" ::: "memory");
        s2 = g_page->seq;
    } while ((s1 & 1u) || s1 != s2);

    return out->magic == PROC_PAGE_MAGIC;
}

} // namespace

long spawn(const char *path, const char *const *argv)
{
    return spawn_ex(path, SPAWN_CONSOLE_HIDDEN, argv);
}

long spawn_ex(const char *path, uint32_t flags, const char *const *argv)
{
    int argc = 0;
    if (argv) {
        while (argv[argc] && argc < PROC_ARGC_MAX)
            argc++;
    }
    return hsrc::sdk::syscall4(SYS_SPAWN, (long)path, (long)flags, (long)argv, (long)argc);
}

bool console_show(pid_t pid, bool visible)
{
    return hsrc::sdk::syscall2(SYS_CONSOLE_SHOW, (long)pid, visible ? 1L : 0L) == 0;
}

long getargc()
{
    return hsrc::sdk::syscall0(SYS_GETARGC);
}

long getargv(int index, char *buf, size_t buflen)
{
    return hsrc::sdk::syscall3(SYS_GETARGV, (long)index, (long)buf, (long)buflen);
}

long waitpid(pid_t pid, int *status_out, int options)
{
    return hsrc::sdk::syscall3(SYS_WAITPID, (long)pid, (long)status_out, (long)options);
}

long kill(pid_t pid)
{
    return hsrc::sdk::syscall1(SYS_KILL, (long)pid);
}

long getpid()
{
    return hsrc::sdk::syscall0(SYS_GETPID);
}

long getppid()
{
    return hsrc::sdk::syscall0(SYS_GETPPID);
}

bool map_proc_page()
{
    if (g_page && g_page->magic == PROC_PAGE_MAGIC)
        return true;
    long p = hsrc::sdk::syscall0(SYS_PROC_MAP);
    if (p <= 0)
        return false;
    const proc_page_t *pp = (const proc_page_t *)(uintptr_t)p;
    if (!pp || pp->magic != PROC_PAGE_MAGIC)
        return false;
    g_page = pp;
    return true;
}

bool snapshot(ProcListEntry *entries, int max_entries, int *count_out, SysInfo *info_out)
{
    proc_page_t page{};
    if (!read_page(&page))
        return false;

    int n = (int)page.count;
    if (n > max_entries)
        n = max_entries;
    if (n < 0)
        n = 0;

    if (entries && n > 0) {
        for (int i = 0; i < n; i++) {
            entries[i].pid = page.entries[i].pid;
            entries[i].ppid = page.entries[i].ppid;
            entries[i].state = page.entries[i].state;
            entries[i].is_user = page.entries[i].is_user;
            entries[i].cpu_ticks = page.entries[i].cpu_ticks;
            entries[i].uptime_ticks = page.entries[i].uptime_ticks;
            entries[i].mem_bytes = page.entries[i].mem_bytes;
            memcpy(entries[i].name, page.entries[i].name, sizeof(entries[i].name));
            entries[i].name[sizeof(entries[i].name) - 1] = 0;
        }
    }

    if (count_out)
        *count_out = n;
    if (info_out) {
        info_out->uptime_ticks = page.uptime_ticks;
        info_out->total_cpu_ticks = page.total_cpu_ticks;
        info_out->total_ram_bytes = page.total_ram_bytes;
        info_out->used_ram_bytes = page.used_ram_bytes;
        info_out->free_ram_bytes = page.free_ram_bytes;
        info_out->process_count = page.process_count;
    }
    return true;
}

uint32_t snapshot_seq()
{
    if (!map_proc_page() || !g_page)
        return 0;
    return g_page->seq;
}

uint32_t snapshot_generation()
{
    if (!map_proc_page() || !g_page)
        return 0;
    return g_page->generation;
}

long proc_list(ProcListEntry *entries, int max_entries)
{
    int count = 0;
    if (snapshot(entries, max_entries, &count, nullptr))
        return count;
    return hsrc::sdk::syscall2(SYS_PROC_LIST, (long)entries, (long)max_entries);
}

long proc_stat(pid_t pid, ProcStat *out)
{
    return hsrc::sdk::syscall2(SYS_PROC_STAT, (long)pid, (long)out);
}

long sysinfo(SysInfo *out)
{
    if (snapshot(nullptr, 0, nullptr, out))
        return 0;
    return hsrc::sdk::syscall1(SYS_SYSINFO, (long)out);
}

long getenv(const char *name, char *buf, size_t buflen)
{
    return hsrc::sdk::syscall3(SYS_GETENV, (long)name, (long)buf, (long)buflen);
}

long setenv(const char *name, const char *val, int global)
{
    return hsrc::sdk::syscall3(SYS_SETENV, (long)name, (long)val, (long)global);
}

long unsetenv(const char *name, int global)
{
    return hsrc::sdk::syscall3(SYS_SETENV, (long)name, 0, (long)global);
}

const char *state_name(uint32_t state)
{
    switch (state) {
    case Ready:
        return "Ready";
    case Running:
        return "Running";
    case Blocked:
        return "Blocked";
    case Zombie:
        return "Zombie";
    default:
        return "Unused";
    }
}

} // namespace hsrc::sdk::process
