// Extract the actual CLI recovery function and its offset assertions for the
// Linux synthetic-Darwin adapter. Fail closed if source markers change.
const root=process.argv[2], out=process.argv[3];
async function source(path: string) {
    return process.env.BASELINE_REV
        ? await Bun.$`git -C ${root} show ${process.env.BASELINE_REV}:${path}`.text()
        : await Bun.file(`${root}/${path}`).text();
}
// Real pre-checkpoint ABI, evaluated with the same live pin/PIC conventions.
const oldJit=await Bun.$`git -C ${root} show 083eca69:asbestos/guest-arm64/jit.c`.text();
const abiStart=oldJit.indexOf('static uint32_t jit_abi(void)'),abiEnd=oldJit.indexOf('\nstatic void aot_init(',abiStart);
const version=oldJit.match(/^#define JIT_CODE_VERSION (\d+)$/m)?.[1];
if(abiStart<0||abiEnd<abiStart||!version)throw Error('Baseline ABI boundaries changed');
await Bun.write(`${out}/prior-abi.h`,oldJit.slice(abiStart,abiEnd)
    .replace('jit_abi(void)','prior_jit_abi(void)').replaceAll('JIT_CODE_VERSION',version));
const s=await source('main.c');
const a=s.indexOf('#define CRASH_CPU_pc'), b=s.indexOf('static struct termios saved_termios',a);
if(a<0||b<a)throw Error('CLI recovery boundaries changed');
await Bun.write(`${out}/cli-recovery.c`, s.slice(a,b));
// Exercise the actual post-fiber crash policy, including precise-PC lifetime,
// cache invalidation and the 16-crash bound, without porting the full backend.
const dispatch=await source('asbestos/asbestos.c');
const start=dispatch.indexOf('#ifdef GUEST_ARM64\n        ',dispatch.indexOf('interrupt = fiber_enter(block, frame, tlb);'));
const end=dispatch.indexOf('        // (debug trace removed)',start);
if(start<0||end<start)throw Error('Dispatch recovery boundaries changed');
const tlb=await source('emu/tlb.c');
const flushStart=tlb.indexOf('void tlb_flush('),flushEnd=tlb.indexOf('\nvoid tlb_free(',flushStart);
if(flushStart<0||flushEnd<flushStart)throw Error('TLB flush boundaries changed');
await Bun.write(`${out}/dispatch-recovery.c`, `${tlb.slice(flushStart,flushEnd)}
static int dispatch_after_fiber(int interrupt, struct fiber_frame *frame,
        struct tlb *tlb, struct asbestos *asbestos, unsigned *retries) {
    struct fiber_block **cache=tlb->block_cache;
    unsigned crash_retry_count=*retries;
${dispatch.slice(start,end)}
    *retries=crash_retry_count;
    return interrupt;
}
`);
