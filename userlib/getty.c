/* VernisOS getty — prints the login banner, then hands off to login.
 * No fork/respawn here: init (PID 1) owns the respawn loop, so getty just
 * execs login in place (getty -> login -> shell all share one PID).
 */
#include <stddef.h>
#include "syscall.h"
#include "libc.h"

#ifndef LOGIN_PATH
#define LOGIN_PATH "/bin/login64"
#endif

static const char BANNER[] =
    "\r\n"
    "  VernisOS  --  Secure Microkernel\r\n"
    "  --------------------------------\r\n"
    "\r\n";

int main(void) {
    write(1, BANNER, (int)strlen(BANNER));
    execve(LOGIN_PATH, (char *const[]){ (char *)LOGIN_PATH, (char *)0 },
           (char *const[]){ (char *)0 });
    write(2, "getty: exec login failed\r\n", 26);
    _exit(1);
}
