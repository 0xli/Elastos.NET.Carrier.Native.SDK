/*
 * express_parse_test — drive the Express pull parser directly.
 *
 * Includes express.c so the libcurl write callback (http_read_data), the
 * frame parser and the hold store can be fed crafted bytes deterministically:
 * each bad kind of offline item first, in the middle and last between valid
 * ones; the terminator alone in its own callback; the response split at
 * every byte boundary; a cut-off transfer and its retry; injected local
 * failures; hostile lengths; hold caps; release on accept and after a
 * restart. No network: the carrier has no bootstraps and its one Express
 * node is 127.0.0.1:1 (connection refused).
 *
 * Built with -DOLD_EXPRESS and the express.c of b746ffe it shows the old
 * behaviour (one bad item fails the whole body, nothing behind it arrives).
 *
 * Run with the same cmake tree as express_multinode_test; it links the
 * static libcarrier.a, so the extern names of the included express.c are
 * renamed to avoid clashing with the archive's copy. The carrier itself uses
 * the archive's connector (same source, same layout); this file's copy of the
 * parser is what the test calls.
 */
#define express_connector_create             test_express_connector_create
#define express_connector_kill               test_express_connector_kill
#define express_enqueue_post_request         test_express_enqueue_post_request
#define express_enqueue_post_message_with_receipt test_express_enqueue_post_message_with_receipt
#define express_enqueue_pull_messages        test_express_enqueue_pull_messages
#define express_enqueue_hold_release         test_express_enqueue_hold_release
#define http_read_data                       test_http_read_data
#define http_write_data                      test_http_write_data

#ifndef OLD_EXPRESS
#define EXPRESS_TEST_HOOKS 1
#include "express.c"
#else
#include EXPRESS_C_PATH
#endif

#include "packet_generated.h"
#include <sys/stat.h>
#include <sodium.h>

#ifndef EXP_PULL_MAX_BUFFER            /* the old express.c has no cap */
#define EXP_PULL_MAX_BUFFER (16u * 1024 * 1024)
#endif

/* ------------------------------------------------------------------ */

static int g_failures;
static int g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (cond) { printf("  [ok  ] " __VA_ARGS__); printf("\n"); } \
    else { g_failures++; printf("  [FAIL] " __VA_ARGS__); printf("   (%s:%d)\n", __FILE__, __LINE__); } \
    fflush(stdout); \
} while (0)

typedef struct {
    char from[CARRIER_MAX_ID_LEN + 1];
    char text[64];
    uint64_t ts;
} Delivered;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Delivered g_msgs[2048];
static int g_nmsgs;
static Delivered g_reqs[2048];
static int g_nreqs;

static void on_request(Carrier *w, const char *userid, const CarrierUserInfo *info,
                       const char *hello, void *ctx)
{
    (void)w; (void)info; (void)ctx;
    pthread_mutex_lock(&g_lock);
    if (g_nreqs < (int)(sizeof(g_reqs) / sizeof(g_reqs[0]))) {
        strncpy(g_reqs[g_nreqs].from, userid, CARRIER_MAX_ID_LEN);
        strncpy(g_reqs[g_nreqs].text, hello ? hello : "", sizeof(g_reqs[0].text) - 1);
        g_nreqs++;
    }
    pthread_mutex_unlock(&g_lock);
}

static void on_message(Carrier *w, const char *from, const void *msg, size_t len,
                       int64_t timestamp, bool offline, void *ctx)
{
    (void)w; (void)offline; (void)ctx;
    pthread_mutex_lock(&g_lock);
    if (g_nmsgs < (int)(sizeof(g_msgs) / sizeof(g_msgs[0]))) {
        size_t n = len < sizeof(g_msgs[0].text) - 1 ? len : sizeof(g_msgs[0].text) - 1;
        strncpy(g_msgs[g_nmsgs].from, from, CARRIER_MAX_ID_LEN);
        memcpy(g_msgs[g_nmsgs].text, msg, n);
        g_msgs[g_nmsgs].text[n] = '\0';
        g_msgs[g_nmsgs].ts = (uint64_t)timestamp;
        g_nmsgs++;
    }
    pthread_mutex_unlock(&g_lock);
}

static int msgs_count(void) { int n; pthread_mutex_lock(&g_lock); n = g_nmsgs; pthread_mutex_unlock(&g_lock); return n; }
static int reqs_count(void) { int n; pthread_mutex_lock(&g_lock); n = g_nreqs; pthread_mutex_unlock(&g_lock); return n; }
static void reset_counts(void) { pthread_mutex_lock(&g_lock); g_nmsgs = 0; g_nreqs = 0; pthread_mutex_unlock(&g_lock); }

/* Wait until at least `m` messages and `r` requests arrived, or time out. */
static void settle(int m, int r, int timeout_ms)
{
    int waited = 0;
    int quiet = 0, lm = -1, lr = -1;
    while ((msgs_count() < m || reqs_count() < r) && waited < timeout_ms) {
        usleep(20000);
        waited += 20;
    }
    // then wait until nothing new arrives for 300 ms (stragglers, duplicates)
    while (quiet < 300 && waited < timeout_ms + 3000) {
        int cm = msgs_count(), cr = reqs_count();
        if (cm == lm && cr == lr)
            quiet += 50;
        else
            quiet = 0;
        lm = cm; lr = cr;
        usleep(50000);
        waited += 50;
    }
}

/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t pk[PUBLIC_KEY_BYTES];
    uint8_t sk[SECRET_KEY_BYTES];
    char userid[CARRIER_MAX_ID_LEN + 1];
    uint8_t shared[SYMMETRIC_KEY_BYTES];   // with the receiver
} Identity;

static Carrier *g_w;
static pthread_t g_run_thread;
static char g_data_dir[PATH_MAX];
static uint8_t g_node_key[SYMMETRIC_KEY_BYTES];   // the "node shared key" the frames are encrypted with
static char g_node_pk_b58[CARRIER_MAX_ID_LEN + 1];
static char g_my_address[CARRIER_MAX_ADDRESS_LEN + 1];
static uint8_t g_my_pk[PUBLIC_KEY_BYTES];

static void identity_new(Identity *id)
{
    size_t len = sizeof(id->userid);
    // The shared base58 drops a leading zero byte, so a key that starts with
    // 0x00 (1 in 256) gets an id nothing accepts. Not what this test is about.
    do {
        crypto_create_keypair(id->pk, id->sk);
    } while (id->pk[0] == 0);
    base58_encode(id->pk, sizeof(id->pk), id->userid, &len);
    crypto_compute_symmetric_key(g_my_pk, id->sk, id->shared);
}

static void *run_thread(void *arg)
{
    carrier_run((Carrier *)arg, 50);
    return NULL;
}

static void start_carrier(void)
{
    CarrierOptions opts;
    CarrierCallbacks cbs;
    ExpressNode node;
    size_t len;

    memset(&node, 0, sizeof(node));
    node.ipv4 = "127.0.0.1";
    node.port = "1";
    node.public_key = g_node_pk_b58;

    memset(&opts, 0, sizeof(opts));
    opts.persistent_location = g_data_dir;
    opts.udp_enabled = true;
    opts.log_level = getenv("EXPRESS_TEST_LOGLEVEL") ? atoi(getenv("EXPRESS_TEST_LOGLEVEL")) : CarrierLogLevel_Warning;
    opts.express_nodes_size = 1;
    opts.express_nodes = &node;

    memset(&cbs, 0, sizeof(cbs));
    cbs.friend_request = on_request;
    cbs.friend_message = on_message;

    g_w = carrier_new(&opts, &cbs, NULL);
    if (!g_w) {
        fprintf(stderr, "carrier_new failed: 0x%x\n", carrier_get_error());
        exit(2);
    }
    carrier_get_address(g_w, g_my_address, sizeof(g_my_address));
    len = sizeof(g_my_pk);
    base58_decode(g_w->me.userid, strlen(g_w->me.userid), g_my_pk, len);
    g_w->is_ready = true;   // carrier_accept_friend needs it; no DHT here
    pthread_create(&g_run_thread, NULL, run_thread, g_w);
}

static void stop_carrier(void)
{
    carrier_kill(g_w);
    pthread_join(g_run_thread, NULL);
    g_w = NULL;
}

static ExpressConnector *conn(void) { return g_w->connector; }

/* ---- run a function on the connector's laundry thread ------------- */

typedef struct {
    ExpTasklet base;
    void (*fn)(void *);
    void *arg;
    pthread_mutex_t m;
    pthread_cond_t c;
    int done;
} TestTasklet;

static int test_runner(ExpTasklet *b)
{
    TestTasklet *t = (TestTasklet *)b;
    t->fn(t->arg);
    pthread_mutex_lock(&t->m);
    t->done = 1;
    pthread_cond_signal(&t->c);
    pthread_mutex_unlock(&t->m);
    return 0;
}

static void on_laundry(void (*fn)(void *), void *arg)
{
    ExpressConnector *c = conn();
    TestTasklet *t = rc_zalloc(sizeof(*t), NULL);
    t->base.entry.data = t;
    t->base.connector = c;
    t->base.runner = test_runner;
    t->fn = fn;
    t->arg = arg;
    pthread_mutex_init(&t->m, NULL);
    pthread_cond_init(&t->c, NULL);

    pthread_mutex_lock(&c->lock);
    linked_list_push_head(c->task_list, &t->base.entry);
    pthread_mutex_unlock(&c->lock);
    pthread_cond_signal(&c->cond);

    pthread_mutex_lock(&t->m);
    while (!t->done)
        pthread_cond_wait(&t->c, &t->m);
    pthread_mutex_unlock(&t->m);
    pthread_mutex_destroy(&t->m);
    pthread_cond_destroy(&t->c);
    deref(t);
}

/* ---- building bodies ---------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} Body;

static void body_init(Body *b) { b->cap = 1 << 20; b->buf = malloc(b->cap); b->len = 0; }
static void body_free(Body *b) { free(b->buf); b->buf = NULL; }
static void body_grow(Body *b, size_t need)
{
    while (b->len + need > b->cap) {
        b->cap *= 2;
        b->buf = realloc(b->buf, b->cap);
    }
}

static void body_raw_frame(Body *b, uint32_t magic_net, uint32_t len_host, const uint8_t *cipher, size_t clen)
{
    uint32_t len_net = htonl(len_host);
    body_grow(b, 8 + clen);
    memcpy(b->buf + b->len, &magic_net, 4);
    memcpy(b->buf + b->len + 4, &len_net, 4);
    memcpy(b->buf + b->len + 8, cipher, clen);
    b->len += 8 + clen;
}

/* One frame: encrypt `plain` with the node key. */
static void body_frame(Body *b, const uint8_t *plain, size_t plen)
{
    uint8_t *cipher = malloc(plen + NONCE_BYTES + ZERO_BYTES);
    ssize_t clen = encrypt_data(g_node_key, (uint8_t *)plain, plen, cipher);
    assert(clen > 0);
    body_raw_frame(b, conn()->magic_num, (uint32_t)clen, cipher, (size_t)clen);
    free(cipher);
}

static void body_terminator(Body *b)
{
    body_raw_frame(b, conn()->magic_num, 0, NULL, 0);
}

/* The pullmsg flatbuffer as the server builds it. from/address may be NULL. */
static size_t build_pullmsg(uint8_t **out, const char *from, uint8_t type, uint64_t ts,
                            const char *address, const uint8_t *payload, size_t plen)
{
    flatcc_builder_t B;
    flatbuffers_string_ref_t from_ref = 0, addr_ref = 0;
    flatbuffers_uint8_vec_ref_t payload_ref = 0;
    flatcc_builder_ref_t root;
    size_t size = 0;

    flatcc_builder_init(&B);
    flatcc_builder_start_buffer(&B, 0, 0, 0);
    if (from)
        from_ref = flatcc_builder_create_string_str(&B, from);
    if (address)
        addr_ref = flatcc_builder_create_string_str(&B, address);
    if (payload)
        payload_ref = flatbuffers_uint8_vec_create(&B, payload, plen);
    // start/add/end, not _create: the server omits absent fields, and a
    // zero offset for an absent string is (rightly) rejected by the verifier.
    carrier_pullmsg_start(&B);
    carrier_pullmsg_id_add(&B, ts);
    if (from_ref)
        carrier_pullmsg_from_add(&B, from_ref);
    carrier_pullmsg_type_add(&B, type);
    carrier_pullmsg_timestamp_add(&B, ts);
    if (addr_ref)
        carrier_pullmsg_address_add(&B, addr_ref);
    if (payload_ref)
        carrier_pullmsg_payload_add(&B, payload_ref);
    root = carrier_pullmsg_end(&B);
    flatcc_builder_end_buffer(&B, root);
    *out = flatcc_builder_finalize_buffer(&B, &size);
    flatcc_builder_clear(&B);
    return size;
}

/* A carrier MESSAGE packet carrying `text`, encrypted for the receiver by `sender`. */
static size_t friend_payload(uint8_t **out, const Identity *sender, const char *text)
{
    Packet *cp = packet_create(PACKET_TYPE_MESSAGE, NULL);
    size_t plen = 0;
    uint8_t *packet;
    ssize_t clen;

    packet_set_raw_data(cp, text, strlen(text));
    packet = packet_encode(cp, &plen);
    packet_free(cp);
    *out = malloc(plen + NONCE_BYTES + ZERO_BYTES);
    clen = encrypt_data((uint8_t *)sender->shared, packet, plen, *out);
    free(packet);
    assert(clen > 0);
    return (size_t)clen;
}

static void add_item_M(Body *b, const Identity *sender, const char *text, uint64_t ts)
{
    uint8_t *payload, *fb;
    size_t plen = friend_payload(&payload, sender, text);
    size_t flen = build_pullmsg(&fb, sender->userid, 'M', ts, NULL, payload, plen);
    body_frame(b, fb, flen);
    free(fb);
    free(payload);
}

static void add_item_R(Body *b, const Identity *sender, const char *hello, uint64_t ts, const char *address)
{
    Packet *cp = packet_create(PACKET_TYPE_FRIEND_REQUEST, NULL);
    size_t plen = 0;
    uint8_t *packet, *fb;
    size_t flen;

    packet_set_name(cp, "parse-test");
    packet_set_descr(cp, "");
    packet_set_hello(cp, hello);
    packet = packet_encode(cp, &plen);
    packet_free(cp);
    flen = build_pullmsg(&fb, sender->userid, 'R', ts, address ? address : g_my_address, packet, plen);
    body_frame(b, fb, flen);
    free(fb);
    free(packet);
}

/* The bad kinds. `decodable_ts` says whether the item's timestamp survives
 * to the watermark (the flatbuffer and outer frame are readable). */
typedef enum {
    BAD_UNDECRYPTABLE_FRAME,   // random bytes where the node ciphertext should be
    BAD_EMPTY_PLAINTEXT,       // a frame whose plaintext is zero bytes
    BAD_GARBAGE_FLATBUFFER,    // decrypts to bytes that are not a pullmsg
    BAD_UNKNOWN_TYPE,          // type 'X'
    BAD_R_OTHER_ADDRESS,       // a friend request for another nospam
    BAD_R_EMPTY,               // a friend request with no payload
    BAD_R_NO_ADDRESS,          // a friend request without address
    BAD_M_SHORT,               // a message payload shorter than nonce+MAC
    BAD_M_NO_FROM,             // no sender
    BAD_M_UNDECRYPTABLE,       // from a friend, but the friend payload is garbage
    BAD_M_BAD_FROM,            // sender id that is not base58 of a key
    BAD_KINDS
} BadKind;

static const char *bad_name[BAD_KINDS] = {
    "undecryptable frame", "empty plaintext", "garbage flatbuffer", "unknown type",
    "R other address", "R empty payload", "R no address", "M short payload",
    "M no from", "M undecryptable (friend)", "M bad from",
};

static int bad_decodable_ts[BAD_KINDS] = { 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1 };

static Identity g_friend, g_stranger;

static void add_bad(Body *b, BadKind kind, uint64_t ts)
{
    uint8_t junk[64];
    uint8_t *fb;
    size_t flen;
    char other_addr[CARRIER_MAX_ADDRESS_LEN + 1];
    int i;

    for (i = 0; i < (int)sizeof(junk); i++)
        junk[i] = (uint8_t)(i * 7 + 3);

    switch (kind) {
    case BAD_UNDECRYPTABLE_FRAME:
        body_raw_frame(b, conn()->magic_num, sizeof(junk), junk, sizeof(junk));
        break;
    case BAD_EMPTY_PLAINTEXT: {
        // nonce + MAC of an empty message (NaCl allows it; crystal does not build it)
        uint8_t cipher[NONCE_BYTES + MAC_BYTES];
        crypto_random_nonce(cipher);
        crypto_secretbox_easy(cipher + NONCE_BYTES, junk, 0, cipher, g_node_key);
        body_raw_frame(b, conn()->magic_num, sizeof(cipher), cipher, sizeof(cipher));
        break;
    }
    case BAD_GARBAGE_FLATBUFFER:
        body_frame(b, junk, sizeof(junk));
        break;
    case BAD_UNKNOWN_TYPE:
        flen = build_pullmsg(&fb, g_friend.userid, 'X', ts, NULL, junk, sizeof(junk));
        body_frame(b, fb, flen); free(fb);
        break;
    case BAD_R_OTHER_ADDRESS: {
        // same key, other nospam: decode our address, flip nospam, fix checksum
        uint8_t addr[DHT_ADDRESS_SIZE];
        size_t alen = sizeof(other_addr);
        base58_decode(g_my_address, strlen(g_my_address), addr, sizeof(addr));
        addr[32] ^= 0x5a;
        addr[36] = 0; addr[37] = 0;
        for (i = 0; i < 36; i++)
            addr[36 + (i % 2)] ^= addr[i];
        base58_encode(addr, sizeof(addr), other_addr, &alen);
        add_item_R(b, &g_stranger, "wrong-address", ts, other_addr);
        break;
    }
    case BAD_R_EMPTY:
        flen = build_pullmsg(&fb, g_stranger.userid, 'R', ts, g_my_address, junk, 0);
        body_frame(b, fb, flen); free(fb);
        break;
    case BAD_R_NO_ADDRESS:
        flen = build_pullmsg(&fb, g_stranger.userid, 'R', ts, NULL, junk, sizeof(junk));
        body_frame(b, fb, flen); free(fb);
        break;
    case BAD_M_SHORT:
        flen = build_pullmsg(&fb, g_friend.userid, 'M', ts, NULL, junk, 5);
        body_frame(b, fb, flen); free(fb);
        break;
    case BAD_M_NO_FROM:
        flen = build_pullmsg(&fb, NULL, 'M', ts, NULL, junk, sizeof(junk));
        body_frame(b, fb, flen); free(fb);
        break;
    case BAD_M_UNDECRYPTABLE:
        flen = build_pullmsg(&fb, g_friend.userid, 'M', ts, NULL, junk, sizeof(junk));
        body_frame(b, fb, flen); free(fb);
        break;
    case BAD_M_BAD_FROM:
        flen = build_pullmsg(&fb, "not-a-key!!", 'M', ts, NULL, junk, sizeof(junk));
        body_frame(b, fb, flen); free(fb);
        break;
    default:
        abort();
    }
}

/* ---- feeding the parser on the laundry thread ---------------------- */

typedef struct {
    ExpPullTasklet task;
    const uint8_t *buf;
    size_t len;
    size_t split;        // 0: all at once; else two calls split here
    size_t chunk;        // >0: fixed chunk size (overrides split)
    size_t ret;          // return of the last callback (0 = aborted)
    size_t fed;          // bytes handed over before an abort
} Feed;

static void feed_fn(void *arg)
{
    Feed *f = (Feed *)arg;
    size_t off = 0;

    f->ret = 1;
    f->fed = 0;
    while (off < f->len) {
        size_t n;
        if (f->chunk)
            n = f->chunk;
        else if (f->split && off == 0)
            n = f->split;
        else
            n = f->len - off;
        if (n > f->len - off)
            n = f->len - off;
        f->ret = http_read_data((char *)f->buf + off, 1, n, &f->task);
        if (f->ret != n)
            break;   // aborted: curl would stop the transfer here
        off += n;
        f->fed = off;
    }
    if (f->task.data) {            // what the pull runner does between nodes
        deref(f->task.data);
        f->task.data = NULL;
        f->task.len = 0;
    }
}

static void feed_init(Feed *f, const Body *b, uint64_t last_ts)
{
    memset(f, 0, sizeof(*f));
    f->task.base.connector = conn();
    f->task.shared_key_cache = g_node_key;
    f->task.last_timestamp = last_ts;
    f->buf = b->buf;
    f->len = b->len;
}

static void feed(Feed *f) { on_laundry(feed_fn, f); }

/* ---- hold helpers ---------------------------------------------------- */

#ifndef OLD_EXPRESS
typedef struct { const char *from; int count; } HeldCount;

static void held_count_fn(void *arg)
{
    HeldCount *hc = (HeldCount *)arg;
    linked_list_iterator_t it;
    ExpHeldItem *cur;
    hc->count = 0;
    if (!conn()->held)
        return;
    linked_list_iterate(conn()->held, &it);
    while (linked_list_iterator_next(&it, (void **)&cur) == 1) {
        if (!hc->from || strcmp(cur->from, hc->from) == 0)
            hc->count++;
        deref(cur);
    }
}

static int held_count(const char *from)
{
    HeldCount hc = { from, 0 };
    on_laundry(held_count_fn, &hc);
    return hc.count;
}

static int hold_files(void)
{
    char dir[PATH_MAX];
    DIR *d;
    struct dirent *de;
    int n = 0;
    snprintf(dir, sizeof(dir), "%s/%s", g_data_dir, EXP_HOLD_DIRNAME);
    d = opendir(dir);
    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL)
        if (strstr(de->d_name, ".held"))
            n++;
    closedir(d);
    return n;
}
#endif

/* ================================================================== */

static uint64_t g_ts_base = 1700000000000000ULL;
static uint64_t next_ts_round(void) { g_ts_base += 1000000; return g_ts_base; }

/* 1. each bad kind first, middle, last, between valid items */
static void test_bad_kinds(void)
{
    static const char *posname[3] = { "first", "middle", "last" };
    int kind, pos;

    printf("\n== 1. one bad item never blocks the valid ones behind it\n");
    for (kind = 0; kind < BAD_KINDS; kind++) {
        for (pos = 0; pos < 3; pos++) {
            Body b;
            Feed f;
            uint64_t t0 = next_ts_round();
            uint64_t ts[3] = { t0 + 1, t0 + 2, t0 + 3 };
            uint64_t expect_wm;
            int msgs_before = msgs_count(), reqs_before = reqs_count();

            body_init(&b);
            if (pos == 0) add_bad(&b, kind, ts[0]);
            add_item_R(&b, &g_stranger, "hello-r", pos == 0 ? ts[1] : ts[0], NULL);
            if (pos == 1) add_bad(&b, kind, ts[1]);
            add_item_M(&b, &g_friend, "msg-m", pos == 2 ? ts[1] : ts[2]);
            if (pos == 2) add_bad(&b, kind, ts[2]);
            body_terminator(&b);

            expect_wm = (pos == 2 && !bad_decodable_ts[kind]) ? ts[1] : ts[2];

            feed_init(&f, &b, 0);
            feed(&f);
            settle(msgs_before + 1, reqs_before + 1, 3000);

            CHECK(f.ret != 0 && msgs_count() == msgs_before + 1 && reqs_count() == reqs_before + 1 &&
                  f.task.last_timestamp == expect_wm,
                  "%-26s %-6s: body accepted=%d, +msgs=%d +reqs=%d, watermark %s",
                  bad_name[kind], posname[pos], f.ret != 0,
                  msgs_count() - msgs_before, reqs_count() - reqs_before,
                  f.task.last_timestamp == expect_wm ? "right" : "WRONG");
            body_free(&b);
        }
    }
#ifndef OLD_EXPRESS
    // the stranger's requests were delivered; nothing was held by this test
    CHECK(held_count(NULL) == 0, "nothing held after the bad-kind rounds");
#endif
}

/* 2. the terminator alone in its own callback must not move the watermark */
static void test_terminator_alone(void)
{
    Body b;
    Feed f;
    uint64_t t0 = next_ts_round();

    printf("\n== 2. terminator alone in its own callback\n");
    body_init(&b);
    body_terminator(&b);
    feed_init(&f, &b, 777);
    feed(&f);
    CHECK(f.ret != 0 && f.task.last_timestamp == 777,
          "terminator-only body: watermark stays 777 (got %" PRIu64 ")", f.task.last_timestamp);
    body_free(&b);

    body_init(&b);
    add_item_R(&b, &g_stranger, "r-before-term", t0 + 1, NULL);
    body_terminator(&b);
    feed_init(&f, &b, 0);
    f.split = b.len - 8;    // first call: the request frame; second call: terminator only
    feed(&f);
    CHECK(f.ret != 0 && f.task.last_timestamp == t0 + 1,
          "request then terminator in its own callback: watermark = request ts (got %" PRIu64 ", want %" PRIu64 ")",
          f.task.last_timestamp, t0 + 1);
    settle(0, reqs_count() + 1, 2000);   // let that request land before the next test counts
    body_free(&b);
}

/* 3. the body split at every byte boundary, and byte by byte */
static void test_splits(void)
{
    Body b;
    uint64_t t0 = next_ts_round();
    size_t k;
    int bad_rounds = 0, rounds = 0;
    int m0, r0;

    printf("\n== 3. response split at every boundary\n");
    body_init(&b);
    add_item_R(&b, &g_stranger, "s1", t0 + 1, NULL);
    add_item_M(&b, &g_friend, "s2", t0 + 2);
    add_item_R(&b, &g_stranger, "s3", t0 + 3, NULL);
    body_terminator(&b);

    for (k = 1; k < b.len; k++) {
        Feed f;
        m0 = msgs_count(); r0 = reqs_count();
        feed_init(&f, &b, 0);
        f.split = k;
        feed(&f);
        settle(m0 + 1, r0 + 2, 3000);
        rounds++;
        if (!(f.ret != 0 && msgs_count() == m0 + 1 && reqs_count() == r0 + 2 && f.task.last_timestamp == t0 + 3)) {
            bad_rounds++;
            printf("     split at %zu: ret=%zu msgs+%d reqs+%d wm=%" PRIu64 "\n", k, f.ret,
                   msgs_count() - m0, reqs_count() - r0, f.task.last_timestamp);
            { int q; for (q = r0; q < reqs_count(); q++) printf("        req[%d]=%s\n", q - r0, g_reqs[q].text); }
        }
    }
    CHECK(bad_rounds == 0, "%d split positions (%zu-byte body): every round 1 msg + 2 reqs, watermark t+3",
          rounds, b.len);

    {
        Feed f;
        m0 = msgs_count(); r0 = reqs_count();
        feed_init(&f, &b, 0);
        f.chunk = 1;
        feed(&f);
        settle(m0 + 1, r0 + 2, 3000);
        CHECK(f.ret != 0 && msgs_count() == m0 + 1 && reqs_count() == r0 + 2 && f.task.last_timestamp == t0 + 3,
              "byte-by-byte: 1 msg + 2 reqs, watermark t+3");
    }
    body_free(&b);
}

/* 4. a cut-off transfer acknowledges only what was handled; the retry does not deliver twice */
static void test_cutoff_retry(void)
{
    Body b;
    Feed f;
    uint64_t t0 = next_ts_round();
    int m0 = msgs_count();
    size_t first_frame_end;

    printf("\n== 4. cut-off transfer and retry\n");
    body_init(&b);
    add_item_M(&b, &g_friend, "c1", t0 + 1);
    first_frame_end = b.len;
    add_item_M(&b, &g_friend, "c2", t0 + 2);
    body_terminator(&b);

    // the connection drops in the middle of the second frame
    feed_init(&f, &b, 0);
    f.len = first_frame_end + 20;
    feed(&f);
    settle(m0 + 1, 0, 2000);
    CHECK(msgs_count() == m0 + 1 && f.task.last_timestamp == t0 + 1,
          "cut inside frame 2: 1 delivered, watermark = ts1 (nothing past it acknowledged)");

    // retry from the committed watermark: the server still has both
    feed_init(&f, &b, t0 + 1);
    feed(&f);
    settle(m0 + 2, 0, 2000);
    CHECK(msgs_count() == m0 + 2 && strcmp(g_msgs[m0 + 1].text, "c2") == 0 && f.task.last_timestamp == t0 + 2,
          "retry: only c2 delivered (c1 not twice), watermark = ts2");
    body_free(&b);
}

#ifndef OLD_EXPRESS
/* 5. local transient failures: never acknowledge the item or anything after it */
static void test_transient(void)
{
    Body b;
    Feed f;
    uint64_t t0 = next_ts_round();
    int r0 = reqs_count();
    int h0 = held_count(NULL);

    printf("\n== 5. local transient failure stops at the safely handled prefix\n");
    body_init(&b);
    add_item_R(&b, &g_stranger, "t1", t0 + 1, NULL);
    add_item_M(&b, &g_stranger, "held-t2", t0 + 2);     // stranger: goes to the hold
    add_item_R(&b, &g_stranger, "t3", t0 + 3, NULL);
    body_terminator(&b);

    express_test_fail_hold = 1;
    feed_init(&f, &b, 0);
    feed(&f);
    settle(0, r0 + 1, 2000);
    CHECK(f.ret == 0 && reqs_count() == r0 + 1 && f.task.last_timestamp == t0 + 1 && held_count(NULL) == h0,
          "hold write fails: transfer aborted, t1 delivered, watermark = ts1, nothing held, t3 not delivered");
    express_test_fail_hold = 0;

    feed_init(&f, &b, t0 + 1);
    feed(&f);
    settle(0, r0 + 2, 2000);
    CHECK(f.ret != 0 && reqs_count() == r0 + 2 && held_count(g_stranger.userid) == 1 && f.task.last_timestamp == t0 + 3,
          "retry: message held, t3 delivered once, watermark = ts3");

    express_test_fail_alloc = 1;
    feed_init(&f, &b, 0);
    feed(&f);
    CHECK(f.ret == 0 && f.task.last_timestamp == 0, "buffer growth fails: aborted before anything, watermark unchanged");
    express_test_fail_alloc = 0;
    body_free(&b);
}
#endif

/* 6. hostile input in the frame layer */
static void test_hostile(void)
{
    Body b;
    Feed f;
    uint8_t junk[32] = {0};
    uint64_t t0 = next_ts_round();
    int r0;

    printf("\n== 6. hostile lengths and headers\n");

    body_init(&b);
    add_item_R(&b, &g_stranger, "h1", t0 + 1, NULL);
    body_raw_frame(&b, conn()->magic_num, 0xFFFFFFF0u, junk, sizeof(junk));
    r0 = reqs_count();
    feed_init(&f, &b, 0);
    feed(&f);
    settle(0, r0 + 1, 2000);
    CHECK(f.ret == 0 && reqs_count() == r0 + 1 && f.task.last_timestamp == t0 + 1,
          "frame length 0xFFFFFFF0: transfer aborted, the valid item before it kept");
    body_free(&b);

    body_init(&b);
    body_raw_frame(&b, 0x12345678u, 8, junk, 8);
    feed_init(&f, &b, 0);
    feed(&f);
    CHECK(f.ret == 0, "bad magic: transfer aborted");
    body_free(&b);

    {
        size_t big = (size_t)EXP_PULL_MAX_BUFFER + 1;
        uint8_t *zeros = calloc(1, big);
        Body z = { zeros, big, big };
        feed_init(&f, &z, 0);
        feed(&f);
        CHECK(f.ret == 0, "%zu bytes in one callback: over the buffer cap, aborted", big);
        free(zeros);
    }

    // an incomplete frame that never completes: nothing delivered, nothing acknowledged
    body_init(&b);
    add_item_R(&b, &g_stranger, "h2", t0 + 2, NULL);
    b.len -= 1;
    feed_init(&f, &b, 0);
    feed(&f);
    CHECK(f.ret != 0 && f.task.last_timestamp == 0, "frame short by one byte: waits, watermark unchanged");
    body_free(&b);
}

#ifndef OLD_EXPRESS
/* 7. the hold: kept, bounded, released on accept, survives a restart */
static void test_hold(void)
{
    Body b;
    Feed f;
    Identity s2, s3;
    uint64_t t0 = next_ts_round();
    int m0, i;
    int files_before;

    printf("\n== 7. hold for not-yet-friends\n");

    identity_new(&s2);
    identity_new(&s3);

    // one message from s2, then s2's friend request (the real queue's shape)
    body_init(&b);
    add_item_M(&b, &s2, "early-from-s2", t0 + 1);
    add_item_R(&b, &s2, "please-add-me", t0 + 2, NULL);
    body_terminator(&b);
    m0 = msgs_count();
    feed_init(&f, &b, 0);
    feed(&f);
    settle(0, 0, 500);
    CHECK(f.ret != 0 && held_count(s2.userid) == 1 && hold_files() >= 1 && msgs_count() == m0 &&
          f.task.last_timestamp == t0 + 2,
          "message from a non-friend is held on disk, not delivered, queue acknowledged past it");
    body_free(&b);

    // accept s2 -> the held message arrives
    CHECK(carrier_accept_friend(g_w, s2.userid) == 0, "carrier_accept_friend(s2)");
    settle(m0 + 1, 0, 3000);
    CHECK(msgs_count() == m0 + 1 && strcmp(g_msgs[m0].text, "early-from-s2") == 0 &&
          strcmp(g_msgs[m0].from, s2.userid) == 0 && g_msgs[m0].ts == t0 + 1,
          "after accept: the held message is delivered once, with its original timestamp");
    CHECK(held_count(s2.userid) == 0, "and removed from the hold");

    // a flood from one stranger is capped per sender
    body_init(&b);
    for (i = 0; i < EXP_HOLD_SENDER_MAX_ITEMS + 8; i++) {
        char text[32];
        snprintf(text, sizeof(text), "flood-%d", i);
        add_item_M(&b, &s3, text, t0 + 100 + i);
    }
    body_terminator(&b);
    feed_init(&f, &b, 0);
    feed(&f);
    CHECK(f.ret != 0 && held_count(s3.userid) == EXP_HOLD_SENDER_MAX_ITEMS,
          "%d messages from one stranger: %d held (per-sender cap %d), queue acknowledged",
          EXP_HOLD_SENDER_MAX_ITEMS + 8, held_count(s3.userid), EXP_HOLD_SENDER_MAX_ITEMS);
    body_free(&b);

    // a flood of strangers is capped in total and does not touch s3's newest
    {
        int strangers = EXP_HOLD_MAX_ITEMS + 20;
        Identity *ids = calloc(strangers, sizeof(Identity));
        body_init(&b);
        for (i = 0; i < strangers; i++) {
            identity_new(&ids[i]);
            add_item_M(&b, &ids[i], "flood", t0 + 1000 + i);
        }
        body_terminator(&b);
        feed_init(&f, &b, 0);
        feed(&f);
        CHECK(f.ret != 0 && held_count(NULL) <= EXP_HOLD_MAX_ITEMS && held_count(NULL) >= EXP_HOLD_MAX_ITEMS - 1,
              "%d strangers: %d held in total (cap %d)", strangers, held_count(NULL), EXP_HOLD_MAX_ITEMS);
        CHECK(held_count(s3.userid) == 0 && held_count(ids[strangers - 1].userid) == 1,
              "oldest evicted first: s3's older items gone, the newest stranger kept");
        body_free(&b);
        free(ids);
    }

    // restart: the hold is loaded from disk and released on accept
    body_init(&b);
    add_item_M(&b, &s2, "after-restart-s2-is-friend", t0 + 5000);   // s2 is a friend now: direct
    identity_new(&s3);
    add_item_M(&b, &s3, "survives-restart", t0 + 5001);
    body_terminator(&b);
    m0 = msgs_count();
    feed_init(&f, &b, 0);
    feed(&f);
    settle(m0 + 1, 0, 2000);
    CHECK(msgs_count() == m0 + 1 && strcmp(g_msgs[m0].text, "after-restart-s2-is-friend") == 0,
          "a message from the now-friend s2 is delivered directly");
    files_before = hold_files();
    body_free(&b);

    stop_carrier();
    start_carrier();
    usleep(300000);
    CHECK(held_count(s3.userid) == 1 && hold_files() == files_before,
          "after restart: s3's message is back in the hold (%d files)", hold_files());
    m0 = msgs_count();
    CHECK(carrier_accept_friend(g_w, s3.userid) == 0, "carrier_accept_friend(s3)");
    settle(m0 + 1, 0, 3000);
    CHECK(msgs_count() == m0 + 1 && strcmp(g_msgs[m0].text, "survives-restart") == 0,
          "the held message is delivered after the restart + accept");
    CHECK(held_count(s3.userid) == 0, "and removed from the hold");
}
#endif

#ifndef OLD_EXPRESS
/* The delete-until value is nonce + ciphertext, base58 in the URL. The node
 * decodes it with a standard base58 decoder, so leading zero bytes must be
 * kept as leading '1's (an encoder that drops them made 1 delete in 256 fail
 * with HTTP 400). Checked against fixed vectors and a round trip. */
static void test_until_encoding(void)
{
    static const struct { uint8_t in[4]; size_t len; const char *out; } vec[] = {
        { { 0x00 },                   1, "1"     },
        { { 0x00, 0x00 },             2, "11"    },
        { { 0x00, 0x01 },             2, "12"    },
        { { 0x00, 0x00, 0x01 },       3, "112"   },
        { { 0x61 },                   1, "2g"    },
        { { 0x00, 0x61 },             2, "12g"   },
        { { 0x62, 0x62, 0x62 },       3, "a3gV"  },
        { { 0x00, 0x62, 0x62, 0x62 }, 4, "1a3gV" },
    };
    char buf[128];
    uint8_t data[40], back[64];
    size_t i;
    int ok = 1, lead;

    printf("\n== 8. delete-until value keeps leading zero bytes\n");

    for (i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
        char *r = base58_encode_std(vec[i].in, vec[i].len, buf, sizeof(buf));
        if (!r || strcmp(r, vec[i].out) != 0) {
            printf("    vector %zu: got \"%s\", want \"%s\"\n", i, r ? r : "(null)", vec[i].out);
            ok = 0;
        }
    }
    CHECK(ok, "fixed vectors, with and without leading zero bytes");

    // nonce + ciphertext of a timestamp is 40 bytes; 0, 1 and 2 leading zeros
    ok = 1;
    for (lead = 0; lead <= 2; lead++) {
        char *r;
        ssize_t n;

        for (i = 0; i < sizeof(data); i++)
            data[i] = (uint8_t)(0xA0 + i);
        memset(data, 0, lead);
        r = base58_encode_std(data, sizeof(data), buf, sizeof(buf));
        n = r ? base58_decode(r, strlen(r), back, sizeof(back)) : -1;
        if (!r || strspn(r, "1") != (size_t)lead || n != (ssize_t)sizeof(data) ||
            memcmp(back, data, sizeof(data)) != 0) {
            printf("    %d leading zero byte(s): \"%s\" decodes to %zd bytes\n", lead, r ? r : "(null)", n);
            ok = 0;
        }
    }
    CHECK(ok, "40-byte value with 0, 1, 2 leading zero bytes: one '1' per zero byte, decodes back whole");

    CHECK(base58_encode_std(data, sizeof(data), buf, 2) == NULL, "a buffer that is too small is refused");
}
#endif

int main(int argc, char *argv[])
{
    uint8_t node_pk[PUBLIC_KEY_BYTES], node_sk[SECRET_KEY_BYTES];
    size_t len = sizeof(g_node_pk_b58);
    char cmd[PATH_MAX + 32];

    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IOLBF, 0);

    snprintf(g_data_dir, sizeof(g_data_dir), "%s/express-parse-test-data",
             getenv("EXPRESS_TEST_WORKDIR") ? getenv("EXPRESS_TEST_WORKDIR") : "/tmp");
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_data_dir);
    system(cmd);
    mkdir(g_data_dir, 0700);

    do {
        crypto_create_keypair(node_pk, node_sk);
    } while (node_pk[0] == 0);          // see identity_new
    base58_encode(node_pk, sizeof(node_pk), g_node_pk_b58, &len);
    memcpy(g_node_key, node_sk, sizeof(g_node_key));   // any 32 random bytes will do

    start_carrier();
    identity_new(&g_friend);
    identity_new(&g_stranger);
    if (carrier_accept_friend(g_w, g_friend.userid) != 0) {
        fprintf(stderr, "carrier_accept_friend(friend) failed: 0x%x\n", carrier_get_error());
        return 2;
    }
    printf("receiver %s, friend %s, stranger %s\n", g_w->me.userid, g_friend.userid, g_stranger.userid);

    {
        // EXPRESS_TEST_SECTIONS="1 2 6": run only those (the old code dies early)
        const char *only = getenv("EXPRESS_TEST_SECTIONS");
#define WANT(n) (!only || strstr(only, #n))
        if (WANT(1)) test_bad_kinds();
        if (WANT(2)) test_terminator_alone();
        if (WANT(3)) test_splits();
        if (WANT(4)) test_cutoff_retry();
#ifndef OLD_EXPRESS
        if (WANT(5)) test_transient();
#endif
        if (WANT(6)) test_hostile();
#ifndef OLD_EXPRESS
        if (WANT(7)) test_hold();
        if (WANT(8)) test_until_encoding();
#endif
    }

    stop_carrier();

    printf("\n%s: %d checks, %d failed\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
