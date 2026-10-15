# Guest JIT pattern: generate code in anonymous RWX memory, run it many times,
# rewrite it in place, run again. Exercises anonymous-module translation,
# chaining inside JIT'd code and invalidation on code writes.
import ctypes, mmap, struct
buf = mmap.mmap(-1, 65536, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
addr = ctypes.addressof(ctypes.c_char.from_buffer(buf))

def emit(off, words):
    buf[off:off + 4 * len(words)] = struct.pack('<%dI' % len(words), *words)

def loop_fn(k):
    # x0 = n; acc = 0; do { acc += i * k; } while (--n); return acc
    return [0xD2800001,                      # mov x1, #0          (acc)
            0xD2800002 | (k << 5),           # mov x2, #k
            0x9B020403,                      # madd x3, x0, x2, x1 -> x3 = x0*k + x1
            0xAA0303E1,                      # mov x1, x3
            0xF1000400,                      # subs x0, x0, #1
            0x54FFFFA1,                      # b.ne -3 (madd)
            0xAA0103E0,                      # mov x0, x1
            0xD65F03C0]                      # ret

total = 0
for gen in range(6):
    for slot in range(8):                    # 8 functions per page, rewritten every generation
        k = gen * 8 + slot + 1
        emit(slot * 256, loop_fn(k))
    fns = [ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_uint64)(addr + slot * 256) for slot in range(8)]
    for rep in range(200):
        for slot, f in enumerate(fns):
            n = 50 + rep
            k = gen * 8 + slot + 1
            got = f(n)
            want = k * n * (n + 1) // 2
            if got != want:
                raise SystemExit(f"❌ gen {gen} slot {slot} n {n}: got {got} want {want}")
            total += got
print("SELFMOD OK", total)
