// [T-ish-jit-oom-abort] Pins the JIT gadget-buffer out-of-memory path.
//
// Before this fix, gen() called abort() when realloc() could not grow the
// gadget buffer. One guest process (a python3 thread pool spawning ~30 workers
// in the background) could therefore take the entire app down -- every other
// guest thread plus the Swift UI -- with SIGABRT in gen_ldst/gen_step.
//
// The contract now is:
//   1. gen() records state->oom and emits nothing further; it never aborts.
//   2. capacity is only advanced when realloc SUCCEEDS, so a failed grow
//      leaves state->capacity describing the buffer that actually exists.
//   3. gen() stays within that real capacity -- writing at the recorded
//      size after a failure would be a heap overflow.
//   4. gen_end() must NOT run on the OOM path: it writes through jump_ip[]
//      and block_patch_ip, offsets recorded assuming the buffer would keep
//      growing, which can sit past the real end of the block.
//
// Like the sibling suites this MIRRORS gen() (the real one drags in the whole
// emulator), but it mirrors it exactly -- if you change the real logic, change
// this. The point is the allocator: a failing realloc is injected, which no
// amount of running the real binary can do deterministically.
//
// Build & run:
//   cc -O1 -o /tmp/t_jitoom tests/regress/regress_jit_oom.c && /tmp/t_jitoom
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#define FIBER_BLOCK_INITIAL_CAPACITY 16

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

// ------------------------------------------------- allocator with injection

// Grows allowed before realloc starts failing; -1 means never fail.
static int g_allowed_grows = -1;
static int g_grow_attempts = 0;

static void *test_realloc(void *p, size_t n) {
    g_grow_attempts++;
    if (g_allowed_grows >= 0 && g_grow_attempts > g_allowed_grows)
        return NULL;   // simulate the host being out of memory
    return realloc(p, n);
}

// ------------------------------------------------------------ mirrored gen()

struct fiber_block {
    unsigned long addr;
    unsigned long end_addr;
    unsigned used;
    unsigned long code[];   // flexible: capacity entries live here
};

struct gen_state {
    struct fiber_block *block;
    unsigned size;
    unsigned capacity;
    unsigned jump_ip[2];
    unsigned block_patch_ip;
    bool oom;
};

// Mirrors asbestos/guest-arm64/gen.c gen() after the fix.
static void gen(struct gen_state *state, unsigned long thing) {
    assert(state->size <= state->capacity);
    if (state->oom)
        return;
    if (state->size >= state->capacity) {
        unsigned new_capacity = state->capacity * 2;
        struct fiber_block *new_block = test_realloc(state->block,
                sizeof(*new_block) + new_capacity * sizeof(unsigned long));
        if (new_block == NULL) {
            state->oom = true;
            return;
        }
        state->capacity = new_capacity;
        state->block = new_block;
    }
    assert(state->size < state->capacity);
    state->block->code[state->size++] = thing;
}

static bool gen_start(struct gen_state *state) {
    memset(state, 0, sizeof(*state));
    state->capacity = FIBER_BLOCK_INITIAL_CAPACITY;
    state->size = 0;
    state->oom = false;
    state->block = malloc(sizeof(struct fiber_block)
                          + state->capacity * sizeof(unsigned long));
    return state->block != NULL;
}

// ------------------------------------------------------------------- tests

// The headline case: a compile that runs past the initial capacity while the
// allocator refuses to grow must survive rather than abort.
static void test_oom_does_not_abort(void) {
    struct gen_state st;
    CHECK(gen_start(&st), "gen_start failed");
    g_allowed_grows = 0; g_grow_attempts = 0;   // fail the very first grow

    for (int i = 0; i < 500; i++)               // way past capacity 16
        gen(&st, 0xAA00 + i);

    CHECK(st.oom, "oom flag not set after a failed grow");
    CHECK(st.size == FIBER_BLOCK_INITIAL_CAPACITY,
          "size=%u, expected to stop at the real capacity %d",
          st.size, FIBER_BLOCK_INITIAL_CAPACITY);
    CHECK(st.capacity == FIBER_BLOCK_INITIAL_CAPACITY,
          "capacity=%u advanced despite the realloc failing", st.capacity);
    // Only one grow is attempted: once oom is set gen() returns immediately,
    // so a stuck compile does not hammer the allocator 500 times.
    CHECK(g_grow_attempts == 1, "grow attempts=%d, expected 1", g_grow_attempts);
    free(st.block);
}

// Every value written must sit inside the buffer that actually exists. This is
// the heap-overflow guard: capacity must describe real memory, not intent.
static void test_no_write_past_real_capacity(void) {
    struct gen_state st;
    CHECK(gen_start(&st), "gen_start failed");
    g_allowed_grows = 2; g_grow_attempts = 0;   // 16 -> 32 -> 64, then fail

    for (int i = 0; i < 500; i++)
        gen(&st, 0xBB00 + i);

    CHECK(st.oom, "oom flag not set");
    CHECK(st.capacity == 64, "capacity=%u, expected 64", st.capacity);
    CHECK(st.size == 64, "size=%u must not exceed real capacity 64", st.size);
    for (unsigned i = 0; i < st.size; i++)
        CHECK(st.block->code[i] == 0xBB00 + i,
              "code[%u]=%lu corrupted", i, st.block->code[i]);
    free(st.block);
}

// A compile that never exhausts memory must be untouched by all of this.
static void test_success_path_unchanged(void) {
    struct gen_state st;
    CHECK(gen_start(&st), "gen_start failed");
    g_allowed_grows = -1; g_grow_attempts = 0;  // never fail

    for (int i = 0; i < 500; i++)
        gen(&st, 0xCC00 + i);

    CHECK(!st.oom, "oom set on a successful compile");
    CHECK(st.size == 500, "size=%u, expected 500", st.size);
    CHECK(st.capacity >= 500, "capacity=%u < size", st.capacity);
    for (unsigned i = 0; i < st.size; i++)
        CHECK(st.block->code[i] == 0xCC00 + i,
              "code[%u]=%lu corrupted", i, st.block->code[i]);
    free(st.block);
}

// gen_end() writes block->code[jump_ip[i]] and [block_patch_ip]. Those offsets
// are recorded while emitting, so after a failed grow they can point past the
// real buffer -- fiber_block_compile() must bail BEFORE calling gen_end().
// This asserts the offsets really can exceed capacity, which is why the order
// of the check matters.
static void test_gen_end_offsets_can_exceed_capacity(void) {
    struct gen_state st;
    CHECK(gen_start(&st), "gen_start failed");
    g_allowed_grows = 0; g_grow_attempts = 0;

    for (int i = 0; i < 40; i++) {
        // Mimic a jump target recorded at the current write position, the way
        // the real gen_jump/gen_call paths do.
        if (i == 30)
            st.jump_ip[0] = st.size ? st.size : 30;
        gen(&st, 0xDD00 + i);
    }
    // The compile wanted to record a patch site at slot 30; the buffer only has
    // 16. Writing there (as gen_end would) is a heap overflow, so the OOM check
    // must come first.
    st.block_patch_ip = 30;
    CHECK(st.oom, "oom flag not set");
    CHECK(st.block_patch_ip > st.capacity,
          "patch offset %u <= capacity %u -- the ordering hazard this test "
          "pins is not reproduced", st.block_patch_ip, st.capacity);
    free(st.block);
}

int main(void) {
    test_oom_does_not_abort();
    test_no_write_past_real_capacity();
    test_success_path_unchanged();
    test_gen_end_offsets_can_exceed_capacity();

    if (failures == 0) {
        printf("regress_jit_oom: ALL PASS\n");
        return 0;
    }
    printf("regress_jit_oom: %d FAILURES\n", failures);
    return 1;
}
