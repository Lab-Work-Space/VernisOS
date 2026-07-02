/* VernisOS getty — terminal line discipline, spawns login */
#include <stddef.h>
#include "syscall.h"
#include "libc.h"

static const char BANNER[] =
    "\r\n"
    "  VernisOS  --  Secure Microkernel\r\n"
    "  --------------------------------\r\n"
    "\r\n";

int main(void) {
    write(1, BANNER, strlen(BANNER));

    while (1) {
        int pid = fork();
        if (pid == 0) {
            execve("/bin/login", (char *const[]){"/bin/login", (char *)0}, (char *const[]){(char *)0});
            write(2, "getty: exec login failed\r\n", 25);
            _exit(1);
        }
        if (pid > 0) {
            waitpid(pid);
        }
    }
}
