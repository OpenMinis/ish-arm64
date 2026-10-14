# Daily workloads: results

## Status and how to use (2026-10-12, 106e1569 + notes)

Everyday commands on the Windows x86_64 ish, each in a fresh ish.exe as Minis runs them. "First
baseline" is 1418c8c0 before any of this work, with Defender scanning the folders; "now" is
6ef704f1 with Defender excluding them (since 13:40), so the two columns are not one experiment:
the sections below give each change's own interleaved before/after. Median of 3, seconds.

| case | first baseline | now |
|---|---:|---:|
| boot.true (start ish, /bin/true) | 0.067 | 0.035 |
| proc.forkexec (200 x /bin/true) | 1.20 | 0.84 |
| proc.pipe (256 MB through busybox wc) | 14.7 | 2.28 |
| py.start | 0.31 | 0.22 |
| py.import (9 stdlib modules) | 1.48 | 0.86 |
| py.loop | 5.36 | 2.47 |
| py.script (JSON + regex over a 12 MB log) | 4.53 | 2.72 |
| py.pip_list | 3.73 | 2.01 |
| py.pip_install (6 local wheels) | 14.5 | 7.46 |
| node.start | 0.88 | 0.50 |
| node.json | 1.72 | 1.41 |
| node.fs | 2.51 | 1.35 |
| node.npm_install (4 local tarballs) | 56.2 | 16.0 |
| sh.ls_r | 1.16 | 0.37 |
| sh.find (/usr) | 4.42 | 1.53 |
| sh.grep_r (214 MB) | 28.8 | 10.1 |
| sh.sed_awk (12 MB) | 2.98 | 1.74 |
| sh.untar | 0.64 | 0.33 |
| fs.smallfiles (1000 creates + rm -rf) | 1.12 | 0.75 |
| fs.stat (ls -lR) | 0.85 | 0.39 |
| git.clone (local) | 2.44 | 0.75 |
| build.gcc (hello.c) | crashed ish, then 1.02 | 0.47 |
| build.make (7 files) | 4.46 | 3.28 |
| build.go (hello, cached) | built programs crashed | 1.99 |

What changed, by effect (details in the numbered sections):
- correctness: gcc in the guest crashed ish; rm -rf left files; programs built by `go build`
  crashed (NaN through cvtss2sd/cvtsd2ss); concurrent `-b` binds failed; stat on a bind mount
  reported device 0; unfakefsify.exe imported instead of exporting;
- speed: hashed TLB index (1), stat without a handle (3), SSE translated natively (6, 7), poll
  notify pipes reused (9), freed code reused lowest address first (12), futex on an event (2),
  stack accesses checked once per run (16: 1-9% fewer CPU cycles, measured 2026-10-13, after
  the table above), flags written back at a block's end with stores only (23: 3% over the
  suite, 9-12% on the Python loop, sed/awk, grep);
- the Defender exclusions alone: npm install 33-38 s -> 18-23 s on the same build.

Tried without a gain and not committed, so that nobody repeats them: check groups across other
accesses (5), a cheap contended write lock (8, 9), node start-up shortcuts (10), one long-lived
ish per session (11), a translation image (13, 14: translating a block costs 2.9 us, decoding
it 0.85 us, a cache hit 4.2 us; the ceiling of a pre-linked image would be 23-30% of a short
command and about 7% of a long one, for days of work on the fault, invalidation and chaining
paths), looking the TLB up instead of predicting the page, for all accesses or by a fixed rule
(17: a large heap gains 10-15%, tight loops lose as much; an experimental switch), access sites
that decide that for themselves in their miss path, and a larger site table (18), a second
tier that re-translates hot blocks with what the first translation counted (19), sites shared
by base register (20), another build of the guest's Python (21), an inline indirect dispatch
with half the instructions (22), a return stack on the host's (23), guest memory in a host
window checked through a software page map (24).

Against a native CPython on the same machine everyday Python is 4-7x slower (15); most of that
is translated code, and the largest part of it that is not the guest's own work is the page
check before each memory access.

Open:
- What is left is memory (23, last paragraph): every access is a page check and an address the
  host cannot rename, 2-6x of native per access; calls are mostly that. Returns through the
  host's return stack gave nothing, and translating across the fall-through of a branch would
  give about 2%. The remaining lever is guest memory at a fixed offset from the host's, with
  the host's page protection instead of any check (24: what was measured for it, and its size).
- node start-up has no single hot spot; every start compiles each block twice (gadgets, then
  native after four runs). Only the pre-linked image would remove that (not planned).
- Floating point arithmetic ignores the guest's MXCSR rounding mode (always to nearest) and
  never sets its exception flags, in gadgets and translated code alike. No program needing
  another mode has turned up.
- ISH_XCACHE_FILE (translations shared between ish processes through a file) is experimental
  and slower than translating; leave it unset.

Re-running (MSYS2 bash on the Windows test machine; build with `ninja -j 2`, others use it):

```sh
benchmark/daily/mkrootfs.sh                      # once: C:/src/rootfs/daily-x86_64 (needs the network)
/clang64/bin/python3 benchmark/daily/run.py --ish base=C:/src/ish-old.exe \
    --ish new=build-x86_64-jit/ish.exe --rootfs C:/src/rootfs/daily-x86_64 --reps 3   # interleaved A/B
ISH_SYSCALL_TIMES=C:/tmp/t.txt build-x86_64-jit/ish.exe -f ... ; python3 benchmark/daily/sctimes.py t.txt
/c/src/tools/python312/tools/python.exe benchmark/daily/ab.py 5 old=@C:/src/ish-old.exe \
    new=@build-x86_64-jit/ish.exe                # CPU cycles, least of 5: for a machine others are using
ISH_XLAT_SITEDUMP=C:/tmp/sd.txt build-x86_64-jit/ish.exe -f ... ; python3 benchmark/daily/sitedump.py sd.txt
benchmark/daily/persist.sh                       # long-lived ish against a fresh one per command
COMPAT_TIMEOUT_SCALE=2 ARCHS=x86 ISH_X86=... FAKEFS_X86=C:/src/rootfs/compat-x86_64 \
    bash benchmark/run.sh compat                 # 232 cases; 226 pass, 6 need programs the rootfs lacks
```

Checks every change went through: the x86_64 difftest (19696 lines, JIT default, ISH_XLAT_HOT=1,
ISH_XLAT_NOGROUP=1, gadgets), tests/xlat/{sse,sse_fp,fp_nan}.c against the same programs built
natively on the Windows machine (tests/xlat/flags.c too), tests/xlat/{spgroup,sites}.c against the gadgets, tests/agent (18 cases) on macOS and Windows, the compat suite
on Windows, and run.py's own comparison of every case's output between the builds. Switches for
bisecting: ISH_XLAT=0, ISH_XLAT_NOSSE=1, ISH_XLAT_NOGROUP=1, ISH_XLAT_NOSPGROUP=1, ISH_XLAT_SLOWSYNC=1, ISH_XLAT_ALLPOKE=1,
ISH_STAT_BY_HANDLE=1, ISH_XCACHE=0.

## Measurements in the order they were made

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

Re-tried after the pipe pool and the Defender exclusions, the cheap contended write lock again:
one binary, ISH_WRLOCK_MODE switching between the sleep ramp, writers yielding and retrying for
300 us first, and that plus readers giving way. npm install, 7 interleaved runs on a quiet
machine: 16.85 s median (15.4-18.4) for the ramp against 15.80 s (15.1-17.7) with yielding
writers, per-run ratios 0.82-1.14 with median 0.956; the readers giving way added nothing
(17.5 s against 17.0 s in a noisier 5-run set). A gain of about 4% that the noise does not let
me claim: not committed.

**Defender.** Until 13:40 on 2026-10-12 Defender scanned C:\src in real time; since then C:\src,
C:\Users\wsvn5\work and the Minis data folder are excluded. Numbers from before are not directly
comparable with later ones (npm install: 33-38 s before, 18-23 s after, same build). The host's
open() in npm install's main thread: 0.3-7.6 s for 1585 opens before, 0.05-0.07 s after (one run
0.48 s).

### 10. node startup and V8 under the translator: no single hot spot (nothing committed)

`node -e 0` (0.43-0.50 s) and `node js/json.js` (1.1 s), PX_PROF, share of the samples:

| | node -e 0 | node json.js |
|---|---:|---:|
| translated guest code | 19% | 30% |
| ish C code | 53% | 40% |
| - native translation (xlat) | 13.5% | 11.8% |
| - decoding to gadgets (gen) | 11.9% | 7.0% |
| - memory, TLB, page tables | 7.8% | 3.1% |
| - gadget execution (cold blocks) | 6.7% | 5.3% |
| - block cache, invalidation | 3.1% | 6.0% |
| Windows system calls (ntdll) | 23% | 24% |

One start translates 24121 blocks (122k instructions, 15 MB of code; 60k blocks counted, 24k
promoted). Inside the translated code the split is the usual one (page checks 27-36%, memory
accesses 16%, slot registers 11%); indirect dispatch is 1-6%, gadget fallbacks 2% of the
instructions (calls: `e8` 1766 of 2239). The main thread's syscalls are 78 ms of CPU (munmap 31,
read 16, mmap 16).

Tried:
- The cross-process translation cache (ISH_XCACHE_FILE, off by default): 24003 hits, 658
  instructions left to translate, 37 MB of cache, and the same wall time (450-520 ms against
  460-500 ms). Loading and linking the cached blocks (xc_link, prefetching the file, TLB misses
  on its pages) costs what translating them did; every block is still decoded to gadgets first.
- asbestos_invalidate_range, the largest single C function (7% with the cache on): V8 maps,
  unmaps and protects ~750 regions while it starts and each large one sweeps every bucket.
  Returning early when the range lies outside the pages that ever held blocks: correct
  (difftest, agent), geomean 0.990 over 7 cases at 7 repetitions (node.start 0.953, node.fs
  0.961, npm 0.984, node.json 1.057), inside the noise. Not committed.

What is left is spread thin: no item above 8% of a start. The structural cost is compiling
every block twice (gadgets, then native once it has run four times) on every start; removing it
means either translating cached blocks without decoding them again, or one long-lived ish per
session so that later commands find the code already translated (2c9e51ea shares translations
between the processes of one ish).

### 11. One long-lived ish per session against a fresh ish per command (measurement only)

`benchmark/daily/persist.sh`: five commands, 5 rounds, in one ish.exe (a shell running them in
sequence) and with a fresh ish.exe for each, as Minis runs commands today. Quiet machine, Defender
excluded, 040c0aee. Milliseconds:

| command | fresh ish, median of 5 | long-lived: first run | long-lived: runs 2-5 | later / fresh |
|---|---:|---:|---|---:|
| node -e 0 | 499 | 383 | 617 493 459 625 | 1.11 |
| python3 -c pass | 268 | 203 | 284 206 246 307 | 0.99 |
| python3 -c 'import json,re,os,sys,subprocess' | 374 | 344 | 669 454 469 560 | 1.38 |
| node js/json.js | 1045 | 1148 | 1699 1110 1173 1303 | 1.18 |
| npm install (4 local tarballs) | 17638 | 23333 | 26827 24157 22917 31557 | 1.45 |

Two more long-lived runs (with and without the in-process translation sharing) gave node -e 0
396-562, python3 -c pass 188-273, the imports 366-604, node json 1014-1350, npm install 19.7-27.2 s.

So with the code as it is a long-lived ish does not pay: it saves the 35-60 ms of starting ish
on the first, trivial commands and nothing after that. Sharing translations between the
processes of one ish (2c9e51ea) does not show (ISH_XCACHE=0 gives the same times), as section 10
found for the file cache: reusing a block costs what translating it does. npm install is slower
in a long-lived ish in every run (1.1-1.5x) and the imports creep up round after round: something
accumulates in the process. Not found yet; a first check (three light commands, npm install,
the same three again) shows the imports going from ~370 to 430-460 ms after one npm install.

Memory of the long-lived ish.exe over the 5 rounds (25 commands, 5 npm installs):

| | idle between commands | peak (during npm install) |
|---|---:|---:|
| default (translations shared) | 500-540 MB private, 460-490 MB working set | 1.56 GB private, 690 MB working set |
| ISH_XCACHE=0 | 220-300 MB private, 190-255 MB working set | 1.32 GB private, 450 MB working set |

A fresh ish per command frees everything when the command ends. The test machine has 4 GB.

What a long-lived mode would need from Minis (none of it exists today; each command is a whole
guest that dies with its ish.exe):
- per-command environment and cwd: a guest shell or a small supervisor in the guest that execs
  each command with its own env and cwd, and returns the exit status, stdout and stderr streams
  per command (today they are the process's);
- cancelling one command: kill its guest process group, not ish.exe; a command that ignores
  SIGTERM/SIGKILL semantics in the guest, or wedges the emulator, still needs the whole ish
  killed, which takes the session's other commands and state with it;
- crash recovery: an ish crash (or the memory cap of the Job Object) ends every running command
  of the session; Minis must restart the ish and report the lost commands;
- background jobs: a command that forks a daemon keeps it alive in the ish after the command
  returns (today it dies with the command); that changes what "the command is done" means, and
  leaked processes pile up in the long-lived guest;
- binds: `-b` mounts are made when ish starts, so a session whose folders change needs a way to
  rebind in a running ish (the harness has none); read-only is enforced per ish process;
- memory: one ish per session holds 0.2-0.5 GB between commands instead of nothing.

Given the numbers, not recommended now: the gain is at most the ~40 ms of process start, the
costs above are real, and the slowdown in a long-lived ish has to be found first.

### 12. A long-lived ish no longer slows down: freed code is reused lowest address first

The slow-down of section 11 was in guest CPU time, not in syscalls or waits (ISH_SYSCALL_TIMES:
the same `python3 -c 'import ...'` went from 265-359 ms of guest CPU to 390-453 ms after three
npm installs, with the same syscalls). It grew with every process when translations were not
shared (ISH_XCACHE=0: 265 ms rising to 421-515 ms over 16 runs) and after npm install otherwise
(V8's own code is never shared). Ruled out by that: the fakefs database, fd leaks, the write
lock. The cause was the code region's allocator: a process frees its blocks in hash order and
the free lists handed them out again last-in-first-out, so each later process's code was
scattered over everything ever freed (instruction TLB and cache misses). The free pieces of a
size are now a heap ordered by address and the lowest is taken first.

| 16 x python import in one ish, guest CPU ms | first 4 | last 4 |
|---|---|---|
| before, ISH_XCACHE=0 | 281 312 312 296 | 421 421 359 390 (peak 515) |
| after, ISH_XCACHE=0 | 281 312 312 312 | 328 281 328 312 |
| before, default | 359 359 296 296 | 265 312 328 328 |
| after, default | 296 375 265 296 | 250 312 296 234 |

benchmark/daily/persist.sh, long-lived, 5 rounds, two runs each, ms:

| command | before | after |
|---|---|---|
| python import | 361-424, up to 807 | 340-423 |
| npm install | 18.4-18.6 s first, then 21.7-23.3 s | 18.0-21.2 s, first and later alike |
| node json | 1009-1366 | 940-1137 |
| python3 -c pass | 194-275 | 169-232 |

A fresh ish per command is unaffected (geomean 0.992 over 33 cases, outputs identical). Difftest
in three configurations, agent 18/18 on macOS and Windows, compat on Windows 226/232.

### 13. Translation image, step 0: how much of a start is compiling (measurement only)

ISH_SYSCALL_TIMES gained wall-only spans around a block's first compile (decoding to gadgets),
its promotion (decoding again and translating natively once it ran four times) and the
translation inside that. Windows, median of 3, milliseconds, summed over all threads:

| command | wall | first compile (blocks) | promotion (blocks) | of which translation | both | of wall |
|---|---:|---:|---:|---:|---:|---:|
| node -e 0 | 543 | 70 (60064) | 81 (24148) | 65 | 151 | 27.8% |
| python3 -c pass | 246 | 25 (33510) | 49 (18206) | 40 | 74 | 30.2% |
| python3 -c 'import json,re,os,sys,subprocess' | 412 | 32 (39282) | 65 (23492) | 52 | 96 | 23.4% |
| node js/json.js | 1361 | 170 (104879) | 209 (55447) | 166 | 379 | 27.8% |
| pip list | 2369 | 54 (59340) | 116 (38171) | 92 | 170 | 7.2% |
| npm install | 19789 | 479 (235644) | 1011 (173695) | 845 | 1490 | 7.5% |

That is the ceiling for an image that makes both free, 23-30% of a short command (the stop
point was 15%), less what a hit costs and the code that cannot be cached (V8's own), plus the
runs in gadgets a block needs before it is promoted. Long commands gain 7% at most.

### 14. Translation image, step 1: stopped, a cache hit costs more than translating

Before writing a new image format, what the existing file cache (ISH_XCACHE_FILE) already does:
with it a hit is linked at the block's first compile (xc_eager, the default), so a hit already
skips the promotion, the second decode and the four runs in gadgets. Same spans as step 0,
warm cache, Windows, 5 interleaved runs:

| command | no cache: first compile + promotion | warm cache: first compile + promotion | wall median, off -> on |
|---|---:|---:|---:|
| python3 -c pass | 28 + 52 = 80 ms | 105 + 0 = 105 ms | 238 -> 301 ms |
| python3 -c 'import ...' | 30 + 64 = 94 ms | 102 + 0 = 102 ms | 451 -> 529 ms |
| node -e 0 | 58 + 75 = 133 ms | 126 + 1 = 127 ms | 582 -> 602 ms |

Per block: translating costs 2.9 us (52 ms for 18208 blocks), decoding to gadgets 0.85 us, a
cache hit 4.2 us. A hit compares the instruction bytes, copies the code into the assembler's
buffers, rebuilds fixups and labels and then runs xlat_finish like a fresh translation
(allocate, copy again, resolve, set the entries): the back half of translating plus the check,
to save a front half that takes 1-2 us. Copy-and-fix-up caching cannot win against this
translator.

What could: an image whose hit is a table lookup and a byte compare (under 0.3 us), with no
copy, no fixup and no block to build. Translated code refers to its block's own stream slots
and to the block (jump patch slots, cross-page fallback entries, last_block), which are
per-process allocations, so the blocks and their gadget streams would have to be in the image
too, mapped copy-on-write at a fixed address, with every internal pointer of a stream relocated
when the image is written. That is a new memory layout for blocks, through the fault-recovery,
invalidation and chaining paths: days, not the one to two hours this step was given, and
nothing smaller tests it. Stopped here, as agreed; no code for the image was written. The
ceiling stays what step 0 measured (23-30% of a short command, 7% of a long one).

### 15. Where the gap to a native Python is (measurement, 2026-10-13)

Twelve everyday Python chores (start, imports, a pure loop, strings, json, regex over a file,
sqlite, hashes, subprocess, pip, venv), timed inside the guest (median of 3, seconds), on the
Windows test machine: python.org's CPython 3.12.10 for Windows against Alpine's 3.12.15 in ish.

| task | native | ish | ish / native |
|---|---:|---:|---:|
| `python3 -c pass` | 0.04 | 0.16 | 4x |
| import of nine stdlib modules | 0.18 | 1.01 | 5.6x |
| pure Python loop | 0.51 | 2.76 | 5.4x |
| strings, dicts, sorting | 0.09 | 0.44 | 4.9x |
| json dumps/loads | 0.29 | 1.36 | 4.7x |
| regex over a 100k-line file | 0.30 | 1.79 | 6.0x |
| sqlite in memory | 0.14 | 0.91 | 6.5x |
| sha256/md5/zlib of 20 MB | 0.20 | 0.98 | 4.9x |
| `pip --version` | 0.30 | 2.24 | 7.5x |
| venv without pip | 0.08 | 0.54 | 6.8x |

Not the same binary (MSVC with PGO against gcc with musl), so part of the factor is Alpine's
build, not ish; there is no Linux on this hardware to tell how much.

Where the time goes (PX_PROF samples; ISH_SYSCALL_TIMES for pip):

| | loop | json | `pip --version` |
|---|---:|---:|---:|
| translated code | 90% | 77% | 64% |
| ish's C code (translating, the run loop, TLB misses) | 7% | 16% | 17% |
| the host's system calls (files) | 2% | 5% | 19% |

`pip --version` (2.15 s): 0.30 s in guest system calls (1277 stat at 80 us, 648 open at 123 us,
1160 read), 0.16 s compiling and translating 57k blocks, the rest translated code. So even the
start of a program is mostly translated code, and inside translated code (loop, by instruction
sampled): the guest's own arithmetic and branches 43%, the page check before a memory access
26%, the access itself 11%, r8-r15 living in memory 6%, lookups after a failed check 5%, flags
4%. The page check is the largest thing that is not the guest's own work.

ISH_XLAT_SITESTATS=1 (counts, printed when a thread exits) and ISH_XLAT_SITEDUMP=<file> with
benchmark/daily/sitedump.py (per access site) say how those checks behave:

| | loop | json | `pip --version` |
|---|---:|---:|---:|
| checks | 2.71e9 | 762e6 | 324e6 |
| of them on the stack (push, pop, call, ret, rsp-relative) | 38% | 50% | 43% |
| lookups after a failed check | 3.8% | 7.7% | 18% |
| TLB flushes (another mapping changed) | 491 | 903 | 2537 |
| checks at sites that miss under 1% of the time | 83% | 72% | 17% |
| checks at sites that miss over 20% of the time | 11% | 14% | 32% |

Two populations: sites whose page never changes (the stack, an interpreter's frame) and sites
that follow pointers into a heap, which miss most of the time. A small heap (the loop) is nearly
all the first kind, a large one (pip: imports, many objects) a third the second.

### 16. Stack accesses checked once per run (find_sp_groups)

A push, a pop, a call's or a return's own access and an rsp-relative operand each checked their
page, although a function's prologue touches six adjacent words. Now a run of them is one group,
as accesses off one unchanged base register already were: rsp may move by constants in between
(push, pop, `sub $n, %rsp`, `lea n(%rsp), %rsp`), nothing else may touch memory, and the range is
at most 128 bytes. The first access checks the whole range, the others load the page's
data_minus_addr and go. A range that crosses a page (128 of 4096 positions) runs that block's
remainder in gadgets, as before for groups. `ISH_XLAT_NOSPGROUP=1` turns it off.

Checks: loop 2.71e9 -> 2.35e9 (-13%), `pip --version` 324e6 -> 255e6 (-21%); accesses that need
no check of their own 160e6 -> 520e6 and 19e6 -> 87e6. CPU cycles of the ish process
(benchmark/daily/ab.py: least of 5 interleaved runs, billions; the machine was busy with
someone else's work, which is why this is in cycles and not in seconds):

| case | before | after | after/before |
|---|---:|---:|---:|
| sh.sed_awk | 10.94 | 9.39 | 0.86 |
| py.import | 3.21 | 2.93 | 0.91 |
| py.script | 9.75 | 8.97 | 0.92 |
| py.strops | 1.68 | 1.58 | 0.94 |
| node.start | 1.31 | 1.25 | 0.95 |
| proc.forkexec | 2.79 | 2.67 | 0.96 |
| node.json | 3.40 | 3.31 | 0.97 |
| py.pip_list | 6.88 | 6.77 | 0.98 |
| py.start | 0.65 | 0.64 | 0.99 |
| proc.pipe | 9.24 | 9.18 | 0.99 |
| py.loop | 9.07 | 9.37 | 1.03 (0.99 in another run of 5) |
| geomean of 11 | | | **0.954** |

Fewer checks by 13-21% for 1-9% fewer cycles: as section 5 found, a check that is predicted
overlaps with the guest's work; what is saved is its instructions, not waiting.

tests/xlat/spgroup.c: a prologue, a frame, calls, `ret $n`, `push %rsp`, pops at every stack
position around a page boundary and on pages never touched; the same 146 lines as the gadgets
(ISH_XLAT=0), with ISH_XLAT_HOT=1 and without groups, on macOS and Windows.
The other checks: difftest in six configurations, sse/sse_fp/fp_nan the same lines as before,
agent 18/18 on macOS. Compat suite on Windows 225/232: the six known ones and `gh config`, which
timed out once in the run and then passed 40 of 40 with this build and with the one before (the
machine was running someone else's GUI tests during the run).

### 17. Looking the TLB up instead of predicting the page (experimental switch, not the default)

A failed check costs a mispredicted branch and then a lookup; a site that fails most of the time
would do better looking the TLB up at once (ten instructions instead of six, no misprediction,
and a TLB entry is shared by every site that touches the page, where each site's own entry is
another cache line). `ISH_XLAT_TLBDIRECT=1` does that for every access, `=2` for those through a
pointer (not rsp, rbp or a fixed address), `=3` for rbp too. Cycles relative to section 16's
build (least of 5):

| case | all (=1) | pointers (=2) | pointers and rbp (=3) |
|---|---:|---:|---:|
| py.import | 0.85 | 0.91 | 0.90 |
| py.pip_list | 0.86 | 0.90 | 0.86 |
| py.script | 1.06 | 0.97 | 0.96 |
| node.json | 0.97 | 0.98 | 0.98 |
| py.strops | 1.01 | 0.99 | 0.97 |
| proc.forkexec | 0.95 | 0.94 | 0.97 |
| node.start | 1.05 | 0.99 | 0.98 |
| py.start | 0.95 | 1.09 | 0.92 |
| sh.sed_awk | 1.25 | 1.13 | 1.05 |
| proc.pipe | 1.20 | 1.07 | 1.04 |
| py.loop | 1.25 | 1.01 | 1.06 |
| geomean of 11 | 1.03 | 0.997 | 0.97 |

(An earlier run in CPU seconds had npm install at 0.80 and pip install at 0.80 with =1.) Programs
with a large heap gain 10-15%, tight loops over the same few pages lose as much: no fixed rule by
operand fits both, since the same instruction (`mov 8(%rdi), %rax` in the interpreter) is one
kind in one program and the other in the next. Left off; the switch stays for whoever picks this
up.

### 18. Sites that decide for themselves, in the miss path: tried, slower, not committed

The per-site numbers of section 15 ask for a decision per site at run time. Tried without
re-translating: a site's first 32 accesses all go through its miss path (its page is kept
empty), which counts how often the page changed; over 5 changes and the site keeps its page
empty for good, so that its check fails every time, predictably, and the miss path looks the TLB
up; otherwise it predicts as before; every 256 misses, and every 50 ms for the others, a site
watches again. With it the site table had to grow from 8192 to 65536 entries (no two sites
sharing one), flushes emptied only the entries in use (a list), and a group's lead passed
data_minus_addr through one slot of its own. Correct (difftest in six configurations,
tests/xlat/sites.c, agent 18/18), and the miss counts were as designed (loop: 15% of the checks
through the miss path, which is its share of bad sites).

Cycles against the build of section 16 (least of 5): py.loop 1.03, py.import 1.21, py.strops
1.18, py.pip_list 1.32, py.start 1.18, proc.forkexec 1.14; with the decision switched off (the
larger table and the list alone) 0.98, 1.01, 1.01, 1.05, 1.05, 1.14 (every new process fills
its megabyte of table). So the larger table buys nothing by itself, and a site
that "looks up from its miss path" is the worst of both: it still reads its own entry (a cache
line per site), jumps to cold code and back, and reads its decision there, to save one
misprediction. What section 17 gains comes from not touching per-site state at all, and that
needs the decision compiled in.

What would do it: the miss path only measures (as above, cheap), a site found bad over two
separate windows is noted, and the run loop re-translates its block with that access looking up
directly (the block is invalidated like one whose code was written to; requests batched, since
an invalidation empties every thread's block cache). Hot blocks only, a few thousand per
program. Not done: it is a second tier with its own failure modes, and the gain is the 10-15%
of section 17 for programs with a large heap, without its losses.

### 19. A second tier, three ways: correct, no gain, not committed (2026-10-13)

Built what the last paragraph of 18 asked for. A block's first translation counts its runs at
its entry (a word in its data that wraps after 1024) and, in every access's miss path, its
misses; then INT_XJIT_TIER2 takes it to the run loop, which either moves the entry past the
count or translates the block again (the old code stays until the block is freed, unlinked so
that it leaves through the stream; callers are linked to the new entry). Correct: difftest in
six configurations, spgroup, sites, sse, agent 18/18.

- Accesses that missed in 15% of the runs or more look the TLB up inline in the second
  translation. Lookups after a failed check fell as designed (loop 175e6 -> 14e6, `pip --version`
  50e6 -> 14e6; python -c pass translates 446 blocks again and lets 929 stop counting). Cycles
  (least of 5): geomean 1.00-1.02 over 11 cases, py.import 1.02-1.04, py.pip_list 1.00-1.06,
  py.loop 0.97-1.00. Removing 70-90% of the mispredicted checks buys nothing.
- The other way round: every access looks up directly in the first translation and gets its
  site once the block has run 1024 times. py.pip_list 1.05, py.import 1.03, py.loop 0.94:
  nothing of the 0.80-0.86 that direct lookups everywhere give those two.

So what 17 gains on a large heap is neither the mispredictions nor the cold blocks. The next
experiment says what it is.

### 20. Sites shared by base register: the site table's cache footprint (not committed)

`pip` runs tens of thousands of sites, 16 bytes of per-thread table each, and the page's
data_minus_addr that the access adds to its address is loaded from there: the table does not fit
the first-level cache, and every access waits for its entry. A TLB entry is per page, and the
pages in use are few. To see it: all rsp-relative accesses share one site entry (s1), or all
accesses through the same base register do (s2). Lookups after a failed check, `pip --version`:
51e6, 56e6, 74e6; loop: 165e6, 195e6, 544e6. Cycles (least of 5):

| case | stack shares (s1) | every base register shares (s2) |
|---|---:|---:|
| py.pip_list | 0.94 | 0.90 |
| py.import | 0.95 | 0.95 |
| py.start | 0.97 | 0.98 |
| py.loop | 0.97 | 1.16 |
| py.script | 1.02 | 1.09 |
| node.json | 1.04 | 1.05 |
| proc.pipe | 1.05 | 1.05 |
| sh.sed_awk | 1.00 | 0.98 |
| geomean of 8 | 0.99 | 1.02 |

More misses and still faster where there are many sites: the footprint is the cost there. But a
shared entry misses more, and where the sites are few (a loop) that is all it does. Within
this scheme (a predicted page per site, checked) the two pull against each other, and nothing
tried gets both; s1 alone is 1% over the suite. Left out.

### 21. Is it Alpine's Python? No

python-build-standalone's CPython 3.12.15 for x86_64 musl (clang, PGO and LTO), unpacked beside
Alpine's 3.12.15 (gcc) in the rootfs, both under ish, cycles (least of 5): loop 7.60 against
8.52 (0.89), nine imports 2.43 against 2.77 (0.88), strings 1.53 against 1.61, the JSON and
regex script 9.62 against 8.87 (1.08), `-c pass` 0.56 against 0.61. Another build of the guest
Python moves things by ten percent either way; the factor to native is ish's.

### 22. What costs how much: micro kernels, and a leaner indirect dispatch (not committed)

benchmark/daily/micro.c, the same source built natively (clang -O2) and as a guest (gcc -O2),
seconds on the Windows test machine:

| kernel | native | ish | ish / native |
|---|---:|---:|---:|
| registers only | 0.744 | 0.771 | 1.04 |
| pointer chase on one page | 0.316 | 0.330 | 1.04 |
| pointer chase, another page every step (cache misses) | 4.84 | 7.21 | 1.5 |
| switch in a loop (a jump through a table) | 1.18 | 1.85 | 1.6 |
| fields of a stack frame and a heap object | 0.180 | 0.324 | 1.8 |
| calls through a table of four functions | 0.640 | 2.48 | 3.9 |
| recursive fib (clang turns half the calls into a loop) | 0.079 | 0.298 | 3.8 |
| a loop over an array (clang vectorizes it, gcc -O2 does not) | 0.050 | 0.266 | 5.3 |

Arithmetic and a predicted memory access cost nothing. Calls, returns and indirect jumps cost a
factor of four each, memory accesses that cannot be grouped about 1.5-2. And the Python loop
(ISH_XLAT_SITESTATS, with counters per branch kind for this): 5.9e9 guest instructions in 0.98e9
block runs (six instructions a block), 2.36e9 page checks, 109e6 indirect jumps, 50e6 indirect
calls, 34e6 direct calls, 84e6 returns, 8.5e9 cycles: 1.4 cycles per guest instruction.

Tried on that: the inline lookup of an indirect jump, call or uncached return was 26
instructions (a pending-interrupt check, the timer tick's count, the block cache's generation
through three pointers, a hash, the block, its address, its stream). Made 13: a count-down in
the frame for the tick, the generation through one pointer, and a per-thread table of {guest
address, stream} in one cache line that fiber_ret_lookup fills. Kernels: switch 1.99 -> 1.73 s,
calls through a table 2.29 -> 2.14 s. The daily cases, nine interleaved runs, cycles: pipe 1.006,
py.loop 1.008, py.script 0.998, py.pip_list 1.001, node.json 0.985, sed/awk 0.963, with the same
build against itself at 0.991-0.997. Nothing in the Python cases for half the instructions of
159e6 dispatches.

That, 16's 13% fewer checks for 1% and 19 say the same thing: in these programs the host is not
short of instruction throughput. What is left is what a block-at-a-time translator with checked
memory is made of: a taken jump at the end of every six-instruction block (the fall-through of
a guest branch is a jump to another block), returns and indirect jumps that the host predicts
as indirect jumps and resolves late (a return is a load from the guest stack, a table lookup, a
compare and a jump through the block's stream before the host knows where it goes; natively it
is predicted by the return stack), and an address that every memory access has to add to.

### 23. A return stack: no gain. Flags written back with stores only: 3% over the suite

Returns first, as planned: a translated call pushed {where its return continues, the guest
return address} on the host stack and was a host call, a return whose top frame had its address
a host ret (a bottom frame nothing matches, a scan of six frames for returns that ran as
gadgets, a depth limit; ISH_XLAT_RSTACK). 99.7% of the Python loop's 84e6 returns took the host
ret. Kernels (benchmark/daily/micro2.c: assembly, so the same machine code native and guest,
least of 4 interleaved, ns per iteration): call and return 1.67 -> 1.74, indirect call 2.83 ->
2.76. Daily suite 1.000 over 11 cases, and worse than without it next to the change below
(py.loop 0.94 against 0.86). The host predicts these returns as indirect jumps well enough;
what a call and return cost is their fifty instructions, and the frame adds as many as it
saves. Not kept.

micro2.c is what found the next thing. ns per iteration, native and ish:

| kernel | native | ish | |
|---|---:|---:|---|
| `add $1, %rax` (the loop itself) | 0.20 | 0.26 | |
| `jmp` to the next instruction | 0.23 | 1.60 | one more block per iteration |
| a conditional jump taken every other time | 0.21 | 0.84 | |
| a conditional jump never taken | 0.23 | 0.39 | one more block too, but cheap |
| call an empty function | 0.25 | 1.67 | |
| push, pop | 0.20 | 0.67 | |
| `add (%rdi), %rax` | 0.21 | 0.47 | |
| `addq $1, (%rdi)` | 0.21 | 1.28 | the host renames a [reg+disp] store to its load; not ours |
| `add %r12, %r13; add %r13, %r12` (registers kept in memory) | 0.41 | 0.63 | as cheap as rbx/rbp (0.59) |

A block boundary costs 0.1 ns when the next block overwrites the flags and 1.3 ns when it might
read them: then they are written to the lazy flags, and that was fourteen instructions reading
and rewriting cpu->eflags and cpu->flags_res, the same two bytes at every block's exit, each
waiting for the store of the one before. The Python loop does it 202e6 times, mostly before
calls, returns and indirect jumps (which always do).

Now stores only, when the host has all of PF AF ZF SF (it nearly always does): lahf's byte is
eflags' low byte as it is (its CF bit is nobody's, CF lives in cpu->cf), and with all four in
eflags flags_res is 0; setc and seto go straight to cf and of. ISH_XLAT_SLOWSYNC=1 is the old
form. Kernels: `jmp` 1.60 -> 0.70, alternating conditional jump 0.84 -> 0.41, call a leaf
function 2.69 -> 2.38.

Daily suite, CPU cycles, least of 5 interleaved runs, all 33 cases: **geomean 0.971**.

| case | before (Gcyc) | after | after/before |
|---|---:|---:|---:|
| py.strops | 1.70 | 1.48 | 0.875 |
| sh.sed_awk | 10.07 | 8.83 | 0.877 |
| sh.grep_r | 34.57 | 31.45 | 0.909 |
| py.loop | 8.84 | 8.05 | 0.911 (0.858 in a run of nine) |
| py.start | 0.78 | 0.71 | 0.913 |
| sh.ls_r | 1.46 | 1.35 | 0.925 |
| py.script | 9.00 | 8.57 | 0.953 |
| proc.pipe | 9.60 | 9.21 | 0.959 |
| py.import, py.pip_list, node.npm_install, build.go | | | 0.964-0.965 |
| worst: boot.true (0.09 -> 0.10, one tick of the unit), proc.forkexec, sh.gzip | | | 1.107, 1.037, 1.025 |

Checked: tests/xlat/flags.c (1260 lines, the same as the hardware, both forms and the gadgets,
macOS and Windows), difftest in six configurations and the gadget build, spgroup, sites, sse,
sse_fp, fp_nan, agent 18/18 on both, compat suite on Windows 226/232 (the six known).

What micro2.c says about the rest. A not-taken conditional jump costs 0.1-0.2 ns more than no
jump, so translating across the fall-through of a branch (the planned next step) would give
about 2% on the Python loop for its 1e9 blocks: not worth a new block structure. What is dear
is memory: 2-3x for a load, 3.5x for push and pop, 6x for a read-modify-write, because every
access is a check and an address the host cannot rename, and calls (6x) are mostly their two
stack accesses and fifty instructions. Past this the lever is guest memory at a fixed offset
from the host's, and nothing smaller.

### 24. Guest memory at a fixed offset, phase 0: a software page map is slower; what is not (2026-10-13)

The guest's address space is already in the low 4 GB (stack at 0xffffe000, mmap below it), so a
window is cheap: ISH_LINEAR=1 reserved 4 GB of host address space per address space and put
private memory at window + guest address (anonymous memory there directly, private file
mappings copied in; after a fork the parent's window froze, since the child has its pages).
Translated code tried the window first: a byte per page said "this page and the next are in the
window, readable / writable" (filled as the TLB is, emptied when mappings change), and on a set
bit the access added the window's base without touching a site or the TLB; otherwise the site
check as before. Correct (difftest, sites, spgroup, flags, Python, pip, subprocesses), and
nearly every access took the window (pip: lookups after a failed site check 50e6 -> 0.8e6).

Cycles against 63d52337 (least of 5): py.pip_list 0.84, py.import 0.91, py.script 1.00, py.loop
1.03, py.start 1.10, node.json 1.18, sh.sed_awk 1.21, build.gcc 1.24, proc.pipe 1.25, sh.grep_r
1.35, node.start 1.38, proc.forkexec 1.54; geomean of 14: 1.14. Kernels (micro2.c): load 0.47 ->
0.64 ns, push and pop 0.65 -> 1.05. The large heap gains what it gains from anything that stops
using a table entry per site; everything else pays for the check and for the prototype's
copies at exec. Not kept.

Why the check is slow, and what is not: benchmark/daily/checklab.c, native on the test machine,
the same load and the same push/pop pair behind each kind of check (ns per iteration):

| check before the access | load | push + pop |
|---|---:|---:|
| none: the native instruction | 0.205 | 0.204 |
| none, but through base + address | 0.203 | 0.252 |
| is the address under 4 GB (`mov %r13d, %r15d; cmp; jne`, or `test` with a mask) | 0.208 | 0.33-0.42 |
| the same with `shr $32` | 0.41 | 0.72 |
| the site check, as now | 0.303 | 0.638 (0.495 as a group) |
| the page map: shift, compare, byte test | 0.51-0.81 | 0.96-1.27 |

Any check that shifts the address to index a table costs a cycle or more here; the site check
costs half a cycle; a bare range test costs nothing. So the window pays only if nothing is
looked up at all: the host's own page protection does the checking, and an access to a page
that is not there (or not writable, or not in the window) is a host fault that goes to the slow
path. Measured on this machine for that: a handled access violation takes 1.2 us; Windows maps
a view of a section into a placeholder at 4K granularity (VirtualAlloc2 / MapViewOfFile3: a 4K
view at a 4K offset worked), and a second, copy-on-write view of the same pages behaves as a
fork needs.

That is the design that is left, and its size: guest memory as views of sections in a
placeholder window (anonymous memory on the page file, files directly), fork as copy-on-write
views in the child's window, mprotect mirrored on the host, a vectored handler that takes a
fault in translated code to that access's slow path, and translated accesses with only the
range test. Per access it saves 0.1 ns on a load and 0.3 ns on a push/pop pair against today,
on 2.4e9 accesses in the Python loop's 2.3 s: an estimated 10-20% for code like that, more
where the stack dominates, plus what a large heap gains (16% above).

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
- On a bind mount `stat`/`lstat` reported st_dev 0 (and no block count or nanoseconds) for
  anything already in meta.db, while `fstat` reported the real device: the branch of fakefs_stat
  that stats the host path copied five fields only. coreutils compares the two after opening a
  file ("file was replaced while being copied"). Now all fields, checked on macOS and Windows.
- 8b26ab2a: `unfakefsify.exe` imported instead of exporting on Windows (the name check missed
  the .exe), with the arguments swapped.
- 1418c8c0: `rm -rf` left files behind in directories of more than ~50 entries (seekdir after
  getdents recounted entries and skipped past deleted ones).

## Java (2026-10-14)

`java` did not start in the x86_64 guest: OpenJDK 17 (Alpine's openjdk17-jdk) died in three
places, one after the other.

- getcpu (syscall 309) was a stub; HotSpot then calls the vsyscall page at 0xffffffffff600800,
  which is not mapped: SIGSEGV. Now implemented (CPU 0, node 0).
- CPUID leaf 1 said family 0. HotSpot reads feature bits only of family 5 and later, so "SSE2
  not supported". The x86_64 guest now reports family 6 model 0x2c (Westmere, the generation
  that has the advertised features and no AVX).
- No clflush bit: HotSpot's guarantee fails. The instruction was decoded already (as nothing:
  writes to code are noticed anyway); the bit and the line size are reported now.

With those: `java -version` 1.2 s, `javac Hello.java` 19.8 s, a small program (streams, a
HashMap, a thread) 6.5 s with the JVM's compilers and 25.7 s with -Xint, same output, 5 runs of
5. Checks: difftest in six configurations and the gadget build, agent 18/18, compat suite on
Windows 226/232 as before. Cost: with the family and clflush both reported, py.pip_list takes
4% more cycles (7 interleaved runs in one binary: 1.043; either alone 1.00), node.start 1%:
some library takes another path for the CPU it now sees; the other cases are within noise.

## Known issues


- Results above are from a shared machine; Defender scans every file ish opens for the first
  time, which shows as wall time above cpu time (grep_r, npm_install, git.clone).
