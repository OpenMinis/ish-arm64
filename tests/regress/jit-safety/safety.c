// Include real upstream functions with only Apple APIs/context declarations adapted.
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
static size_t dump_sizes[4], dump_calls;
static uint64_t dump_length;
static size_t capture_fwrite(const void *p, size_t size, size_t count, FILE *f) {
    (void) f;
    assert(dump_calls < 4);
    dump_sizes[dump_calls] = size * count;
    if (dump_calls == 1) dump_length = *(const uint64_t *)p;
    dump_calls++;
    return count;
}
#define fwrite capture_fwrite
#include "jit-adapted.c"
#undef fwrite

static unsigned char seen[1024];
static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;
static void *allocate_worker(void *arg) {
    (void) arg;
    for (;;) {
        uint8_t *p = region_alloc(1);
        if (!p) break;
        size_t offset = p - region;
        assert(offset >= REGION_SIZE - sizeof(seen) * 16);
        unsigned slot = (offset - (REGION_SIZE - sizeof(seen) * 16)) / 16;
        assert(slot < sizeof(seen) && !(offset & 15));
        pthread_mutex_lock(&seen_lock);
        assert(!seen[slot]); seen[slot] = 1;
        pthread_mutex_unlock(&seen_lock);
    }
    return NULL;
}
static void allocator_test(void) {
    region = mmap(NULL, REGION_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(region != MAP_FAILED);
    region_used = REGION_SIZE;
    assert(region_alloc(16) == NULL);
    assert(region_used == REGION_SIZE); // fails on frozen upstream
    assert(region_alloc(SIZE_MAX) == NULL && region_used == REGION_SIZE);
    region_used = 0;
    assert(region_alloc(SIZE_MAX) == NULL && region_used == 0);
    assert(region_alloc(0) == NULL && region_used == 0);
    assert(region_alloc(1) == region && region_used == 16);
    assert(region_alloc(17) == region + 16 && region_used == 48);
    region_used = REGION_SIZE - 16;
    assert(region_alloc(16) == region + REGION_SIZE - 16);
    for (int i = 0; i < 1000; i++) assert(region_alloc(1) == NULL);
    assert(region_used == REGION_SIZE);
    region_used = REGION_SIZE - sizeof(seen) * 16;
    pthread_t threads[8];
    for (int i = 0; i < 8; i++) assert(!pthread_create(&threads[i], NULL, allocate_worker, NULL));
    for (int i = 0; i < 8; i++) assert(!pthread_join(threads[i], NULL));
    for (unsigned i = 0; i < sizeof(seen); i++) assert(seen[i] == 1);
    assert(region_used == REGION_SIZE);
    munmap(region, REGION_SIZE); region = NULL; region_used = 0;
    assert(region_alloc(16) == NULL && region_used == 0);
    puts("jit-region-ok: overflow, alignment, exhaustion, null, 8-thread unique reservations");
}
static void sync_test(void) {
    pin_init(true);
    struct cpu_state cpu = {0};
    probe_mcontext mc = {0}; probe_ucontext uc = {.uc_mcontext = &mc};
    struct { struct aot_trans trans; struct aot_loop loop; } meta = {0};
    struct aot_module img = {.text_start=(void *)0x10000, .text_end=(void *)0x20000,
                             .trans=&meta.trans, .ntrans=1};
    const struct aot_module *images[] = {&img}; aot_images=images; aot_nimages=1; region=NULL;
    mc.__ss.__pc=0x11000; mc.__ss.__x[1]=(uintptr_t)&cpu;
    for(int i=0;i<n_pinned;i++)mc.__ss.__x[pin_host[i]]=0x12340000+i;
    mc.__ss.__x[H_CYC]=0x1234567800000027ull;
    cpu.cycle=0x7654321000000004ul; // str w15 must preserve the field's upper bytes
    assert(jit_crash_sync(&uc)); // fails on frozen AOT-only guard
    for(int i=0;i<n_pinned;i++)assert(*(uint64_t *)((char *)&cpu+greg_off(pin_guest[i]))==0x12340000u+i);
    assert(cpu.cycle==0x7654321000000027ul);
    // Promoted x8 uses pinned x0's host register; x0's saved value is its donor.
    meta.trans.loop=(int32_t)((char *)&meta.loop-(char *)&meta.trans.loop);
    meta.loop=(struct aot_loop){.lo=(void *)0x10800,.hi=(void *)0x11800,.n=1,
                               .g={8},.d={0},.h={19}};
    cpu.regs[0]=0xfedc; mc.__ss.__x[19]=0xabcd;
    assert(jit_crash_sync(&uc)); assert(cpu.regs[0]==0xfedc && cpu.regs[8]==0xabcd);
    // Outside images and at the exclusive end boundary: never dereference x1.
    mc.__ss.__x[1]=1;
    mc.__ss.__pc=1; assert(!jit_crash_sync(&uc));
    mc.__ss.__pc=0x20000; assert(!jit_crash_sync(&uc));
    mc.__ss.__pc=0x11000; pin_init(false); assert(!jit_crash_sync(&uc));
    pin_init(true); aot_nimages=0; region=(void *)0x10000000;
    mc.__ss.__pc=(uintptr_t)region+0x100; mc.__ss.__x[1]=(uintptr_t)&cpu;
    assert(jit_crash_sync(&uc)); assert(cpu.regs[0]==0xabcd);
    assert(cpu.cycle==0x7654321000000027ul);
    region=NULL; aot_images=NULL;
    puts("jit-sync-ok: AOT-only/native pinned, cycle, promotion/donor, unrelated/end PC, pin-off");
}
static void dump_test(void) {
    // Intercept fwrite so a negative test cannot actually read beyond a mapping.
    jit_on=true; region=(void *)0x10000000; region_used=REGION_SIZE+64;
    setenv("ISH_JIT_DUMP", "/dev/null", 1);
    jit_report();
    assert(dump_length==REGION_SIZE && dump_sizes[2]==REGION_SIZE);
    assert(dump_calls==3);
    dump_calls=0; region=NULL; region_used=64;
    jit_report(); assert(dump_length==0 && (dump_calls==2 || dump_sizes[2]==0));
    dump_calls=0; region=(void *)0x10000000; region_used=32;
    jit_report();assert(dump_length==32 && dump_sizes[2]==32);
    puts("jit-dump-ok: bounded length, no-region zero length, normal length");
}
int main(int argc, char **argv) {
    assert(argc==2);
    unsetenv("ISH_JIT_MAP");unsetenv("ISH_JIT_STATS");
    if (!strcmp(argv[1],"allocator")) allocator_test();
    else if (!strcmp(argv[1],"sync")) sync_test();
    else if (!strcmp(argv[1],"dump")) dump_test();
    else abort();
}
