// Include the upstream emitter with only the platform adapter changed.
#include "jit-adapted.c"
#undef ucontext_t
#include <assert.h>
#include <signal.h>
#include <unistd.h>
extern void probe_call(void *code, struct cpu_state *cpu, void *tlb);
static void init_em(struct em *e) {
 memset(e,0,sizeof(*e)); memset(e->host,-1,sizeof(e->host)); memset(e->owner,-1,sizeof(e->owner));
 memcpy(e->pin,pin_of,sizeof(e->pin));
}
static void *executable(struct em *e) {
 // Resolve upstream bailout placeholders to a test-only trap after RET.
 // A correct TLB hit must never reach it; a miss is not silently ignored.
 unsigned trap=e->n; put(e,0);
 for(unsigned i=0;i<e->nfix;i++) {
  assert(e->fix[i].kind==1 || e->fix[i].kind==2);
  e->buf[e->fix[i].at]=0x54000000u | ((trap-e->fix[i].at)<<5) | (e->fix[i].kind==1?1u:8u);
 }
 size_t size=65536;
 uint32_t *p=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); assert(p!=MAP_FAILED);
 memcpy(p,e->buf,e->n*4); __builtin___clear_cache((char *)p,(char *)(p+e->n));
 assert(mprotect(p,size,PROT_READ|PROT_EXEC)==0); return p;
}
static void saturation_probe(void) {
 pin_init(false); simd_on=true; pic_on=false;
 // A table of integer/vector emit cases to check acceptance, flags and results.
 struct cpu_state cpu={0}; struct em e; init_em(&e); bool mem;
 assert(emit_insn(&e,0x91000400,0x1000,0,&mem)); // add x0,x0,#1
 assert(emit_insn(&e,0xd1000401,0x1004,1,&mem)); // sub x1,x0,#1
 assert(emit_insn(&e,0xab000022,0x1008,2,&mem)); // adds x2,x1,x0
 assert(emit_insn(&e,0x9a9f17e3,0x100c,3,&mem)); // cset x3,eq
 put(&e,0xd65f03c0); void *code=executable(&e);
 cpu.regs[0]=41; probe_call(code,&cpu,NULL);
 printf("arithmetic x0=%lu x1=%lu x2=%lu x3=%lu\n",cpu.regs[0],cpu.regs[1],cpu.regs[2],cpu.regs[3]);
 assert(cpu.regs[0]==42 && cpu.regs[1]==41 && cpu.regs[2]==83 && cpu.regs[3]==0); munmap(code,65536);
}
static void simd_filter_probe(void) {
 uint32_t ops[]={0x4e220c20,0x6e220c20,0x4e207820,0x6e207820,0x0e214820,0x2e214820,0x6f096420,0x4f097420,0x6f097420,0x0f0f9c20,0x2f0f8420,0x0f0f9420,0x6e203820,0x4e203820};
 pin_init(false); simd_on=true;
 unsigned rejected=0; for(unsigned i=0;i<sizeof(ops)/sizeof(*ops);i++) { struct em e;init_em(&e);bool mem; rejected+=!emit_insn(&e,ops[i],0x1000,0,&mem); }
 printf("SIMD saturation filter rejected=%u/14 (QC-changing forms fall back)\n",rejected);assert(rejected==14);
 struct em e;init_em(&e); bool mem; assert(emit_insn(&e,0x4e226420,0x1000,0,&mem)); put(&e,0xd65f03c0);
 struct cpu_state cpu={0}; memset(cpu.fp[1].b,42,16);memset(cpu.fp[2].b,43,16);void *p=executable(&e);probe_call(p,&cpu,NULL);
 for(int i=0;i<16;i++)assert(cpu.fp[0].b[i]==43);munmap(p,65536); puts("SIMD smax 16/16 lanes match");
}
static void slot_probe(void) {
 pic_on=true;struct fiber_block old={0}, replacement={0}; struct reg_entry r={.idx=0};
#ifdef PROBE_FAMILY
 // The newer branch's slot claims reject a second live owner.
 char *storage=calloc(1,CTX_FAR+CTX_BLK+CTX_SLOT);
 struct jit_ctx *ctx=(void *)(storage+CTX_FAR); ctx->slot[0].blk=&old;
 reg_install(&replacement,ctx,&r); assert(ctx->slot[0].blk==&old && replacement.jit_ctx==NULL);
 puts("context ownership: second live owner rejected (family tip)");free(storage);
#else
 struct jit_ctx *ctx=calloc(1,sizeof(*ctx)+8);ctx->blk[0]=&old;
 reg_install(&replacement,ctx,&r);assert(ctx->blk[0]==&replacement);
 puts("context ownership: second live owner overwrites first (native tip)");free(ctx);
#endif
 pic_on=false;
}
#include "differential-ops.h"
extern uint64_t reference_call(void *,struct cpu_state *,unsigned);
static uint64_t rng=0xa6cf98e193ull;
static uint64_t random64(void) {rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
static void differential_probe(void) {
 _Static_assert(offsetof(struct cpu_state,regs)==16,"reference ABI");
 unsigned cases=0;
 for(unsigned pinned=0;pinned<2;pinned++) for(unsigned i=0;i<sizeof(diff_ops)/sizeof(*diff_ops);i++) {
  uint32_t w=diff_ops[i];pin_init(pinned);pic_on=false;struct em e;init_em(&e);bool mem;
  if(pinned)load_pinned(&e); assert(emit_insn(&e,w,0x1000,0,&mem));assert(!e.fail && !mem);
  if(pinned) for(int k=0;k<n_pinned;k++)put(&e,0xf9000000u|((greg_off(pin_guest[k])/8)<<10)|(1u<<5)|pin_host[k]);
  put(&e,0xd65f03c0);void *jit=executable(&e);struct em r;init_em(&r);put(&r,w);put(&r,0xd65f03c0);void *ref=executable(&r);
  for(unsigned j=0;j<32;j++) {struct cpu_state expected={0},got={0};for(int k=0;k<8;k++)expected.regs[k]=j==0?0:j==1?~0ull:random64();expected.nzcv=(random64()&15)<<28;got=expected;
   expected.nzcv=reference_call(ref,&expected,offsetof(struct cpu_state,nzcv));probe_call(jit,&got,NULL);
   if(memcmp(expected.regs,got.regs,8*8)||expected.nzcv!=got.nzcv) {printf("DIFF FAIL op=%08x pin=%u case=%u flags=%08x/%08x\n",w,pinned,j,expected.nzcv,got.nzcv);abort();}cases++;
  }munmap(jit,65536);munmap(ref,65536);
 }
 printf("integer differential cases=%u (native oracle, pin on/off) PASS\n",cases);
}
#include "memory-ops.h"
static void memory_probe(void) {
 unsigned cases=0;
 for(unsigned pinned=0;pinned<2;pinned++) for(unsigned i=0;i<sizeof(memory_ops)/sizeof(*memory_ops);i++) {
  uint32_t w=memory_ops[i];pin_init(pinned);pic_on=false;struct em e;init_em(&e);bool mem;
  if(pinned)load_pinned(&e);assert(emit_insn(&e,w,0x1000,0,&mem));assert(!e.fail && mem);
  if(pinned)for(int k=0;k<n_pinned;k++)put(&e,0xf9000000u|((greg_off(pin_guest[k])/8)<<10)|(1u<<5)|pin_host[k]);
  put(&e,0xd65f03c0);void *jit=executable(&e);struct em r;init_em(&r);put(&r,w);put(&r,0xd65f03c0);void *ref=executable(&r);
  struct tlb *tlb=calloc(1,sizeof(*tlb));struct mmu mmu={0};tlb->mmu=&mmu;
  void *a=mmap(NULL,4096,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0),*b=mmap(NULL,4096,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  // Use different real host addresses as guest VAs, mapped to the same logical bytes.
  uintptr_t guest=(uintptr_t)b;unsigned ix=((guest>>12)^(guest>>25))&8191;
  tlb->entries[ix].page=guest;tlb->entries[ix].page_if_writable=guest;tlb->entries[ix].data_minus_addr=0;tlb->entries[ix].gen=mmu.changes;
  for(unsigned j=0;j<16;j++) {for(int k=0;k<512;k++)((uint64_t *)a)[k]=random64();memcpy(b,a,4096);
   struct fiber_frame got_frame={0};
   struct cpu_state ex={0}, *got=&got_frame.cpu;for(int k=0;k<8;k++)ex.regs[k]=random64();ex.regs[4]=(uintptr_t)a+128+j;ex.regs[5]=16;*got=ex;got->regs[4]=(uintptr_t)b+128+j;
   reference_call(ref,&ex,offsetof(struct cpu_state,nzcv));probe_call(jit,got,tlb->entries);ex.regs[4]+=(uintptr_t)b-(uintptr_t)a;
   if(memcmp(ex.regs,got->regs,8*8)||memcmp(a,b,4096)) {printf("MEM DIFF FAIL op=%08x pin=%u case=%u\n",w,pinned,j);abort();}cases++;
  }munmap(a,4096);munmap(b,4096);free(tlb);munmap(jit,65536);munmap(ref,65536);
 }
 printf("memory differential cases=%u (native oracle, pin on/off, unaligned/pair/sign extension/writeback) PASS\n",cases);
}
#ifdef PROBE_FAMILY
static void moved_probe(void) {
 struct {struct aot_trans t;uint32_t compact[13];} f={0};
 f.t.key=(int32_t)((char *)f.compact-(char *)&f.t.key); f.t.off=0x100;
 uint32_t key[16]={0,0x100,0,0x100,1,~0u,~0u,0,2,1,0,0,0x91000400,0,0,0};
 memcpy(f.compact,key,7*4);for(int i=0;i<4;i++)f.compact[7+i]=key[7+i];f.compact[11]=key[12];f.compact[12]=key[13];
 assert(aot_key_same(&f.t,key,16));key[12]^=1;assert(!aot_key_same(&f.t,key,16));key[12]^=1;
 struct fiber_block b={.addr=0x100320};uint64_t db;bool fixed;
 key[1]=0x320;key[3]=0x320;assert(aot_fits_moved(&f.t,key,16,0x320,&b,&db,&fixed));assert(db==0x100220 && !fixed);
 key[12]^=1;assert(!aot_fits_moved(&f.t,key,16,0x320,&b,&db,&fixed));key[12]^=1;
 f.compact[11]=key[12]=0x10000000; // adr x0, .
 assert(aot_fits_moved(&f.t,key,16,0x320,&b,&db,&fixed));assert(db==0x100220);
 f.compact[11]=key[12]=0x90000000; // adrp x0, . (page rounding)
 assert(aot_fits_moved(&f.t,key,16,0x320,&b,&db,&fixed));assert(db==0x100000);
 puts("family exact/moved/add/ADR/ADRP/mutated-word probes PASS");
}
#else
static void moved_probe(void){}
#endif
int main(void) { saturation_probe(); simd_filter_probe(); differential_probe(); memory_probe(); moved_probe(); slot_probe(); puts("PRESERVATION_OK"); }
