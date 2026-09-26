# AOT image cookbook

Recorded AOT images live here but are not committed (see `.gitignore`: everything under
`benchmark/aojit/images/` except this file). They are large (the set below is about 2 GB with
recordings) and tied to one ish build, so regenerate them instead of sharing them.

## Layout

```
images/
└── <abi>/                 abi fingerprint of the ish that recorded them (/proc/ish/jit: "abi xxxxxxxx")
    ├── latest/            recorded on the current package versions
    │   ├── S/aot_<name>.S     images (tools/jit_aot/gen.py output), for -Dcli_aot
    │   ├── ios/aot_<name>_ios.o   the same, assembled for iOS, for ISH_AOT_OBJECTS
    │   └── rec/<name>.jsonl   raw recordings (ISH_JIT_RECORD); gen.py turns them into .S again
    └── old/               recorded on older package versions (family-matching A/B)
        ├── S/ …
        └── rec/ …
```

`<name>` and what each image covers are in `../images.json` (module substring, recording
workload, packages).

An image only loads into an ish with the same abi (`jit_abi()`: `JIT_CODE_VERSION` in
`asbestos/guest-arm64/jit.c`, the gadget conventions, the struct layouts). Any other ish lists it as
`rejected` in `/proc/ish/jit`. After a change to `jit.c`, `gen.c`, the gadgets or the offsets, record
again into a new `<abi>/` directory. Within one abi, an image serves its exact module (build-id) and,
through family matching, other versions of it (`libz.so.1*`, …).

## Current set: abi `daf8fcc7` (JIT_CODE_VERSION 7, branch feat/aot-family-match, build 822)

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

Rootfs used: `latest` = `alpine-arm64-321-latest` (musl, busybox, zlib, python, node) and
`alpine-arm64-321-full` (the other seven). `old` = `alpine-arm64-321-old`. All three are in the main checkout,
`/Users/ethan/Src/github.com/ish-arm64/` (untracked). The build 822 IPA links all 12 `latest/ios/*.o`.

The musl, busybox, zlib, python and node images of this set were recorded with the workloads
before they moved into `../guest/`. Those workloads had the same scripts under `/tmp/p0`, `/tmp/p3`,
`/tmp/p5`, `/tmp/p6` and `/tmp/pz`, so a new recording differs only in noise.

## Regenerate

All commands run from the repo root (`ish-arm64-ios-opt`). `R` is a rootfs directory and `ABI`
is the abi of the recording build.

### 1. Recording build: JIT with run-time translation

```sh
meson setup build-arm64-jit -Dguest_arch=arm64 -Djit=true -Djit_emit=true -Dbuildtype=release
ninja -C build-arm64-jit
build-arm64-jit/ish -r $R /bin/cat /proc/ish/jit | grep abi     # -> ABI
```

### 2. Rootfs with the packages and the suite

```sh
build-arm64-jit/ish -r $R /sbin/apk add -u $(python3 -c "import json; print(' '.join(json.load(open('benchmark/aojit/images.json'))['packages']['apk']))")
build-arm64-jit/ish -r $R /usr/bin/pip install --break-system-packages python-pptx
benchmark/aojit/install.sh build-arm64-jit/ish $R [old rootfs]   # suite -> $R/tmp/aojit, inputs
```

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
```

On the phone: `sh /tmp/phone.sh <host> <repo dir on host>` (see `../phone.sh`). Add `OLDLIB=<dir>`
for the family cases.

Reference, 822 on the Mac, quick tier:

| case | speed-up |
|---|---|
| pip list | 2.57× |
| ash loop | 5.15× |
| node log/hash/zlib | 3.11× |
| family cases on the old modules | 1.47–4.20× |

The full tier is 1.4–4.8×. The exceptions are bzip2 (no image) and node.fs.
