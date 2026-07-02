/* VernisOS login — credential prompt, spawns vsh on success */
#include <stddef.h>
#include "syscall.h"
#include "libc.h"

#define LINE_MAX 128

static int readline(int fd, char *buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c = 0;
        int n = read(fd, &c, 1);
        if (n <= 0) break;
        if (c == '\r' || c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

static int streq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

int main(void) {
    char user[LINE_MAX];
    char pass[LINE_MAX];

    write(1, "login: ", 7);
    readline(0, user, sizeof(user));
    write(1, "\r\n", 2);

    write(1, "password: ", 10);
    readline(0, pass, sizeof(pass));
    write(1, "\r\n", 2);

    /* Default credentials: root / vernis */
    if (streq(user, "root") && streq(pass, "vernis")) {
        write(1, "Login successful.\r\n\r\n", 21);
        execve("/bin/vsh64", (char *const[]){"/bin/vsh64", (char *)0}, (char *const[]){(char *)0});
        write(2, "login: exec shell failed\r\n", 25);
    } else {
        write(1, "Login incorrect.\r\n", 18);
    }
    _exit(1);
}
