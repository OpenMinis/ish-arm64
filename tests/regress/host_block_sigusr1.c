// Host helper for run.sh: run a command with SIGUSR1 blocked. A signal mask
// survives execve, so iSH then starts the way it does inside an app that
// boots guest processes from a GCD worker thread (Darwin workqueue threads
// block every asynchronous signal), which is how Minis runs them.
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s command [args...]\n", argv[0]);
        return 2;
    }
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);
    execvp(argv[1], argv + 1);
    perror(argv[1]);
    return 127;
}
