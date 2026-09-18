// [T-ish-openat-inode-early-ref] A successful open() must yield a working fd
// even when another process is unlinking and recreating that very path
// concurrently: fstat() on it must succeed (Linux keeps the inode alive; fakefs
// must keep its metadata row until the last close). This is the interleaving
// the 2019 inodes_lock fix (d57b6d26) guards, re-run at high rate.
//
// Build for the guest (static): aarch64-linux-musl-gcc -static -O0 -o race regress_open_unlink_race.c
// Run inside iSH:              ish -f <rootfs> /bin/sh -c 'cat >/tmp/race; chmod +x /tmp/race; /tmp/race' < race
// Exit 0 when every worker saw only clean opens; 1 otherwise.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define PATH "/tmp/open_unlink_race"
#define WORKERS 8
#define ITERS 3000

static int worker(void) {
    int bad = 0, opened = 0;
    for (int i = 0; i < ITERS; i++) {
        int fd = open(PATH, O_RDONLY);
        if (fd < 0) {
            if (errno == ENOENT) continue;   // raced the unlink: legitimate
            printf("worker: open failed: %s\n", strerror(errno)); bad++; continue;
        }
        opened++;
        struct stat st;
        if (fstat(fd, &st) < 0) { printf("DOOMED FD: fstat after successful open: %s\n", strerror(errno)); bad++; }
        char b;
        if (read(fd, &b, 1) < 0) { printf("worker: read on open fd: %s\n", strerror(errno)); bad++; }
        close(fd);
    }
    if (opened == 0) { printf("worker: never opened the file (test not exercised)\n"); return 1; }
    return bad;
}

int main(void) {
    int fd = open(PATH, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { perror("create"); return 1; }
    write(fd, "x", 1); close(fd);
    pid_t pids[WORKERS];
    for (int w = 0; w < WORKERS; w++) {
        pids[w] = fork();
        if (pids[w] == 0) _exit(worker() ? 1 : 0);
    }
    for (int i = 0; i < ITERS; i++) {   // churn: the deletion side of the race
        unlink(PATH);
        int f = open(PATH, O_CREAT | O_WRONLY, 0644);
        if (f >= 0) { write(f, "y", 1); close(f); }
    }
    int fails = 0;
    for (int w = 0; w < WORKERS; w++) {
        int st; waitpid(pids[w], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) fails++;
    }
    unlink(PATH);
    printf("open_unlink_race: %d/%d workers clean\n", WORKERS - fails, WORKERS);
    return fails ? 1 : 0;
}
