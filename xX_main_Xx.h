#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <syslog.h>
#include <sys/stat.h>
#include "kernel/init.h"
#include "kernel/fs.h"
#include "fs/devices.h"
#include "fs/dev.h"
#include "fs/path.h"
#include "fs/real.h"
#include "kernel/native_offload.h"
#include "kernel/calls.h"
#ifdef __APPLE__
#include <sys/resource.h>
#define IOPOL_TYPE_VFS_HFS_CASE_SENSITIVITY 1
#define IOPOL_VFS_HFS_CASE_SENSITIVITY_FORCE_CASE_SENSITIVE 1
#endif

void real_tty_reset_term(void);

static void exit_handler(struct task *task, int code) {
    if (task->parent != NULL)
        return;
    real_tty_reset_term();
    if (code & 0xff) {
        // Guest died from a signal. Don't raise on the host (our crash_handler
        // would intercept it). Just exit with the conventional 128+signal code.
        _exit(128 + (code & 0xff));
    }
    exit(code >> 8);
}

// this function parses command line arguments and initializes global
// data structures. thanks programming discussions discord server for the name.
// https://discord.gg/9zT7NHP
static inline int xX_main_Xx(int argc, char *const argv[], const char *envp) {
#ifdef __APPLE__
    // Enable case-sensitive filesystem mode on macOS, if possible.
    // In order for this to succeed, either we need to be running as root, or
    // be given the com.apple.private.iopol.case_sensitivity entitlement. The
    // second option isn't possible so you'll need to give iSH the setuid root
    // bit. In that case it's important to drop root permissions ASAP.
    // https://worthdoingbadly.com/casesensitive-iossim/
    int iopol_err = setiopolicy_np(IOPOL_TYPE_VFS_HFS_CASE_SENSITIVITY,
            IOPOL_SCOPE_PROCESS,
            IOPOL_VFS_HFS_CASE_SENSITIVITY_FORCE_CASE_SENSITIVE);
    if (iopol_err != 0 && errno != EPERM)
        perror("could not enable case sensitivity");
    setgid(getgid());
    setuid(getuid());
#endif

    // parse cli options
    int opt;
    const char *root = NULL;
    const char *workdir = NULL;
    const struct fs_ops *fs = &realfs;
    const char *console = "/dev/tty1";
    while ((opt = getopt(argc, argv, "+r:f:d:c:n:")) != -1) {
        switch (opt) {
            case 'r':
            case 'f':
                root = optarg;
                if (opt == 'f')
                    fs = &fakefs;
                break;
            case 'd':
                workdir = optarg;
                break;
            case 'c':
                console = optarg;
                break;
            case 'n':
                if (native_offload_add(optarg) < 0)
                    fprintf(stderr, "warning: ignoring -n %s\n", optarg);
                break;
        }
    }

    openlog(argv[0], 0, LOG_USER);

    char root_realpath[MAX_PATH + 1] = "/";
    if (root != NULL && realpath(root, root_realpath) == NULL) {
        perror(root);
        exit(1);
    }
    if (fs == &fakefs)
        strcat(root_realpath, "/data");
    int err = mount_root(fs, root_realpath);
    if (err < 0)
        return err;

    become_first_process();
    current->thread = pthread_self();

    // Create essential device nodes (only works with fakefs)
    if (fs != &realfs) {
        generic_mknodat(AT_PWD, "/dev/null", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_NULL_MINOR));
        generic_mknodat(AT_PWD, "/dev/zero", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_ZERO_MINOR));
        generic_mknodat(AT_PWD, "/dev/full", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_FULL_MINOR));
        generic_mknodat(AT_PWD, "/dev/random", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_RANDOM_MINOR));
        generic_mknodat(AT_PWD, "/dev/urandom", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_URANDOM_MINOR));
        generic_mknodat(AT_PWD, "/dev/tty", S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_TTY_MINOR));
        generic_mknodat(AT_PWD, "/dev/console", S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_CONSOLE_MINOR));
        generic_mknodat(AT_PWD, "/dev/ptmx", S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR));
    }

    char cwd[MAX_PATH + 1];
    if (root == NULL && workdir == NULL) {
        getcwd(cwd, sizeof(cwd));
        workdir = cwd;
    }
    if (workdir != NULL) {
        struct fd *pwd = generic_open(workdir, O_RDONLY_, 0);
        if (IS_ERR(pwd)) {
            fprintf(stderr, "error opening working dir: %ld\n", PTR_ERR(pwd));
            return 1;
        }
        fs_chdir(current->fs, pwd);
    }

    if (argv[optind] == NULL)
	    return _ENOENT;
    // [ish#42] The guest argv, NUL-separated, sized from the real arguments.
    // It used to be a fixed 4096-byte stack buffer filled with unchecked
    // strcpy, so a command line of ~16 KB (a long `sh -c` script) overran it,
    // smashed main's frame and crashed the host. Like the kernel's execve,
    // the arguments themselves (every string's NUL plus the final one) may
    // take at most ARGV_MAX bytes; past that the exec fails with E2BIG.
    size_t user_bytes = 1;
    for (int k = optind; k < argc; k++)
        user_bytes += strlen(argv[k]) + 1;
    if (user_bytes > ARGV_MAX) {
        fprintf(stderr, "%s: argument list too long (%zu bytes, limit %d)\n",
                argv[optind], user_bytes, (int) ARGV_MAX);
        return _E2BIG;
    }
    size_t argv_bytes = user_bytes;
#ifdef GUEST_ARM64
    // Inject V8 flags for node to work around scope corruption in emulation.
    // --jitless: disable JIT (avoids V8 code gen incompatible with our JIT)
    // --predictable: disable concurrent GC/compilation (avoids race conditions)
    // --no-lazy: eager compilation (avoids Zone reuse patterns that corrupt scopes)
    // --single-generation: skip young gen (reduces GC-triggered Zone resets)
    // This is needed here because the initial do_execve bypasses sys_execve.
    static const char *v8_flags[] = {
        "--jitless",
        "--no-lazy",
        "--no-expose-wasm",
        "--max-old-space-size=512",
        // Force single-threaded V8: in jitless mode worker threads
        // have no JIT to do, but V8 still creates them and they
        // block on futex/epoll waiting for the slow main thread.
        // Cuts `node -e 0` from 2.3s → 0.65s on iSH ARM64.
        // Validated 2026-05-05 via syscall profile.
        "--no-concurrent-marking",
        "--no-concurrent-recompilation",
        "--no-lazy-compile-dispatcher",
        // --predictable forces V8 into deterministic single-threaded
        // mode: disables concurrent GC, parallel scavenger, and
        // background compilation. Empirically yields the biggest
        // single speedup on iSH ARM64 (reduces wall time another
        // 30-40% on top of the no-concurrent-* flags above).
        "--predictable",
    };
    const size_t v8_flag_count = sizeof(v8_flags) / sizeof(v8_flags[0]);
    bool inject_v8 = false;
    {
        const char *base = strrchr(argv[optind], '/');
        base = base ? base + 1 : argv[optind];
        if (strcmp(base, "node") == 0) {
            size_t flag_bytes = 0;
            for (size_t fi = 0; fi < v8_flag_count; fi++)
                flag_bytes += strlen(v8_flags[fi]) + 1;
            // Like sys_execve's injection: only when it still fits.
            if (argv_bytes + flag_bytes <= ARGV_MAX) {
                inject_v8 = true;
                argv_bytes += flag_bytes;
            }
        }
    }
#endif
    char *argv_copy = malloc(argv_bytes);
    if (argv_copy == NULL)
        return _ENOMEM;
    size_t p = 0;
    size_t exec_argc = 0;
    for (int k = optind; k < argc; k++) {
        size_t len = strlen(argv[k]) + 1;
        memcpy(&argv_copy[p], argv[k], len);
        p += len;
        exec_argc++;
#ifdef GUEST_ARM64
        // The flags go right after argv[0].
        if (k == optind && inject_v8) {
            for (size_t fi = 0; fi < v8_flag_count; fi++) {
                size_t flen = strlen(v8_flags[fi]) + 1;
                memcpy(&argv_copy[p], v8_flags[fi], flen);
                p += flen;
                exec_argc++;
            }
        }
#endif
    }
    argv_copy[p] = '\0';
#ifdef GUEST_ARM64
    // Inject environment variables for the initial exec.
    // sys_execve has its own injection, but the initial do_execve bypasses it.
    {
        static char envp_buf[4096];
        size_t ep = 0;
        if (envp != NULL) {
            const char *e = envp;
            while (*e) {
                size_t len = strlen(e) + 1;
                if (ep + len < sizeof(envp_buf) - 256) {
                    memcpy(&envp_buf[ep], e, len);
                    ep += len;
                }
                e += len;
            }
        }
        // Inject env vars that sys_execve would normally add.
        // These are injected only if not already present.
        static const char *inject_vars[] = {
            "GODEBUG=asyncpreemptoff=1",  // Disable Go async preemption
            "GOMAXPROCS=2",               // Limit Go thread count
            "NO_COLOR=1",                 // Disable color output
            "PYTHONMALLOC=malloc",        // Bypass pymalloc arenas
            "PYTHONDONTWRITEBYTECODE=1",  // Skip .pyc generation
            // Reduce libuv worker pool from 4→1: short scripts never use them
            // but pay the futex sync cost. Saves ~1s on `node -e 0`.
            "UV_THREADPOOL_SIZE=1",
        };
        for (size_t vi = 0; vi < sizeof(inject_vars)/sizeof(inject_vars[0]); vi++) {
            // Check if already present (search by prefix up to '=')
            const char *eq = strchr(inject_vars[vi], '=');
            size_t prefix_len = eq ? (size_t)(eq - inject_vars[vi] + 1) : strlen(inject_vars[vi]);
            int found = 0;
            const char *scan = envp_buf;
            const char *scan_end = envp_buf + ep;
            while (scan < scan_end && *scan) {
                if (strncmp(scan, inject_vars[vi], prefix_len) == 0) {
                    found = 1;
                    break;
                }
                scan += strlen(scan) + 1;
            }
            if (!found) {
                size_t vlen = strlen(inject_vars[vi]) + 1;
                if (ep + vlen < sizeof(envp_buf) - 64) {
                    memcpy(&envp_buf[ep], inject_vars[vi], vlen);
                    ep += vlen;
                }
            }
        }
        // Node-specific: LD_PRELOAD for zero_free.so
        const char *base2 = strrchr(argv[optind], '/');
        base2 = base2 ? base2 + 1 : argv[optind];
        if (strcmp(base2, "node") == 0) {
            static const char *ld_preload = "LD_PRELOAD=/lib/zero_free.so";
            size_t plen = strlen(ld_preload) + 1;
            if (ep + plen < sizeof(envp_buf) - 2) {
                memcpy(&envp_buf[ep], ld_preload, plen);
                ep += plen;
            }
        }
        envp_buf[ep] = '\0';
        err = do_execve(argv[optind], exec_argc, argv_copy, envp_buf);
    }
#else
    err = do_execve(argv[optind], exec_argc, argv_copy, envp == NULL ? "\0" : envp);
#endif
    free(argv_copy); // do_execve copied it into the guest
    if (err < 0)
        return err;
    tty_drivers[TTY_CONSOLE_MAJOR] = &real_tty_driver;
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        err = create_stdio(console, TTY_CONSOLE_MAJOR, 1);
        if (err < 0)
            return err;
    } else {
        err = create_piped_stdio();
        if (err < 0)
            return err;
    }
    exit_hook = exit_handler;
    return 0;
}
