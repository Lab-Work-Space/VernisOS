/* VernisOS netcat-ish TCP client (Phase 52 socket-fd demo).
 * Connects to 10.0.2.2:7000 (QEMU slirp -> host), sends a line, prints the
 * reply. Proves userland can use the network via socket file descriptors.
 */
#include "syscall.h"
#include "libc.h"

#define HOST_IP   ((10u << 24) | (0u << 16) | (2u << 8) | 2u)  /* 10.0.2.2 */
#define HOST_PORT 7000

int main(void)
{
    puts("netcat: socket()");
    int fd = socket(SOCK_STREAM);
    if (fd < 0) { puts("netcat: socket failed"); return 1; }

    puts("netcat: connect 10.0.2.2:7000");
    if (connect(fd, HOST_IP, HOST_PORT) < 0) {
        puts("netcat: connect failed");
        close(fd);
        return 1;
    }
    puts("netcat: connected");

    const char *msg = "hello from vernisos userland\n";
    int wn = write(fd, msg, (int)strlen(msg));
    printf("netcat: sent %d bytes\n", wn);

    /* Poll for the echo reply (recv returns 0 while nothing has arrived). */
    char buf[256];
    int got = 0;
    for (int spins = 0; spins < 200000 && got == 0; spins++) {
        int n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("netcat: recv: %s", buf);
            got = 1;
        } else if (n < 0) {
            puts("netcat: connection closed");
            break;
        } else {
            yield();
        }
    }
    if (!got) puts("netcat: no reply (timeout)");

    close(fd);
    puts("netcat: done");
    return 0;
}
