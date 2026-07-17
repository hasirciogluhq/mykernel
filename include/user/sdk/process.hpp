#pragma once

#include <kernel/types.h>
#include <kernel/proc_abi.h>
#include <user/sdk/syscall.hpp>

namespace hsrc::sdk::process {

constexpr int kMaxProcesses = PROC_PAGE_MAX;

enum State : uint32_t {
    Unused = 0,
    Ready = 1,
    Running = 2,
    Blocked = 3,
    Zombie = 4,
};

struct ProcListEntry {
    pid_t    pid = 0;
    pid_t    ppid = 0;
    uint32_t state = Unused;
    uint32_t is_user = 0;
    uint64_t cpu_ticks = 0;
    uint64_t uptime_ticks = 0;
    uint32_t mem_bytes = 0;
    char     name[PROC_PAGE_NAME]{};
};

struct ProcStat {
    pid_t    pid = 0;
    pid_t    ppid = 0;
    uint32_t state = Unused;
    uint32_t is_user = 0;
    uint64_t cpu_ticks = 0;
    uint64_t start_ticks = 0;
    uint64_t uptime_ticks = 0;
    uint32_t mem_bytes = 0;
    char     name[PROC_PAGE_NAME]{};
};

struct SysInfo {
    uint64_t uptime_ticks = 0;
    uint64_t total_cpu_ticks = 0;
    uint32_t total_ram_bytes = 0;
    uint32_t used_ram_bytes = 0;
    uint32_t free_ram_bytes = 0;
    uint32_t process_count = 0;
};

enum SpawnFlags : uint32_t {
    ConsoleVisible = SPAWN_CONSOLE_VISIBLE,
    ConsoleHidden  = SPAWN_CONSOLE_HIDDEN,
};

long spawn(const char *path, const char *const *argv = nullptr);
long spawn_ex(const char *path, uint32_t flags, const char *const *argv = nullptr);
bool console_show(pid_t pid, bool visible);
long getargc();
long getargv(int index, char *buf, size_t buflen);
long waitpid(pid_t pid, int *status_out = nullptr, int options = 0);
long kill(pid_t pid);
long getpid();
long getppid();

/* Map shared proc page once (SYS_PROC_MAP). Safe to call repeatedly. */
bool map_proc_page();
/* Hot path: seqlock-read snapshot — no syscall after map. */
bool snapshot(ProcListEntry *entries, int max_entries, int *count_out, SysInfo *info_out);
/* seq changes when kernel republishes; generation bumps on create/exit. */
uint32_t snapshot_seq();
uint32_t snapshot_generation();

/* Syscall fallbacks (also used when page unavailable). */
long proc_list(ProcListEntry *entries, int max_entries);
long proc_stat(pid_t pid, ProcStat *out);
long sysinfo(SysInfo *out);

long getenv(const char *name, char *buf, size_t buflen);
long setenv(const char *name, const char *val, int global = 0);
long unsetenv(const char *name, int global = 0);

const char *state_name(uint32_t state);

[[noreturn]] inline void exit(int code)
{
    hsrc::sdk::exit(code);
}

} // namespace hsrc::sdk::process
