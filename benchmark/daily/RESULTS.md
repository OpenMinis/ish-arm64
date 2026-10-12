# Daily workloads: results

Windows x86_64 ish (xlat JIT) on the test machine (i5-13600K VM, 4 vCPU, 4 GB RAM, shared:
Minis builds and Defender run on it too, so expect ±20% between runs; compare builds only
inside one interleaved `run.py` call).

How to run (MSYS2 bash on the test machine):

```sh
benchmark/daily/mkrootfs.sh                        # once: C:/src/rootfs/daily-x86_64
/clang64/bin/python3 benchmark/daily/run.py --ish new=build-x86_64-jit/ish.exe \
    --ish base=C:/src/ish-daily0.exe --rootfs C:/src/rootfs/daily-x86_64 --reps 3
```

Every run is a fresh `ish.exe` (as Minis runs each command); `cpu` is the ish process's
user+kernel time over all threads. A case whose wall time is well above its cpu time is waiting
(Defender scanning files on open, network), not computing.

## Baseline (2026-10-12, 1418c8c0, median of 3 after 1 warmup)

| case | wall (s) | cpu (s) | notes |
|---|---:|---:|---|
| boot.true (ish start + /bin/true) | 0.067 | 0.03 | |
| boot.sh (`echo hi`) | 0.040 | 0.03 | |
| proc.forkexec (200 × /bin/true) | 1.20 | 1.09 | 6 ms per fork+exec |
| proc.pipe (256 MB dd \| cat \| wc) | **14.7** | 14.9 | ~17 MB/s, all CPU |
| proc.py_subproc (40 × subprocess.run) | 0.94 | 0.95 | |
| py.start | 0.31 | 0.28 | |
| py.import (json re os sys subprocess urllib ssl sqlite3 asyncio) | 1.48 | 1.44 | |
| py.loop | 5.36 | 5.08 | |
| py.strops | 0.84 | 0.83 | |
| py.script (JSON + regex over 12 MB log) | 4.53 | 4.41 | |
| py.pip_list | 3.73 | 3.45 | |
| py.pip_install (6 local wheels) | **14.5** | 12.6 | |
| py.venv | 1.10 | 1.00 | |
| node.start | 0.88 | 0.64 | |
| node.json | 1.72 | 1.61 | |
| node.fs | 2.51 | 2.05 | |
| node.npm_install (4 local tarballs) | **56.2** | 43.1 | |
| sh.ls_r (python3.12 tree) | 1.16 | 1.02 | |
| sh.find (/usr) | 4.42 | 3.92 | |
| sh.grep_r (python3.12 tree, 214 MB) | **28.8** | 18.2 | waits ~10 s |
| sh.sort_uniq (200k lines) | 1.54 | 1.64 | |
| sh.sed_awk (12 MB log) | 2.98 | 4.34 | |
| sh.tar_gz | 0.24 | 0.28 | |
| sh.untar | 0.64 | 0.61 | |
| sh.gzip (7 MB) | 0.63 | 0.78 | |
| sh.unzip | 0.27 | 0.25 | |
| fs.smallfiles (1000 creates + rm -rf) | 1.12 | 0.95 | |
| fs.stat (ls -lR) | 0.85 | 0.86 | |
| git.status | 0.19 | 0.14 | |
| git.clone (local, 31 commits) | 2.44 | 1.47 | |
| build.gcc (hello.c) | 1.02 | 0.66 | crashed ish before d5e234b8 |
| build.make (7 files) | 4.46 | 4.33 | |
| build.go | — | — | fails: `go build` hangs in futex waits (also on macOS); open |
| net.dns | 0.22 | 0.05 | |
| net.curl_tls | 1.39 | 0.48 | |
| net.py_https | 1.74 | 0.83 | |
| net.apk_add (zip, no cache) | 5.32 | 3.62 | |

All outputs match across runs (except build.go).

## Optimizations

### 1. Hashed TLB index (x86_64 guest)

The direct-mapped TLB indexed by `addr >> 12` alone: pages 32 MB apart share a slot. busybox
`wc -c` reading stdin touches its stack (0xffffd000) and its data (0xefffd000, 256 MB lower) for
every byte, so both missed every time: 26 M misses for 13 MB of input. The index is hashed again
(`(addr >> 12) ^ (addr >> 25)`, as for the other guests); the translated code's fast path checks
its per-site page cache and never computes the index, so the hash costs nothing there.

Windows, interleaved, median of 3 (base = 1418c8c0 build):

| case | before (s) | after (s) | after/before |
|---|---:|---:|---:|
| proc.pipe (256 MB through busybox wc) | 12.57 | 2.68 | 0.23 |
| sh.grep_r | 26.91 | 14.89 | 0.69 |
| sh.find | 4.32 | 3.21 | 0.74 |
| git.status | 0.21 | 0.11 | 0.42 |
| git.clone | 1.95 | 1.26 | 0.62 |
| build.gcc | 0.98 | 0.60 | 0.61 |
| fs.stat | 0.83 | 0.70 | 0.84 |
| py.pip_list | 3.79 | 3.38 | 0.89 |
| node.npm_install | 31.67 | 31.61 | 1.00 |
| geomean, all 32 non-net cases | | | **0.861** |

Outputs identical for every case. Difftest 19696/19696 (default, ISH_XLAT_HOT=1,
ISH_XLAT_NOGROUP=1, gadget build), agent 18/18. The macOS compat run had 4 timeouts (ruby, yt-dlp,
pandas, meitu-cli) under load average 7.5; old and new builds time the same on them.

### 2. Futex waits on an event (Windows)

A pipe on Windows is two loopback sockets: every guest futex wait was a WSAPoll, every wake a
send, every thread's first wait a new socket pair (WSAPoll 23% and socket-pair creation 5% of
ish's CPU in npm install). Tasks now wait on an auto-reset event together with the thread's
interrupt event.

| case | before (s) | after (s) | cpu before | cpu after |
|---|---:|---:|---:|---:|
| node.npm_install | 32.57 | 33.18 | 38.02 | 33.95 |
| py.pip_install | 10.22 | 10.44 | 9.31 | 9.98 |
| node.json | 1.37 | 1.35 | 1.36 | 1.25 |
| geomean of 8 threaded cases | | 0.980 | | |

CPU down, wall unchanged: npm's wall time is not in futex waits (see next steps).

### 3. stat without a handle (Windows 11 24H2+)

A guest stat of an existing file took 74 us, ~55 us of it the host stat: CreateFile, a query and
CloseHandle, each a trip through the filter drivers. `GetFileInformationByName
(FileStatBasicByNameInfo)` answers from the path in one call, volume serial included (stat and
fstat must agree on st_dev). Reparse points and errors other than "not found" still go through a
handle; `ISH_STAT_BY_HANDLE=1` forces the old path. The two paths stat 14226 files under /usr,
/etc, /tmp/daily and a bind mount identically. Existing-file stat: 45.5 us -> 20.4 us.

| case | before (s) | after (s) | after/before |
|---|---:|---:|---:|
| sh.find (/usr) | 5.33 | 2.25 | 0.42 |
| sh.ls_r | 1.04 | 0.60 | 0.59 |
| fs.stat (ls -lR) | 0.87 | 0.54 | 0.62 |
| node.start | 0.94 | 0.75 | 0.66 |
| py.import | 1.66 | 1.43 | 0.82 |
| py.pip_list | 3.50 | 3.20 | 0.94 |
| py.pip_install | 14.56 | 13.68 | 0.94 |
| geomean of 21 cases | | | **0.864** |

### 4. File open and the fakefs metadata lookup (measured, mostly left alone)

Host primitives on the test machine (native loop): CreateFile(read)+CloseHandle 11.9 us,
CreateFile(attributes)+CloseHandle 8.0 us, GetFileInformationByHandle 2.2 us,
GetFileInformationByHandleEx(FileBasicInfo) 0.57 us, GetFileInformationByName 3.6 us, ReadFile
256 B 1.25 us. In the guest (after 3): stat 14 us, open+close 27 us, fstat 10 us, independent of
path depth.

- px_openat only needs the attributes after CreateFile: FileBasicInfo instead of
  GetFileInformationByHandle, ~1 us per open (within noise in the guest loop).
- The fakefs meta.db lookup per stat costs ~1-2 us: a profile of 300k stats of a missing file
  (5.7 us each) is half the Python loop itself and 13% sqlite's WAL shared-memory locks
  (LockFile/UnlockFile per read transaction). A metadata cache would have to stay coherent with
  other ish processes writing the same meta.db; not worth ~1 us.
- The rest of an open is CreateFile+CloseHandle itself (~12 us), which an open cannot avoid.

### 5. Python interpreter loop (py.loop): tried, no gain, not committed

Profile of py.loop (2547 samples): 87% translated code; of that guest ALU/branches 45%, per-site
page checks 22%, guest memory accesses 11%, r8-r15 slot traffic 8%, flag syncs 5%, cold/TLB
lookups 4%, indirect dispatch 1.6%; gadget fallbacks negligible (77k native instructions, 943
template). Tried: let a check group span accesses with other bases, the members after them
rechecking with one `bt $63` on the lead's site page (a TLB flush empties every site). Correct
(difftest also with every recheck forced to its slow path) but no gain: geomean 0.999 over 16
cases, py.loop 2.91 s -> 2.93 s. The page checks overlap with the guest's own work in the
out-of-order core; fewer of them does not shorten the critical path. A first, unrepeated A/B had
shown -15..-24%: machine noise.

### 6. SSE moves and integer/logic operations translated natively

The translator left every SSE instruction to the gadgets, and their 128-bit helpers are C calls
with all registers saved: in grep -r 5.5% of the samples were in vec_* helpers alone. gcc -O2
code is full of a few of them (zeroing a struct: pxor + movups; two fields at once: movq +
punpcklqdq + movups; copies: movdqu). Now native: movups/movaps/movdqu/movdqa/movss/movsd/
movlps/movhps/movq/movd loads and stores, pmovmskb, pshufd/hw/lw, and the integer and logic
operations of 66 0f (no flags, no MXCSR). Guest xmm registers go through host xmm0/xmm1 (the only
ones free on Windows, where xmm6-15 are callee-saved); aligned moves become unaligned ones and a
16-byte memory operand of an operation is loaded with movdqu first, so the host never faults on
alignment. `ISH_XLAT_NOSSE=1` turns it off. tests/xlat/sse.c (every form, register and unaligned
memory, xmm8-15, r8-r15, page crossing) prints the same 780 lines as the gadget build on macOS
and Windows.

| case | before (s) | after (s) | after/before |
|---|---:|---:|---:|
| sh.grep_r | 12.91 | 10.95 | 0.83 |
| py.loop | 3.03 | 2.50 | 0.86 |
| sh.untar | 0.60 | 0.49 | 0.89 |
| sh.sed_awk | 2.09 | 1.88 | 0.90 |
| py.script | 3.07 | 2.85 | 0.93 |
| node.start | 0.46 | 0.45 | 0.92 |
| build.make | 3.74 | 3.55 | 0.96 |
| geomean of 31 cases | | | **0.977** |

### 7. SSE floating point translated natively

How ish treats the guest's MXCSR: it is stored (ldmxcsr/stmxcsr, signal frames) and its rounding
control applies only to the rounding conversions (cvtsd2si/cvtss2si/roundsd); arithmetic always
rounds to nearest and the exception flags are never set. The host's own instructions under the
host's default MXCSR do exactly that, so add/sub/mul/div/min/max/sqrt (ss/sd/ps/pd), cvtss2sd/
cvtsd2ss, cvtsi2sd/ss and cvttsd2si/cvttss2si now run natively; cvtsd2si/cvtss2si stay with the
gadgets. min/max in the gadgets now follow x86 for +-0 (the second operand), like the hardware.

Checked against the hardware: tests/xlat/sse_fp.c (NaN, inf, +-0, denormals, inexact values,
scalar and packed, xmm8-15, r8-r15, unaligned scalar operands; packed operands aligned, as x86
requires) built natively on the Windows test machine and as a guest program:

| test (lines) | JIT vs hardware | gadgets vs hardware |
|---|---:|---:|
| sse_fp.c, round to nearest (5856) | 0 | 64 (NaN choice in addps/mulps/addpd/mulpd) |
| sse_fp.c, the three other rounding modes | differ wherever inexact: ish ignores the mode, as before | same |
| sse.c (780) | 0 | 0 |
| fp_nan.c (12) | 0 | 0 |

The 64 gadget lines are an old inaccuracy: the C helpers (`*dst += *src`) let the compiler swap
the operands, which picks the other NaN when both are NaN. The translated code is right there.

Windows, interleaved, median of 3 (base: the NaN fix build): node.start 0.50 -> 0.47, node.fs 1.73
-> 1.48, build.gcc 0.49 -> 0.47, py.loop 2.47 -> 2.37; geomean of 18 cases 0.970, outputs
identical.

### 8. Where npm install's time goes (ISH_SYSCALL_TIMES)

PX_PROF samples program counters and cannot tell a thread that computes from one that sits in a
kernel wait. `ISH_SYSCALL_TIMES=<file>` (kernel/sctimes.c) measures instead: per guest thread
and syscall the calls, wall time and host-thread CPU time, plus spans inside them (fakefs mutex,
sqlite BEGIN/COMMIT, the host's open, waits for mem->lock as a writer). `sctimes.py <file>` sums
it up; the main threads (tid == pid) are the critical path. Off: one load and a branch per call
site (geomean 1.02 over 6 cases at 9 repetitions, inside the noise).

npm install of 4 local tarballs, main thread, quiet machine (three runs, 33-38 s wall):

| where | seconds | |
|---|---:|---|
| guest code (node/V8, translated) | 13-15 | |
| ish's own CPU inside syscalls | 8-9 | epoll_pwait 3.9-4.5 (5200 calls, 0.8 ms each), futex 1.5-1.9, statx 1.0, open 1.0 |
| waiting for mem->lock as a writer | 1.9-3.5 | 370-440 contended munmap/mmap, ~6 ms each |
| waiting in epoll_pwait/futex for workers | 2-3 | |
| the host's open (CreateFile) | 0.3-7.6 | 1585 opens; 4.8 ms each in one run, 0.2 ms in the others: Defender |
| off-cpu outside syscalls | 2-4 | |

Not the bottleneck: the fakefs mutex and sqlite (0.2 s on the main thread).

Tried and reverted: making the contended write lock cheap (yield-and-retry before the sleep ramp,
readers giving way for up to 200 us). The writers' waits fell from 9.5-14.4 s to 2.2-3.3 s over
all threads, but the wall time did not move (33-38 s before, 35-42 s after): the main thread
spent the difference off-cpu, giving way and waiting out the munmaps. Its CPU is the critical
path.

Proposed next: epoll_pwait. With no epoll on Windows every wait hands the whole interest set to
poll() (WSAPoll over every socket, three allocations) after polling each fd on its own; node
keeps a dozen fds registered, so one epoll_pwait is 15-20 WSAPolls, 0.8 ms of CPU, 4 s of the
main thread's 23 s. Keep the readiness of the registered fds incrementally (one WSAPoll for the
whole set, or AFD/IOCP readiness) instead. Then the guest code itself (V8 under the translator).

### 9. Poll notify pipes are reused (Windows)

Every poll(), select() and epoll_wait() made a pipe to be woken through and closed it again; on
Windows that is a pair of loopback TCP sockets (socket, bind, listen, connect, accept). Released
pipes now go to a pool of 16, drained. Nothing writes to a pipe after its poll gave it up
(poll_wakeup checks under poll->lock).

npm install, main thread, with ISH_SYSCALL_TIMES, two interleaved runs each:

| | before | after |
|---|---:|---:|
| epoll_pwait CPU | 3.9 s | 0.55-0.78 s |
| CPU inside syscalls | 6.1-8.1 s | 2.4-3.7 s |
| wall | 23.1 / 18.4 s | 18.5 / 15.3 s |

benchmark/daily, interleaved: node.npm_install 18.82 s -> 15.95 s (0.853; 0.836 at 5
repetitions), the other 21 non-net cases within noise (geomean 0.987), outputs identical. Agent
18/18, compat on Windows 226/232 (the same six missing programs), difftest unchanged.

**Defender.** Until 13:40 on 2026-10-12 Defender scanned C:\src in real time; since then C:\src,
C:\Users\wsvn5\work and the Minis data folder are excluded. Numbers from before are not directly
comparable with later ones (npm install: 33-38 s before, 18-23 s after, same build). The host's
open() in npm install's main thread: 0.3-7.6 s for 1585 opens before, 0.05-0.07 s after (one run
0.48 s).

## Compatibility suite on Windows (2026-10-12)

The macOS runs had become unusable for this (25 timeouts at load average 10, 4 at 7.5), so the
232-case suite (`benchmark/run.sh compat`, ARCHS=x86, COMPAT_TIMEOUT_SCALE=2) ran on the Windows
test machine, on a copy of its package-laden rootfs (C:/src/rootfs/compat-x86_64, made with
unfakefsify + fakefsify, 861 MB), baseline and current build back to back while no Minis build
was running.

| build | pass | wrong output / error | timeout | wall |
|---|---:|---:|---:|---:|
| baseline: 30afd0f2 (before the SSE translation) | 225 | 6 | 1 | 8 min |
| current: 8a1a23e4 (SSE translation) | 226 | 6 | 0 | 4 min |

Every failure is the rootfs, not ish, and the same in both runs:

| case | why |
|---|---|
| Network / wget | `wget` is busybox's, which has no `--version` |
| Skill / akshare | the module is not installed |
| AICLIs / bun lang+stdlib | bun is not installed |
| AICLIs / claude --version, claude -p | claude is not installed |
| AICLIs / codex --version | codex is not installed |
| Skill / mcporter (baseline only) | timeout in `npx -y mcporter` (network download) |

So: every case whose program is installed passes, with no differing output, on both builds; the
SSE translation adds no failure. (The macOS 232/232 baseline used a rootfs with all of them
installed.)

## Fixed while building the suite

- d5e234b8: any `gcc` in the guest killed ish with 0xC0000005 (128-bit vector helpers read
  unaligned guest memory with movaps).
- 5a2caa26: two ish processes binding the same path at once (`-b`) failed with EEXIST, and a
  rebind briefly removed the mount under the other process's running command.
- `go build` ran, but every program it built threw "fatal error: float32nan" at startup:
  cvtss2sd/cvtsd2ss went through the float-to-integer conversion helper, whose out-of-range
  check turned a NaN into -2^31, and Go's compiler folds float32 NaN comparisons through them.
  build.go now passes (100-140 s with an empty build cache, 3 s cached). A Go build cache filled
  by an older ish holds the miscompiled packages: clear it once (`go clean -cache`). vec.c is
  built for the x86 guests only: the iOS app, which runs the arm64 guest, never had the bug. The "go version" futex stall of the
  first baseline no longer happens (0.2 s).
- 8b26ab2a: `unfakefsify.exe` imported instead of exporting on Windows (the name check missed
  the .exe), with the arguments swapped.
- 1418c8c0: `rm -rf` left files behind in directories of more than ~50 entries (seekdir after
  getdents recounted entries and skipped past deleted ones).

## Known issues

- On a bind mount, `stat` and `fstat` of the same file disagree on (st_dev, st_ino) (the stat goes
  through the path hook, the fstat through the open file): `cp` from a bound folder may report
  "file was replaced while being copied". Present before these changes.

- Results above are from a shared machine; Defender scans every file ish opens for the first
  time, which shows as wall time above cpu time (grep_r, npm_install, git.clone).
