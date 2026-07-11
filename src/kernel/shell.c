#include <kernel/shell.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/process.h>
#include <kernel/vfs.h>
#include <drivers/vga.h>
#include <user/apps.h>

static void sh_print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

static void sh_print_uint(uint32_t n)
{
    char buf[11];
    int i = 0;
    if (n == 0) {
        sh_print("0");
        return;
    }
    while (n && i < 10) {
        buf[i++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (i > 0) {
        char c = buf[--i];
        sys_write(STDOUT_FILENO, &c, 1);
    }
}

static void cmd_help(void)
{
    sh_print("commands:\n");
    sh_print("  help              show this help\n");
    sh_print("  clear             clear screen\n");
    sh_print("  ps                list processes\n");
    sh_print("  uname             kernel info\n");
    sh_print("  cat <file>        read file (motd, hello.txt)\n");
    sh_print("  run <ping|pong>   start userspace process\n");
    sh_print("  kill <pid>        kill userspace process\n");
    sh_print("  echo <text>       print text\n");
}

static void cmd_clear(void)
{
    vga_clear(VGA_ATTR(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
}

static const char *state_name(proc_state_t s)
{
    switch (s) {
    case PROC_READY:   return "ready";
    case PROC_RUNNING: return "run";
    case PROC_BLOCKED: return "block";
    case PROC_ZOMBIE:  return "zombie";
    default:           return "?";
    }
}

static void cmd_ps(void)
{
    process_t *table = process_table();
    sh_print("  PID  TYPE  STATE   NAME\n");
    for (int i = 0; i < PROC_MAX; i++) {
        if (table[i].state == PROC_UNUSED)
            continue;
        sh_print("  ");
        sh_print_uint((uint32_t)table[i].pid);
        sh_print(table[i].is_user ? "  user  " : "  kern  ");
        sh_print(state_name(table[i].state));
        sh_print("   ");
        sh_print(table[i].name);
        sh_print("\n");
    }
}

static void cmd_uname(void)
{
    sh_print("mykernel i386 cooperative + ring3 userspace\n");
}

static void cmd_cat(const char *path)
{
    char full[64];
    if (path[0] != '/') {
        full[0] = '/';
        strncpy(full + 1, path, sizeof(full) - 2);
    } else {
        strncpy(full, path, sizeof(full) - 1);
    }

    int fd = (int)sys_open(full, O_RDONLY);
    if (fd < 0) {
        sh_print("cat: not found\n");
        return;
    }
    char buf[64];
    for (;;) {
        long n = sys_read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        sys_write(STDOUT_FILENO, buf, (size_t)n);
    }
    sys_close(fd);
}

static void cmd_run(const char *name)
{
    pid_t pid = -1;
    if (strcmp(name, "ping") == 0)
        pid = process_create_user("ping", user_ping_main);
    else if (strcmp(name, "pong") == 0)
        pid = process_create_user("pong", user_pong_main);
    else {
        sh_print("run: unknown app (ping|pong)\n");
        return;
    }

    if (pid < 0) {
        sh_print("run: failed\n");
        return;
    }
    sh_print("started pid ");
    sh_print_uint((uint32_t)pid);
    sh_print(" (userspace)\n");
}

static void cmd_kill(const char *arg)
{
    pid_t pid = 0;
    for (const char *p = arg; *p; p++) {
        if (*p < '0' || *p > '9') {
            sh_print("kill: bad pid\n");
            return;
        }
        pid = pid * 10 + (*p - '0');
    }
    if (process_kill(pid) < 0)
        sh_print("kill: failed\n");
    else
        sh_print("killed\n");
}

static void cmd_echo(const char *text)
{
    if (text) {
        sh_print(text);
        sh_print("\n");
    }
}

static char *skip_ws(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static void handle_line(char *line)
{
    line = skip_ws(line);
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
        line[--n] = '\0';
    if (n == 0)
        return;

    char *arg = line;
    while (*arg && *arg != ' ')
        arg++;
    if (*arg) {
        *arg++ = '\0';
        arg = skip_ws(arg);
    } else {
        arg = NULL;
    }

    if (strcmp(line, "help") == 0)
        cmd_help();
    else if (strcmp(line, "clear") == 0)
        cmd_clear();
    else if (strcmp(line, "ps") == 0)
        cmd_ps();
    else if (strcmp(line, "uname") == 0)
        cmd_uname();
    else if (strcmp(line, "cat") == 0) {
        if (!arg || !*arg)
            sh_print("cat: missing file\n");
        else
            cmd_cat(arg);
    } else if (strcmp(line, "run") == 0) {
        if (!arg || !*arg)
            sh_print("run: missing name\n");
        else
            cmd_run(arg);
    } else if (strcmp(line, "kill") == 0) {
        if (!arg || !*arg)
            sh_print("kill: missing pid\n");
        else
            cmd_kill(arg);
    } else if (strcmp(line, "echo") == 0)
        cmd_echo(arg ? arg : "");
    else {
        sh_print("unknown: ");
        sh_print(line);
        sh_print(" (try help)\n");
    }
}

void shell_main(void)
{
    char line[128];

    sh_print("\nmykernel console — type 'help'\n");

    for (;;) {
        sh_print("mykernel$ ");
        long n = sys_read(STDIN_FILENO, line, sizeof(line) - 1);
        if (n <= 0) {
            sys_yield();
            continue;
        }
        line[n] = '\0';
        handle_line(line);
    }
}
