/* VernisOS login — authenticate against /etc/shadow, drop privileges,
 * enter the home directory, then exec the shell. Phase 60 multiuser.
 */
#include <stddef.h>
#include "syscall.h"
#include "libc.h"

#ifndef SHELL_PATH
#define SHELL_PATH "/bin/vsh64"
#endif

#define LINE_MAX 64

static int readline(int fd, char *buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c = 0;
        int n = read(fd, &c, 1);
        if (n < 0) break;      /* fd closed */
        if (n == 0) { yield(); continue; }
        if (c == '\r' || c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

/* Build the home path for a user: /root for uid 0, /home/<name> otherwise. */
static void home_path(int uid, const char *user, char *out) {
    if (uid == 0) {
        strcpy(out, "/root");
        return;
    }
    const char *pre = "/home/";
    int j = 0;
    for (int i = 0; pre[i]; i++) out[j++] = pre[i];
    for (int i = 0; user[i] && j < LINE_MAX - 1; i++) out[j++] = user[i];
    out[j] = '\0';
}

int main(void) {
    char user[LINE_MAX];
    char pass[LINE_MAX];
    char home[LINE_MAX];

    for (;;) {
        write(1, "login: ", 7);
        readline(0, user, sizeof(user));
        write(1, "\r\n", 2);

        write(1, "password: ", 10);
        readline(0, pass, sizeof(pass));
        write(1, "\r\n", 2);

        int uid = auth(user, pass);
        if (uid < 0) {
            write(1, "Login incorrect\r\n\r\n", 19);
            continue;
        }

        /* Create + enter the home directory while still root, then drop
         * privileges so the shell runs as the logged-in user. */
        home_path(uid, user, home);
        mkdir_sys(home);                 /* idempotent-ish; ignore result */
        if (chdir(home) < 0) chdir("/");

        setgid(uid);
        setuid(uid);

        printf("\r\nWelcome %s (uid=%d), home=%s\r\n", user, getuid(), home);
        printf("shell: %s\r\n\r\n", SHELL_PATH);

        execve(SHELL_PATH, (char *const[]){ (char *)SHELL_PATH, (char *)0 },
               (char *const[]){ (char *)0 });
        write(2, "login: exec shell failed\r\n", 26);
        _exit(1);
    }
}
