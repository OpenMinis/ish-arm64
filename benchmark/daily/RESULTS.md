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

## Fixed while building the suite

- d5e234b8: any `gcc` in the guest killed ish with 0xC0000005 (128-bit vector helpers read
  unaligned guest memory with movaps).
- 5a2caa26: two ish processes binding the same path at once (`-b`) failed with EEXIST, and a
  rebind briefly removed the mount under the other process's running command.
- 1418c8c0: `rm -rf` left files behind in directories of more than ~50 entries (seekdir after
  getdents recounted entries and skipped past deleted ones).

## Known issues

- On a bind mount, `stat` and `fstat` of the same file disagree on (st_dev, st_ino) (the stat goes
  through the path hook, the fstat through the open file): `cp` from a bound folder may report
  "file was replaced while being copied". Present before these changes.

- `go build` hangs (futex stall), on Windows and macOS, JIT and gadget builds alike.
- Results above are from a shared machine; Defender scans every file ish opens for the first
  time, which shows as wall time above cpu time (grep_r, npm_install, git.clone).
