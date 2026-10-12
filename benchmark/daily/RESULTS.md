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

## Fixed while building the suite

- d5e234b8: any `gcc` in the guest killed ish with 0xC0000005 (128-bit vector helpers read
  unaligned guest memory with movaps).
- 1418c8c0: `rm -rf` left files behind in directories of more than ~50 entries (seekdir after
  getdents recounted entries and skipped past deleted ones).

## Known issues

- `go build` hangs (futex stall), on Windows and macOS, JIT and gadget builds alike.
- Results above are from a shared machine; Defender scans every file ish opens for the first
  time, which shows as wall time above cpu time (grep_r, npm_install, git.clone).
