// Per-feature kernels: the same source native and as a guest, to see what the translator pays for.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_MSC_VER) || defined(__clang__) || defined(__GNUC__)
#define NOINLINE __attribute__((noinline))
#endif
static double now(void) { struct timespec ts; timespec_get(&ts, TIME_UTC); return ts.tv_sec + ts.tv_nsec / 1e9; }
static volatile uint64_t sink;

NOINLINE static uint64_t k_alu(uint64_t n) {            // registers only
    uint64_t a = 1, b = 2, c = 3;
    for (uint64_t i = 0; i < n; i++) { a = a * 31 + b; b ^= a >> 7; c += a & b; a += c << 3; }
    return a ^ b ^ c;
}
NOINLINE static uint64_t k_array(uint32_t *v, uint64_t n, int rounds) {   // sequential loads and stores, one base
    uint64_t s = 0;
    for (int r = 0; r < rounds; r++)
        for (uint64_t i = 0; i < n; i++) { s += v[i]; v[i] = (uint32_t) (s >> 3); }
    return s;
}
struct node { struct node *next; uint64_t v; };
NOINLINE static uint64_t k_chase(struct node *n, uint64_t steps) {       // a load whose page changes every time
    uint64_t s = 0;
    for (uint64_t i = 0; i < steps; i++) { s += n->v; n = n->next; }
    return s;
}
NOINLINE static uint64_t fib(uint64_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }   // call, ret, stack
typedef uint64_t (*op_t)(uint64_t, uint64_t);
NOINLINE static uint64_t op_add(uint64_t a, uint64_t b) { return a + b; }
NOINLINE static uint64_t op_xor(uint64_t a, uint64_t b) { return a ^ (b << 1); }
NOINLINE static uint64_t op_mul(uint64_t a, uint64_t b) { return a * 3 + b; }
NOINLINE static uint64_t op_sub(uint64_t a, uint64_t b) { return a - (b >> 1); }
NOINLINE static uint64_t k_indirect(const uint8_t *prog, uint64_t n) {   // calls through a table, as an interpreter makes
    static op_t ops[4] = {op_add, op_xor, op_mul, op_sub};
    uint64_t a = 1;
    for (uint64_t i = 0; i < n; i++) a = ops[prog[i & 1023]](a, i);
    return a;
}
NOINLINE static uint64_t k_switch(const uint8_t *prog, uint64_t n) {     // a bytecode loop: switch (a jump through a table)
    uint64_t a = 1, b = 7;
    for (uint64_t i = 0; i < n; i++) {
        switch (prog[i & 1023] & 7) {
            case 0: a += b; break;
            case 1: b ^= a << 1; break;
            case 2: a = a * 3 + 1; break;
            case 3: b -= a >> 2; break;
            case 4: a ^= i; break;
            case 5: b += i & a; break;
            case 6: a -= b | 1; break;
            default: b = b * 5 + a; break;
        }
    }
    return a ^ b;
}
NOINLINE static uint64_t k_struct(uint64_t n) {          // a frame's worth of fields off the stack pointer and a heap object
    struct { uint64_t a, b, c, d; } *o = malloc(64), l = {1, 2, 3, 4};
    memset(o, 1, 64);
    volatile uint64_t *vo = &o->a;
    for (uint64_t i = 0; i < n; i++) {
        o->a += l.b + i; l.c ^= o->a; o->b = l.c + o->d; l.d += o->b; o->d = l.d ^ *vo;
    }
    uint64_t r = o->a ^ o->b ^ l.c ^ l.d;
    free(o);
    return r;
}

int main(void) {
    static uint8_t prog[1024];
    uint64_t seed = 12345;
    for (int i = 0; i < 1024; i++) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; prog[i] = (uint8_t) (seed >> 33) & 3; }
    uint32_t *arr = malloc(4 * 4096);          // 16 KB: stays in L1
    memset(arr, 1, 4 * 4096);
    enum { NODES = 4096 };                     // one node per page, 16 MB apart in all: every step another page
    char *heap = malloc((size_t) NODES * 4096 + 4096);
    struct node **nodes = malloc(NODES * sizeof(*nodes));
    for (int i = 0; i < NODES; i++) nodes[i] = (struct node *) (heap + (size_t) i * 4096 + (i % 60) * 64);
    for (int i = 0; i < NODES; i++) { nodes[i]->v = (uint64_t) i; nodes[i]->next = nodes[(i * 61 + 17) % NODES]; }
    struct node *small[16];                     // and sixteen nodes on one page
    for (int i = 0; i < 16; i++) small[i] = (struct node *) (heap + 4096 * 5 + 2048 + i * 64);
    for (int i = 0; i < 16; i++) { small[i]->v = (uint64_t) i; small[i]->next = small[(i + 1) % 16]; }
    double t;
#define RUN(name, expr) t = now(); sink = (expr); printf("%-10s %7.3f s  %016llx\n", name, now() - t, (unsigned long long) sink); fflush(stdout)
    RUN("alu", k_alu(400000000));
    RUN("array", k_array(arr, 4096, 60000));
    RUN("chase-1pg", k_chase(small[0], 300000000));
    RUN("chase-far", k_chase(nodes[0], 300000000));
    RUN("fib", fib(38));
    RUN("indirect", k_indirect(prog, 300000000));
    RUN("switch", k_switch(prog, 300000000));
    RUN("struct", k_struct(300000000));
    return 0;
}
