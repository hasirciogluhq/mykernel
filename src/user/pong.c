#include <kernel/syscall.h>
#include <kernel/string.h>

static void busy_delay(void)
{
    for (volatile unsigned i = 0; i < 20000000; i++)
        ;
}

void user_pong_main(void)
{
    for (;;) {
        const char *msg = "[user:pong] PONG\n";
        sys_write(STDOUT_FILENO, msg, strlen(msg));
        busy_delay();
        sys_yield();
    }
}
