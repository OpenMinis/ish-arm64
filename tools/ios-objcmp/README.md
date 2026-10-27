# iOS object comparison

Proves a change leaves iOS untouched: build the iOS libraries of a reference
tree and of the changed tree, then compare code, data, symbols and
relocations of every object.

```bash
# the reference: the tree iOS ships, e.g. integ/stage2-on-depsish merged with deps/ish's commit
T=$(git merge-tree --write-tree <ref-a> <ref-b> | head -1)    # or just a commit
mkdir -p /tmp/ioscmp/s && git archive $T | tar -x -C /tmp/ioscmp/s
tools/ios-objcmp/mkios.sh /tmp/ioscmp/s /tmp/ioscmp/b
mkdir /tmp/ioscmp/ref && (cd /tmp/ioscmp/b && find . -name '*.o' | cpio -pdm ../ref)
# the changed tree, at the SAME path (__FILE__ strings)
rm -rf /tmp/ioscmp/s && mkdir /tmp/ioscmp/s && git archive HEAD | tar -x -C /tmp/ioscmp/s
find /tmp/ioscmp/s -name '*.[chS]' -exec touch {} +
tools/ios-objcmp/mkios.sh /tmp/ioscmp/s /tmp/ioscmp/b
tools/ios-objcmp/cmp.sh /tmp/ioscmp/ref /tmp/ioscmp/b   # expect "same N, differ 0"
```

Objects only in the changed tree (kernel/hvf.c.o, sba.c.o, pmu.c.o) must
be empty on iOS: `nm` shows only `ltmp0`.
