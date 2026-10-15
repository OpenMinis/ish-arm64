// A private file mapping made read-only and then writable with mprotect: the
// writes are the mapping's own, not the file's or another mapping's. On
// Windows such a mapping is a read view and the kernel copies a page on its
// first write.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    const char *path = "/tmp/filemap.dat";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    char buf[3 * 4096];
    for (unsigned i = 0; i < sizeof(buf); i++)
        buf[i] = 'a' + i % 26;
    write(fd, buf, sizeof(buf));

    int bad = 0;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s\n", what); bad++; } } while (0)
    char *a = mmap(NULL, sizeof(buf), PROT_READ, MAP_PRIVATE, fd, 0);
    char *b = mmap(NULL, sizeof(buf), PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(a != MAP_FAILED && b != MAP_FAILED, "mmap");
    CHECK(memcmp(a, buf, sizeof(buf)) == 0, "read-only mapping reads the file");
    CHECK(mprotect(a + 4096, 4096, PROT_READ | PROT_WRITE) == 0, "mprotect to writable");
    a[4096 + 10] = '!';
    CHECK(a[4096 + 10] == '!', "write reads back");
    CHECK(a[4096 + 11] == buf[4096 + 11], "rest of the page kept");
    CHECK(memcmp(a, buf, 4096) == 0 && memcmp(a + 8192, buf + 8192, 4096) == 0, "other pages untouched");
    CHECK(b[4096 + 10] == buf[4096 + 10], "another mapping does not see it");
    char now[sizeof(buf)];
    pread(fd, now, sizeof(now), 0);
    CHECK(memcmp(now, buf, sizeof(buf)) == 0, "the file does not see it");

    pid_t pid = fork();
    if (pid == 0) {
        a[4096 + 20] = '?';
        _exit(a[4096 + 10] == '!' && a[4096 + 20] == '?' ? 0 : 1);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child sees and writes its copy");
    CHECK(a[4096 + 20] == buf[4096 + 20], "parent does not see the child's write");

    mprotect(a + 4096, 4096, PROT_READ);
    CHECK(a[4096 + 10] == '!', "made read-only again, keeps its data");
    munmap(a, sizeof(buf));
    munmap(b, sizeof(buf));
    close(fd);
    unlink(path);
    printf("%d wrong\n", bad);
    return bad != 0;
}
