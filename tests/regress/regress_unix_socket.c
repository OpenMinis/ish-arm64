// socketpair() at RLIMIT_NOFILE fails with EMFILE (and later unix socket
// calls still work); an AF_LOCAL connect() completes before accept().
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
static void on_alarm(int s) { (void) s; printf("HUNG\n"); fflush(stdout); _exit(1); }
int main(void) {
    signal(SIGALRM, on_alarm);
    pid_t c = fork();
    if (c == 0) {
        alarm(10);
        struct rlimit rl = {32, 32};
        setrlimit(RLIMIT_NOFILE, &rl);
        int sv[2], n = 0;
        while (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) n++;
        int e = errno;
        close(sv[0]); close(20); close(21); close(22);
        int ok = socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
        printf("socketpair at the limit: %s after %d, then %s\n", e == EMFILE ? "EMFILE" : strerror(e), n,
               ok ? "works again" : "fails");
        _exit(0);
    }
    int st; waitpid(c, &st, 0);
    alarm(10);
    unlink("/tmp/us.sock");
    int l = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strcpy(a.sun_path, "/tmp/us.sock");
    bind(l, (struct sockaddr *) &a, sizeof a);
    listen(l, 4);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    int r = connect(s, (struct sockaddr *) &a, sizeof a);
    printf("connect before accept: %d\n", r);
    write(s, "hi", 2);
    int x = accept(l, NULL, NULL);
    char buf[4] = {0};
    read(x, buf, 2);
    printf("accept then read: %s\n", buf);
    unlink("/tmp/us.sock");
    return 0;
}
