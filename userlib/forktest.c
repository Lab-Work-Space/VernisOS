/* VernisOS CoW fork test (Phase 69).
 * Parent fills a global buffer + a stack buffer with patterns and forks.
 * The child overwrites both (forcing CoW copies), verifies its own view,
 * and exits with a code encoding success. The parent waits, then checks
 * its buffers are untouched — with shared frames and no CoW, the child's
 * writes would leak into the parent and this fails loudly.
 */
#include "syscall.h"
#include "libc.h"

#define BUF_SZ 2048
static unsigned char g_buf[BUF_SZ];

static int check(const unsigned char *p, int n, unsigned char v) {
    for (int i = 0; i < n; i++)
        if (p[i] != v) return 0;
    return 1;
}

int main(void)
{
    unsigned char s_buf[256];
    for (int i = 0; i < BUF_SZ; i++) g_buf[i] = 0xAA;
    for (int i = 0; i < (int)sizeof(s_buf); i++) s_buf[i] = 0xBB;

    puts("forktest: forking");
    int pid = fork();
    if (pid < 0) { puts("forktest: fork failed"); return 1; }

    if (pid == 0) {
        /* Child: overwrite both buffers, verify own view, exit 42. */
        for (int i = 0; i < BUF_SZ; i++) g_buf[i] = 0x55;
        for (int i = 0; i < (int)sizeof(s_buf); i++) s_buf[i] = 0x66;
        if (!check(g_buf, BUF_SZ, 0x55) || !check(s_buf, sizeof(s_buf), 0x66)) {
            puts("forktest: CHILD VIEW BROKEN");
            return 2;
        }
        puts("forktest: child wrote its copy");
        return 42;
    }

    /* Parent: wait for the child (non-blocking waitpid + yield). */
    int code = -1;
    for (int spins = 0; spins < 2000000; spins++) {
        code = waitpid(pid);
        if (code >= 0) break;
        yield();
    }
    if (code != 42) {
        printf("forktest: BAD CHILD EXIT code=%d\n", code);
        return 3;
    }
    if (!check(g_buf, BUF_SZ, 0xAA)) {
        puts("forktest: PARENT GLOBAL CORRUPTED (CoW broken)");
        return 4;
    }
    if (!check(s_buf, sizeof(s_buf), 0xBB)) {
        puts("forktest: PARENT STACK CORRUPTED (CoW broken)");
        return 5;
    }
    puts("forktest: PASS (parent memory isolated)");
    return 0;
}
