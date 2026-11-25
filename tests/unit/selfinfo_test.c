/*
 * Bounded self info: what the application hands in is length-checked too.
 *
 * CarrierUserInfo and friends carry fixed char arrays. A language binding
 * that copies a longer string into one of them leaves the field without a
 * terminator inside its array; the setters used to strcpy/strlen such a field.
 *
 *  (a) carrier_set_self_info: a field over its limit is truncated at a UTF-8
 *      boundary, the call succeeds, and what is stored is terminated;
 *  (b) every string field full to the last byte: same;
 *  (c) carrier_set_client_info / carrier_set_self_profile_ext: an
 *      unterminated field is refused with invalid-args;
 *  (d) send, id and address checks refuse over-long input before copying it;
 *  (e) carrier_get_strerror with a buffer exactly as long as the facility
 *      prefix writes nothing past it;
 *  (f) the invite history key takes the longest "id:extension" address;
 *  (g) an invite response that carries a status but no reason is handled.
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

/* White-box: some of the paths are static, so the test compiles carrier.c
 * in; the rest of libcarrier.a links as usual. */
#include "carrier.c"

static int failures = 0;
#define CHECK(cond, what) do { if (cond) printf("  ok   %s\n", what); \
    else { printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); failures++; } } while (0)

#define TERMINATED(a) (memchr((a), 0, sizeof(a)) != NULL)

static int irsp_calls = 0;
static int irsp_status = 0;
static bool irsp_reason_ok = false;
static void on_invite_response(Carrier *w, const char *from, const char *bundle,
                               int status, const char *reason,
                               const void *data, size_t len, void *ctx)
{
    irsp_calls++;
    irsp_status = status;
    irsp_reason_ok = (reason != NULL && *reason == '\0');
}

static bool invalid_args(void)
{
    return carrier_get_error() == CARRIER_GENERAL_ERROR(ERROR_INVALID_ARGS);
}

int main(void)
{
    char dir[] = "/tmp/carrier-selfinfo-XXXXXX";
    CarrierOptions opts;
    BootstrapNode bs;
    CarrierCallbacks cbs;
    Carrier *w;
    uint8_t secret[32];
    CarrierUserInfo ui, got;
    int rc;

    if (!mkdtemp(dir)) { printf("mkdtemp failed\n"); return 1; }
    memset(&opts, 0, sizeof(opts)); memset(&bs, 0, sizeof(bs)); memset(&cbs, 0, sizeof(cbs));
    for (int i = 0; i < 32; i++) secret[i] = (uint8_t)(0x41 + i);
    bs.ipv4 = "127.0.0.1"; bs.port = "33445";
    bs.public_key = "3KygiXesrAACKrF9cnWQrcWW5KqSRY9rKzVqkKUsEoWL";
    opts.persistent_location = dir;
    opts.secret_key = secret;
    opts.udp_enabled = true;
    opts.log_level = CarrierLogLevel_Error;
    opts.bootstraps_size = 1;
    opts.bootstraps = &bs;

    w = carrier_new(&opts, &cbs, NULL);
    CHECK(w != NULL, "carrier_new");
    if (!w) return 1;

    printf("(a) one self info field over its limit\n");
    {
        /* What a binding does with a 37-byte value for the 31-byte phone
         * field: an unbounded copy that runs on into the next field, which is
         * then filled in turn, so phone is left with no terminator of its
         * own. */
        const char *device = "An iPhone with quite a long name 16 Pro";
        const char *email = "someone@example.org";

        CHECK(strlen(device) > CARRIER_MAX_PHONE_LEN, "test value is over the phone limit");
        carrier_get_self_info(w, &ui);
        strcpy(ui.name, "tester");
        memcpy(ui.phone, device, strlen(device) + 1);   /* runs into ui.email */
        memcpy(ui.email, email, strlen(email) + 1);
        CHECK(!TERMINATED(ui.phone), "phone field has no terminator of its own");

        rc = carrier_set_self_info(w, &ui);
        CHECK(rc == 0, "process alive, carrier_set_self_info succeeds");
        carrier_get_self_info(w, &got);
        CHECK(TERMINATED(got.phone) && strlen(got.phone) == CARRIER_MAX_PHONE_LEN &&
              !strncmp(got.phone, device, CARRIER_MAX_PHONE_LEN), "phone truncated to 31");
        CHECK(!strcmp(got.email, email), "email kept");
        CHECK(!strcmp(got.name, "tester"), "name kept");
    }

    printf("(b) every self info field full to the last byte\n");
    {
        carrier_get_self_info(w, &ui);
        /* 'a' then 21 three-byte characters: 64 bytes, so the limit of 63
         * falls inside the last character. */
        ui.name[0] = 'a';
        for (size_t i = 1; i < sizeof(ui.name); i++)
            ui.name[i] = "\xe5\x90\x8d"[(i - 1) % 3];
        memset(ui.description, 'd', sizeof(ui.description));
        memset(ui.gender, 'g', sizeof(ui.gender));
        memset(ui.phone, '7', sizeof(ui.phone));
        memset(ui.email, 'e', sizeof(ui.email));
        memset(ui.region, 'r', sizeof(ui.region));   /* last field: nothing terminates it */

        rc = carrier_set_self_info(w, &ui);
        CHECK(rc == 0, "process alive, carrier_set_self_info succeeds");
        carrier_get_self_info(w, &got);
        CHECK(TERMINATED(got.name) && TERMINATED(got.description) && TERMINATED(got.gender) &&
              TERMINATED(got.phone) && TERMINATED(got.email) && TERMINATED(got.region),
              "every stored field is terminated");
        CHECK(strlen(got.name) == 61 && !memcmp(got.name + 58, "\xe5\x90\x8d", 3),
              "name cut at a UTF-8 boundary (20 whole characters kept)");
        CHECK(strlen(got.description) == CARRIER_MAX_USER_DESCRIPTION_LEN, "description truncated to 127");
        CHECK(strlen(got.gender) == CARRIER_MAX_GENDER_LEN, "gender truncated to 31");
        CHECK(strlen(got.phone) == CARRIER_MAX_PHONE_LEN, "phone truncated to 31");
        CHECK(strlen(got.email) == CARRIER_MAX_EMAIL_LEN, "email truncated to 127");
        CHECK(strlen(got.region) == CARRIER_MAX_REGION_LEN, "region truncated to 127");

        rc = carrier_set_self_info(w, &ui);
        CHECK(rc == 0, "the same over-long info again succeeds");

        memset(ui.userid, 'u', sizeof(ui.userid));
        rc = carrier_set_self_info(w, &ui);
        CHECK(rc == -1 && invalid_args(), "an unterminated userid is refused");
    }

    printf("(c) client info and profile extension\n");
    {
        CarrierClientInfo ci;
        CarrierProfileExt pe;

        carrier_get_client_info(w, &ci);
        memset(ci.app_version, 'v', sizeof(ci.app_version));   /* last field */
        rc = carrier_set_client_info(w, &ci);
        CHECK(rc == -1 && invalid_args(), "unterminated app_version refused");
        carrier_get_client_info(w, &ci);
        memset(ci.platform, 'p', sizeof(ci.platform));
        rc = carrier_set_client_info(w, &ci);
        CHECK(rc == -1 && invalid_args(), "unterminated platform refused");
        carrier_get_client_info(w, &ci);
        strcpy(ci.app_version, "1.8.52");
        CHECK(carrier_set_client_info(w, &ci) == 0, "a well-formed client info is accepted");

        carrier_get_self_profile_ext(w, &pe);
        memset(pe.extra, 'x', sizeof(pe.extra));               /* last field */
        rc = carrier_set_self_profile_ext(w, &pe);
        CHECK(rc == -1 && invalid_args(), "unterminated extra refused");
        carrier_get_self_profile_ext(w, &pe);
        memset(pe.avatar_url, 'a', sizeof(pe.avatar_url));
        rc = carrier_set_self_profile_ext(w, &pe);
        CHECK(rc == -1 && invalid_args(), "unterminated avatar_url refused");
        carrier_get_self_profile_ext(w, &pe);
        CHECK(TERMINATED(pe.avatar_url) && TERMINATED(pe.extra), "stored profile extension untouched");
    }

    printf("(d) over-long ids and addresses\n");
    {
        char longid[200];
        char out[CARRIER_MAX_ID_LEN + 1];
        const char *friendid = "7UNFCSSc1111111111111111111111111111111111";

        memset(longid, '1', sizeof(longid) - 1);
        longid[sizeof(longid) - 1] = '\0';

        CHECK(!carrier_id_is_valid(longid), "200-character id is not valid");
        CHECK(!carrier_address_is_valid(longid), "200-character address is not valid");
        CHECK(carrier_get_id_by_address(longid, out, sizeof(out)) == NULL, "no id from a 200-character address");
        CHECK(carrier_get_id_by_address(NULL, out, sizeof(out)) == NULL, "no id from a NULL address");

        rc = carrier_send_friend_message(w, longid, "hi", 2, NULL, NULL, NULL);
        CHECK(rc == -1 && invalid_args(), "send to a 200-character id refused");
        rc = carrier_send_friend_message(w, friendid, NULL, 2, NULL, NULL, NULL);
        CHECK(rc == -1 && invalid_args(), "send of a NULL message refused");
        rc = carrier_send_friend_message(w, friendid, "hi", 0, NULL, NULL, NULL);
        CHECK(rc == -1 && invalid_args(), "send of an empty message refused");
        rc = carrier_send_friend_message_live(w, longid, "hi", 2);
        CHECK(rc == -1 && invalid_args(), "live send to a 200-character id refused");

        rc = carrier_set_friend_label(w, friendid, longid);
        CHECK(rc == -1 && invalid_args(), "200-character label refused");
    }

    printf("(e) carrier_get_strerror with a short buffer\n");
    {
        char buf[32];
        const char *prefix = "[General] ";
        size_t plen = strlen(prefix);
        int err = CARRIER_GENERAL_ERROR(ERROR_INVALID_ARGS);

        memset(buf, '#', sizeof(buf));
        CHECK(carrier_get_strerror(err, buf, plen) == NULL && buf[plen] == '#',
              "buffer of exactly the prefix length: refused, nothing written past it");
        memset(buf, '#', sizeof(buf));
        CHECK(carrier_get_strerror(err, buf, plen + 1) == NULL && buf[plen + 1] == '#',
              "one byte more: still refused, nothing written past it");
        {
            char big[256];
            CHECK(carrier_get_strerror(err, big, sizeof(big)) != NULL &&
                  !strncmp(big, prefix, plen), "a large buffer gets the message");
        }
    }

    printf("(f) invite history key for the longest address\n");
    {
        char from[CARRIER_MAX_ID_LEN + CARRIER_MAX_EXTENSION_NAME_LEN + 2];
        int64_t tid = 0x1122334455667788LL;

        memset(from, 'A', CARRIER_MAX_ID_LEN);
        from[CARRIER_MAX_ID_LEN] = ':';
        memset(from + CARRIER_MAX_ID_LEN + 1, 'e', CARRIER_MAX_EXTENSION_NAME_LEN);
        from[sizeof(from) - 1] = '\0';

        transaction_history_put_invite(w->thistory, from, tid);
        CHECK(transaction_history_get_invite(w->thistory, from) == tid, "stored and found again");
        transaction_history_remove_invite(w->thistory, from);
        CHECK(transaction_history_get_invite(w->thistory, from) == 0, "removed");
    }

    printf("(g) invite response with a status and no reason\n");
    {
        FriendInfo *fi;
        TransactedCallback *tcb;
        Packet *cp;
        uint8_t *buf;
        size_t len;
        int64_t tid = 0x0102030405060708LL;

        fi = (FriendInfo *)rc_zalloc(sizeof(FriendInfo), NULL);
        strcpy(fi->info.user_info.userid, "7UNFCSSc1111111111111111111111111111111111");
        fi->friend_number = 7;
        friends_put(w->friends, fi);
        deref(fi);

        tcb = (TransactedCallback *)rc_zalloc(sizeof(TransactedCallback), NULL);
        tcb->tid = tid;
        tcb->friend_number = 7;
        tcb->callback_func = on_invite_response;
        tcb->callback_context = NULL;
        tcb->bundle = NULL;
        gettimeofday_elapsed(&tcb->expire_time, 60);
        transacted_callbacks_put(w->tcallbacks, tcb);
        deref(tcb);

        cp = packet_create(PACKET_TYPE_INVITE_RESPONSE, NULL);
        packet_set_tid(cp, &tid);
        packet_set_totalsz(cp, 0);
        packet_set_status(cp, 1);
        buf = packet_encode(cp, &len);
        packet_free(cp);
        CHECK(buf != NULL, "response without a reason encodes");
        if (buf) {
            notify_friend_message_cb(7, buf, len, w);
            free(buf);
            CHECK(irsp_calls == 1 && irsp_status == 1, "process alive, response delivered");
            CHECK(irsp_reason_ok, "the callback gets an empty reason, not NULL");
        }
    }

    carrier_kill(w);
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
