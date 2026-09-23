// [T-ish-offload-path-scope] OpenMinis#288 — see native_offload_policy.h.
#include "kernel/native_offload_policy.h"

#include <string.h>

// Names whose offload shadows a real, user-installable program.
static const char *const generic_names[] = {
    "ffmpeg",
    "ffprobe",
};

// The only directories a generic offload may claim. Deliberately NOT the sbin
// directories: the default PATH starts with /usr/local/sbin, and busybox ash
// tries execve() on every PATH entry in turn, so /usr/local/sbin/ffmpeg is the
// FIRST candidate for a bare `ffmpeg`. Leaving it out costs nothing — that
// probe falls through to emulated exec, gets ENOENT, and ash moves on to
// /usr/local/bin/ffmpeg, which is offloaded — while a user who genuinely put
// their own build in an sbin directory gets it.
static const char *const system_bin_dirs[] = {
    "/bin/",
    "/usr/bin/",
    "/usr/local/bin/",
};

bool native_offload_name_is_generic(const char *guest_name) {
    if (guest_name == NULL)
        return false;
    for (size_t i = 0; i < sizeof(generic_names) / sizeof(generic_names[0]); i++) {
        if (strcmp(guest_name, generic_names[i]) == 0)
            return true;
    }
    return false;
}

bool native_offload_path_allowed(const char *guest_name, const char *guest_path) {
    if (guest_name == NULL || guest_path == NULL)
        return false;
    if (!native_offload_name_is_generic(guest_name))
        return true;

    // Exact match against "<dir><name>". No normalisation is attempted, on
    // purpose: "/usr/bin/../../tmp/ffmpeg" or "/usr//bin/ffmpeg" simply fail
    // the match and run the user's file — the safe direction to err in.
    size_t name_len = strlen(guest_name);
    size_t path_len = strlen(guest_path);
    for (size_t i = 0; i < sizeof(system_bin_dirs) / sizeof(system_bin_dirs[0]); i++) {
        const char *dir = system_bin_dirs[i];
        size_t dir_len = strlen(dir);
        if (path_len == dir_len + name_len
                && strncmp(guest_path, dir, dir_len) == 0
                && strcmp(guest_path + dir_len, guest_name) == 0)
            return true;
    }
    return false;
}

bool native_offload_env_disabled(const char *guest_name, const char *envp) {
    if (envp == NULL || !native_offload_name_is_generic(guest_name))
        return false;
    for (const char *p = envp; *p != '\0'; p += strlen(p) + 1) {
        if (strcmp(p, "MINIS_NO_FFMPEG_OFFLOAD=1") == 0 || strcmp(p, "NO_OFFLOAD=1") == 0)
            return true;
    }
    return false;
}
