/* VernisOS init (PID 1) — Phase 53.
 * First user-space process: spawns the shell, reaps it when it exits
 * (or is killed) and respawns it. yield() keeps the wait loop from
 * burning scheduler quanta.
 */
#include "syscall.h"
#include "libc.h"

#ifndef SHELL_PATH
#define SHELL_PATH "/bin/vsh64"
#endif

int main(void)
{
    puts("init: VernisOS user-space init (PID 1)");

    for (;;) {
        int pid = fork();
        if (pid == 0) {
            /* child: become the shell */
            execve(SHELL_PATH, 0, 0);
            puts("init: exec " SHELL_PATH " failed");
            _exit(127);
        }
        if (pid < 0) {
            puts("init: fork failed, retrying");
            for (int i = 0; i < 2000; i++)
                yield();
            continue;
        }

        /* reap: waitpid is non-blocking (-1 = still running) */
        for (;;) {
            int code = waitpid(pid);
            if (code >= 0) {
                puts("init: shell exited - respawning");
                break;
            }
            yield();
        }
    }
    return 0;
}
