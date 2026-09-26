#ifndef ASBESTOS_ARM64_AOT_H
#define ASBESTOS_ARM64_AOT_H

// AOT images: translations recorded from the PIC JIT (ISH_JIT_RECORD) and
// laid out as assembly by tools/jit_aot/gen.py. An image is not part of ish:
// a target that wants one adds the .S to its own sources, and a constructor
// in it calls ish_aot_register() at load time. The layout here must match
// what gen.py emits.

#include <stdint.h>

struct aot_seg {
    const uint32_t *code;
    uint32_t pos;                 // code-stream slot it replaces
    uint32_t words;
};

struct aot_loop {                 // self-loop block: promoted registers in [lo, hi)
    const uint32_t *lo, *hi;
    uint8_t n, pad[7];
    int8_t g[8], d[8], h[8];      // promoted guest reg, donor guest reg, host reg
};

struct aot_trans {
    uint64_t off;                 // file offset of the block
    uint32_t idx, nkey;           // module-context index; key length in words
    const uint32_t *key;          // module word and gadget words zero
    const void *const *gadget;    // per unit: the gadget at the unit's start, or 0
    const struct aot_seg *seg;
    uint32_t nseg, nunits;
    const uint32_t *link[2];      // direct-link branch per jump_ip slot, or 0
    const struct aot_loop *loop;
};

// Content hash of a translation's key (key_content_hash()) -> its index in
// trans[], sorted by hash: finds a block whose bytes moved in another version.
struct aot_hash {
    uint64_t hash;
    uint32_t trans, pad;
};

struct aot_module {
    const char *path;             // guest path of the module
    uint64_t size;
    uint8_t sha256[32];
    uint32_t ntrans, nidx;        // translations (sorted by off), context indices used
    const struct aot_trans *trans;
    int32_t prologue_words, entry_off, n_pinned;   // conventions the code was made with
    uint32_t abi;                 // jit_abi() of the ish that made it (layouts, code version)
    const uint32_t *text_start, *text_end;
    uint32_t build_id_len;        // NT_GNU_BUILD_ID of the module, matched before the path
    uint8_t build_id[20];
    const char *family;           // fnmatch() pattern over file names this image may also serve
                                  // (other versions: blocks are matched by content), or NULL
    const struct aot_hash *by_hash;
    uint32_t nhash, pad2;
};

// Called by an image's constructor, before main. Needs ish built with -Djit=true.
void ish_aot_register(const struct aot_module *m);

#endif
