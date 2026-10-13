// Native lab: what each way of checking a guest memory access costs on this host, around the same
// load ("add (guest), %rax") and the same push/pop pair. Guest address g lives at base + g.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <windows.h>
static double now(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double) c.QuadPart / f.QuadPart; }
#define N 300000000
#define STR2(x) #x
#define STR(x) STR2(x)
// r12 -> state: 0 site page, 8 site dma, 16 stale, 24 base (xlin), 32 site2 page, 40 site2 dma
uint64_t state[8];
uint64_t g_data, g_stack;   // guest addresses
#define KERNEL(name, body) \
    uint64_t name(void); \
    __asm__(".text\n.globl " #name "\n" #name ":\n push %rbx\n push %rsi\n push %rdi\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n" \
            " lea state(%rip), %r12\n mov g_data(%rip), %rdi\n mov g_stack(%rip), %r8\n mov 24(%r12), %r9\n lea (%rdi,%r9), %rsi\n xor %eax, %eax\n xor %ebx, %ebx\n mov $" STR(N) ", %ecx\n" \
            "1:\n" body "\n dec %ecx\n jnz 1b\n pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rdi\n pop %rsi\n pop %rbx\n ret\n2: ud2\n");
#define SITE(n)  " mov %r13, %r15\n sub " #n "(%r12), %r15\n or 16(%r12), %r15\n cmp $0xff8, %r15\n ja 2b\n mov " #n "+8(%r12), %r9\n"
#define WIN      " lea (%r13), %r15\n shr $12, %r15\n cmp $0xfffff, %r15\n ja 2b\n mov 24(%r12), %r9\n testb $4, -0x100000(%r9,%r15)\n jz 2b\n"
#define WINREG   " lea (%r13), %r15\n shr $12, %r15\n cmp $0xfffff, %r15\n ja 2b\n testb $4, -0x100000(%r9,%r15)\n jz 2b\n"
#define WINCMP   " lea (%r13), %r15\n shr $12, %r15\n cmp $0xfffff, %r15\n ja 2b\n cmpb $0, -0x100000(%r9,%r15)\n je 2b\n"
#define RANGE    " mov %r13, %r15\n shr $32, %r15\n jnz 2b\n"
KERNEL(load_native,  " add (%rsi), %rax")
KERNEL(load_nocheck, " lea (%rdi), %r13\n add (%r13,%r9), %rax")
KERNEL(load_range,   " lea (%rdi), %r13\n" RANGE " add (%r13,%r9), %rax")
KERNEL(load_site,    " lea (%rdi), %r13\n" SITE(0) " add (%r13,%r9), %rax")
KERNEL(load_win,     " lea (%rdi), %r13\n" WIN " add (%r13,%r9), %rax")
KERNEL(load_winreg,  " lea (%rdi), %r13\n" WINREG " add (%r13,%r9), %rax")
KERNEL(load_wincmp,  " lea (%rdi), %r13\n" WINCMP " add (%r13,%r9), %rax")
KERNEL(v_shr,        " lea (%rdi), %r13\n lea (%r13), %r15\n shr $12, %r15\n add (%r13,%r9), %rax")
KERNEL(v_shr_cmp,    " lea (%rdi), %r13\n lea (%r13), %r15\n shr $12, %r15\n cmp $0xfffff, %r15\n ja 2b\n add (%r13,%r9), %rax")
KERNEL(v_test_only,  " lea (%rdi), %r13\n testb $4, -0x100000+0x300(%r9)\n jz 2b\n add (%r13,%r9), %rax")
KERNEL(v_shr_test,   " lea (%rdi), %r13\n lea (%r13), %r15\n shr $12, %r15\n testb $4, -0x100000(%r9,%r15)\n jz 2b\n add (%r13,%r9), %rax")
KERNEL(v_shrx_test,  " lea (%rdi), %r13\n mov $12, %r14d\n shrx %r14, %r13, %r15\n testb $4, -0x100000(%r9,%r15)\n jz 2b\n add (%r13,%r9), %rax")
KERNEL(v_shrx_cmpb,  " lea (%rdi), %r13\n mov $12, %r14d\n shrx %r14, %r13, %r15\n cmpb $0, -0x100000(%r9,%r15)\n je 2b\n add (%r13,%r9), %rax")
KERNEL(v_shrx_movzx, " lea (%rdi), %r13\n mov $12, %r14d\n shrx %r14, %r13, %r15\n movzbl -0x100000(%r9,%r15), %r15d\n test %r15d, %r15d\n jz 2b\n add (%r13,%r9), %rax")
KERNEL(v_mask,       " lea (%rdi), %r13\n movabs $0xffffffff00000000, %r14\n test %r14, %r13\n jnz 2b\n add (%r13,%r9), %rax")
KERNEL(v_rorx_full,  " lea (%rdi), %r13\n rorx $12, %r13, %r15\n mov %r15d, %r15d\n testb $4, -0x100000(%r9,%r15)\n jz 2b\n add (%r13,%r9), %rax")
#define R32      " mov %r13d, %r15d\n cmp %r13, %r15\n jne 2b\n"
#define MASKM    " test %r13, 48(%r12)\n jnz 2b\n"
KERNEL(load_r32,     " lea (%rdi), %r13\n" R32 " add (%r13,%r9), %rax")
KERNEL(load_maskm,   " lea (%rdi), %r13\n" MASKM " add (%r13,%r9), %rax")
#define PUSH(chk) " lea -8(%r8), %r13\n" chk " mov %rax, (%r13,%r9)\n lea -8(%r8), %r8\n"
#define POP(chk)  " mov %r8, %r13\n" chk " mov (%r13,%r9), %rbx\n lea 8(%r8), %r8\n"
KERNEL(pp_nocheck,   PUSH("") POP(""))
KERNEL(pp_range,     PUSH(RANGE) POP(RANGE))
KERNEL(pp_site,      PUSH(SITE(32)) POP(SITE(32)))
KERNEL(pp_win,       PUSH(WIN) POP(WIN))
KERNEL(pp_winreg,    PUSH(WINREG) POP(WINREG))
KERNEL(pp_sitegrp,   PUSH(SITE(32)) " mov %r8, %r13\n mov 40(%r12), %r9\n mov (%r13,%r9), %rbx\n lea 8(%r8), %r8\n")
KERNEL(pp_r32,       PUSH(R32) POP(R32))
KERNEL(pp_maskm,     PUSH(MASKM) POP(MASKM))
KERNEL(pp_native,    " push %rax\n pop %rbx")
static volatile char *bad;
static volatile long nfault;
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    nfault++;
    ep->ContextRecord->Rip += 3;    // the faulting instruction below: movzbl (%rcx), %eax
    return EXCEPTION_CONTINUE_EXECUTION;
}
__attribute__((noinline)) static int touch(volatile char *p) {
    int v = 0;
    __asm__ volatile("movzbl (%%rcx), %%eax" : "=a"(v) : "c"(p));
    return v;
}
typedef PVOID (WINAPI *va2_t)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef PVOID (WINAPI *mv3_t)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
static void os_tests(void) {
    // how long a handled access violation takes
    AddVectoredExceptionHandler(1, veh);
    bad = VirtualAlloc(NULL, 4096, MEM_RESERVE, PAGE_NOACCESS);
    double t = now();
    for (int i = 0; i < 200000; i++) touch(bad);
    printf("exception     %6.2f us each (%ld)\n", (now() - t) * 1e6 / 200000, nfault);
    // commit and decommit, protect
    char *w = VirtualAlloc(NULL, 64 << 20, MEM_RESERVE, PAGE_NOACCESS);
    t = now();
    for (int i = 0; i < 20000; i++) { VirtualAlloc(w + (i % 1000) * 4096, 4096, MEM_COMMIT, PAGE_READWRITE); w[(i % 1000) * 4096] = 1; VirtualFree(w + (i % 1000) * 4096, 4096, MEM_DECOMMIT); }
    printf("commit+touch+decommit a page %6.2f us\n", (now() - t) * 1e6 / 20000);
    // placeholders: a view of a section at 4K granularity, and copy-on-write between two views
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    va2_t va2 = (va2_t) (void *) GetProcAddress(kb, "VirtualAlloc2");
    mv3_t mv3 = (mv3_t) (void *) GetProcAddress(kb, "MapViewOfFile3");
    printf("VirtualAlloc2 %p MapViewOfFile3 %p\n", (void *) va2, (void *) mv3);
    if (!va2 || !mv3) return;
    char *ph = va2(GetCurrentProcess(), NULL, 1 << 20, MEM_RESERVE | 0x00040000 /* MEM_RESERVE_PLACEHOLDER */, PAGE_NOACCESS, NULL, 0);
    printf("placeholder %p err %lu\n", ph, GetLastError());
    HANDLE sec = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 1 << 20, NULL);
    for (int off = 4096; off <= 65536; off *= 16) {
        // split the placeholder so that [ph+off, ph+off+4096) is its own
        BOOL s1 = VirtualFree(ph + off, 4096, MEM_RELEASE | 0x00000002 /* MEM_PRESERVE_PLACEHOLDER */);
        DWORD e1 = GetLastError();
        char *v = mv3(sec, GetCurrentProcess(), ph + off, off, 4096, 0x00004000 /* MEM_REPLACE_PLACEHOLDER */, PAGE_READWRITE, NULL, 0);
        printf("view at +%d (4K, offset %d): split %d err %lu, map %p err %lu\n", off, off, s1, e1, v, GetLastError());
        if (v) { v[0] = 42; }
    }
    // the same page copy-on-write somewhere else
    char *cow = MapViewOfFile(sec, FILE_MAP_COPY, 0, 0, 65536 * 2);
    char *rw = MapViewOfFile(sec, FILE_MAP_WRITE, 0, 0, 65536 * 2);
    if (cow && rw) { rw[65536] = 7; char a = cow[65536]; cow[65536] = 9; printf("cow view: saw %d, after its own write %d, the shared one still %d\n", a, cow[65536], rw[65536]); }
    t = now();
    for (int i = 0; i < 2000; i++) { char *m = MapViewOfFile(sec, FILE_MAP_COPY, 0, 0, 1 << 20); m[0] = 1; UnmapViewOfFile(m); }
    printf("map+touch+unmap a 1 MB copy-on-write view %6.2f us\n", (now() - t) * 1e6 / 2000);
}
int main(void) {
    os_tests();
    // 1 MB map, then a 16 MB window
    char *r = VirtualAlloc(NULL, (1 << 20) + (16 << 20), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    char *base = r + (1 << 20);
    memset(r, 0xf, 1 << 20);
    g_data = 0x300000 + 0x100; g_stack = 0x800000 + 0x800;
    *(uint64_t *) (base + g_data) = 3;
    state[0] = g_data & ~0xfffull; state[1] = (uint64_t) base; state[2] = 0; state[3] = (uint64_t) base;
    state[4] = (g_stack - 8) & ~0xfffull; state[5] = (uint64_t) base; state[6] = 0xffffffff00000000ull;
    static const struct { const char *name; uint64_t (*fn)(void); } ks[] = {
#define K(n) {#n, n}
        K(load_native), K(load_nocheck), K(load_range), K(load_site), K(load_win), K(load_winreg), K(load_wincmp), K(v_shr), K(v_shr_cmp), K(v_test_only), K(v_shr_test), K(v_shrx_test), K(v_shrx_cmpb), K(v_shrx_movzx), K(v_mask), K(v_rorx_full), K(load_r32), K(load_maskm),
        K(pp_native), K(pp_nocheck), K(pp_range), K(pp_site), K(pp_sitegrp), K(pp_win), K(pp_winreg), K(pp_r32), K(pp_maskm),
    };
    for (int rep = 0; rep < 3; rep++)
        for (unsigned i = 0; i < sizeof(ks) / sizeof(ks[0]); i++) {
            double t = now();
            uint64_t v = ks[i].fn();
            printf("%-13s %6.3f %llx\n", ks[i].name, (now() - t) * 1e9 / N, (unsigned long long) (v & 0xf));
            fflush(stdout);
        }
    return 0;
}
