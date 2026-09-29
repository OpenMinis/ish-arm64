// Isolated Linux/AArch64 function tests, NOT a port of the Apple JIT backend.
import {mkdtemp, mkdir, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {resolve} from 'node:path';
const root=resolve(import.meta.dir,'../../..');
const out=process.env.EVIDENCE_DIR || await mkdtemp(`${tmpdir()}/ish-jit-safety-`);
await mkdir(out,{recursive:true});
try {
 if(process.platform!=='linux'||process.arch!=='arm64')throw Error('Linux AArch64 required for this adapter');
 const cc=process.env.CC || 'clang';
 await Bun.$`${cc} -DGUEST_ARM64=1 -DISH_JIT=1 -D_GNU_SOURCE -include tools/staticdefine.h -I. -S asbestos/offsets.c -o ${out}/offsets.s`.cwd(root);
 const asm=await Bun.file(`${out}/offsets.s`).text();
 const defs=[...asm.matchAll(/\.ascii\s+"->(\w+) ([0-9]+) [^"]*"/g)].map(m=>`#define ${m[1]} ${m[2]}`);
 if(defs.length<30)throw Error('offset generation failed');
 await Bun.write(`${out}/cpu-offsets.h`,defs.join('\n')+'\n');
 const baseline=process.env.BASELINE_REV;
 const original=baseline
    ? await Bun.$`git -C ${root} show ${baseline}:asbestos/guest-arm64/jit.c`.text()
    : await Bun.file(`${root}/asbestos/guest-arm64/jit.c`).text();
 const restriction=/#if !defined\(__APPLE__\)[\s\S]*?#endif/;
 if(!restriction.test(original))throw Error('Apple restriction changed; review adapter');
 let source=original.replace(restriction,'// Platform restriction removed ONLY in isolated test copy.');
 source=source.replace(/^#include <(?:TargetConditionals\.h|libkern\/OSCacheControl\.h|mach\/mach\.h|mach\/mach_time\.h|sys\/ucontext\.h)>\n/gm,'');
 await Bun.write(`${out}/jit-adapted.c`,'#include "apple-adapter.h"\n'+(baseline?'#include "asbestos/frame.h"\n':'')+source);
 await Bun.$`bun ${import.meta.dir}/prepare-cli.ts ${root} ${out}`;
 for(const opt of ['-O0','-O2']) {
  console.log(`=== ${baseline || 'candidate'} ${opt} ===`);
  await Bun.$`${cc} -fsyntax-only -Wno-format -DGUEST_ARM64=1 -DISH_JIT=1 -D_GNU_SOURCE -include tools/staticdefine.h -I. -I${out} asbestos/asbestos.c`.cwd(root);
  const flags=[opt,'-g','-Wno-format','-Werror=incompatible-pointer-types','-DGUEST_ARM64=1','-DISH_JIT=1','-D_GNU_SOURCE',`-I${root}`,`-I${out}`,`-I${import.meta.dir}`,'-ffunction-sections','-fdata-sections'];
  await Bun.$`${cc} ${flags} ${import.meta.dir}/restart.c ${import.meta.dir}/restart-call.S -Wl,--gc-sections -pthread -ldl -o ${out}/restart`;
  if(baseline) {
   // The frozen pre-checkpoint emitter/handler must fail the same exact-PC
   // assertion in all register/PIC modes, not time out or die during setup.
   for(const mode of ['0','1','2','3','4']) {
    const r=await Bun.$`timeout 45 ${out}/restart ${mode}`.nothrow();
    if(r.exitCode!==134 || !r.stderr.toString().includes('frame.cpu.pc==guest_pc'))
     throw Error(`Baseline ${mode}: expected exact-PC assertion abort, got ${r.exitCode}`);
    console.log(`baseline-negative-ok mode=${mode} assertion exit=134`);
   }
   continue;
  }
  await Bun.$`timeout 45 ${out}/restart`;
  await Bun.$`${cc} ${flags} ${import.meta.dir}/safety.c -Wl,--gc-sections -pthread -ldl -o ${out}/safety`;
  for(const mode of ['allocator','sync','dump'])await Bun.$`timeout 30 ${out}/safety ${mode}`;
  await Bun.$`${cc} ${flags} -DPROBE_FAMILY=1 ${import.meta.dir}/preservation.c ${import.meta.dir}/call.S ${import.meta.dir}/diff-call.S -Wl,--gc-sections -pthread -ldl -o ${out}/preservation`;
  await Bun.$`timeout 60 ${out}/preservation`;
 }
} finally { if(!process.env.EVIDENCE_DIR)await rm(out,{recursive:true,force:true}); }
