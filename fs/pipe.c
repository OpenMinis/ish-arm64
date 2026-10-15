#include <sys/stat.h>
#include <unistd.h>
#include "kernel/calls.h"
#include "fs/fd.h"
#include "fs/real.h"
#include "debug.h"

// [T-ish-host-fd-double-close] Owns pipe_fd from here on: on any failure it
// is already closed (f_install() closes the fd it could not install, and with
// it real_fd), so a caller must never close() it again - by then the number
// may belong to another thread of the app (CFNetwork, boringssl, AVAudio).
static fd_t pipe_f_create(int pipe_fd, int flags) {
    struct fd *fd = adhoc_fd_create(&realfs_fdops);
    if (fd == NULL) {
        close(pipe_fd);
        return _ENOMEM;
    }
    fd->real_fd = pipe_fd;
    fd->stat.mode = S_IFIFO | 0660;
    fd->stat.uid = current->uid;
    fd->stat.gid = current->gid;
    return f_install(fd, flags);
}

int_t sys_pipe2(addr_t pipe_addr, int_t flags) {
    STRACE("pipe2(%#x, %#x)", pipe_addr, flags);
    if (flags & ~(O_CLOEXEC_|O_NONBLOCK_)) {
        FIXME("unsupported pipe2 flags");
        return _EINVAL;
    }

    int p[2];
    int err = pipe(p);
    if (err < 0)
        return err;

    // [T-ish-host-fd-double-close] Each host end is closed exactly once:
    // by pipe_f_create() when it fails, by f_close() once it is installed,
    // and here only while it has no guest fd yet.
    int fp[2];
    err = fp[0] = pipe_f_create(p[0], flags);
    if (fp[0] < 0) {
        close(p[1]);
        return err;
    }
    err = fp[1] = pipe_f_create(p[1], flags);
    if (fp[1] < 0)
        goto close_fake_0;

    err = _EFAULT;
    if (user_put(pipe_addr, fp))
        goto close_fake_1;
    STRACE(" [%d %d]", fp[0], fp[1]);
    return 0;

close_fake_1:
    f_close(fp[1]);
close_fake_0:
    f_close(fp[0]);
    return err;
}

int_t sys_pipe(addr_t pipe_addr) {
    return sys_pipe2(pipe_addr, 0);
}
