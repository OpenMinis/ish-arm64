// Actual emitter + actual CLI handler. Linux signal ABI is adapted only at the
// boundary; recovery returns through a test trampoline with the same exit-SP ABI.
#include "jit-adapted.c"
#include <assert.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <sys/wait.h>
__thread struct fiber_frame *jit_active_frame;
__thread volatile sig_atomic_t in_jit;
__thread volatile uint64_t jit_saved_pc;
__thread volatile uint64_t jit_last_host_fault, jit_last_x7, jit_last_x10;
__thread volatile int jit_crash_count;
extern void jit_crash_trampoline(void);
#include "cli-recovery.c"
#include "emu/interrupt.h"
// INT_GPF fixup is not reached on native retry; fail if that contract changes.
static void fiber_fix_fault_pc(struct asbestos *a, struct fiber_frame *f, struct tlb *t) {
    (void)a;(void)f;(void)t;abort();
}
#include "dispatch-recovery.c"
#undef ucontext_t
extern int restart_call(void *,struct fiber_frame *,void *,void *);
static volatile sig_atomic_t faults;
static void signal_adapter(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *linux_uc=ctx; probe_mcontext mc={0}; probe_ucontext uc={.uc_mcontext=&mc};
    for(int i=0;i<29;i++)mc.__ss.__x[i]=linux_uc->uc_mcontext.regs[i];
    mc.__ss.__fp=linux_uc->uc_mcontext.regs[29];mc.__ss.__lr=linux_uc->uc_mcontext.regs[30];
    mc.__ss.__pc=linux_uc->uc_mcontext.pc;mc.__ss.__sp=linux_uc->uc_mcontext.sp;
    // ESR deliberately wrong: native recovery must use checkpoint direction.
    mc.__es.__esr=0;
    faults++;
    crash_handler(sig,si,&uc);
    linux_uc->uc_mcontext.pc=mc.__ss.__pc;linux_uc->uc_mcontext.sp=mc.__ss.__sp;
}
static void init_em(struct em *e) {
    memset(e,0,sizeof(*e));memset(e->host,-1,sizeof(e->host));memset(e->owner,-1,sizeof(e->owner));
    memcpy(e->pin,pin_of,sizeof(e->pin));
}
static void save_pins(struct em *e) {
    for(int g=0;g<=G_SP;g++)if(e->pin[g]>=0)
        put(e,0xf9000000u|((greg_off(g)/8)<<10)|(1u<<5)|e->pin[g]);
}
static void *executable(struct em *e) {
    // The tested access must be a TLB HIT; a bailout traps instead of hiding it.
    unsigned trap=e->n;put(e,0);
    for(unsigned i=0;i<e->nfix;i++) {
        assert(e->fix[i].kind==1||e->fix[i].kind==2);
        e->buf[e->fix[i].at]=0x54000000u|((trap-e->fix[i].at)<<5)|(e->fix[i].kind==1?1u:8u);
    }
    void *p=mmap(NULL,65536,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p!=MAP_FAILED);
    memcpy(p,e->buf,e->n*4);__builtin___clear_cache(p,(char *)p+e->n*4);
    assert(!mprotect(p,65536,PROT_READ|PROT_EXEC));return p;
}
static void emit(struct em *e,uint32_t w,uint64_t pc) {bool mem;assert(emit_insn(e,w,pc,0,&mem));assert(!e->fail);}
static unsigned cases;
static void run_case(unsigned mode,uint32_t op,bool store,unsigned bytes,bool wb,bool post,bool vector,bool second_unit) {
    // modes: unpinned, pinned, promoted-loop, PIC moved/AOT-only,
    // pinned across a separate native code allocation.
    bool pic=mode==3,chain=mode==4;pic_on=pic;pin_init(mode!=0);simd_on=true;
    uint64_t guest_pc=pic?0x501234:0x1004;
    struct fiber_frame frame={0};frame.cpu.regs[0]=7;frame.cpu.regs[4]=0x4000+(post?0:wb?0:32);
    frame.cpu.regs[5]=0x5000;frame.cpu.regs[3]=0x1122334455667788ull;frame.cpu.regs[6]=0x8877665544332211ull;
    frame.cpu.regs[8]=0x2468;frame.cpu.regs[19]=0x1357;frame.cpu.cycle=0x123400000015ul;
    frame.cpu.nzcv=0xa0000000;frame.fault_stream=0xbad;
    memset(frame.cpu.fp[0].b,0x37,16);memset(frame.cpu.fp[1].b,0x49,16);
    char *target=mmap(NULL,4096,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0),*side=mmap(NULL,4096,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(target!=MAP_FAILED && side!=MAP_FAILED);memset(target,0x5a,4096);memset(side,0,4096);
    struct mmu mmu={0};struct tlb *tlb=calloc(1,sizeof(*tlb));tlb->mmu=&mmu;
    for(int i=0;i<2;i++){unsigned va=0x4000+i*0x1000,ix=((va>>12)^(va>>25))&8191;
        tlb->entries[ix].page=tlb->entries[ix].page_if_writable=va;tlb->entries[ix].gen=0;
        tlb->entries[ix].data_minus_addr=(uintptr_t)(i?side:target)-va;}
    // One PIC slot: recorded PC is shifted to a different module/base.
    char storage[CTX_FAR+CTX_BLK+CTX_SLOT]={0};struct jit_ctx *ctx=(void *)(storage+CTX_FAR);
    ctx->slot[0].base=0x500000;
    struct em e;init_em(&e);e.idx=0;e.base=0x1000;
    if(n_pinned)load_pinned(&e);
    if(mode==2){e.loop=true;e.npromo=1;e.promo_g[0]=8;e.donor_g[0]=19;e.promo_h[0]=pin_of[19];
        e.pin[19]=-1;e.pin[8]=pin_of[19];emit_promote(&e);}
    emit(&e,0xb1000400,guest_pc-4); // adds x0,x0,#1 (must happen once, NZCV=0)
    emit(&e,0x91000908,guest_pc-4); // add x8,x8,#2 (promoted value must be saved)
    emit(&e,0xf90000a0,guest_pc-4); // str x0,[x5] (earlier memory side effect)
    if(n_pinned)put(&e,0x11000def); // add w15,w15,#3: live cycle differs from backing word
    if(second_unit)emit(&e,0xd2800aa7,guest_pc-4); // preceding MOVZ in a fused unit
    uint64_t record_pc=pic?e.base+0x1234:guest_pc;
    struct em prefix;
    if(chain){prefix=e;init_em(&e);} // direct chain preserves pinned host state
    emit(&e,op,record_pc); // access; PC must point here, not the unit/block
    if(mode==2)emit_canon(&e);
    save_pins(&e);put(&e,0xd65f03c0);
    void *code=executable(&e),*entry=code;
    if(chain){ // branch from prefix mapping, not through the C caller
        put(&prefix,0x58000050);put(&prefix,0xd61f0200); // ldr x16,+8; br x16
        put(&prefix,(uintptr_t)code);put(&prefix,(uintptr_t)code>>32);
        entry=executable(&prefix);
    }
    struct aot_module image={.text_start=(uint32_t *)code,.text_end=(uint32_t *)code+e.n};const struct aot_module *images[]={&image};
    region=pic?NULL:code;aot_images=pic?images:NULL;aot_nimages=pic?1:0;
    assert(!mprotect(target,4096,PROT_NONE));jit_active_frame=&frame;in_jit=1;jit_saved_pc=guest_pc-0x100;
    int oldfaults=faults;int result=restart_call(entry,&frame,tlb->entries,ctx);in_jit=0;
    assert(result==0x100 && faults==oldfaults+1);
    assert(frame.cpu.pc==guest_pc && frame.cpu.segfault_precise_pc==guest_pc);
    unsigned access_off=post?0:wb?16:32;
    assert(frame.cpu.segfault_addr==0x4000+access_off && frame.cpu.segfault_was_write==store);
    assert(frame.cpu.regs[0]==8 && *(uint64_t *)side==8);
    assert(frame.cpu.regs[4]==0x4000+(post?0:wb?0:32));
    assert(frame.cpu.regs[8]==0x246a && frame.cpu.regs[19]==0x1357);
    assert(frame.cpu.nzcv==0 && !frame.fault_stream);
    if(second_unit)assert(frame.cpu.regs[7]==0x55);
    assert(frame.cpu.cycle==(n_pinned?0x123400000018ul:0x123400000015ul));
    assert(frame.cpu.regs[3]==0x1122334455667788ull && frame.cpu.regs[6]==0x8877665544332211ull);
    for(unsigned i=0;i<16;i++){assert(frame.cpu.fp[0].b[i]==0x37);assert(frame.cpu.fp[1].b[i]==0x49);}
    assert(!frame.native_fault_host_pc);
    struct asbestos asbestos={.invalidate_gen=123};unsigned retries=0;
    frame.last_block=(void *)1;frame.ret_cache[2]=1;tlb->block_cache[2]=(void *)1;
    assert(dispatch_after_fiber(result,&frame,tlb,&asbestos,&retries)==INT_NONE);
    assert(retries==1 && frame.cpu.pc==guest_pc && frame.cpu.segfault_precise_pc==guest_pc);
    assert(!frame.last_block && !frame.ret_cache[2] && !tlb->block_cache[2] && tlb->block_cache_gen==123);
    for(unsigned i=0;i<TLB_SIZE;i++)assert(tlb->entries[i].page==1 && tlb->entries[i].page_if_writable==1);
    // Model translation refill; the actual TLB invalidation above must happen
    // before retry. Also exercise the existing bounded-escalation policy.
    retries=15;assert(dispatch_after_fiber(INT_JIT_CRASH,&frame,tlb,&asbestos,&retries)==INT_GPF);
    assert(!retries && frame.cpu.pc==guest_pc && frame.cpu.segfault_precise_pc==guest_pc);
    for(int i=0;i<2;i++){unsigned va=0x4000+i*0x1000,ix=((va>>12)^(va>>25))&8191;
        tlb->entries[ix].page=tlb->entries[ix].page_if_writable=va;tlb->entries[ix].gen=0;
        tlb->entries[ix].data_minus_addr=(uintptr_t)(i?side:target)-va;}
    assert(!mprotect(target,4096,3));
    // Resume at EXACT guest instruction through the actual emitter again.
    // This is the dispatch contract, not a rewind of the previous host sequence.
    struct em r;init_em(&r);r.idx=0;r.base=0x1000;if(n_pinned)load_pinned(&r);
    emit(&r,op,record_pc);save_pins(&r);put(&r,0xd65f03c0);void *retry=executable(&r);
    if(pic){image.text_start=(uint32_t *)retry;image.text_end=(uint32_t *)retry+r.n;}else region=retry;
    in_jit=1;result=restart_call(retry,&frame,tlb->entries,ctx);in_jit=0;
    assert(result==0 && faults==oldfaults+1 && !frame.native_fault_host_pc);
    assert(frame.cpu.regs[0]==8 && *(uint64_t *)side==8);
    assert(dispatch_after_fiber(INT_NONE,&frame,tlb,&asbestos,&retries)==INT_NONE);
    assert(!frame.cpu.segfault_precise_pc && !retries);
    if(wb)assert(frame.cpu.regs[4]==0x4010);
    if(vector){if(store)assert(!memcmp(target+access_off,frame.cpu.fp[0].b,bytes));
        else for(unsigned i=0;i<bytes;i++)assert(((unsigned char *)frame.cpu.fp)[i]==0x5a);}
    else if(store){assert(!memcmp(target+access_off,&frame.cpu.regs[3],bytes>8?8:bytes));
        if(bytes==16)assert(!memcmp(target+access_off+8,&frame.cpu.regs[6],8));}
    else {if(bytes>=8)assert(frame.cpu.regs[3]==0x5a5a5a5a5a5a5a5aull);
        if(bytes==16)assert(frame.cpu.regs[6]==0x5a5a5a5a5a5a5a5aull);}
    if(chain)munmap(entry,65536);
    munmap(code,65536);munmap(retry,65536);munmap(target,4096);munmap(side,4096);free(tlb);
    region=NULL;aot_images=NULL;aot_nimages=0;jit_active_frame=NULL;cases++;
}
static void rejected_pc_test(void) {
    for(unsigned aot=0;aot<2;aot++) {
        pid_t p=fork();assert(p>=0);
        if(!p) {
            struct fiber_frame frame={0};jit_active_frame=&frame;in_jit=1;
            // A stale armed checkpoint must not authorise any other native PC.
            frame.native_fault_guest_pc=0x1234;
            struct aot_module image={0};
            const struct aot_module *images[]={&image};
            region=NULL;aot_images=aot?images:NULL;aot_nimages=aot;
            // Deliberate host access to zero, not a checkpointed guest access.
            uint32_t *bad=mmap(NULL,4096,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(bad!=MAP_FAILED);
            bad[0]=0xd2800009;bad[1]=0xf9400120;bad[2]=0xd65f03c0;
            __builtin___clear_cache((char *)bad,(char *)(bad+3));assert(!mprotect(bad,4096,5));
            if(aot){image.text_start=bad;image.text_end=bad+3;}else region=(void *)bad;
            frame.native_fault_host_pc=(uintptr_t)bad+8;
            close(STDERR_FILENO);
            restart_call(bad,&frame,NULL,NULL);
            _exit(77); // must be diagnostic fail-stop, never trampoline/retry
        }
        int status;assert(waitpid(p,&status,0)==p);assert(WIFEXITED(status)&&WEXITSTATUS(status)==139);
    }
    puts("jit-restart-reject-ok: native/AOT unmatched PC fails closed");
}
#include "prior-abi.h"
static void abi_test(void) {
    pin_init(true);pic_on=true;
    assert(prior_jit_abi()!=jit_abi());
    struct aot_module old={.abi=prior_jit_abi(),.prologue_words=prologue_words,.entry_off=entry_off(),.n_pinned=n_pinned,.path="old"};
    struct aot_module current=old;current.abi=jit_abi();current.path="current";
    aot_nregistered=aot_nimages=aot_nrejected=0;ish_aot_register(&old);ish_aot_register(&current);aot_init();
    assert(aot_nimages==1 && aot_images[0]==&current && aot_nrejected==1);free(aot_images);aot_images=NULL;aot_nimages=0;
}
int main(int argc, char **argv) {
    unsigned first_mode=0,last_mode=5;
    if(argc==2){first_mode=(unsigned)atoi(argv[1]);assert(first_mode<5);last_mode=first_mode+1;}
    struct sigaction sa={.sa_sigaction=signal_adapter,.sa_flags=SA_SIGINFO};sigemptyset(&sa.sa_mask);assert(!sigaction(SIGSEGV,&sa,NULL));
    for(unsigned mode=first_mode;mode<last_mode;mode++)for(unsigned fused=0;fused<2;fused++) {
        run_case(mode,0xf9400083,false,8,false,false,false,fused); // ldr x3,[x4]
        run_case(mode,0xf9000083,true,8,false,false,false,fused); // str x3,[x4]
        run_case(mode,0xf8410c83,false,8,true,false,false,fused); // ldr x3,[x4,#16]!
        run_case(mode,0xf8010483,true,8,true,true,false,fused); // str x3,[x4],#16
        run_case(mode,0xa9401883,false,16,false,false,false,fused); // ldp x3,x6,[x4]
        run_case(mode,0xa9001883,true,16,false,false,false,fused); // stp x3,x6,[x4]
        run_case(mode,0x3dc00080,false,16,false,false,true,fused); // ldr q0,[x4]
        run_case(mode,0x3d800080,true,16,false,false,true,fused); // str q0,[x4]
        run_case(mode,0xad400480,false,32,false,false,true,fused); // ldp q0,q1,[x4]
        run_case(mode,0xad000480,true,32,false,false,true,fused); // stp q0,q1,[x4]
    }
    rejected_pc_test();abi_test();printf("jit-restart-ok cases=%u actual faults, no duplicate prefix, exact PC/address/writeback, PIC/AOT relocation\n",cases);
}
