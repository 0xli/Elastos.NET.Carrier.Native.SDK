/*
 * Over-long strings from a peer must never abort the process (2026-11-24).
 *
 * Group 11 on snoopy aborted ("*** buffer overflow detected ***") and then
 * aborted again on every start: a friend's userinfo with a field longer than
 * the fixed CarrierUserInfo array was copied with strcpy, and the userinfo is
 * persisted, so loading the friend list hit the same copy. A friend request
 * had the same copy behind assert(), which a Release build drops.
 *
 *  (a) userinfo (notify_friend_description_cb, the path both a live update and
 *      the startup load take): every field over its limit is truncated, at a
 *      UTF-8 boundary, and the friend_info callback still fires once;
 *  (b) the same over-long userinfo again does not count as a change;
 *  (c) friend request (carrier_notify_friend_request): over-long name and
 *      description are truncated and the request is still delivered.
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

/* White-box: the userinfo handlers are static, so the test compiles carrier.c
 * in; the rest of libcarrier.a links as usual. */
#include "carrier.c"

static int failures = 0;
#define CHECK(cond, what) do { if (cond) printf("  ok   %s\n", what); \
    else { printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); failures++; } } while (0)

static int info_calls = 0;
static CarrierFriendInfo last_info;
static void on_friend_info(Carrier *w, const char *friendid, const CarrierFriendInfo *info, void *ctx)
{
    info_calls++;
    last_info = *info;
}

static int req_calls = 0;
static CarrierUserInfo last_req;
static void on_friend_request(Carrier *w, const char *userid, const CarrierUserInfo *info,
                              const char *hello, void *ctx)
{
    req_calls++;
    last_req = *info;
}

static char *repeat(const char *unit, size_t times)
{
    size_t u = strlen(unit);
    char *s = malloc(u * times + 1);
    for (size_t i = 0; i < times; i++) memcpy(s + i * u, unit, u);
    s[u * times] = '\0';
    return s;
}

static bool valid_utf8_prefix_of(const char *got, const char *src)
{
    size_t n = strlen(got);
    if (strncmp(got, src, n)) return false;
    /* must not end inside a multi-byte character */
    return ((unsigned char)src[n] & 0xC0) != 0x80;
}

int main(void)
{
    char dir[] = "/tmp/carrier-peerstr-XXXXXX";
    CarrierOptions opts;
    BootstrapNode bs;
    CarrierCallbacks cbs;
    Carrier *w;
    uint8_t secret[32];
    FriendInfo *fi;
    Packet *cp;
    uint8_t *buf;
    size_t len;

    char *name   = repeat("名", 100);       /* 300 bytes, 3-byte chars */
    char *descr  = repeat("d", 900);
    char *gender = repeat("g", 200);
    char *phone  = repeat("7", 200);
    char *email  = repeat("e", 300);
    char *region = repeat("r", 300);

    if (!mkdtemp(dir)) { printf("mkdtemp failed\n"); return 1; }
    memset(&opts, 0, sizeof(opts)); memset(&bs, 0, sizeof(bs)); memset(&cbs, 0, sizeof(cbs));
    for (int i = 0; i < 32; i++) secret[i] = (uint8_t)(0x31 + i);
    bs.ipv4 = "127.0.0.1"; bs.port = "33445";
    bs.public_key = "3KygiXesrAACKrF9cnWQrcWW5KqSRY9rKzVqkKUsEoWL";
    opts.persistent_location = dir;
    opts.secret_key = secret;
    opts.udp_enabled = true;
    opts.log_level = CarrierLogLevel_Error;
    opts.bootstraps_size = 1;
    opts.bootstraps = &bs;
    cbs.friend_info = on_friend_info;
    cbs.friend_request = on_friend_request;

    w = carrier_new(&opts, &cbs, NULL);
    CHECK(w != NULL, "carrier_new");
    if (!w) return 1;

    printf("(a) over-long userinfo from a friend\n");
    fi = (FriendInfo *)rc_zalloc(sizeof(FriendInfo), NULL);
    strcpy(fi->info.user_info.userid, "7UNFCSSc1111111111111111111111111111111111");
    fi->friend_number = 7;
    friends_put(w->friends, fi);
    deref(fi);

    cp = packet_create(PACKET_TYPE_USERINFO, NULL);
    packet_set_name(cp, name);
    packet_set_descr(cp, descr);
    packet_set_gender(cp, gender);
    packet_set_phone(cp, phone);
    packet_set_email(cp, email);
    packet_set_region(cp, region);
    buf = packet_encode(cp, &len);
    packet_free(cp);
    printf("  userinfo packet %zu bytes\n", len);

    notify_friend_description_cb(7, buf, len, w);
    CHECK(info_calls == 1, "process alive, friend_info callback fired once");
    CHECK(strlen(last_info.user_info.name) <= CARRIER_MAX_USER_NAME_LEN &&
          strlen(last_info.user_info.name) >= CARRIER_MAX_USER_NAME_LEN - 2 &&
          valid_utf8_prefix_of(last_info.user_info.name, name), "name truncated at a UTF-8 boundary");
    CHECK(strlen(last_info.user_info.description) == CARRIER_MAX_USER_DESCRIPTION_LEN, "description truncated to 127");
    CHECK(strlen(last_info.user_info.gender) == CARRIER_MAX_GENDER_LEN, "gender truncated to 31");
    CHECK(strlen(last_info.user_info.phone) == CARRIER_MAX_PHONE_LEN, "phone truncated to 31");
    CHECK(strlen(last_info.user_info.email) == CARRIER_MAX_EMAIL_LEN, "email truncated to 127");
    CHECK(strlen(last_info.user_info.region) == CARRIER_MAX_REGION_LEN, "region truncated to 127");

    printf("(b) the same userinfo again is not a change\n");
    notify_friend_description_cb(7, buf, len, w);
    CHECK(info_calls == 1, "no second friend_info callback");
    free(buf);

    printf("(c) over-long friend request\n");
    {
        uint8_t pk[32];
        for (int i = 0; i < 32; i++) pk[i] = (uint8_t)(0xc0 + i);
        cp = packet_create(PACKET_TYPE_FRIEND_REQUEST, NULL);
        packet_set_name(cp, name);
        packet_set_descr(cp, descr);
        packet_set_hello(cp, "hi");
        buf = packet_encode(cp, &len);
        packet_free(cp);
        carrier_notify_friend_request(pk, buf, len, w);
        free(buf);
        CHECK(req_calls == 1, "process alive, request delivered");
        CHECK(valid_utf8_prefix_of(last_req.name, name) && strlen(last_req.name) <= CARRIER_MAX_USER_NAME_LEN,
              "request name truncated at a UTF-8 boundary");
        CHECK(strlen(last_req.description) == CARRIER_MAX_USER_DESCRIPTION_LEN, "request description truncated");
    }

    carrier_kill(w);
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
