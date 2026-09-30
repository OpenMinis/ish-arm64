// Linux-only test adapter for the Darwin CLI at e6521d9c. Not app code.
// Usage: bun tests/regress/netlink-linux-build.ts build-netlink release|debug
import { resolve } from "node:path";
const root = resolve(import.meta.dir, "../..");
const build = resolve(root, process.argv[2] ?? "build-netlink-linux");
const type = process.argv[3] ?? "release";
if (!["release", "debug"].includes(type)) throw Error("expected release or debug");
async function run(args: string[]) {
    const child = Bun.spawn(args, { cwd: root, env: {...process.env, CC: "clang"}, stdout: "inherit", stderr: "inherit" });
    if (await child.exited !== 0) throw Error(`failed: ${args.join(" ")}`);
}
await run(["meson", "setup", build, "-Dguest_arch=arm64", `--buildtype=${type}`]);
let main = await Bun.file(resolve(root, "main.c")).text();
function replace(old: string, value: string) {
    if (main.split(old).length !== 2) throw Error(`source drift: ${old}`);
    main = main.replace(old, value);
}
replace("#include <mach/mach.h>", "#ifdef __APPLE__\n#include <mach/mach.h>\n#endif");
replace("#include <sys/sysctl.h>", "#ifdef __APPLE__\n#include <sys/sysctl.h>\n#endif //");
replace("#if defined(__aarch64__) && defined(GUEST_ARM64)", "#if defined(__APPLE__) && defined(__aarch64__) && defined(GUEST_ARM64)");
replace("#ifdef __aarch64__", "#if defined(__APPLE__) && defined(__aarch64__)");
replace("    task_vm_info_data_t info;", "#ifdef __APPLE__\n    task_vm_info_data_t info;");
replace("    return (uint64_t) info.phys_footprint;", "    return (uint64_t) info.phys_footprint;\n#else\n    return 0;\n#endif");
replace("    uint64_t bytes = 0;", "#ifdef __APPLE__\n    uint64_t bytes = 0;");
replace("    return bytes;", "    return bytes;\n#else\n    return 0;\n#endif");
await Bun.write(resolve(build, "main-linux.c"), main);
await Bun.write(resolve(build, "fake-linux.c"), `#define st_mtimespec st_mtim
#define st_atimespec st_atim
#define st_ctimespec st_ctim
#define F_GETPATH 0x7ffffffe /* Unsupported fakefs bind-mount operation. */
#include "${resolve(root, "fs/fake.c")}"
`);
// Generate offsets first: upstream main.c lacks an explicit dependency on them.
await run(["ninja", "-C", build, "cpu-offsets.h"]);
const ninjaFile = resolve(build, "build.ninja");
let ninja = await Bun.file(ninjaFile).text();
for (const [source, adapter] of [["main.c", "main-linux.c"], ["fs/fake.c", "fake-linux.c"]]) {
    const pattern = new RegExp(`(c_COMPILER )[^\\n ]*/${source.replaceAll(".", "\\.")}(?= |\\n)`);
    if (!pattern.test(ninja)) throw Error(`missing build rule: ${source}`);
    ninja = ninja.replace(pattern, `$1${adapter}`);
}
await Bun.write(ninjaFile, ninja);
await run(["ninja", "-C", build, "-j6"]);
