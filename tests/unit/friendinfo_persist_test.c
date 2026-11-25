/*
 * A friend's userinfo survives a restart that was not a clean exit.
 *
 * carrier.data was written when a friend was added or removed and when
 * carrier_run() returned, never when a friend's userinfo changed. A process
 * that was killed came back with the name each friend had when it was added
 * (usually none).
 *
 * Two carriers, A and B, run in this process and find each other on
 * loopback (B bootstraps from A; nothing leaves the machine). "B is killed"
 * is a copy of B's data directory taken while B is still running: exactly
 * what a kill would leave on disk. A carrier opened on the copy is never
 * run, so what it reports is what was saved, before any friend reconnects.
 *
 *  (a) A sets a name; after the save delay, B's directory holds it;
 *  (b) A changes the name again while B runs: same;
 *  (c) a burst of changes is written once per delay, not once per change;
 *  (d) an interrupted write (short or empty journal next to the data file)
 *      loads the data file; a complete journal without a data file loads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#include <sys/time.h>
#include <sys/stat.h>

/* White-box: needs the save delay, the DHT handle and the file names. */
#include "carrier.c"

#include <tox/tox.h>

static int failures = 0;
#define CHECK(cond, what) do { if (cond) printf("  ok   %s\n", what); \
    else { printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); failures++; } } while (0)

typedef struct Node {
    const char *tag;
    char dir[64];
    Carrier *w;
    pthread_t thread;
    volatile int ready;
    volatile int friend_connected;
    volatile int info_calls;
    char last_name[CARRIER_MAX_USER_NAME_LEN + 1];
    pthread_mutex_t lock;
} Node;

static void on_ready(Carrier *w, void *ctx)
{
    ((Node *)ctx)->ready = 1;
}

static void on_friend_connection(Carrier *w, const char *friendid,
                                 CarrierConnectionStatus status, void *ctx)
{
    ((Node *)ctx)->friend_connected = (status == CarrierConnectionStatus_Connected);
}

static void on_friend_info(Carrier *w, const char *friendid,
                           const CarrierFriendInfo *info, void *ctx)
{
    Node *n = (Node *)ctx;

    pthread_mutex_lock(&n->lock);
    strcpy(n->last_name, info->user_info.name);
    n->info_calls++;
    pthread_mutex_unlock(&n->lock);
}

static void *run_thread(void *arg)
{
    carrier_run(((Node *)arg)->w, 50);
    return NULL;
}

static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* Poll `cond` every 50 ms for up to `secs` seconds. */
#define WAIT_FOR(cond, secs) ({ double end_ = now_s() + (secs); \
    while (!(cond) && now_s() < end_) usleep(50000); (cond); })

static bool name_is(Node *n, const char *name)
{
    bool same;

    pthread_mutex_lock(&n->lock);
    same = strcmp(n->last_name, name) == 0;
    pthread_mutex_unlock(&n->lock);
    return same;
}

static Carrier *open_carrier(Node *n, const char *dir, uint8_t seed,
                             const char *bs_port, const char *bs_pk)
{
    CarrierOptions opts;
    CarrierCallbacks cbs;
    BootstrapNode bs;
    uint8_t secret[32];
    int i;

    memset(&opts, 0, sizeof(opts));
    memset(&cbs, 0, sizeof(cbs));
    memset(&bs, 0, sizeof(bs));
    for (i = 0; i < 32; i++)
        secret[i] = (uint8_t)(seed + i);

    bs.ipv4 = "127.0.0.1";
    bs.port = bs_port;
    bs.public_key = bs_pk;
    opts.persistent_location = dir;
    opts.secret_key = secret;
    opts.udp_enabled = true;
    opts.log_level = CarrierLogLevel_Error;
    opts.bootstraps_size = 1;
    opts.bootstraps = &bs;
    cbs.ready = on_ready;
    cbs.friend_connection = on_friend_connection;
    cbs.friend_info = on_friend_info;

    return carrier_new(&opts, &cbs, n);
}

/* What a kill would leave: B's directory as it is on disk right now. */
static void snapshot(const char *from, const char *to)
{
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && cp -R '%s' '%s'", to, from, to);
    if (system(cmd) != 0)
        printf("  (snapshot command failed)\n");
}

/* The friend's name as a carrier opened on `dir` reports it, without running. */
static bool saved_name(const char *dir, const char *friendid, char *name, size_t len)
{
    Node probe;
    Carrier *w;
    CarrierFriendInfo fi;
    bool ok = false;

    memset(&probe, 0, sizeof(probe));
    pthread_mutex_init(&probe.lock, NULL);
    name[0] = '\0';

    /* Same identity as B; the bootstrap is never contacted (not run). */
    w = open_carrier(&probe, dir, 0x51, "1", "3KygiXesrAACKrF9cnWQrcWW5KqSRY9rKzVqkKUsEoWL");
    if (!w)
        return false;
    if (carrier_get_friend_info(w, friendid, &fi) == 0) {
        snprintf(name, len, "%s", fi.user_info.name);
        ok = true;
    }
    carrier_kill(w);
    return ok;
}

static ino_t data_inode(const char *dir)
{
    char path[128];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s", dir, data_filename);
    return stat(path, &st) == 0 ? st.st_ino : 0;
}

static void set_name(Carrier *w, const char *name)
{
    CarrierUserInfo ui;

    carrier_get_self_info(w, &ui);
    snprintf(ui.name, sizeof(ui.name), "%s", name);
    if (carrier_set_self_info(w, &ui) < 0)
        printf("  (carrier_set_self_info failed: 0x%x)\n", carrier_get_error());
}

int main(void)
{
    Node a, b;
    char a_id[CARRIER_MAX_ID_LEN + 1], b_id[CARRIER_MAX_ID_LEN + 1];
    char a_dht[CARRIER_MAX_ID_LEN + 1], a_port[16];
    uint8_t dht_id[TOX_PUBLIC_KEY_SIZE];
    size_t len;
    Tox *a_tox;
    char snap[80], name[CARRIER_MAX_USER_NAME_LEN + 1];
    int delay = PERSIST_DEBOUNCE_SECONDS;
    bool ok;

    setvbuf(stdout, NULL, _IOLBF, 0);
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.tag = "A"; b.tag = "B";
    pthread_mutex_init(&a.lock, NULL);
    pthread_mutex_init(&b.lock, NULL);
    strcpy(a.dir, "/tmp/carrier-persist-a-XXXXXX");
    strcpy(b.dir, "/tmp/carrier-persist-b-XXXXXX");
    if (!mkdtemp(a.dir) || !mkdtemp(b.dir)) { printf("mkdtemp failed\n"); return 1; }
    snprintf(snap, sizeof(snap), "%s-snapshot", b.dir);

    /* A first; B bootstraps from A's UDP port and DHT key on loopback. */
    a.w = open_carrier(&a, a.dir, 0x11, "1", "3KygiXesrAACKrF9cnWQrcWW5KqSRY9rKzVqkKUsEoWL");
    CHECK(a.w != NULL, "carrier A created");
    if (!a.w) return 1;
    a_tox = *(Tox **)&a.w->dht;             // struct DHT { Tox *tox; }
    tox_self_get_dht_id(a_tox, dht_id);
    len = sizeof(a_dht);
    base58_encode(dht_id, sizeof(dht_id), a_dht, &len);
    snprintf(a_port, sizeof(a_port), "%u", (unsigned)tox_self_get_udp_port(a_tox, NULL));

    b.w = open_carrier(&b, b.dir, 0x51, a_port, a_dht);
    CHECK(b.w != NULL, "carrier B created (bootstraps from A on loopback)");
    if (!b.w) return 1;

    carrier_get_userid(a.w, a_id, sizeof(a_id));
    carrier_get_userid(b.w, b_id, sizeof(b_id));
    printf("  A %s (udp %s), B %s\n", a_id, a_port, b_id);

    pthread_create(&a.thread, NULL, run_thread, &a);
    pthread_create(&b.thread, NULL, run_thread, &b);

    ok = WAIT_FOR(a.ready && b.ready, 120);
    CHECK(ok, "both carriers connected to each other's DHT");
    if (!ok) goto out;

    CHECK(carrier_accept_friend(a.w, b_id) == 0, "A adds B");
    CHECK(carrier_accept_friend(b.w, a_id) == 0, "B adds A");
    ok = WAIT_FOR(a.friend_connected && b.friend_connected, 180);
    CHECK(ok, "A and B are connected as friends");
    if (!ok) goto out;

    printf("(a) A sets a name, B is killed after the save delay\n");
    set_name(a.w, "e2e-C");
    ok = WAIT_FOR(name_is(&b, "e2e-C"), 30);
    CHECK(ok, "B's friend_info callback reports the name");
    snapshot(b.dir, snap);
    saved_name(snap, a_id, name, sizeof(name));
    printf("  right after the change, B's directory holds \"%s\" (a kill now loses the change)\n", name);
    sleep(delay + 2);
    snapshot(b.dir, snap);
    ok = saved_name(snap, a_id, name, sizeof(name));
    CHECK(ok && strcmp(name, "e2e-C") == 0, "B restarted from its directory: the friend's name is there, before A reconnects");
    if (!ok || strcmp(name, "e2e-C") != 0)
        printf("    got \"%s\"\n", name);

    printf("(b) A changes the name while B is running, then B is killed\n");
    set_name(a.w, "second name");
    ok = WAIT_FOR(name_is(&b, "second name"), 30);
    CHECK(ok, "B's friend_info callback reports the new name");
    sleep(delay + 2);
    snapshot(b.dir, snap);
    ok = saved_name(snap, a_id, name, sizeof(name));
    CHECK(ok && strcmp(name, "second name") == 0, "B restarted from its directory: the new name is there");
    if (!ok || strcmp(name, "second name") != 0)
        printf("    got \"%s\"\n", name);

    printf("(c) a burst of changes is one write\n");
    {
        ino_t before = data_inode(b.dir), cur;
        int writes = 0, i, calls0 = b.info_calls;
        double end;
        char burst[32];

        for (i = 1; i <= 6; i++) {
            snprintf(burst, sizeof(burst), "burst-%d", i);
            set_name(a.w, burst);
            WAIT_FOR(name_is(&b, burst), 10);
            cur = data_inode(b.dir);
            if (cur != before) { writes++; before = cur; }
            usleep(200000);
        }
        CHECK(name_is(&b, "burst-6") && b.info_calls - calls0 >= 6, "B saw all six changes");
        CHECK(writes == 0, "no write while the burst is arriving (it took under the save delay)");
        end = now_s() + delay + 3;
        while (now_s() < end) {
            cur = data_inode(b.dir);
            if (cur != before) { writes++; before = cur; }
            usleep(50000);
        }
        printf("  carrier.data replaced %d time(s) for 6 changes\n", writes);
        CHECK(writes == 1, "exactly one write for the burst");
        snapshot(b.dir, snap);
        ok = saved_name(snap, a_id, name, sizeof(name));
        CHECK(ok && strcmp(name, "burst-6") == 0, "and it holds the last name of the burst");
    }

    printf("(d) an interrupted write\n");
    {
        char data[160], journal[160], cmd[512];
        struct stat st;

        snprintf(data, sizeof(data), "%s/%s", snap, data_filename);
        snprintf(journal, sizeof(journal), "%s/%s.journal", snap, data_filename);
        stat(data, &st);

        // killed while the journal was being written: half of it is there
        snprintf(cmd, sizeof(cmd), "head -c %lld '%s' > '%s'", (long long)st.st_size / 2, data, journal);
        system(cmd);
        ok = saved_name(snap, a_id, name, sizeof(name));
        CHECK(ok && strcmp(name, "burst-6") == 0, "half-written journal: the data file is loaded");

        // killed right after the journal was created
        snprintf(cmd, sizeof(cmd), ": > '%s'", journal);
        system(cmd);
        ok = saved_name(snap, a_id, name, sizeof(name));
        CHECK(ok && strcmp(name, "burst-6") == 0, "empty journal: the data file is loaded");

        // killed after the journal was synced, before it replaced the data file
        snprintf(cmd, sizeof(cmd), "mv '%s' '%s'", data, journal);
        system(cmd);
        ok = saved_name(snap, a_id, name, sizeof(name));
        CHECK(ok && strcmp(name, "burst-6") == 0, "complete journal and no data file: the journal is loaded");
    }

out:
    carrier_kill(a.w);
    carrier_kill(b.w);
    pthread_join(a.thread, NULL);
    pthread_join(b.thread, NULL);
    {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s' '%s'", a.dir, b.dir, snap);
        system(cmd);
    }
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
