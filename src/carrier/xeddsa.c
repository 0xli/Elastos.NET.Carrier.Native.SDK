/* See xeddsa.h. Field arithmetic for the X25519 -> Ed25519 public-key
 * conversion is the public-domain TweetNaCl gf code (the same code
 * curve25519-js ports); everything else is libsodium. */

#include <string.h>
#include <sodium.h>
#include "xeddsa.h"

typedef int64_t gf[16];

static const gf gf1 = {1};

static void car25519(gf o)
{
    int i;
    int64_t c;
    for (i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b)
{
    int64_t t, i, c = ~(b - 1);
    for (i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n)
{
    int i, j, b;
    gf m, t;
    for (i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (i = 0; i < 16; i++) {
        o[2 * i] = t[i] & 0xff;
        o[2 * i + 1] = t[i] >> 8;
    }
}

static void unpack25519(gf o, const uint8_t *n)
{
    int i;
    for (i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void fe_add(gf o, const gf a, const gf b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fe_sub(gf o, const gf a, const gf b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void fe_mul(gf o, const gf a, const gf b)
{
    int64_t i, j, t[31];
    for (i = 0; i < 31; i++) t[i] = 0;
    for (i = 0; i < 16; i++) for (j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}

static void fe_inv(gf o, const gf i)
{
    gf c;
    int a;
    for (a = 0; a < 16; a++) c[a] = i[a];
    for (a = 253; a >= 0; a--) {
        fe_mul(c, c, c);
        if (a != 2 && a != 4) fe_mul(c, c, i);
    }
    for (a = 0; a < 16; a++) o[a] = c[a];
}

void xeddsa_convert_public_key(uint8_t ed[32], const uint8_t x25519[32])
{
    gf x, a, b;
    unpack25519(x, x25519);
    fe_add(a, x, gf1);     /* u + 1 */
    fe_sub(b, x, gf1);     /* u - 1 */
    fe_inv(a, a);
    fe_mul(a, a, b);       /* (u - 1) / (u + 1) */
    pack25519(ed, a);
}

int xeddsa_sign(uint8_t sig[64], const uint8_t sk[32],
                const uint8_t *msg, size_t len, const uint8_t *random)
{
    uint8_t a[32], a_wide[64], a_red[32], A[32];
    uint8_t prefix[32], rnd[64], hash[64], r[32], R[32], h[32], ha[32], s[32];
    crypto_hash_sha512_state st;
    int rc = -1;

    if (!sig || !sk || (!msg && len)) return -1;
    if (sodium_init() < 0) return -1;

    if (random) memcpy(rnd, random, 64); else randombytes_buf(rnd, 64);

    /* Ed25519 private scalar and public key from the X25519 secret. */
    memcpy(a, sk, 32);
    a[0] &= 248; a[31] &= 127; a[31] |= 64;
    if (crypto_scalarmult_ed25519_base_noclamp(A, a) != 0) goto out;

    /* r = H(0xFE ‖ 0xFF*31 ‖ a ‖ msg ‖ Z) mod L */
    prefix[0] = 0xFE; memset(prefix + 1, 0xFF, 31);
    crypto_hash_sha512_init(&st);
    crypto_hash_sha512_update(&st, prefix, 32);
    crypto_hash_sha512_update(&st, a, 32);
    if (len) crypto_hash_sha512_update(&st, msg, len);
    crypto_hash_sha512_update(&st, rnd, 64);
    crypto_hash_sha512_final(&st, hash);
    crypto_core_ed25519_scalar_reduce(r, hash);
    if (crypto_scalarmult_ed25519_base_noclamp(R, r) != 0) goto out;

    /* h = H(R ‖ A ‖ msg) mod L */
    crypto_hash_sha512_init(&st);
    crypto_hash_sha512_update(&st, R, 32);
    crypto_hash_sha512_update(&st, A, 32);
    if (len) crypto_hash_sha512_update(&st, msg, len);
    crypto_hash_sha512_final(&st, hash);
    crypto_core_ed25519_scalar_reduce(h, hash);

    /* s = r + h * a mod L  (a reduced first; same value mod L) */
    memset(a_wide, 0, 64); memcpy(a_wide, a, 32);
    crypto_core_ed25519_scalar_reduce(a_red, a_wide);
    crypto_core_ed25519_scalar_mul(ha, h, a_red);
    crypto_core_ed25519_scalar_add(s, r, ha);

    memcpy(sig, R, 32);
    memcpy(sig + 32, s, 32);
    sig[63] |= (A[31] & 0x80);   /* curve25519-js carries A's sign bit here */
    rc = 0;

out:
    sodium_memzero(a, sizeof(a)); sodium_memzero(a_wide, sizeof(a_wide));
    sodium_memzero(a_red, sizeof(a_red)); sodium_memzero(rnd, sizeof(rnd));
    sodium_memzero(hash, sizeof(hash)); sodium_memzero(r, sizeof(r));
    sodium_memzero(h, sizeof(h)); sodium_memzero(ha, sizeof(ha));
    sodium_memzero(s, sizeof(s));
    return rc;
}

int xeddsa_verify(const uint8_t pk[32], const uint8_t *msg, size_t len,
                  const uint8_t sig[64])
{
    uint8_t edpk[32], s[64];

    if (!pk || !sig || (!msg && len)) return 0;
    if (sodium_init() < 0) return 0;

    xeddsa_convert_public_key(edpk, pk);
    edpk[31] |= (sig[63] & 0x80);
    memcpy(s, sig, 64);
    s[63] &= 0x7f;
    return crypto_sign_ed25519_verify_detached(s, msg, len, edpk) == 0 ? 1 : 0;
}
