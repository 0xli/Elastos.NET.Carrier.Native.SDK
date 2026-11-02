/*
 * Unit tests for the requester's avatar on a friend request (2026-11-01):
 * friendreq gains gender (CryptoPunk id) and avatar_url, appended.
 *
 *  (a) an OLD reader (the 3-field schema as shipped in iOS/Android, flatcc
 *      verifier ON) accepts a 5-field request and reads name/descr/hello;
 *  (b) the NEW reader accepts an old 3-field request (fields absent);
 *      a new writer with no avatar emits the same bytes as an old writer;
 *  (c) the transport cap: TOX_MAX_FRIEND_REQUEST_LENGTH, what a request
 *      with name 63 / descr 127 / punk / 128-byte URL leaves for hello, and
 *      that avatar_url is dropped first when the request would not fit;
 *  (d) the receive path: the request lands on the friend_request callback
 *      with info->gender = punk and carrier_get_friend_request_profile_ext()
 *      returns the URL. Express offline requests enter through the same
 *      function (handle_offline_friend_request_cb -> notify_friend_request_cb)
 *      with the same bytes carrier_add_friend hands to Express.
 * No network: a Carrier is created from a fixed secret in a temp dir and
 * never run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>

#include <carrier.h>
#include <tox/tox.h>
#include "packet.h"
#include "carrier_impl.h"
#include "packet_v1_generated.h"

static int failures = 0;
#define CHECK(cond, what) do { if (cond) printf("  ok   %s\n", what); \
    else { printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); failures++; } } while (0)

static const char *PUNK = "4242";
static const char *URL  = "https://cdn.example.invalid/avatars/4242.png";

static uint8_t *encode_new(const char *name, const char *descr, const char *hello,
                           const char *gender, const char *url, size_t *len)
{
    Packet *cp = packet_create(PACKET_TYPE_FRIEND_REQUEST, NULL);
    uint8_t *out;
    packet_set_name(cp, name);
    packet_set_descr(cp, descr);
    packet_set_hello(cp, hello);
    if (gender) packet_set_gender(cp, gender);
    if (url) packet_set_avatar_url(cp, url);
    out = packet_encode(cp, len);
    packet_free(cp);
    return out;
}

/* The OLD writer: the 3-field builder from packet_v1.fbs. */
static uint8_t *encode_v1(const char *name, const char *descr, const char *hello, size_t *len)
{
    flatcc_builder_t b;
    flatbuffers_string_ref_t s;
    carrier_v1_friendreq_ref_t ref;
    uint8_t *out;

    flatcc_builder_init(&b);
    carrier_v1_friendreq_start(&b);
    s = flatcc_builder_create_string_str(&b, name);  carrier_v1_friendreq_name_add(&b, s);
    s = flatcc_builder_create_string_str(&b, descr); carrier_v1_friendreq_descr_add(&b, s);
    s = flatcc_builder_create_string_str(&b, hello); carrier_v1_friendreq_hello_add(&b, s);
    ref = carrier_v1_friendreq_end(&b);
    carrier_v1_packet_start_as_root(&b);
    carrier_v1_packet_type_add(&b, PACKET_TYPE_FRIEND_REQUEST);
    carrier_v1_packet_body_add(&b, carrier_v1_anybody_as_friendreq(ref));
    carrier_v1_packet_end_as_root(&b);
    out = flatcc_builder_finalize_buffer(&b, len);
    flatcc_builder_clear(&b);
    return out;
}

static void test_old_reader(void)
{
    size_t len;
    uint8_t *buf = encode_new("Ann", "hi there", "pin-1", PUNK, URL, &len);
    carrier_v1_packet_table_t pkt;
    carrier_v1_friendreq_table_t freq;

    printf("(a) old reader (3-field schema, verifier on) on a 5-field request\n");
    CHECK(buf != NULL, "new writer encodes");
    CHECK(carrier_v1_packet_verify_as_root(buf, len) == 0, "old verifier accepts the buffer");
    pkt = carrier_v1_packet_as_root(buf);
    CHECK(pkt != NULL, "old reader opens the root");
    CHECK(carrier_v1_packet_type(pkt) == PACKET_TYPE_FRIEND_REQUEST, "type is friend request");
    CHECK(carrier_v1_packet_body_type(pkt) == carrier_v1_anybody_friendreq, "union tag is friendreq");
    freq = carrier_v1_packet_body(pkt);
    CHECK(freq != NULL && !strcmp(carrier_v1_friendreq_name(freq), "Ann"), "old reader: name");
    CHECK(!strcmp(carrier_v1_friendreq_descr(freq), "hi there"), "old reader: descr");
    CHECK(!strcmp(carrier_v1_friendreq_hello(freq), "pin-1"), "old reader: hello");
    /* The new fields are there for a new reader of the same bytes. */
    {
        Packet *cp = packet_decode(buf, len);
        CHECK(cp && !strcmp(packet_get_gender(cp), PUNK) && !strcmp(packet_get_avatar_url(cp), URL),
              "same bytes: new reader sees punk + URL");
        packet_free(cp);
    }
    free(buf);
}

static void test_new_reader_on_old_packet(void)
{
    size_t len, len2;
    uint8_t *buf = encode_v1("Bob", "yo", "pin-2", &len);
    uint8_t *buf2;
    Packet *cp;

    printf("(b) new reader on an old 3-field request\n");
    CHECK(buf != NULL, "old writer encodes");
    cp = packet_decode(buf, len);
    CHECK(cp != NULL && packet_get_type(cp) == PACKET_TYPE_FRIEND_REQUEST, "new reader decodes it");
    CHECK(cp && !strcmp(packet_get_name(cp), "Bob") && !strcmp(packet_get_descr(cp), "yo") &&
          !strcmp(packet_get_hello(cp), "pin-2"), "name/descr/hello read");
    CHECK(cp && packet_get_gender(cp) == NULL, "gender absent -> NULL");
    CHECK(cp && packet_get_avatar_url(cp) == NULL, "avatar_url absent -> NULL");
    packet_free(cp);

    buf2 = encode_new("Bob", "yo", "pin-2", "", "", &len2);
    CHECK(len2 == len && memcmp(buf, buf2, len) == 0,
          "new writer without an avatar emits the old writer's exact bytes");
    free(buf2);
    free(buf);
}

static void test_round_trip(void)
{
    size_t len;
    uint8_t *buf = encode_new("Cy", "d", "h", PUNK, URL, &len);
    Packet *cp = packet_decode(buf, len);
    printf("round trip\n");
    CHECK(cp && !strcmp(packet_get_gender(cp), PUNK), "gender round-trips");
    CHECK(cp && !strcmp(packet_get_avatar_url(cp), URL), "avatar_url round-trips");
    packet_free(cp);
    free(buf);
    buf = encode_new("Cy", "d", "h", PUNK, NULL, &len);
    cp = packet_decode(buf, len);
    CHECK(cp && !strcmp(packet_get_gender(cp), PUNK) && packet_get_avatar_url(cp) == NULL,
          "punk without URL");
    packet_free(cp);
    free(buf);
}

static void test_size_cap(void)
{
    char name[CARRIER_MAX_USER_NAME_LEN + 1], descr[CARRIER_MAX_USER_DESCRIPTION_LEN + 1];
    char url[CARRIER_MAX_FRIEND_REQUEST_AVATAR_URL_LEN + 1];
    char hello[2048];
    size_t len, base, budget, i;
    uint8_t *buf;

    printf("(c) transport cap\n");
    CHECK(TOX_MAX_FRIEND_REQUEST_LENGTH == CARRIER_MAX_FRIEND_REQUEST_PACKET_LEN,
          "CARRIER_MAX_FRIEND_REQUEST_PACKET_LEN == toxcore TOX_MAX_FRIEND_REQUEST_LENGTH");
    printf("  TOX_MAX_FRIEND_REQUEST_LENGTH = %d\n", TOX_MAX_FRIEND_REQUEST_LENGTH);

    memset(name, 'n', sizeof(name) - 1); name[sizeof(name) - 1] = 0;
    memset(descr, 'd', sizeof(descr) - 1); descr[sizeof(descr) - 1] = 0;
    memcpy(url, "https://", 8); memset(url + 8, 'u', sizeof(url) - 9); url[sizeof(url) - 1] = 0;

    buf = encode_new(name, descr, "", "9999", url, &base); free(buf);
    printf("  name 63 + descr 127 + punk \"9999\" + URL 128, empty hello: %zu bytes\n", base);
    buf = encode_new(name, descr, "", "9999", NULL, &len); free(buf);
    printf("  same without the URL: %zu bytes\n", len);
    buf = encode_new(name, descr, "", NULL, NULL, &len); free(buf);
    printf("  legacy 3-field, empty hello: %zu bytes\n", len);

    /* Largest hello that still fits with everything else at its maximum. */
    budget = 0;
    for (i = 1; i < sizeof(hello) - 1; i++) {
        memset(hello, 'h', i); hello[i] = 0;
        buf = encode_new(name, descr, hello, "9999", url, &len); free(buf);
        if (len > CARRIER_MAX_FRIEND_REQUEST_PACKET_LEN) break;
        budget = i;
    }
    printf("  hello budget with the URL: %zu bytes; the SDK puts no limit of its own on hello\n", budget);
    CHECK(budget >= 512, "a 512-byte hello still fits beside a 128-byte URL");

    /* Over the cap: packet_encode_fit drops the URL, keeps the punk. */
    {
        Packet *cp = packet_create(PACKET_TYPE_FRIEND_REQUEST, NULL);
        bool dropped = false;
        memset(hello, 'h', budget + 1); hello[budget + 1] = 0;
        packet_set_name(cp, name); packet_set_descr(cp, descr); packet_set_hello(cp, hello);
        packet_set_gender(cp, "9999"); packet_set_avatar_url(cp, url);
        buf = packet_encode_fit(cp, CARRIER_MAX_FRIEND_REQUEST_PACKET_LEN, &len, &dropped);
        CHECK(buf && dropped, "one byte over: avatar_url dropped");
        CHECK(len <= CARRIER_MAX_FRIEND_REQUEST_PACKET_LEN, "and the request now fits");
        if (buf) {
            Packet *rd = packet_decode(buf, len);
            CHECK(rd && !strcmp(packet_get_gender(rd), "9999") && packet_get_avatar_url(rd) == NULL,
                  "punk kept, URL gone");
            CHECK(rd && !strcmp(packet_get_hello(rd), hello), "hello untouched");
            packet_free(rd);
            free(buf);
        }
        packet_free(cp);
    }
    {
        /* Under the cap: nothing dropped. */
        Packet *cp = packet_create(PACKET_TYPE_FRIEND_REQUEST, NULL);
        bool dropped = true;
        packet_set_name(cp, name); packet_set_descr(cp, descr); packet_set_hello(cp, "pin");
        packet_set_gender(cp, "9999"); packet_set_avatar_url(cp, url);
        buf = packet_encode_fit(cp, CARRIER_MAX_FRIEND_REQUEST_PACKET_LEN, &len, &dropped);
        CHECK(buf && !dropped && len <= CARRIER_MAX_FRIEND_REQUEST_PACKET_LEN, "a short hello keeps the URL");
        free(buf);
        packet_free(cp);
    }
}

/* (d) the receive path, through the DHT hook carrier_new installs. */
static struct {
    int calls;
    char userid[CARRIER_MAX_ID_LEN + 1];
    CarrierUserInfo info;
    char hello[256];
    CarrierProfileExt ext_in_cb;
    int ext_rc_in_cb;
} got;

static void on_friend_request(Carrier *w, const char *userid, const CarrierUserInfo *info,
                              const char *hello, void *context)
{
    (void)context;
    got.calls++;
    strcpy(got.userid, userid);
    got.info = *info;
    strncpy(got.hello, hello, sizeof(got.hello) - 1);
    got.ext_rc_in_cb = carrier_get_friend_request_profile_ext(w, userid, &got.ext_in_cb);
}

static void test_receive_path(void)
{
    char dir[] = "/tmp/carrier-freq-XXXXXX";
    CarrierOptions opts;
    BootstrapNode bs;
    CarrierCallbacks cbs;
    Carrier *w;
    CarrierProfileExt ext;
    uint8_t secret[32];
    uint8_t requester_pk[32];
    size_t len;
    uint8_t *buf;
    char requester_id[CARRIER_MAX_ID_LEN + 1];
    size_t idlen = sizeof(requester_id);

    printf("(d) receive path: callback + carrier_get_friend_request_profile_ext\n");
    if (!mkdtemp(dir)) { CHECK(0, "mkdtemp"); return; }
    memset(&opts, 0, sizeof(opts)); memset(&bs, 0, sizeof(bs)); memset(&cbs, 0, sizeof(cbs));
    for (int i = 0; i < 32; i++) secret[i] = (uint8_t)(0x11 + i);
    for (int i = 0; i < 32; i++) requester_pk[i] = (uint8_t)(0xa0 + i);
    bs.ipv4 = "127.0.0.1"; bs.port = "33445";
    bs.public_key = "3KygiXesrAACKrF9cnWQrcWW5KqSRY9rKzVqkKUsEoWL";
    opts.persistent_location = dir;
    opts.secret_key = secret;
    opts.udp_enabled = true;
    opts.log_level = CarrierLogLevel_Error;
    opts.bootstraps_size = 1;
    opts.bootstraps = &bs;
    cbs.friend_request = on_friend_request;

    w = carrier_new(&opts, &cbs, NULL);
    CHECK(w != NULL, "carrier_new");
    if (!w) { printf("  error 0x%x\n", carrier_get_error()); return; }

    base58_encode(requester_pk, 32, requester_id, &idlen);
    CHECK(carrier_get_friend_request_profile_ext(w, requester_id, &ext) == -1 &&
          carrier_get_error() == CARRIER_GENERAL_ERROR(ERROR_NOT_EXIST), "unknown requester: ERROR_NOT_EXIST");

    /* A 5-field request from a new client. */
    memset(&got, 0, sizeof(got));
    buf = encode_new("Dee", "status", "pin-d", PUNK, URL, &len);
    carrier_notify_friend_request(requester_pk, buf, len, w);
    free(buf);
    CHECK(got.calls == 1, "friend_request callback fired");
    CHECK(!strcmp(got.userid, requester_id), "userid is the requester");
    CHECK(!strcmp(got.info.name, "Dee") && !strcmp(got.info.description, "status") &&
          !strcmp(got.hello, "pin-d"), "name/descr/hello as before");
    CHECK(!strcmp(got.info.gender, PUNK), "info->gender carries the punk id");
    CHECK(got.ext_rc_in_cb == 0 && !strcmp(got.ext_in_cb.avatar_url, URL),
          "avatar URL readable inside the callback");
    CHECK(carrier_get_friend_request_profile_ext(w, requester_id, &ext) == 0 && !strcmp(ext.avatar_url, URL),
          "and later, for the pending list");

    /* An old 3-field request from the same requester replaces it: no avatar. */
    memset(&got, 0, sizeof(got));
    buf = encode_v1("Dee", "status", "pin-e", &len);
    carrier_notify_friend_request(requester_pk, buf, len, w);
    free(buf);
    CHECK(got.calls == 1 && got.info.gender[0] == '\0', "old sender: gender empty");
    CHECK(carrier_get_friend_request_profile_ext(w, requester_id, &ext) == 0 && ext.avatar_url[0] == '\0',
          "old sender: avatar_url empty, entry present");

    /* Ring eviction: 32 slots. */
    for (int n = 0; n < CARRIER_FRIEND_REQUEST_EXT_SLOTS; n++) {
        uint8_t pk[32]; memset(pk, (uint8_t)n, 32); pk[0] = 1;
        buf = encode_new("x", "y", "z", "1", "https://e.invalid/1", &len);
        carrier_notify_friend_request(pk, buf, len, w);
        free(buf);
    }
    CHECK(carrier_get_friend_request_profile_ext(w, requester_id, &ext) == -1,
          "evicted after 32 newer requesters");

    carrier_kill(w);
    { char cmd[300]; snprintf(cmd, sizeof(cmd), "rm -rf %s", dir); (void)system(cmd); }
}

int main(void)
{
    test_old_reader();
    test_new_reader_on_old_packet();
    test_round_trip();
    test_size_cap();
    test_receive_path();
    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
