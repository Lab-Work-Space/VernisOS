/* VernisOS TCP echo server (Phase 65: SYS_LISTEN/SYS_ACCEPT demo).
 * Listens on port 7777, accepts one connection, echoes everything it
 * receives back to the peer, exits when the peer closes. Proves userland
 * can serve TCP via socket file descriptors.
 */
#include "syscall.h"
#include "libc.h"

#define SRV_PORT 7777

int main(void)
{
    puts("tcpsrv: socket()");
    int fd = socket(SOCK_STREAM);
    if (fd < 0) { puts("tcpsrv: socket failed"); return 1; }

    if (bind_port(fd, SRV_PORT) < 0) {
        puts("tcpsrv: bind failed");
        close(fd);
        return 1;
    }
    if (listen(fd) < 0) {
        puts("tcpsrv: listen failed");
        close(fd);
        return 1;
    }
    printf("tcpsrv: listening on port %d\n", SRV_PORT);

    int conn = accept(fd);
    if (conn < 0) {
        puts("tcpsrv: accept timeout");
        close(fd);
        return 1;
    }
    puts("tcpsrv: client connected");

    /* Echo loop: read returns 0 while idle, <0 once the peer closed. */
    char buf[256];
    int idle = 0;
    for (;;) {
        int n = read(conn, buf, sizeof(buf));
        if (n > 0) {
            idle = 0;
            write(conn, buf, n);
            buf[n < 255 ? n : 255] = '\0';
            printf("tcpsrv: echoed %d bytes\n", n);
        } else if (n < 0) {
            puts("tcpsrv: client closed");
            break;
        } else {
            if (++idle > 2000000) { puts("tcpsrv: idle timeout"); break; }
            yield();
        }
    }

    close(conn);
    close(fd);
    puts("tcpsrv: done");
    return 0;
}
