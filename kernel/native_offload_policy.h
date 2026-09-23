// [T-ish-offload-path-scope] OpenMinis#288 — which exec()s a native offload may
// take over.
//
// Offloads used to match on the BASENAME alone, so any file called `ffmpeg`
// anywhere in the guest — `./ffmpeg`, `/tmp/build/ffmpeg`, a shell wrapper in
// the user's workspace — was silently replaced by the app's in-process FFmpeg.
// The user's own binary never ran and nothing said so.
//
// This header is the policy only, deliberately free of any kernel dependency so
// it can be compiled and tested on its own (see offload_tests/path_scope). The
// one check that needs the filesystem — "is this a #! script?" — stays at the
// exec call site, which already has generic_open.

#ifndef NATIVE_OFFLOAD_POLICY_H
#define NATIVE_OFFLOAD_POLICY_H

#include <stdbool.h>
#include <stddef.h>

// True for an offload whose name is also a REAL program users install
// themselves (ffmpeg, ffprobe). Only these get the strict rules below.
//
// Every other offload (apple-*, minis-*) is Minis-only: there is no genuine
// binary of that name for a user to be running instead, and in fact no file
// behind it at all — the offload IS the command. Restricting where those may
// be exec'd from, or letting an env var turn them off, would just make the
// command fail.
bool native_offload_name_is_generic(const char *guest_name);

// For a generic name, true only when `guest_path` is exactly one of the
// standard system locations: /bin/<name>, /usr/bin/<name>,
// /usr/local/bin/<name>. Relative paths (./ffmpeg, ../ffmpeg, bare ffmpeg),
// /tmp/**, /root/**, workspaces, and even the sbin directories all return
// false and run the user's own file through normal emulated exec.
//
// Non-generic names always return true — their behaviour is unchanged.
bool native_offload_path_allowed(const char *guest_name, const char *guest_path);

// Escape hatch for generic offloads: true when the NUL-separated, double-NUL
// terminated exec envp contains MINIS_NO_FFMPEG_OFFLOAD=1 or NO_OFFLOAD=1.
// Non-generic names are never disabled by it (see above). `envp` may be NULL.
bool native_offload_env_disabled(const char *guest_name, const char *envp);

#endif
