// [T-ish-offload-path-scope] OpenMinis#288 — unit test for native_offload_policy.
//
// Run: sh kernel/offload_tests/path_scope/run.sh   (from deps/ish)
//
// Compiles the policy file on its own — no kernel, no emulator — so it runs on
// the host in well under a second.

#include "kernel/native_offload_policy.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static void check(const char *label, bool actual, bool expected) {
    if (actual == expected) {
        printf("  ✅ %s\n", label);
    } else {
        printf("  ❌ %s — expected %s, got %s\n", label,
               expected ? "true" : "false", actual ? "true" : "false");
        failures++;
    }
}

// Build a packed exec envp ("A=1\0B=2\0\0") from a NULL-terminated list.
static const char *packed(char *buf, size_t cap, const char *const *vars) {
    size_t off = 0;
    for (; *vars; vars++) {
        size_t n = strlen(*vars) + 1;
        if (off + n + 1 > cap) break;
        memcpy(buf + off, *vars, n);
        off += n;
    }
    buf[off] = '\0';
    return buf;
}

int main(void) {
    printf("▶️  1. system bin paths are still offloaded (VideoToolbox keeps working)\n");
    check("/usr/bin/ffmpeg",        native_offload_path_allowed("ffmpeg", "/usr/bin/ffmpeg"), true);
    check("/usr/local/bin/ffmpeg",  native_offload_path_allowed("ffmpeg", "/usr/local/bin/ffmpeg"), true);
    check("/bin/ffmpeg",            native_offload_path_allowed("ffmpeg", "/bin/ffmpeg"), true);
    check("/usr/bin/ffprobe",       native_offload_path_allowed("ffprobe", "/usr/bin/ffprobe"), true);

    printf("\n▶️  2. the reported bug: user-supplied ffmpeg is no longer hijacked\n");
    check("./ffmpeg",               native_offload_path_allowed("ffmpeg", "./ffmpeg"), false);
    check("../ffmpeg",              native_offload_path_allowed("ffmpeg", "../ffmpeg"), false);
    check("bare ffmpeg (relative)", native_offload_path_allowed("ffmpeg", "ffmpeg"), false);
    check("bin/ffmpeg (relative)",  native_offload_path_allowed("ffmpeg", "bin/ffmpeg"), false);
    check("/tmp/ffmpeg",            native_offload_path_allowed("ffmpeg", "/tmp/ffmpeg"), false);
    check("/tmp/build/ffmpeg",      native_offload_path_allowed("ffmpeg", "/tmp/build/ffmpeg"), false);
    check("/root/ffmpeg",           native_offload_path_allowed("ffmpeg", "/root/ffmpeg"), false);
    check("/var/minis/workspace/x/ffmpeg",
          native_offload_path_allowed("ffmpeg", "/var/minis/workspace/x/ffmpeg"), false);
    check("/opt/bin/ffmpeg",        native_offload_path_allowed("ffmpeg", "/opt/bin/ffmpeg"), false);

    printf("\n▶️  3. near-misses must not match\n");
    // sbin is deliberately excluded (see native_offload_policy.c).
    check("/usr/local/sbin/ffmpeg", native_offload_path_allowed("ffmpeg", "/usr/local/sbin/ffmpeg"), false);
    check("/usr/sbin/ffmpeg",       native_offload_path_allowed("ffmpeg", "/usr/sbin/ffmpeg"), false);
    // No normalisation: these run the user's file, the safe direction.
    check("/usr/bin/../../tmp/ffmpeg",
          native_offload_path_allowed("ffmpeg", "/usr/bin/../../tmp/ffmpeg"), false);
    check("/usr//bin/ffmpeg",       native_offload_path_allowed("ffmpeg", "/usr//bin/ffmpeg"), false);
    check("/usr/bin/sub/ffmpeg",    native_offload_path_allowed("ffmpeg", "/usr/bin/sub/ffmpeg"), false);
    // Prefix/suffix collisions on the name itself.
    check("/usr/bin/ffmpeg2",       native_offload_path_allowed("ffmpeg", "/usr/bin/ffmpeg2"), false);
    check("/usr/bin/xffmpeg",       native_offload_path_allowed("ffmpeg", "/usr/bin/xffmpeg"), false);
    check("/usr/binffmpeg",         native_offload_path_allowed("ffmpeg", "/usr/binffmpeg"), false);
    // A generic name claimed at the OTHER generic name's path.
    check("ffprobe at /usr/bin/ffmpeg",
          native_offload_path_allowed("ffprobe", "/usr/bin/ffmpeg"), false);

    printf("\n▶️  4. Minis-only offloads are unchanged (no real binary to protect)\n");
    check("apple-photos from anywhere",
          native_offload_path_allowed("apple-photos", "/tmp/apple-photos"), true);
    check("minis-open relative",
          native_offload_path_allowed("minis-open", "./minis-open"), true);
    check("apple-* is not generic", native_offload_name_is_generic("apple-photos"), false);
    check("ffmpeg IS generic",      native_offload_name_is_generic("ffmpeg"), true);
    check("ffprobe IS generic",     native_offload_name_is_generic("ffprobe"), true);

    printf("\n▶️  5. env escape hatch\n");
    char buf[512];
    const char *none[] = {"PATH=/usr/bin", "HOME=/root", NULL};
    const char *minis[] = {"PATH=/usr/bin", "MINIS_NO_FFMPEG_OFFLOAD=1", NULL};
    const char *generic[] = {"NO_OFFLOAD=1", "PATH=/usr/bin", NULL};
    const char *zero[] = {"MINIS_NO_FFMPEG_OFFLOAD=0", NULL};
    const char *prefix[] = {"MINIS_NO_FFMPEG_OFFLOAD=10", "XNO_OFFLOAD=1", NULL};
    check("no flag -> offload stays on",
          native_offload_env_disabled("ffmpeg", packed(buf, sizeof buf, none)), false);
    check("MINIS_NO_FFMPEG_OFFLOAD=1 disables ffmpeg",
          native_offload_env_disabled("ffmpeg", packed(buf, sizeof buf, minis)), true);
    check("NO_OFFLOAD=1 disables ffmpeg",
          native_offload_env_disabled("ffmpeg", packed(buf, sizeof buf, generic)), true);
    check("…and ffprobe",
          native_offload_env_disabled("ffprobe", packed(buf, sizeof buf, minis)), true);
    check("=0 does NOT disable",
          native_offload_env_disabled("ffmpeg", packed(buf, sizeof buf, zero)), false);
    check("=10 / XNO_OFFLOAD do NOT disable (exact match only)",
          native_offload_env_disabled("ffmpeg", packed(buf, sizeof buf, prefix)), false);
    // Minis-only offloads ARE the command; turning them off would break them.
    check("NO_OFFLOAD=1 leaves apple-photos alone",
          native_offload_env_disabled("apple-photos", packed(buf, sizeof buf, generic)), false);
    check("NULL envp is safe",      native_offload_env_disabled("ffmpeg", NULL), false);
    char empty[1] = {'\0'};
    check("empty envp is safe",     native_offload_env_disabled("ffmpeg", empty), false);

    printf("\n▶️  6. defensive inputs\n");
    check("NULL path",              native_offload_path_allowed("ffmpeg", NULL), false);
    check("NULL name",              native_offload_path_allowed(NULL, "/usr/bin/ffmpeg"), false);
    check("NULL name not generic",  native_offload_name_is_generic(NULL), false);

    printf("\n%s\n", failures == 0 ? "✅ ALL PASSED" : "❌ FAILURES");
    return failures == 0 ? 0 : 1;
}
