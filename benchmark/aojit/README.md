# AOJIT: AOT images, their workloads and tests

AOT images are the ARM64 JIT's translations of one guest module, recorded ahead of time and linked
into a target (the Mac CLI with `-Dcli_aot`, the app with `ISH_AOT_OBJECTS`). This directory holds
what makes and checks them; the generic tools are in `tools/jit_aot/`.

## Layout

```
tools/jit_aot/             the image pipeline (host, no dependency on this suite)
├── record.sh              record one module with a JIT build and make its .S (RECORDING= keeps the raw file)
├── gen.py                 recording -> aot_<name>.S in the compact table layout
├── compact.py             .S of the older 64-bit table layout -> compact layout (code unchanged)
└── attrib.py              macOS `sample` + ISH_JIT_MAP -> CPU per guest module

benchmark/aojit/           images, workloads and tests (host scripts at the top)
├── images.json            the images: module path substring, recording workload, packages
├── record_images.sh       record the images of images.json (in parallel), IOS=1 also assembles iOS .o
├── install.sh             copy guest/ into a rootfs as /tmp/aojit and make the inputs (Mac CLI)
├── phone.sh               the same inside the app, run the cases, send the results back
├── verify.sh              three-way check: AOT on / AOT off / ISH_JIT=0 must print the same
├── profile.sh             is a module worth an image: wall on/off/all-native, translations, CPU per module
├── compat.sh              the ARM64 compatibility suite (benchmark/run.sh) against a given ish
├── images/                recorded images (not committed, see below)
└── guest/                 copied to /tmp/aojit in the guest
    ├── run_cases.py, cases.json, setup.sh   A/B cases (AOT on/off, hits per module), input generation
    ├── <module>/          per module or area: its recording workload (*_train.sh), the case workloads
    │                      and the generators of their inputs (gen_*); busybox charts doc net node pil
    │                      py pyext rg shell ssh zlib
    ├── verify/            the checks of verify.sh (check.sh, threeway.sh, futex_stress.py)
    └── measure/           workloads for profile.sh (ssh, curl, py, py_local, py_tls, rg, git, zlib)
```

Inputs (JSON, logs, images, certificates, the 10 MB scp file) are generated in the guest by
`guest/setup.sh` and the scripts themselves, never committed.

## Images

Recorded AOT images live in `images/` but are not committed (`.gitignore`). They are large (the set
below is about 2 GB with recordings) and tied to one ish build, so regenerate them instead of
sharing them.

```
images/
└── <abi>/                 abi fingerprint of the ish that recorded them (/proc/ish/jit: "abi xxxxxxxx")
    ├── latest/            recorded on the current package versions
    │   ├── S/aot_<name>.S     images (tools/jit_aot/gen.py output), for -Dcli_aot
    │   ├── ios/aot_<name>_ios.o   the same, assembled for iOS, for ISH_AOT_OBJECTS
    │   └── rec/<name>.jsonl   raw recordings (ISH_JIT_RECORD); gen.py turns them into .S again
    ├── old/               recorded on older package versions (family-matching A/B)
    │   ├── S/ …
    │   └── rec/ …
    └── minis/             the set MinisApp links (its deps/aot), same layout
```

`<name>` and what each image covers are in `images.json` (module substring, recording
workload, packages).

An image only loads into an ish with the same abi (`jit_abi()`: `JIT_CODE_VERSION` in
`asbestos/guest-arm64/jit.c`, the gadget conventions, the struct layouts). Any other ish lists it as
`rejected` in `/proc/ish/jit`. After a change to `jit.c`, `gen.c`, the gadgets or the offsets, record
again into a new `<abi>/` directory. Within one abi, an image serves its exact module (build-id) and,
through family matching, other versions of it (`libz.so.1*`, …).

**Table layout.** Since abi `97963b7f` the tables use the compact layout of
`asbestos/guest-arm64/aot.h`: 32-bit self-relative pointers (the linker resolves them, dyld has
nothing to rebase) and keys without the words that are always 0. `gen.py` writes it; images made
before it (abi `daf8fcc7`, the 64-bit layout) are rewritten with `tools/jit_aot/compact.py <in.S>
<out.S> --abi 97963b7f`, which leaves the native code byte for byte as it is. On the 26 images this
made the linked app 9% smaller (tables 77.1 -> 53.4 MB, rebases 2.43M -> 7.3K); the IPA stays about
the same size, since the relative offsets compress less well than the absolute pointers did.

- `daf8fcc7/`: the 64-bit layout, for feat/aot-family-match up to `0e59def6` (build 822/823). Its
  `rec/` recordings cannot go through the current gen.py (their recorder is gone and gen.py refuses
  that abi); convert the `.S` with compact.py instead.
- `97963b7f/`: the compact layout. `latest/` is the 26-image set converted from `daf8fcc7/latest`;
  `minis/` is MinisApp's 25-image set (musl, busybox and zlib from `old/`, libcrypto, libssl and
  pcre2 recorded on the app's rootfs), converted the same way.

## Current set: abi `97963b7f` (JIT_CODE_VERSION 7, compact tables; recorded at `daf8fcc7`, converted)

| image | module | recorded on | latest/S | old/S |
|---|---|---|---|---|
| musl | ld-musl-aarch64.so.1 | musl 1.2.5-r11 / r8 | ✅ | ✅ |
| busybox | /bin/busybox | busybox r14 / r8 | ✅ | ✅ |
| zlib | libz.so.1 | zlib 1.3.2 / 1.3.1 | ✅ | ✅ |
| python | libpython3.12.so.1.0 | python 3.12.14 / 3.12.12 | ✅ | ✅ |
| node | /usr/bin/node | nodejs 22.23.2 | ✅ | |
| crypto, ssl | libcrypto.so.3, libssl.so.3 | openssl 3.3.7-r1 | ✅ | |
| sqlite | libsqlite3.so.0 | sqlite-libs (Alpine 3.21) | ✅ | |
| imaging, jpeg, webp | PIL `_imaging`, libjpeg.so.8, libwebp.so.7 | py3-pillow, libjpeg-turbo, libwebp | ✅ | |
| numpy | numpy `_multiarray_umath` | py3-numpy (apk) | ✅ | |
| ssh, scp | /usr/bin/ssh, /usr/bin/scp | openssh-client-default 9.9_p2-r0 (build-id df164958…, cbc5234e…) | ✅ | |
| py* (11) | lib-dynload `_json` `_hashlib` `_blake2` `_struct` `_datetime` `binascii` `math` `zlib` `_socket` `select` `_ssl` | python3 3.12.14-r0 | ✅ | |
| rg | /usr/bin/rg | ripgrep 14.1.1-r0 | ✅ | |

Rootfs used: `latest` = `alpine-arm64-321-latest` (musl, busybox, zlib, python, node) and
`alpine-arm64-321-full` (the other seven). `old` = `alpine-arm64-321-old`. All three are in the main checkout,
`/Users/ethan/Src/github.com/ish-arm64/` (untracked). The build 822/823 IPAs link the first 12 `latest/ios/*.o`. ssh and scp came later (2026-09-27, rootfs `alpine-arm64-321-cand` = full + openssh-client). Their workload needs an sshd in a second ish; start it from `guest/ssh/server.sh` on a fakefs rootfs (sshd rejects a realfs `/var/empty`). sftp-mode scp also needs `prctl(PR_SET_DUMPABLE)` support on that server side. The Python extension images (about 6.3 MB of iOS objects) cut local one-liner CPU by about 5%. The rg image (24 MB) makes a directory search 1.21× faster in wall time. rg's worker threads spend most of their CPU in ish's own lock and file-syscall paths, which the image does not change.

The musl, busybox, zlib, python and node images of this set were recorded with the workloads
before they moved into `guest/`. Those workloads had the same scripts under `/tmp/p0`, `/tmp/p3`,
`/tmp/p5`, `/tmp/p6` and `/tmp/pz`, so a new recording differs only in noise.

## Is a module worth an image?

```sh
benchmark/aojit/profile.sh py_local build-arm64-aot/ish build-arm64-jit/ish -r $R
```

It prints the workload's wall time with the images on, off and with everything native (the runtime
JIT build: roughly what images of every module would give), the translations per module and the
CPU per module. A module is worth recording when the gap between "AOT on" and "all native" is
large and its own share of the CPU explains it (ssh: 76% of the CPU in ssh itself, 2.1x; curl:
libcurl about 2%, not worth it). `guest/measure/` holds the workloads; git needs a fakefs rootfs
(`-f`), since git hangs on the Mac CLI's realfs.

## Regenerate

All commands run from the repo root (`ish-arm64-ios-opt`). `R` is a rootfs directory and `ABI`
is the abi of the recording build.

### 1. Recording build: JIT with run-time translation

```sh
meson setup build-arm64-jit -Dguest_arch=arm64 -Djit=true -Djit_emit=true -Dbuildtype=release
ninja -C build-arm64-jit
ISH_JIT_PIC=1 build-arm64-jit/ish -r $R /bin/cat /proc/ish/jit | grep -o 'abi [0-9a-f]*'   # -> ABI
```

The abi also covers the runtime conventions (PIC, pinned registers). Recordings run with
`ISH_JIT_PIC=1`, and a build with images linked turns PIC on by itself. Without the variable a plain
JIT build shows another abi (`2c9b3f6a` instead of `daf8fcc7` for this set). That abi is not the one
of the images.

### 2. Rootfs with the packages and the suite

```sh
build-arm64-jit/ish -r $R /sbin/apk add -u $(python3 -c "import json; print(' '.join(json.load(open('benchmark/aojit/images.json'))['packages']['apk']))")
build-arm64-jit/ish -r $R /usr/bin/pip install --break-system-packages python-pptx
build-arm64-jit/ish -r $R /bin/sh -c "npm i -g --no-audit --no-fund $(python3 -c "import json; print(' '.join(json.load(open('benchmark/aojit/images.json'))['packages']['npm']))")"
benchmark/aojit/install.sh build-arm64-jit/ish $R [old rootfs]   # suite -> $R/tmp/aojit, inputs
```

The npm packages are cf and the MCP servers the node workload starts. Without them
`node/node_train.sh` skips those parts, and the node image covers less.

**Node mode.** `kernel/exec.c` adds V8 flags to every exec of a program named `node`. The default is
the hybrid set: V8 keeps JIT support, so WebAssembly and undici's own fetch work, but its JS tiers
and native regexp are off, so JS runs in the interpreter, which the node image covers.
`ISH_NODE_MODE=jitless` in the environment, or `--jitless` / `--no-expose-wasm` on the command
line, selects the old jitless set with `/lib/wasm-polyfill.js`. The two modes execute different
parts of the node binary, so record node in the mode the app runs. The images under `minis-hybrid/`
are hybrid; the node image of the older sets is jitless.

### 3. Record: `.S`, plus iOS objects with `IOS=1`

```sh
O=benchmark/aojit/images/$ABI/latest
IOS=1 JOBS=3 benchmark/aojit/record_images.sh build-arm64-jit/ish $R $O/tmp        # all images
IOS=1 benchmark/aojit/record_images.sh build-arm64-jit/ish $R $O/tmp zlib python  # or some
mkdir -p $O/S $O/ios $O/rec && mv $O/tmp/aot_*_ios.o $O/ios/ && mv $O/tmp/aot_*.S $O/S/ && mv $O/tmp/rec/* $O/rec/ && rmdir $O/tmp/rec $O/tmp
```

- Each image is one ish run over its workload with `ISH_JIT_PIC=1 ISH_JIT_RECORD=… ISH_JIT_RECORD_MOD=<module>`, followed by `gen.py`.
- node takes about 10 minutes and python about 3. The rest are quick.
- For the `old/` set, run the same commands on the old rootfs with `O=…/$ABI/old` and only the images whose module differs (musl, busybox, zlib, python).

Rebuild only the `.S` from a kept recording (for example after a `gen.py` change that keeps the abi):

```sh
python3 tools/jit_aot/gen.py $O/rec/<name>.jsonl build-arm64-jit/ish $R $O/S/aot_<name>.S --name <name>
xcrun -sdk iphoneos clang -target arm64-apple-ios15.0 -c -o $O/ios/aot_<name>_ios.o $O/S/aot_<name>.S
```

`gen.py` options: `--family '<fnmatch>'` overrides the family pattern (derived from the soname by
default). `--no-links` drops the static direct links.

### 4. Link

Mac CLI (pure AOT, no executable memory at run time, as on iOS):

```sh
S=$PWD/benchmark/aojit/images/$ABI/latest/S
meson setup build-arm64-aot -Dguest_arch=arm64 -Djit=true -Djit_emit=false -Dbuildtype=release \
    -Dcli_aot=$(ls $S/aot_*.S | paste -sd, -)
ninja -C build-arm64-aot
```

App / IPA:

```sh
OBJS=$(ls $PWD/benchmark/aojit/images/$ABI/latest/ios/aot_*_ios.o | tr '\n' ' ')
xcodebuild -project iSH.xcodeproj -scheme iSH-ARM64 -configuration Release \
    -archivePath build-ipa/iSH-ARM64.xcarchive -allowProvisioningUpdates DEVELOPMENT_TEAM=<team> \
    CURRENT_PROJECT_VERSION=<build> ISH_ENABLE_JIT=YES ISH_JIT_EMIT=NO "ISH_AOT_OBJECTS=$OBJS" archive
xcodebuild -exportArchive -archivePath build-ipa/iSH-ARM64.xcarchive -exportPath build-ipa/export \
    -exportOptionsPlist build-ipa/ExportOptions.plist -allowProvisioningUpdates
```

### 5. Check

```sh
build-arm64-aot/ish -r $R /bin/cat /proc/ish/jit      # every image "ok", none "rejected"
build-arm64-aot/ish -r $R /usr/bin/python3 /tmp/aojit/run_cases.py            # quick tier A/B
build-arm64-aot/ish -r $R /usr/bin/python3 /tmp/aojit/run_cases.py --tier all # everything
AOJIT_SSH=127.0.0.1:2222 benchmark/aojit/verify.sh build-arm64-aot/ish -r $R   # three-way, exit 1 on a difference
benchmark/aojit/compat.sh build-arm64-aot/ish                                   # 227 compatibility tests
```

`verify.sh` and the ssh cases need an sshd in a second ish on a fakefs rootfs:
`build-arm64-jit/ish -f <fakefs> /bin/sh /tmp/aojit/ssh/server.sh 2222 < <client public keys>`
(`guest/ssh/keys.sh` prints them). Without `AOJIT_SSH` the ssh part is skipped.

On the phone: `sh /tmp/phone.sh <host> <repo dir on host>` (see `phone.sh`). Add `OLDLIB=<dir>`
for the family cases.

Reference, 822 on the Mac, quick tier:

| case | speed-up |
|---|---|
| pip list | 2.57× |
| ash loop | 5.15× |
| node log/hash/zlib | 3.11× |
| family cases on the old modules | 1.47–4.20× |

The full tier is 1.4–4.8×. The exceptions are bzip2 (no image) and node.fs.
