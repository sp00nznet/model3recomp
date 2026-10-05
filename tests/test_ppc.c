/* model3recomp -- self-check for the PPC semantics lifted code depends on.
 *
 * These are the operations where a plausible-looking C translation is subtly
 * wrong: the rotate mask when it wraps, shifts of 32 or more, the carry rules,
 * and the CR bit numbering. Each one here failed at least once in some
 * recompiler before somebody wrote the test.
 *
 * Built as model3recomp_test_ppc. Returns non-zero on failure.
 */
#include "model3recomp/ppc.h"
#include "model3recomp/func_table.h"

#include <stdio.h>

static int fails;

#define CHECK(expr) do {                                                      \
    if (!(expr)) {                                                            \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr);       \
        fails++;                                                              \
    }                                                                         \
} while (0)

static void test_mask(void)
{
    /* Straight case: rlwinm r,r,0,24,31 keeps the low byte. */
    CHECK(m3_mask(24, 31) == 0x000000FFu);
    CHECK(m3_mask(0, 31)  == 0xFFFFFFFFu);
    CHECK(m3_mask(0, 0)   == 0x80000000u);
    CHECK(m3_mask(31, 31) == 0x00000001u);
    CHECK(m3_mask(8, 15)  == 0x00FF0000u);

    /* Wrapping case: mb > me sets bits mb..31 AND 0..me. This is the one that
     * silently corrupts bitfield extracts when a translation gets it wrong.
     * The Lost World's interrupt library uses rlwinm r0,r0,0,8,6 -- mb=8,
     * me=6 -- to clear exactly one bit, which only works if the mask wraps. */
    CHECK(m3_mask(8, 6)   == 0xFEFFFFFFu);
    CHECK(m3_mask(1, 0)   == 0xFFFFFFFFu);
    CHECK(m3_mask(24, 7)  == 0xFF0000FFu);
}

static void test_rotl(void)
{
    CHECK(m3_rotl32(0x12345678u, 0)  == 0x12345678u);
    CHECK(m3_rotl32(0x12345678u, 8)  == 0x34567812u);
    CHECK(m3_rotl32(0x80000000u, 1)  == 0x00000001u);
    /* A rotate of 32 must be identity, not undefined behaviour. */
    CHECK(m3_rotl32(0x12345678u, 32) == 0x12345678u);
}

/* rlwinm r0,r0,0,8,6 as the game actually uses it: clear bit 7 (0x01000000),
 * keep everything else. */
static void test_rlwinm_clear_one_bit(void)
{
    uint32_t v = 0xFFFFFFFFu;
    uint32_t r = m3_rotl32(v, 0) & m3_mask(8, 6);
    CHECK(r == 0xFEFFFFFFu);
}

static void test_cr(void)
{
    m3_ppc_t *c = &m3_ctx;
    c->cr = 0;
    c->xer = 0;

    /* cr0 lives in the TOP nibble -- PPC numbers CR bits from the MSB. Get
     * this backwards and every branch in the game inverts. */
    m3_cmp_s(c, 0, -1, 0);
    CHECK(m3_cr_get(c, 0) == 8u);          /* LT */
    CHECK((c->cr & 0xF0000000u) == 0x80000000u);

    m3_cmp_s(c, 0, 1, 0);
    CHECK(m3_cr_get(c, 0) == 4u);          /* GT */
    m3_cmp_s(c, 0, 0, 0);
    CHECK(m3_cr_get(c, 0) == 2u);          /* EQ */

    /* cr7 is the bottom nibble. */
    m3_cmp_u(c, 7, 5u, 9u);
    CHECK(m3_cr_get(c, 7) == 8u);
    CHECK((c->cr & 0xFu) == 8u);

    /* Unsigned compare must not sign-extend: 0xFFFFFFFF > 1. */
    m3_cmp_u(c, 0, 0xFFFFFFFFu, 1u);
    CHECK(m3_cr_get(c, 0) == 4u);
    m3_cmp_s(c, 0, (int32_t)0xFFFFFFFFu, 1);
    CHECK(m3_cr_get(c, 0) == 8u);

    /* SO is copied from XER into bit 3 of the field. */
    c->xer = M3_XER_SO;
    m3_cmp_s(c, 0, 0, 0);
    CHECK(m3_cr_get(c, 0) == 3u);          /* EQ | SO */
    c->xer = 0;

    /* Bit indexing: PPC bit 0 is the MSB of cr0. */
    c->cr = 0;
    m3_crbit_set(c, 0, 1);
    CHECK(c->cr == 0x80000000u);
    CHECK(m3_crbit(c, 0) == 1u);
    m3_crbit_set(c, 31, 1);
    CHECK(c->cr == 0x80000001u);
    m3_crbit_set(c, 0, 0);
    CHECK(c->cr == 0x00000001u);
}

static void test_carry(void)
{
    m3_ppc_t *c = &m3_ctx;
    uint64_t t;

    c->xer = 0;
    /* addc 0xFFFFFFFF + 1 carries out. */
    t = (uint64_t)0xFFFFFFFFu + (uint64_t)1u;
    m3_set_ca(c, t >> 32);
    CHECK((uint32_t)t == 0u);
    CHECK(m3_xer_ca(c) == 1u);

    t = (uint64_t)0x7FFFFFFFu + (uint64_t)1u;
    m3_set_ca(c, t >> 32);
    CHECK(m3_xer_ca(c) == 0u);

    /* subfc rD = ~rA + rB + 1, i.e. rB - rA. Borrow shows as carry SET. */
    c->xer = 0;
    t = (uint64_t)(uint32_t)~5u + (uint64_t)10u + 1ull;   /* 10 - 5 */
    m3_set_ca(c, t >> 32);
    CHECK((uint32_t)t == 5u);
    CHECK(m3_xer_ca(c) == 1u);

    /* OV is sticky into SO. */
    c->xer = 0;
    m3_set_ov(c, 1);
    CHECK((c->xer & M3_XER_OV) != 0);
    CHECK((c->xer & M3_XER_SO) != 0);
    m3_set_ov(c, 0);
    CHECK((c->xer & M3_XER_OV) == 0);
    CHECK((c->xer & M3_XER_SO) != 0);      /* stays set */
}

static void test_cntlzw(void)
{
    CHECK(m3_cntlzw(0u)          == 32u);
    CHECK(m3_cntlzw(0x80000000u) == 0u);
    CHECK(m3_cntlzw(1u)          == 31u);
    CHECK(m3_cntlzw(0x00FF0000u) == 8u);
}

static void test_fcmp(void)
{
    m3_ppc_t *c = &m3_ctx;
    double nan = 0.0;
    c->cr = 0;
    m3_fcmp(c, 0, 1.0, 2.0);
    CHECK(m3_cr_get(c, 0) == 8u);
    m3_fcmp(c, 0, 2.0, 1.0);
    CHECK(m3_cr_get(c, 0) == 4u);
    m3_fcmp(c, 0, 1.0, 1.0);
    CHECK(m3_cr_get(c, 0) == 2u);
    /* Unordered sets bit 3 of the field, not LT/GT/EQ. */
    nan = nan / nan;
    m3_fcmp(c, 0, nan, 1.0);
    CHECK(m3_cr_get(c, 0) == 1u);
}

static int g_hit;
static void hit_fn(void) { g_hit++; }

static void test_func_table(void)
{
    func_table_init();
    func_table_register(0xFFF00100u, hit_fn);
    func_table_register(0x00000500u, hit_fn);
    CHECK(func_table_lookup(0xFFF00100u) == hit_fn);
    CHECK(func_table_lookup(0xDEADBEEFu) == NULL);

    g_hit = 0;
    CHECK(func_table_call(0xFFF00100u) == 1);
    CHECK(g_hit == 1);
    /* A miss must report failure, not crash or silently succeed. */
    CHECK(func_table_call(0x12345678u) == 0);
    CHECK(g_hit == 1);

    /* Survive a rehash: register enough to force growth past 1024. */
    {
        uint32_t a;
        for (a = 0; a < 4000; a++)
            func_table_register(0x100000u + a * 4u, hit_fn);
        CHECK(func_table_lookup(0x100000u + 3999u * 4u) == hit_fn);
        CHECK(func_table_lookup(0xFFF00100u) == hit_fn);   /* still there */
    }
}

int main(void)
{
    test_mask();
    test_rotl();
    test_rlwinm_clear_one_bit();
    test_cr();
    test_carry();
    test_cntlzw();
    test_fcmp();
    test_func_table();

    if (fails) {
        fprintf(stderr, "\n%d check(s) failed\n", fails);
        return 1;
    }
    printf("all PPC semantic checks passed\n");
    return 0;
}
