/*
 * Unit tests for AgentNet proto v2 additions: XEdDSA identity signatures
 * (byte-compatible with the JavaScript peer), key export, client metadata
 * and the profile extension. No network: a Carrier is created from a fixed
 * secret in a temp dir and never run.
 *
 * Usage: identity_test [signature-out.json]
 *   With a path, also writes a fresh randomized signature for the JS side to
 *   verify (tools/xeddsa-verify.mjs).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include <carrier.h>
#include "xeddsa.h"
#include "xeddsa_vectors.h"

static int failures = 0;
#define CHECK(cond, what) do { if (cond) printf("  ok   %s\n", what); \
    else { printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); failures++; } } while (0)

static void hexdump(FILE *f, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) fprintf(f, "%02x", b[i]);
}

static void test_vectors(void)
{
    uint8_t sig[64];
    printf("xeddsa vectors from curve25519-js\n");
    for (size_t i = 0; i < sizeof(VEC_CASES) / sizeof(VEC_CASES[0]); i++) {
        const char *m = VEC_CASES[i].msg;
        int rc = xeddsa_sign(sig, VEC_SK, (const uint8_t *)m, strlen(m), VEC_RND);
        CHECK(rc == 0, "sign returns 0");
        CHECK(memcmp(sig, VEC_CASES[i].sig, 64) == 0, "signature bytes match the JS peer");
        CHECK(xeddsa_verify(VEC_PK, (const uint8_t *)m, strlen(m), VEC_CASES[i].sig) == 1,
              "JS signature verifies in C");
        sig[10] ^= 1;
        CHECK(xeddsa_verify(VEC_PK, (const uint8_t *)m, strlen(m), sig) == 0, "tampered signature rejected");
        CHECK(xeddsa_verify(VEC_PK, (const uint8_t *)"other", 5, VEC_CASES[i].sig) == 0, "other message rejected");
    }
    /* A public key whose Ed25519 form has the sign bit set: sig[63] carries it. */
    CHECK((VEC2_SIG[63] & 0x80) != 0, "vector 2 exercises the sign bit");
    CHECK(xeddsa_verify(VEC2_PK, (const uint8_t *)VEC2_MSG, strlen(VEC2_MSG), VEC2_SIG) == 1,
          "sign-bit signature verifies");
    int rc = xeddsa_sign(sig, VEC2_SK, (const uint8_t *)VEC2_MSG, strlen(VEC2_MSG), VEC_RND);
    CHECK(rc == 0 && memcmp(sig, VEC2_SIG, 64) == 0, "sign-bit signature reproduced");
}

static void test_random_signatures(void)
{
    uint8_t a[64], b[64];
    const char *m = "randomized";
    printf("randomized signatures\n");
    CHECK(xeddsa_sign(a, VEC_SK, (const uint8_t *)m, strlen(m), NULL) == 0, "sign A");
    CHECK(xeddsa_sign(b, VEC_SK, (const uint8_t *)m, strlen(m), NULL) == 0, "sign B");
    CHECK(memcmp(a, b, 64) != 0, "two signatures of one message differ");
    CHECK(xeddsa_verify(VEC_PK, (const uint8_t *)m, strlen(m), a) == 1, "A verifies");
    CHECK(xeddsa_verify(VEC_PK, (const uint8_t *)m, strlen(m), b) == 1, "B verifies");
}

static void test_carrier_api(const char *sig_out_path)
{
    char dir[] = "/tmp/carrier-unit-XXXXXX";
    CarrierOptions opts;
    BootstrapNode bs;
    Carrier *w;
    char userid[CARRIER_MAX_ID_LEN + 1];
    uint8_t secret[32], sig[64];
    const char *m = "decent-auth\nhttps://app.beagle.chat\nnonce-xyz";
    CarrierClientInfo ci, ci2;
    CarrierProfileExt ext, ext2;

    printf("carrier API\n");
    if (!mkdtemp(dir)) { CHECK(0, "mkdtemp"); return; }

    memset(&opts, 0, sizeof(opts));
    memset(&bs, 0, sizeof(bs));
    bs.ipv4 = "127.0.0.1"; bs.port = "33445";
    bs.public_key = "3KygiXesrAACKrF9cnWQrcWW5KqSRY9rKzVqkKUsEoWL";
    opts.persistent_location = dir;
    opts.secret_key = VEC_SK;
    opts.udp_enabled = true;
    opts.log_level = CarrierLogLevel_Error;
    opts.bootstraps_size = 1;
    opts.bootstraps = &bs;

    w = carrier_new(&opts, NULL, NULL);
    CHECK(w != NULL, "carrier_new from a fixed secret");
    if (!w) { printf("  error 0x%x\n", carrier_get_error()); return; }

    CHECK(carrier_export_secret_key(w, secret) == 0 && memcmp(secret, VEC_SK, 32) == 0,
          "exported secret is the one imported");
    CHECK(carrier_get_userid(w, userid, sizeof(userid)) != NULL, "userid");

    CHECK(carrier_identity_sign(w, (const uint8_t *)m, strlen(m), sig) == 0, "carrier_identity_sign");
    CHECK(carrier_identity_verify(userid, (const uint8_t *)m, strlen(m), sig) == 1,
          "verifies against the userid");
    CHECK(carrier_identity_verify(userid, (const uint8_t *)"nope", 4, sig) == 0, "wrong message rejected");
    CHECK(carrier_identity_verify("not-a-userid", (const uint8_t *)m, strlen(m), sig) == -1, "bad userid is an error");
    CHECK(xeddsa_verify(VEC_PK, (const uint8_t *)m, strlen(m), sig) == 1, "userid decodes to the vector public key");
    CHECK(carrier_identity_sign(w, (const uint8_t *)m, 0, sig) == -1, "empty message refused");

    if (sig_out_path) {
        FILE *f = fopen(sig_out_path, "w");
        if (f) {
            fprintf(f, "{\"userid\":\"%s\",\"pk\":\"", userid); hexdump(f, VEC_PK, 32);
            fprintf(f, "\",\"msg\":\"decent-auth\\nhttps://app.beagle.chat\\nnonce-xyz\",\"sig\":\""); hexdump(f, sig, 64);
            fprintf(f, "\"}\n"); fclose(f);
            printf("  wrote %s\n", sig_out_path);
        }
    }

    CHECK(carrier_get_client_info(w, &ci) == 0 && ci.proto_version == CARRIER_AGENTNET_PROTO_VERSION,
          "advertises the current protocol version by default");
    CHECK(*ci.platform != '\0', "has a default platform");
    memset(&ci2, 0, sizeof(ci2));
    strcpy(ci2.platform, "ios"); strcpy(ci2.os_version, "26.0"); strcpy(ci2.app_version, "beagle-1.8.28");
    CHECK(carrier_set_client_info(w, &ci2) == 0, "set client info");
    CHECK(carrier_get_client_info(w, &ci) == 0 && !strcmp(ci.platform, "ios") &&
          !strcmp(ci.app_version, "beagle-1.8.28") && ci.proto_version == CARRIER_AGENTNET_PROTO_VERSION,
          "client info round-trips, version forced");

    memset(&ext, 0, sizeof(ext));
    strcpy(ext.avatar_url, "https://example.invalid/a.png");
    strcpy(ext.url, "https://example.invalid");
    strcpy(ext.ens, "air.beagles.eth");
    strcpy(ext.extra, "{\"k\":1}");
    CHECK(carrier_set_self_profile_ext(w, &ext) == 0, "set profile ext");
    CHECK(carrier_get_self_profile_ext(w, &ext2) == 0 && !strcmp(ext2.ens, "air.beagles.eth") &&
          !strcmp(ext2.avatar_url, ext.avatar_url), "profile ext round-trips");

    /* Too big for the status message: every field at its max plus a long name. */
    memset(&ext, 'a', sizeof(ext));
    ext.avatar_url[CARRIER_MAX_AVATAR_URL_LEN] = 0; ext.url[CARRIER_MAX_URL_LEN] = 0;
    ext.ens[CARRIER_MAX_ENS_LEN] = 0; ext.extra[CARRIER_MAX_PROFILE_EXTRA_LEN] = 0;
    {
        CarrierUserInfo me;
        carrier_get_self_info(w, &me);
        memset(me.description, 'd', CARRIER_MAX_USER_DESCRIPTION_LEN); me.description[CARRIER_MAX_USER_DESCRIPTION_LEN] = 0;
        memset(me.region, 'r', CARRIER_MAX_REGION_LEN); me.region[CARRIER_MAX_REGION_LEN] = 0;
        memset(me.email, 'e', CARRIER_MAX_EMAIL_LEN); me.email[CARRIER_MAX_EMAIL_LEN] = 0;
        CHECK(carrier_set_self_info(w, &me) == 0, "big legacy profile still fits");
        CHECK(carrier_set_self_profile_ext(w, &ext) == -1, "oversized profile refused");
        CHECK(carrier_get_self_profile_ext(w, &ext2) == 0 && !strcmp(ext2.ens, "air.beagles.eth"),
              "refused write leaves the old value");
    }

    carrier_kill(w);

    /* Reopen: the fields must come back from carrier.data. */
    opts.secret_key = NULL;
    w = carrier_new(&opts, NULL, NULL);
    CHECK(w != NULL, "reopen");
    if (w) {
        CHECK(carrier_get_client_info(w, &ci) == 0 && !strcmp(ci.app_version, "beagle-1.8.28"), "client info persisted");
        CHECK(carrier_get_self_profile_ext(w, &ext2) == 0 && !strcmp(ext2.ens, "air.beagles.eth"), "profile ext persisted");
        CHECK(carrier_export_secret_key(w, secret) == 0 && memcmp(secret, VEC_SK, 32) == 0, "same identity after reopen");
        carrier_kill(w);
    }
    {
        char cmd[300]; snprintf(cmd, sizeof(cmd), "rm -rf %s", dir); (void)system(cmd);
    }
}

int main(int argc, char **argv)
{
    test_vectors();
    test_random_signatures();
    test_carrier_api(argc > 1 ? argv[1] : NULL);
    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
