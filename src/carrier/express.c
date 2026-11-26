/*
 * Copyright (c) 2018 Elastos Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif
#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#else
#include <sys/time.h>
#endif
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>

#include <cjson/cJSON.h>
#include <crystal.h>

//#include "dht.h"
#include "carrier.h"
#include "carrier_error.h"
#include "carrier_impl.h"
#include "packet.h"
#include "express.h"
#include "http_client.h"

/*
 * One configured Express node. Every node is an independent store with its
 * own keypair, database and clock: a message lives only on the node its
 * sender posted it to, and the timestamps one node hands out say nothing
 * about another node's. So the pull watermark and the DELETE-until owed
 * after a pull are per node, never shared across nodes.
 *
 * Nodes stay in the CONFIGURED order for the whole life of the connector:
 * posting tries them first-to-last and stops at the first that accepts, so
 * new senders keep storing on the first node (the one phones on old builds,
 * which pull from a single node, still read) unless it is down. Pulling and
 * deleting visit every node each cycle.
 */
typedef struct ExpNode {
    char *ipv4;
    uint16_t port;
    uint8_t shared_key[DHT_PUBLIC_KEY_SIZE];

    uint64_t last_timestamp;     // newest offline-message ts delivered from THIS node
    uint64_t pending_del;        // DELETE ?until= still owed to THIS node (0: none)
    unsigned consecutive_fails;  // GET failures in a row, for the log only
    long latency_us;             // HEAD /version round trip at start, log only (-1: failed)
} ExpNode;

struct ExpConnector {
    Carrier *carrier;
    ExpressOnRecvCallback on_msg_cb;
    ExpressOnRecvCallback on_req_cb;
    ExpressOnStatCallback on_stat_cb;

    pthread_mutex_t lock;
    pthread_cond_t cond;

    linked_list_t *task_list;
    int stop_flag;

    // Pull tasklets waiting in task_list (0 or 1, see
    // express_enqueue_pull_messages) and pulls started so far. Under `lock`.
    int pulls_queued;
    unsigned pulls_started;

    uint32_t magic_num;

    // Offline messages from senders who are not friends (yet), kept on disk
    // until the sender becomes a friend or they expire. Laundry thread only.
    linked_list_t *held;
    int held_count;
    size_t held_bytes;

    int express_nodes_size;
    ExpNode express_nodes[0];
};

/* One entry of the hold index; the payload itself lives in the file. */
typedef struct ExpHeldItem {
    linked_list_entry_t le;
    char from[CARRIER_MAX_ID_LEN + 1];
    uint64_t timestamp;      // the Express timestamp the item arrived with
    uint64_t stored_at;      // seconds since the epoch, for the TTL
    uint32_t size;           // payload bytes on disk
    char name[128];          // file name inside the hold directory
} ExpHeldItem;

/* On-disk header of a held item, followed by `size` payload bytes. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t timestamp;
    uint64_t stored_at;
    uint32_t size;
    uint32_t reserved;
    char from[CARRIER_MAX_ID_LEN + 1];
} ExpHeldHeader;

/* Outcome of one offline item. */
enum {
    EXP_ITEM_OK   = 0,   // delivered, held, or a known duplicate: advance past it
    EXP_ITEM_SKIP = 1,   // can never be valid: advance past it
                         // < 0: local transient failure: stop, do not advance
};

typedef struct ExpTasklet {
    linked_list_entry_t entry;
    ExpressConnector *connector;

    int (*runner)(struct ExpTasklet *);
} ExpTasklet;

typedef struct {
    ExpTasklet base;
    struct {
        uint8_t *data;
        ssize_t size;
        size_t pos;
    } crypted_data_cache;

    char to[CARRIER_MAX_ADDRESS_LEN + 1];
    ExpressMessageType type;
    uint32_t msgid;
    size_t data_size;
    uint8_t data[0];
} ExpSendTasklet;

typedef struct {
    ExpTasklet base;
    uint8_t *shared_key_cache;

    uint64_t last_timestamp;
    size_t len;
    uint8_t *data;
} ExpPullTasklet;

static const int  EXP_CURLCODE_MASK    = 0x00001000;
static const int  EXP_HTTP_MAGICNUM    = 0xCA6EE595;
static const int  EXP_HTTP_MAGICSIZE   = 4;
static const int  EXP_HTTP_REQ_TIMEOUT     = 15 * 1000; // ms (was 60 s: one dead node stalled every offline send behind it)
static const int  EXP_HTTP_HEAD_TIMEOUT     = 5 * 1000; // ms
#define  EXP_HTTP_URL_MAXSIZE 1024
static const char *EXP_STATE_FILENAME = "express_state.dat";
static const char *EXP_HOLD_DIRNAME   = "express_hold";

/* Bounds. A hostile server or a flood of strangers must not grow memory or
 * disk without limit, and one sender must not push out the others. */
#define EXP_PULL_MAX_BUFFER        (16u * 1024 * 1024)  // unparsed response bytes
#define EXP_HOLD_MAGIC             0x31485845u            // "EXH1"
#define EXP_HOLD_VERSION           1u
#define EXP_HOLD_TTL_SEC           (14 * 24 * 3600)
#define EXP_HOLD_MAX_ITEMS         256
#define EXP_HOLD_MAX_BYTES         (8u * 1024 * 1024)
#define EXP_HOLD_SENDER_MAX_ITEMS  16
#define EXP_HOLD_SENDER_MAX_BYTES  (1u * 1024 * 1024)
#define EXP_HOLD_ITEM_MAX_BYTES    (1u * 1024 * 1024)

#ifdef EXPRESS_TEST_HOOKS
/* Fault injection for the tests that #include this file. */
int express_test_fail_alloc = 0;   // >0: the next N buffer growths fail
int express_test_fail_hold  = 0;   // >0: the next N hold writes fail
#endif

static void express_state_path(ExpressConnector *connector, char *buf, size_t len)
{
    const char *base = connector && connector->carrier ? connector->carrier->pref.data_location : NULL;
    if (!buf || len == 0)
        return;

    if (!base || !*base) {
        buf[0] = '\0';
        return;
    }

    snprintf(buf, len, "%s/%s", base, EXP_STATE_FILENAME);
}

/*
 * State file, one line per node:
 *
 *     <ipv4>:<port> <last_timestamp> <pending_del>
 *
 * Keyed by host:port so that a node keeps its watermark when the configured
 * list is reordered or extended. The file written before per-node state
 * (commit 290779b) held a single "<last> <pending>" line; that line is read
 * onto the first configured node, which is the only node those builds ever
 * pulled from in practice.
 */
static void save_express_state(ExpressConnector *connector)
{
    FILE *fp;
    char path[PATH_MAX];
    int idx;

    express_state_path(connector, path, sizeof(path));
    if (!path[0])
        return;

    fp = fopen(path, "w");
    if (!fp) {
        vlogW("Express: save state open failed: %s", path);
        return;
    }

    for (idx = 0; idx < connector->express_nodes_size; idx++) {
        ExpNode *node = &connector->express_nodes[idx];
        fprintf(fp, "%s:%u %" PRIu64 " %" PRIu64 "\n",
                node->ipv4, (unsigned)node->port,
                node->last_timestamp, node->pending_del);
    }
    fclose(fp);
}

static ExpNode *find_node(ExpressConnector *connector, const char *key)
{
    int idx;
    char node_key[EXP_HTTP_URL_MAXSIZE];

    for (idx = 0; idx < connector->express_nodes_size; idx++) {
        ExpNode *node = &connector->express_nodes[idx];
        snprintf(node_key, sizeof(node_key), "%s:%u", node->ipv4, (unsigned)node->port);
        if (strcmp(node_key, key) == 0)
            return node;
    }
    return NULL;
}

static void load_express_state(ExpressConnector *connector)
{
    FILE *fp;
    char path[PATH_MAX];
    char line[EXP_HTTP_URL_MAXSIZE + 64];
    int legacy = 0;

    express_state_path(connector, path, sizeof(path));
    if (!path[0])
        return;

    fp = fopen(path, "r");
    if (!fp)
        return;

    while (fgets(line, sizeof(line), fp)) {
        char key[EXP_HTTP_URL_MAXSIZE];
        uint64_t last_ts = 0;
        uint64_t pending_ts = 0;
        ExpNode *node;

        if (sscanf(line, "%1023s %" SCNu64 " %" SCNu64, key, &last_ts, &pending_ts) == 3) {
            node = find_node(connector, key);
            if (!node) {
                vlogD("Express: state for unconfigured node %s ignored", key);
                continue;
            }
        } else if (sscanf(line, "%" SCNu64 " %" SCNu64, &last_ts, &pending_ts) >= 1) {
            // pre per-node format: belongs to the first configured node.
            node = &connector->express_nodes[0];
            legacy = 1;
        } else {
            continue;
        }

        node->last_timestamp = last_ts;
        node->pending_del = pending_ts;
        vlogD("Express: loaded state node=%s:%u last=%" PRIu64 ", pending_del=%" PRIu64,
              node->ipv4, (unsigned)node->port, node->last_timestamp, node->pending_del);
    }
    fclose(fp);

    if (legacy) {
        vlogI("Express: migrated single-node state file to per-node format");
        save_express_state(connector);
    }
}

/*
 * Hold store: offline messages whose sender is not a friend (yet).
 *
 * Express accepts a message from any keypair, and a real queue can hold a
 * stranger's message followed by that stranger's friend request. The message
 * is kept on disk (<data_location>/express_hold/, one file per item) and
 * delivered once the sender is a friend. Bounded per sender and in total,
 * expires after EXP_HOLD_TTL_SEC, survives a restart. Only the laundry
 * thread touches it.
 */
static void express_hold_dir(ExpressConnector *connector, char *buf, size_t len)
{
    const char *base = connector && connector->carrier ? connector->carrier->pref.data_location : NULL;
    if (!buf || len == 0)
        return;
    if (!base || !*base) {
        buf[0] = '\0';
        return;
    }
    snprintf(buf, len, "%s/%s", base, EXP_HOLD_DIRNAME);
}

static uint64_t now_seconds(void)
{
    return (uint64_t)time(NULL);
}

static ExpHeldItem *hold_item_new(const char *from, uint64_t timestamp,
                                  uint64_t stored_at, uint32_t size)
{
    ExpHeldItem *it = rc_zalloc(sizeof(*it), NULL);
    if (!it)
        return NULL;
    strncpy(it->from, from, sizeof(it->from) - 1);
    it->timestamp = timestamp;
    it->stored_at = stored_at;
    it->size = size;
    snprintf(it->name, sizeof(it->name), "%" PRIu64 "-%" PRIu64 "-%s.held",
             stored_at, timestamp, it->from);
    it->le.data = it;
    return it;
}

static void hold_item_path(ExpressConnector *connector, const ExpHeldItem *it,
                           char *path, size_t len)
{
    char dir[PATH_MAX];
    express_hold_dir(connector, dir, sizeof(dir));
    if (!dir[0]) {
        path[0] = '\0';
        return;
    }
    snprintf(path, len, "%s/%s", dir, it->name);
}

/* Drop one held item: file and index entry. `it` must be in the index. */
static void hold_drop(ExpressConnector *connector, ExpHeldItem *it, const char *why)
{
    char path[PATH_MAX];

    hold_item_path(connector, it, path, sizeof(path));
    if (path[0])
        unlink(path);
    vlogI("Express: held message from %s ts=%" PRIu64 " (%u bytes) dropped: %s",
          it->from, it->timestamp, it->size, why);
    connector->held_count--;
    connector->held_bytes -= it->size;
    deref(linked_list_remove_entry(connector->held, &it->le));   // the index's ref
}

/* Insert in timestamp order so release and eviction walk oldest first. */
static void hold_index_insert(ExpressConnector *connector, ExpHeldItem *it)
{
    linked_list_iterator_t iter;
    ExpHeldItem *cur;
    int pos = 0;

    linked_list_iterate(connector->held, &iter);
    while (linked_list_iterator_next(&iter, (void **)&cur) == 1) {
        int after = cur->timestamp > it->timestamp;
        deref(cur);   // iterator_next took a reference
        if (after)
            break;
        pos++;
    }
    linked_list_insert(connector->held, pos, &it->le);   // the list takes a ref
    connector->held_count++;
    connector->held_bytes += it->size;
    deref(it);                                           // the creator's ref
}

static void hold_purge_expired(ExpressConnector *connector)
{
    uint64_t now = now_seconds();
    linked_list_iterator_t iter;
    ExpHeldItem *cur;
    int again = 1;

    while (again) {
        again = 0;
        linked_list_iterate(connector->held, &iter);
        while (linked_list_iterator_next(&iter, (void **)&cur) == 1) {
            int expired = cur->stored_at + EXP_HOLD_TTL_SEC < now || cur->stored_at > now + 86400;
            deref(cur);
            if (expired) {
                hold_drop(connector, cur, "expired");
                again = 1;   // the list changed under the iterator; restart
                break;
            }
        }
    }
}

/* Make room for `size` bytes from `from`: oldest of that sender first, then
 * oldest overall. Returns the number of items dropped. */
static int hold_make_room(ExpressConnector *connector, const char *from, uint32_t size)
{
    int dropped = 0;

    for (;;) {
        linked_list_iterator_t iter;
        ExpHeldItem *cur;
        ExpHeldItem *sender_oldest = NULL;
        ExpHeldItem *oldest = NULL;
        int sender_count = 0;
        size_t sender_bytes = 0;

        linked_list_iterate(connector->held, &iter);
        while (linked_list_iterator_next(&iter, (void **)&cur) == 1) {
            if (!oldest)
                oldest = cur;
            if (strcmp(cur->from, from) == 0) {
                if (!sender_oldest)
                    sender_oldest = cur;
                sender_count++;
                sender_bytes += cur->size;
            }
            deref(cur);
        }

        if (sender_oldest &&
            (sender_count >= EXP_HOLD_SENDER_MAX_ITEMS ||
             sender_bytes + size > EXP_HOLD_SENDER_MAX_BYTES)) {
            hold_drop(connector, sender_oldest, "sender cap");
            dropped++;
            continue;
        }
        if (oldest &&
            (connector->held_count >= EXP_HOLD_MAX_ITEMS ||
             connector->held_bytes + size > EXP_HOLD_MAX_BYTES)) {
            hold_drop(connector, oldest, "total cap");
            dropped++;
            continue;
        }
        return dropped;
    }
}

/*
 * Keep an offline message for later. 0: held (or deliberately not kept,
 * which is final); < 0: a local failure (cannot write), the caller must stop
 * and not acknowledge the item.
 */
static int hold_put(ExpressConnector *connector, const char *from, uint64_t timestamp,
                    const uint8_t *payload, size_t size)
{
    char dir[PATH_MAX];
    char path[PATH_MAX];
    char tmp[PATH_MAX];
    ExpHeldHeader hdr;
    ExpHeldItem *it;
    FILE *fp;
    int rc = 0;

    if (size == 0 || size > EXP_HOLD_ITEM_MAX_BYTES || size > EXP_HOLD_SENDER_MAX_BYTES) {
        vlogW("Express: message from non-friend %s (%zu bytes) not held: size", from, size);
        return EXP_ITEM_SKIP;
    }

    express_hold_dir(connector, dir, sizeof(dir));
    if (!dir[0])
        return CARRIER_EXPRESS_ERROR(ERROR_WRONG_STATE);

#ifdef EXPRESS_TEST_HOOKS
    if (express_test_fail_hold > 0) {
        express_test_fail_hold--;
        vlogW("Express: TEST hook: hold write failure injected");
        return CARRIER_EXPRESS_ERROR(ERROR_WRONG_STATE);
    }
#endif

    hold_purge_expired(connector);
    hold_make_room(connector, from, (uint32_t)size);

    it = hold_item_new(from, timestamp, now_seconds(), (uint32_t)size);
    if (!it)
        return CARRIER_EXPRESS_ERROR(ERROR_OUT_OF_MEMORY);

    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        vlogE("Express: cannot create %s: %s", dir, strerror(errno));
        deref(it);
        return CARRIER_EXPRESS_ERROR(ERROR_WRONG_STATE);
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = EXP_HOLD_MAGIC;
    hdr.version = EXP_HOLD_VERSION;
    hdr.timestamp = it->timestamp;
    hdr.stored_at = it->stored_at;
    hdr.size = it->size;
    strncpy(hdr.from, it->from, sizeof(hdr.from) - 1);

    snprintf(path, sizeof(path), "%s/%s", dir, it->name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fp = fopen(tmp, "wb");
    if (!fp) {
        vlogE("Express: cannot write %s: %s", tmp, strerror(errno));
        deref(it);
        return CARRIER_EXPRESS_ERROR(ERROR_WRONG_STATE);
    }
    if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1 || fwrite(payload, 1, size, fp) != size)
        rc = -1;
    if (fclose(fp) != 0)
        rc = -1;
    if (rc == 0 && rename(tmp, path) != 0)
        rc = -1;
    if (rc != 0) {
        vlogE("Express: cannot store held message %s: %s", path, strerror(errno));
        unlink(tmp);
        deref(it);
        return CARRIER_EXPRESS_ERROR(ERROR_WRONG_STATE);
    }

    hold_index_insert(connector, it);
    vlogI("Express: message from non-friend %s ts=%" PRIu64 " held (%zu bytes; %d items, %zu bytes held)",
          from, timestamp, size, connector->held_count, connector->held_bytes);
    return EXP_ITEM_OK;
}

/* Rebuild the index from the hold directory at start. */
static void hold_load(ExpressConnector *connector)
{
    char dir[PATH_MAX];
    char path[PATH_MAX];
    DIR *d;
    struct dirent *de;

    express_hold_dir(connector, dir, sizeof(dir));
    if (!dir[0])
        return;
    d = opendir(dir);
    if (!d)
        return;

    while ((de = readdir(d)) != NULL) {
        ExpHeldHeader hdr;
        ExpHeldItem *it;
        FILE *fp;
        size_t n;
        size_t namelen = strlen(de->d_name);

        if (namelen < 6 || strcmp(de->d_name + namelen - 5, ".held") != 0) {
            // a leftover .tmp from an interrupted write, or junk
            if (namelen > 4 && strcmp(de->d_name + namelen - 4, ".tmp") == 0) {
                snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
                unlink(path);
            }
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        fp = fopen(path, "rb");
        if (!fp)
            continue;
        n = fread(&hdr, 1, sizeof(hdr), fp);
        fclose(fp);
        hdr.from[sizeof(hdr.from) - 1] = '\0';
        if (n != sizeof(hdr) || hdr.magic != EXP_HOLD_MAGIC || hdr.version != EXP_HOLD_VERSION ||
            hdr.size == 0 || hdr.size > EXP_HOLD_ITEM_MAX_BYTES || !hdr.from[0] ||
            strlen(hdr.from) > CARRIER_MAX_ID_LEN) {
            vlogW("Express: hold file %s unreadable, removed", de->d_name);
            unlink(path);
            continue;
        }
        it = hold_item_new(hdr.from, hdr.timestamp, hdr.stored_at, hdr.size);
        if (!it)
            break;
        if (strcmp(it->name, de->d_name) != 0) {
            // header and file name disagree: not something we wrote
            vlogW("Express: hold file %s does not match its header, removed", de->d_name);
            unlink(path);
            deref(it);
            continue;
        }
        if (connector->held_count >= EXP_HOLD_MAX_ITEMS ||
            connector->held_bytes + it->size > EXP_HOLD_MAX_BYTES) {
            vlogW("Express: hold file %s over the cap at load, removed", de->d_name);
            unlink(path);
            deref(it);
            continue;
        }
        hold_index_insert(connector, it);
    }
    closedir(d);

    hold_purge_expired(connector);
    if (connector->held_count)
        vlogI("Express: %d held message(s), %zu bytes, loaded from %s",
              connector->held_count, connector->held_bytes, dir);
}

static inline int conv_curlcode(int curlcode) {
    return (curlcode | EXP_CURLCODE_MASK);
}

static inline char *my_userid(ExpressConnector *connector)
{
    return connector->carrier->me.userid;
}

static int compute_sharedkey(Carrier *carrier, uint8_t *pk, uint8_t *shared_key)
{
    uint8_t sk[SECRET_KEY_BYTES];

    dht_self_get_secret_key(&carrier->dht, sk);
    crypto_compute_symmetric_key(pk, sk, shared_key);

    return 0;
}

static int compute_friend_sharedkey(Carrier *carrier, const char *friendid, uint8_t *shared_key)
{
    uint8_t friend_pk[PUBLIC_KEY_BYTES];
    ssize_t size;
    int rc;

    size = base58_decode(friendid, strlen(friendid), friend_pk, sizeof(friend_pk));
    if (size != sizeof(friend_pk))  {
        vlogE("Express: Decode base58 friendid %s error", friendid);
        return CARRIER_GENERAL_ERROR(ERROR_INVALID_ARGS);
    }

    rc = compute_sharedkey(carrier, friend_pk, shared_key);
    if (rc < 0) {
        vlogE("Express: compute shared key error(%x)", rc);
        return rc;
    }

    return 0;
}

static ssize_t encrypt_data(uint8_t *key,
                                uint8_t *plain_data, size_t plain_len,
                                uint8_t *crypted_data)
{
    ssize_t rc;
    uint8_t *nonce_ptr = crypted_data;
    uint8_t *data_ptr = crypted_data + NONCE_BYTES;

    crypto_random_nonce(nonce_ptr);
    rc = crypto_encrypt(key, nonce_ptr, plain_data, plain_len, data_ptr);
    if (rc < 0)
        return (ssize_t)(int)CARRIER_EXPRESS_ERROR(ERROR_ENCRYPT);   // see decrypt_data

    return rc + NONCE_BYTES;
}

/* Nonce + MAC are the minimum a ciphertext can carry. Shorter input is
 * rejected here: the old `int data_sz = len - NONCE_BYTES` went negative. */
static ssize_t decrypt_data(uint8_t *key,
                                uint8_t *crypted_data, size_t crypted_len,
                                uint8_t *plain_data)
{
    ssize_t rc;
    uint8_t *nonce_ptr = crypted_data;
    uint8_t *data_ptr = crypted_data + NONCE_BYTES;
    size_t data_sz;

    // CARRIER_EXPRESS_ERROR() is an unsigned constant: widened to ssize_t it
    // is a large positive number, not a negative one. Cast through int.
    if (!crypted_data || crypted_len < NONCE_BYTES + MAC_BYTES)
        return (ssize_t)(int)CARRIER_EXPRESS_ERROR(ERROR_ENCRYPT);
    data_sz = crypted_len - NONCE_BYTES;

    rc = crypto_decrypt(key, nonce_ptr, data_ptr, data_sz, plain_data);
    if (rc < 0)
        return (ssize_t)(int)CARRIER_EXPRESS_ERROR(ERROR_ENCRYPT);

    return rc;
}

/* A base58 user id of the right shape: non-empty, bounded, decodes to a key. */
static int valid_userid(const char *userid)
{
    uint8_t pk[PUBLIC_KEY_BYTES];
    size_t len;

    if (!userid)
        return 0;
    len = strlen(userid);
    if (len == 0 || len > CARRIER_MAX_ID_LEN)
        return 0;
    return base58_decode(userid, len, pk, sizeof(pk)) == (ssize_t)sizeof(pk);
}

/*
 * One decrypted offline item.
 *
 * Returns EXP_ITEM_OK when it was delivered, held or is a known duplicate,
 * EXP_ITEM_SKIP when it can never be valid (malformed, undecryptable,
 * unknown type, another address), and < 0 only for a local transient
 * failure. In the first two cases *timestamp advances past the item; one
 * unacceptable item must never leave everything behind it waiting.
 */
static int process_message(ExpressConnector *connector,
                           uint8_t *data, size_t size,
                           uint64_t last_ts, uint64_t *timestamp)
{
    int rc;
    PacketPullMsg pmsg;

    *timestamp = last_ts;

    rc = packet_decode_pullmsg(data, size, &pmsg);
    if (rc < 0) {
        vlogW("Express: offline item is not a valid pullmsg flatbuffer (%zu bytes), skipped", size);
        return EXP_ITEM_SKIP;
    }

    if (pmsg.timestamp == 0 || pmsg.timestamp == UINT64_MAX) {
        vlogW("Express: offline item with timestamp %" PRIu64 ", skipped", pmsg.timestamp);
        return EXP_ITEM_SKIP;
    }

    if (last_ts >= pmsg.timestamp) {
        vlogW("Express: duplicate/old offline message dropped. ts=%" PRIu64 ", last=%" PRIu64,
              pmsg.timestamp, last_ts);
        return EXP_ITEM_OK;
    }

    // From here on the item is "ours": whatever happens to it, the watermark
    // moves past it unless a local failure says stop.
    *timestamp = pmsg.timestamp;

    if (!valid_userid(pmsg.from)) {
        vlogW("Express: offline item ts=%" PRIu64 " without a valid sender id, skipped", pmsg.timestamp);
        return EXP_ITEM_SKIP;
    }

    if (pmsg.type == 'M') {
        uint8_t shared_key[SYMMETRIC_KEY_BYTES];
        uint8_t *plain_data;
        int plain_size;

        vlogV("Express: received offline message at time %" PRIu64, pmsg.timestamp);

        if (!pmsg.payload || pmsg.payload_sz < NONCE_BYTES + MAC_BYTES + 1) {
            vlogW("Express: offline message from %s ts=%" PRIu64 " too short (%zu bytes), skipped",
                  pmsg.from, pmsg.timestamp, pmsg.payload_sz);
            return EXP_ITEM_SKIP;
        }

        if (!carrier_is_friend(connector->carrier, pmsg.from)) {
            // Not a friend yet (a friend request may follow): keep it.
            rc = hold_put(connector, pmsg.from, pmsg.timestamp, pmsg.payload, pmsg.payload_sz);
            if (rc < 0) {
                *timestamp = last_ts;   // not handled: do not acknowledge it
                return rc;
            }
            return rc;   // EXP_ITEM_OK (held) or EXP_ITEM_SKIP (not keepable)
        }

        rc = compute_friend_sharedkey(connector->carrier, pmsg.from, shared_key);
        if (rc < 0) {
            vlogW("Express: shared key for %s failed (%x), skipped", pmsg.from, rc);
            return EXP_ITEM_SKIP;
        }

        plain_data = (uint8_t *)calloc(1, pmsg.payload_sz);
        if (!plain_data) {
            *timestamp = last_ts;
            return CARRIER_EXPRESS_ERROR(ERROR_OUT_OF_MEMORY);
        }

        rc = decrypt_data(shared_key, (uint8_t *)pmsg.payload, pmsg.payload_sz,
                          plain_data);
        if (rc <= 0) {
            vlogW("Express: offline message from %s ts=%" PRIu64 " does not decrypt (%x), skipped",
                  pmsg.from, pmsg.timestamp, rc < 0 ? rc : 0);
            free(plain_data);
            return EXP_ITEM_SKIP;
        }
        plain_size = rc;

        connector->on_msg_cb(connector->carrier, pmsg.from,
                             plain_data, plain_size,
                             pmsg.timestamp);
        free(plain_data);
    } else if (pmsg.type == 'R') {
        char addr[CARRIER_MAX_ADDRESS_LEN + 1];

        vlogV("Express: received offline request at time %" PRIu64, pmsg.timestamp);

        if (!pmsg.payload || pmsg.payload_sz == 0) {
            vlogW("Express: offline request from %s ts=%" PRIu64 " is empty, skipped",
                  pmsg.from, pmsg.timestamp);
            return EXP_ITEM_SKIP;
        }
        carrier_get_address(connector->carrier, addr, sizeof(addr));
        if (!pmsg.address || strcmp(pmsg.address, addr) != 0) {
            vlogW("Express: offline request from %s ts=%" PRIu64 " for another address, skipped",
                  pmsg.from, pmsg.timestamp);
            return EXP_ITEM_SKIP;
        }

        connector->on_req_cb(connector->carrier, pmsg.from,
                             pmsg.payload, pmsg.payload_sz,
                             pmsg.timestamp);
    } else {
        vlogW("Express: offline item from %s ts=%" PRIu64 " of unknown type %u, skipped",
              pmsg.from, pmsg.timestamp, (unsigned)pmsg.type);
        return EXP_ITEM_SKIP;
    }

    return EXP_ITEM_OK;
}

/* Decrypt one frame with the node key and process it. Same return contract
 * as process_message; a frame that does not decrypt is skipped. */
static int parse_msg(ExpressConnector *connector, uint8_t *shared_key,
                     uint8_t *crypted_data, size_t crypted_size,
                     uint64_t last_ts, uint64_t *timestamp)
{
    int rc;
    uint8_t *plain_data;
    ssize_t plain_size;

    *timestamp = last_ts;

    if (crypted_size < NONCE_BYTES + MAC_BYTES + 1) {
        vlogW("Express: frame of %zu bytes cannot hold a message, skipped", crypted_size);
        return EXP_ITEM_SKIP;
    }

    plain_data = (uint8_t *)calloc(1, crypted_size);
    if (!plain_data)
        return CARRIER_EXPRESS_ERROR(ERROR_OUT_OF_MEMORY);

    plain_size = decrypt_data(shared_key, crypted_data, crypted_size, plain_data);
    if (plain_size <= 0) {
        vlogW("Express: frame of %zu bytes does not decrypt with the node key, skipped", crypted_size);
        free(plain_data);
        return EXP_ITEM_SKIP;
    }

    rc = process_message(connector, plain_data, (size_t)plain_size, last_ts, timestamp);
    free(plain_data);
    return rc;
}

typedef struct {
    uint32_t magic;
    uint32_t len;
    uint8_t  msg[0];
} exp_resp_t;

/*
 * One frame of the GET body: magic, big-endian length, ciphertext. The
 * terminator is a frame of length 0.
 *
 * Returns the bytes consumed (> 0), 0 when the frame is not complete yet,
 * < 0 on a local transient failure or a body that is not an Express body.
 * *timestamp is always set: to the item's timestamp when the item was
 * handled or skipped, otherwise to last_ts (the terminator too; the old code
 * left it untouched there and the caller then read an uninitialised value).
 */
static ssize_t parse_exp_resp(ExpressConnector *connector, uint8_t *shared_key,
                              const uint8_t *data, size_t size,
                              uint64_t last_ts, uint64_t *timestamp)
{
    exp_resp_t hdr;
    uint32_t len;
    int rc;

    *timestamp = last_ts;

    if (size < sizeof(exp_resp_t))
        return 0;

    memcpy(&hdr, data, sizeof(hdr));   // the buffer is not necessarily aligned
    if (hdr.magic != connector->magic_num) {
        vlogW("Express: response is not an Express body (bad magic)");
        return (ssize_t)(int)CARRIER_EXPRESS_ERROR(ERROR_UNKNOWN);   // see decrypt_data
    }

    len = ntohl(hdr.len);
    if (!len)
        return (ssize_t)sizeof(exp_resp_t);

    // `len > size - header`, never `header + len > size`: on a 32-bit size_t
    // the sum can wrap and pass. A frame that can never fit the buffer cap
    // is hostile, not incomplete.
    if (len > EXP_PULL_MAX_BUFFER - sizeof(exp_resp_t)) {
        vlogW("Express: frame length %u exceeds the %u byte cap", len, EXP_PULL_MAX_BUFFER);
        return (ssize_t)(int)CARRIER_EXPRESS_ERROR(ERROR_UNKNOWN);   // see decrypt_data
    }
    if (len > size - sizeof(exp_resp_t))
        return 0;

    rc = parse_msg(connector, shared_key, (uint8_t *)data + sizeof(exp_resp_t), len,
                   last_ts, timestamp);
    if (rc < 0) {
        vlogW("Express: stopping at frame ts>%" PRIu64 " after a local failure (%x); nothing past it is acknowledged",
              last_ts, rc);
        return rc;
    }

    return (ssize_t)(sizeof(exp_resp_t) + len);
}

/*
 * libcurl write callback. Frames handled here move task->last_timestamp,
 * which the pull runner commits as the node's watermark even when the
 * transfer later fails: the prefix was fully handled, so acknowledging it is
 * right, and a retry drops it as duplicates instead of delivering it twice.
 * Returning a short count aborts the transfer (CURLE_WRITE_ERROR).
 */
size_t http_read_data(char *buffer, size_t size, size_t nitems, void *userdata)
{
    ssize_t rc;
    ExpPullTasklet *task = (ExpPullTasklet *)userdata;
    ExpressConnector *connector = task->base.connector;
    size_t buf_sz = size * nitems;
    uint64_t ts;
    size_t parsed;
    uint8_t *grown;

    if (buf_sz == 0)
        return 0;

    if (buf_sz > EXP_PULL_MAX_BUFFER - task->len) {
        vlogW("Express: response exceeds the %u byte buffer cap, transfer aborted", EXP_PULL_MAX_BUFFER);
        goto fail;
    }

#ifdef EXPRESS_TEST_HOOKS
    if (express_test_fail_alloc > 0) {
        express_test_fail_alloc--;
        vlogW("Express: TEST hook: buffer growth failure injected");
        goto fail;
    }
#endif
    if (!task->data)
        grown = rc_alloc(buf_sz, NULL);
    else
        grown = rc_realloc(task->data, task->len + buf_sz);
    if (!grown) {
        vlogE("Express: out of memory growing the response buffer to %zu bytes", task->len + buf_sz);
        goto fail;   // task->data is still the old, valid buffer
    }
    task->data = grown;
    memcpy(task->data + task->len, buffer, buf_sz);
    task->len += buf_sz;

    for (parsed = 0;
         (rc = parse_exp_resp(connector, task->shared_key_cache,
                              task->data + parsed, task->len - parsed,
                              task->last_timestamp, &ts)) > 0;
         parsed += (size_t)rc, task->last_timestamp = ts) ;
    if (rc < 0) {
        carrier_set_error((int)rc);
        goto fail;
    }

    if (task->len -= parsed)
        memmove(task->data, task->data + parsed, task->len);
    else {
        deref(task->data);
        task->data = NULL;
    }

    return buf_sz;

fail:
    if (task->data) {
        deref(task->data);
        task->data = NULL;
    }
    task->len = 0;
    return 0;
}

size_t http_write_data(char *buffer, size_t size, size_t nitems, void *userdata)
{
    ExpSendTasklet *task = (ExpSendTasklet *)userdata;
    int buf_sz = size * nitems;
    int data_sz = task->crypted_data_cache.size - task->crypted_data_cache.pos;

    if(data_sz <= 0) {
        return 0;
    }

    data_sz = (data_sz < buf_sz ? data_sz : buf_sz);
    memcpy(buffer, task->crypted_data_cache.data + task->crypted_data_cache.pos, data_sz);

    task->crypted_data_cache.pos += data_sz;
    if(task->crypted_data_cache.pos >= task->crypted_data_cache.size) {
        task->crypted_data_cache.pos = 0;
        task->crypted_data_cache.size = 0;
    }

    return data_sz;
}

static int http_do(ExpressConnector *connector,
                   const char* ip, uint16_t port,
                   const char* path, http_method_t method,
                   void *userdata)
{
    int rc;
    http_client_t *http_client;
    char url[EXP_HTTP_URL_MAXSIZE];
    long http_client_rescode = 0;

    const char* dowhat = (method == HTTP_METHOD_HEAD ? "heading"
                       : (method == HTTP_METHOD_POST ? "pushing"
                       : (method == HTTP_METHOD_GET ? "pulling"
                       : (method == HTTP_METHOD_DELETE ? "deleting"
                       : "unknown"))));

    snprintf(url, sizeof(url), "https://%s:%d/%s", ip, port, path);
    // vlogD("Express: %s message, node: %s", dowhat, ip);

    http_client = http_client_new();
    if (!http_client) {
        vlogE("Express: Failed to new client.");
        return CARRIER_EXPRESS_ERROR(ERROR_OUT_OF_MEMORY);
    }

    rc = http_client_set_url(http_client, url);
    if(rc != 0) {
        http_client_close(http_client);
        vlogE("Express: Failed to set url.(CURLE: %d)", rc);
        return CARRIER_EXPRESS_ERROR(conv_curlcode(rc));
    }

    rc = http_client_set_method(http_client, method);
    if(rc != 0) {
        http_client_close(http_client);
        vlogE("Express: Failed to set method.(CURLE: %d)", rc);
        return CARRIER_EXPRESS_ERROR(conv_curlcode(rc));
    }

    if (method == HTTP_METHOD_POST) {
        http_client_set_header(http_client, "Content-Type", "application/binary");
        // http_client_set_header(http_client, "messageId", "10");

        rc = http_client_set_request_body(http_client, http_write_data, userdata);
        if (rc != 0) {
            http_client_close(http_client);
            vlogE("Express: Failed to set request body.(CURLE: %d)", rc);
            return CARRIER_EXPRESS_ERROR(conv_curlcode(rc));
        }
    } else if (method == HTTP_METHOD_GET) {
        rc = http_client_set_response_body(http_client, http_read_data, userdata);
        if (rc != 0) {
            http_client_close(http_client);
            vlogE("Express: Failed to set response body.(CURLE: %d)", rc);
            return CARRIER_EXPRESS_ERROR(conv_curlcode(rc));
        }
    }

    http_client_set_timeout(http_client,
                            method == HTTP_METHOD_HEAD ?  EXP_HTTP_HEAD_TIMEOUT : EXP_HTTP_REQ_TIMEOUT);
    rc = http_client_request(http_client);
    if(rc != 0) {
        http_client_close(http_client);
        vlogW("Express: Failed to perform request. node:%s, path:%s. (CURLE: %d)", ip, path, rc);
        return CARRIER_EXPRESS_ERROR(conv_curlcode(rc));
    }

    rc = http_client_get_response_code(http_client, &http_client_rescode);
    if(rc != 0) {
        http_client_close(http_client);
        vlogW("Express: Failed to get response code.(CURLE: %d)", rc);
        return CARRIER_EXPRESS_ERROR(conv_curlcode(rc));
    }
    if((method == HTTP_METHOD_POST && http_client_rescode != 201)
    || (method == HTTP_METHOD_GET && http_client_rescode != 200)
    || (method == HTTP_METHOD_DELETE && http_client_rescode != 205)) {
        http_client_close(http_client);
        vlogW("Express: Failed to %s message from node. rescode=%d.(CURLE: %d)", dowhat, http_client_rescode, rc);
        return CARRIER_EXPRESS_ERROR(ERROR_WRONG_STATE);
    }

    http_client_close(http_client);
    // vlogD("Express: Success to %s message, node: %s", dowhat, ip);

    return 0;
}

/*
 * Base58 for a value the Express node decodes with a standard decoder, where
 * every leading zero byte is a leading '1'. The shared base58_encode drops
 * leading zero bytes, so an encrypted value whose random nonce began with
 * 0x00 (1 in 256) reached the node one byte short and the request was
 * refused (HTTP 400, reported here as 8700000f) until a later attempt drew
 * another nonce.
 */
static char *base58_encode_std(const uint8_t *data, size_t len,
                               char *buf, size_t buf_size)
{
    size_t zeros = 0;
    size_t rest;

    while (zeros < len && data[zeros] == 0)
        zeros++;
    if (zeros + 1 > buf_size)
        return NULL;
    memset(buf, '1', zeros);
    buf[zeros] = '\0';
    if (zeros == len)
        return buf;

    rest = buf_size - zeros;
    if (!base58_encode(data + zeros, len - zeros, buf + zeros, &rest))
        return NULL;
    return buf;
}

/*
 * DELETE /<me>?until=<ts> on ONE node. The timestamp was handed out by that
 * node, so it must never be sent to any other node: another node's clock
 * could be ahead and the request would wipe messages there that were never
 * delivered.
 */
static int del_msgs_on(ExpressConnector *connector, ExpNode *node, uint64_t msg_lasttime)
{
    int rc;
    char lasttime[256];
    int lasttime_len;
    uint8_t *crypted_data;
    int crypted_sz;
    char *encoded_data;
    size_t encoded_sz;
    char path[EXP_HTTP_URL_MAXSIZE];

    if(msg_lasttime == 0)
        return 0;

    snprintf(lasttime, sizeof(lasttime), "%" PRIu64, msg_lasttime);
    lasttime_len = strlen(lasttime);

    crypted_data = alloca(NONCE_BYTES + lasttime_len + ZERO_BYTES);
    rc = encrypt_data(node->shared_key, (uint8_t*)lasttime, lasttime_len, crypted_data);
    if(rc < 0) {
        vlogE("Express: encrypt last time failed.(%x)", rc);
        return rc;
    }
    crypted_sz = rc;

    encoded_sz = crypted_sz * 1.4f + 2;
    encoded_data = alloca(encoded_sz);
    encoded_data = base58_encode_std(crypted_data, crypted_sz,
                                     encoded_data, encoded_sz);
    if(encoded_data == NULL) {
        vlogE("Express: encode last time failed.");
        return CARRIER_EXPRESS_ERROR(ERROR_INVALID_CREDENTIAL);
    }

    snprintf(path, sizeof(path), "%s?until=%s", my_userid(connector), encoded_data);
    rc = http_do(connector, node->ipv4, node->port,
                 path, HTTP_METHOD_DELETE,
                 NULL);
    if (rc < 0) {
        vlogW("Express: delete until=%" PRIu64 " failed on node=%s:%u (%x), will retry next cycle",
              msg_lasttime, node->ipv4, (unsigned)node->port, rc);
        return rc;
    }

    vlogI("Express: delete succeeded until=%" PRIu64 " node=%s:%u",
          msg_lasttime, node->ipv4, (unsigned)node->port);
    return 0;
}

static int postmsg_runner(ExpTasklet *base)
{
    int rc;
    ExpSendTasklet *task = (ExpSendTasklet *)base;
    ExpressConnector *connector = task->base.connector;
    char path[EXP_HTTP_URL_MAXSIZE];
    int idx;

#if 0
    vlogV("Express: send message data: (%d) 0x%02x 0x%02x ~ 0x%02x 0x%02x",
          length, data[0], data[1], data[length - 2], data[length - 1]);
#endif

    /*
     * First success, in the CONFIGURED order. Not the fastest node: while
     * phones on old builds (which pull from a single node) are in the field,
     * a new sender must keep storing on the first node unless it is down,
     * or those phones never see the message.
     */
    snprintf(path, sizeof(path), "%s/%s", task->to, my_userid(connector));
    for(idx = 0; idx < connector->express_nodes_size; idx++) {
        ExpNode *node = &connector->express_nodes[idx];

        task->crypted_data_cache.pos = 0;
        task->crypted_data_cache.size = task->data_size + NONCE_BYTES + ZERO_BYTES;
        task->crypted_data_cache.data = calloc(1, task->crypted_data_cache.size);
        if (!task->crypted_data_cache.data) {
            vlogE("Express: alloc encrypt data error");
            deref(task);
            carrier_set_error(rc);
            return rc;
        }

        rc = encrypt_data(node->shared_key, (uint8_t *)task->data, task->data_size,
                          task->crypted_data_cache.data);
        if (rc < 0) {
            vlogE("Express: Encrypt data error");
            deref(task);
            carrier_set_error(rc);
            return rc;
        }
        task->crypted_data_cache.size = rc;

        rc = http_do(connector, node->ipv4, node->port,
                     path, HTTP_METHOD_POST,
                     task);
        free((void*)task->crypted_data_cache.data);
        task->crypted_data_cache.pos = -1;
        task->crypted_data_cache.size = -1;
        task->crypted_data_cache.data = NULL;
        if (rc >= 0) {
            if (idx > 0)
                vlogW("Express: posted to node #%d %s:%u after %d node(s) failed",
                      idx, node->ipv4, (unsigned)node->port, idx);
            break;
        }
        vlogW("Express: post to node #%d %s:%u failed (%x), trying next",
              idx, node->ipv4, (unsigned)node->port, rc);
    }

    connector->on_stat_cb(connector->carrier, task->to, task->type, task->msgid, rc);
    if(rc < 0) {
        vlogE("Express: Failed to post message on all %d node(s).(%x)",
              connector->express_nodes_size, rc);
        return rc;
    }

    vlogD("Express: Success to post message to %s.", task->to);
    return 0;
}

/*
 * Deliver held messages whose sender is a friend by now, and drop the
 * expired ones. Runs on the laundry thread: after every pull, at start, and
 * when carrier_accept_friend asks for it.
 */
static void hold_release(ExpressConnector *connector)
{
    int again = 1;

    if (!connector->held || !connector->held_count)
        return;

    hold_purge_expired(connector);

    while (again) {
        linked_list_iterator_t iter;
        ExpHeldItem *cur;

        again = 0;
        linked_list_iterate(connector->held, &iter);
        while (linked_list_iterator_next(&iter, (void **)&cur) == 1) {
            uint8_t shared_key[SYMMETRIC_KEY_BYTES];
            ExpHeldHeader hdr;
            uint8_t *crypted = NULL;
            uint8_t *plain = NULL;
            char path[PATH_MAX];
            FILE *fp;
            ssize_t plain_size;
            int ok = 0;

            deref(cur);   // the list keeps it alive while we work on it

            if (!carrier_is_friend(connector->carrier, cur->from))
                continue;

            hold_item_path(connector, cur, path, sizeof(path));
            fp = path[0] ? fopen(path, "rb") : NULL;
            if (fp) {
                crypted = malloc(cur->size);
                plain = crypted ? malloc(cur->size) : NULL;
                if (crypted && plain &&
                    fread(&hdr, sizeof(hdr), 1, fp) == 1 &&
                    hdr.magic == EXP_HOLD_MAGIC && hdr.size == cur->size &&
                    fread(crypted, 1, cur->size, fp) == cur->size &&
                    compute_friend_sharedkey(connector->carrier, cur->from, shared_key) == 0) {
                    plain_size = decrypt_data(shared_key, crypted, cur->size, plain);
                    if (plain_size > 0) {
                        connector->on_msg_cb(connector->carrier, cur->from,
                                             plain, (size_t)plain_size, cur->timestamp);
                        ok = 1;
                    }
                }
                fclose(fp);
            }
            free(crypted);
            free(plain);

            if (ok)
                vlogI("Express: held message from %s ts=%" PRIu64 " delivered, sender is a friend now",
                      cur->from, cur->timestamp);
            hold_drop(connector, cur, ok ? "delivered" : "unreadable or undecryptable");
            again = 1;   // the list changed under the iterator; restart
            break;
        }
    }
}

static int hold_release_runner(ExpTasklet *base)
{
    hold_release(base->connector);
    return 0;
}

static int enqueue_hold_release_tasklet(ExpressConnector *connector)
{
    ExpTasklet *task;

    task = (ExpTasklet *)rc_zalloc(sizeof(ExpTasklet), NULL);
    if (!task)
        return CARRIER_EXPRESS_ERROR(ERROR_OUT_OF_MEMORY);

    task->entry.data = task;
    task->connector = connector;
    task->runner = hold_release_runner;

    pthread_mutex_lock(&connector->lock);
    linked_list_push_head(connector->task_list, &task->entry);
    pthread_mutex_unlock(&connector->lock);

    deref(task);
    pthread_cond_signal(&connector->cond);
    return 0;
}

static int pullmsgs_runner(ExpTasklet *base)
{
    int rc;
    ExpPullTasklet *task = (ExpPullTasklet *)base;
    ExpressConnector *connector = task->base.connector;
    const char *path;
    int idx;

    int pulled = 0;
    int last_rc = 0;

    // From here on a new request queues another pull: this one may already
    // be past the node a message has just been stored on.
    pthread_mutex_lock(&connector->lock);
    if (connector->pulls_queued > 0)
        connector->pulls_queued--;
    connector->pulls_started++;
    vlogD("Express: pull #%u started", connector->pulls_started);
    pthread_mutex_unlock(&connector->lock);

    /*
     * Visit EVERY configured node; a message lives only on the node its
     * sender posted it to. A node that fails is logged and skipped, never
     * aborts the loop. Each node has its own watermark: the GET parses
     * against THAT node's last timestamp, and the DELETE-until that follows
     * goes to THAT node only. This matches the JS client
     * (peer/packages/peer/src/compat/express.ts, pullOnce).
     */
    path = my_userid(connector);
    for(idx = 0; idx < connector->express_nodes_size; idx++) {
        ExpNode *node = &connector->express_nodes[idx];

        task->shared_key_cache = node->shared_key;
        task->last_timestamp = node->last_timestamp;
        if (task->data) {               // leftover bytes from the previous node
            deref(task->data);
            task->data = NULL;
        }
        task->len = 0;

        rc = http_do(connector, node->ipv4, node->port,
                     path, HTTP_METHOD_GET,
                     task);
        if (rc < 0) {
            node->consecutive_fails++;
            last_rc = rc;
            if (node->consecutive_fails == 1 || node->consecutive_fails % 10 == 0)
                vlogW("Express: pull from node #%d %s:%u failed (%x), %u in a row; skipping it this cycle",
                      idx, node->ipv4, (unsigned)node->port, rc, node->consecutive_fails);
            else
                vlogD("Express: pull from node #%d %s:%u failed (%x), %u in a row",
                      idx, node->ipv4, (unsigned)node->port, rc, node->consecutive_fails);
            // A cut-off or aborted transfer: whatever was fully handled before
            // the cut is acknowledged (and deleted on the next good cycle), so
            // a retry does not deliver it twice. Nothing past it is.
            if (task->last_timestamp > node->last_timestamp) {
                node->last_timestamp = task->last_timestamp;
                node->pending_del = task->last_timestamp;
                save_express_state(connector);
                vlogI("Express: node #%d %s:%u partial pull, watermark=%" PRIu64 " (prefix handled)",
                      idx, node->ipv4, (unsigned)node->port, node->last_timestamp);
            }
            continue;
        }
        if (node->consecutive_fails) {
            vlogI("Express: node #%d %s:%u recovered after %u failure(s)",
                  idx, node->ipv4, (unsigned)node->port, node->consecutive_fails);
            node->consecutive_fails = 0;
        }
        pulled++;

        if (task->last_timestamp > node->last_timestamp) {
            node->last_timestamp = task->last_timestamp;
            node->pending_del = task->last_timestamp;
            save_express_state(connector);
            vlogI("Express: pulled from node #%d %s:%u, watermark=%" PRIu64,
                  idx, node->ipv4, (unsigned)node->port, node->last_timestamp);
        } else {
            vlogD("Express: pulled from node #%d %s:%u, nothing new (watermark=%" PRIu64 ")",
                  idx, node->ipv4, (unsigned)node->port, node->last_timestamp);
        }

        if (node->pending_del > 0) {
            rc = del_msgs_on(connector, node, node->pending_del);
            if (rc >= 0) {
                node->pending_del = 0;
                save_express_state(connector);
            }
        }
    }

    if (task->data) {
        deref(task->data);
        task->data = NULL;
        task->len = 0;
    }

    // Senders may have become friends since their messages were held.
    hold_release(connector);

    if (!pulled) {
        vlogE("Express: Failed to pull message from all %d node(s).(%x)",
              connector->express_nodes_size, last_rc);
        return last_rc < 0 ? last_rc : CARRIER_EXPRESS_ERROR(ERROR_WRONG_STATE);
    }
    if (pulled < connector->express_nodes_size)
        vlogW("Express: pulled from %d of %d node(s)", pulled, connector->express_nodes_size);
    else
        vlogD("Express: pulled from all %d node(s)", pulled);
    return 0;
}

static int speedmeter_runner(ExpTasklet *base)
{
    int rc;
    ExpTasklet *task = (ExpTasklet *)base;
    ExpressConnector *connector = task->connector;
    int idx;
    struct timeval starttime, endtime, diff;

    /*
     * Metering only. The nodes are NOT reordered by latency any more: the
     * configured order is the posting order (see postmsg_runner), and pulls
     * visit every node anyway.
     */
    for(idx = 0; idx < connector->express_nodes_size; idx++) {
        ExpNode *node = &connector->express_nodes[idx];

        gettimeofday(&starttime, NULL);
        rc = http_do(connector, node->ipv4, node->port,
                     "version", HTTP_METHOD_HEAD, NULL);
        if(rc >= 0) {
            gettimeofday(&endtime, NULL);
            timersub(&endtime, &starttime, &diff);
            node->latency_us = (long)diff.tv_sec * 1000000L + diff.tv_usec;
            vlogI("Express: node #%d %s:%u HEAD /version %ld.%03ld ms",
                  idx, node->ipv4, (unsigned)node->port,
                  node->latency_us / 1000, node->latency_us % 1000);
        } else {
            node->latency_us = -1;
            vlogW("Express: node #%d %s:%u HEAD /version failed (%x)",
                  idx, node->ipv4, (unsigned)node->port, rc);
        }
    }

    return 0;
}

static int enqueue_speedmeter_tasklet(ExpressConnector *connector)
{
    int rc;
    ExpTasklet *task;

    assert(connector);

    task = (ExpTasklet *)rc_zalloc(sizeof(ExpTasklet), NULL);
    if (!task) {
        rc = CARRIER_EXPRESS_ERROR(ERROR_OUT_OF_MEMORY);
        carrier_set_error(rc);
        return rc;
    }

    task->entry.data = task;
    task->connector = connector;
    task->runner = speedmeter_runner;

    pthread_mutex_lock(&connector->lock);
    linked_list_push_head(connector->task_list, &task->entry);
    pthread_mutex_unlock(&connector->lock);

    deref(task);
    pthread_cond_signal(&connector->cond);

    return 0;
}

static int enqueue_post_tasklet(ExpressConnector *connector, const char *to,
                                ExpressMessageType type,
                                const void *data, size_t size, uint32_t msgid)
{
    int rc;
    ExpSendTasklet *task;
    uint8_t *crypted_data;
    ssize_t crypted_sz;

    assert(connector && to);

    task = (ExpSendTasklet *)rc_zalloc(sizeof(ExpSendTasklet) + size, NULL);
    if (!task) {
        rc = CARRIER_EXPRESS_ERROR(ERROR_OUT_OF_MEMORY);
        carrier_set_error(rc);
        return rc;
    }

    task->base.entry.data = task;
    task->base.connector = connector;
    task->base.runner = postmsg_runner;

    strcpy(task->to, to);

    task->type = type;
    task->msgid = msgid;
    memcpy(task->data, data, size);
    task->data_size = size;

    pthread_mutex_lock(&connector->lock);
    linked_list_push_head(connector->task_list, &task->base.entry);
    pthread_mutex_unlock(&connector->lock);

    deref(task);
    pthread_cond_signal(&connector->cond);

    return 0;
}

static void connector_releaser(void *arg)
{
    ExpressConnector *connector = (ExpressConnector *)arg;
    assert(connector);

    if (connector->task_list) {
        deref(connector->task_list);
        connector->task_list = NULL;
    }

    if (connector->held) {
        deref(connector->held);
        connector->held = NULL;
    }

    connector->carrier = NULL;

    pthread_mutex_destroy(&connector->lock);
    pthread_cond_destroy (&connector->cond);
}

static void *connector_laundry(void *arg)
{
    int rc;
    ExpressConnector *connector = (ExpressConnector *)arg;
    ExpTasklet *task;

    vlogI("Express: Express connector start to running.");

    pthread_mutex_lock(&connector->lock);
    while(!connector->stop_flag) {
        if (linked_list_is_empty(connector->task_list)) {
            pthread_cond_wait(&connector->cond, &connector->lock);
            continue;
        }

        task = linked_list_pop_tail(connector->task_list);
        assert(task);

        pthread_mutex_unlock(&connector->lock);

        rc = task->runner(task);
        deref(task);
        // if(rc < 0) {
        //     vlogW("Express: exec tasklet failed.(%x)", rc);
        //     carrier_set_error(rc);
        // }

        pthread_mutex_lock(&connector->lock);
    }
    pthread_mutex_unlock(&connector->lock);

    deref(connector->carrier);
    deref(connector);

    vlogI("Express: Express connector exited gracefully.");

    return NULL;
}

ExpressConnector *express_connector_create(Carrier *carrier,
                                           ExpressOnRecvCallback on_msg_cb,
                                           ExpressOnRecvCallback on_req_cb,
                                           ExpressOnStatCallback on_stat_cb)
{
    int rc;
    ExpressConnector *connector;
    char url_base[EXP_HTTP_URL_MAXSIZE];
    ExpressNodeBuf *node_0;
    pthread_t tid;
    int idx;

    assert(carrier);
    assert(carrier->pref.express_size > 0);

    connector = rc_zalloc(sizeof(ExpressConnector) + sizeof(ExpNode) * carrier->pref.express_size,
                          connector_releaser); // deref by outside
    if (!connector) {
        carrier_set_error(CARRIER_EXPRESS_ERROR(ERROR_OUT_OF_MEMORY));
        return NULL;
    }

    connector->carrier = carrier;
    connector->on_msg_cb = on_msg_cb;
    connector->on_req_cb = on_req_cb;
    connector->on_stat_cb = on_stat_cb;

    pthread_mutex_init(&connector->lock, NULL);
    pthread_cond_init (&connector->cond, NULL);

    connector->task_list = linked_list_create(0, NULL);
    if (!connector->task_list) {
        deref(connector);
        carrier_set_error(CARRIER_EXPRESS_ERROR(ERROR_OUT_OF_MEMORY));
        return NULL;
    }
    connector->stop_flag = 0;

    connector->magic_num = htonl(EXP_HTTP_MAGICNUM);

    connector->express_nodes_size = carrier->pref.express_size;
    for(idx = 0; idx < connector->express_nodes_size; idx++) {
        connector->express_nodes[idx].ipv4 = carrier->pref.express_nodes[idx].ipv4;
        connector->express_nodes[idx].port = carrier->pref.express_nodes[idx].port;
        connector->express_nodes[idx].last_timestamp = 0;
        connector->express_nodes[idx].pending_del = 0;
        connector->express_nodes[idx].consecutive_fails = 0;
        connector->express_nodes[idx].latency_us = -1;
        rc = compute_sharedkey(carrier,
                               carrier->pref.express_nodes[idx].public_key,
                               connector->express_nodes[idx].shared_key);
        if (rc < 0) {
            deref(connector);
            carrier_set_error(rc);
            return NULL;
        }
    }
    // after the node table: the state file is keyed by node.
    load_express_state(connector);

    connector->held = linked_list_create(0, NULL);
    if (!connector->held) {
        deref(connector);
        carrier_set_error(CARRIER_EXPRESS_ERROR(ERROR_OUT_OF_MEMORY));
        return NULL;
    }
    hold_load(connector);

    rc = enqueue_speedmeter_tasklet(connector);
    if (rc < 0) {
        deref(connector);
        carrier_set_error(rc);
        return NULL;
    }
    // Friends accepted while the previous run was down.
    if (connector->held_count)
        enqueue_hold_release_tasklet(connector);

    ref(connector);
    ref(connector->carrier);
    rc = pthread_create(&tid, NULL, connector_laundry, connector);
    if (rc != 0) {
        deref(connector->carrier);
        deref(connector);
        deref(connector);
        carrier_set_error(CARRIER_EXPRESS_ERROR(ERROR_OUT_OF_MEMORY));
        return NULL;
    }
    pthread_detach(tid);

    return connector;
}

// peer with express_connector_create,
// connector must be deref manually after this function called.
void express_connector_kill(ExpressConnector *connector)
{
    assert(connector);

    pthread_mutex_lock(&connector->lock);
    connector->stop_flag = 1;
    pthread_cond_signal(&connector->cond);
    pthread_mutex_unlock(&connector->lock);
}

int express_enqueue_post_request(ExpressConnector *connector,
                                 const char *address,
                                 const void *hello, size_t size)
{
    return enqueue_post_tasklet(connector, address, EXPRESS_FRIEND_REQUEST, hello, size, 0);
}

int express_enqueue_post_message_with_receipt(ExpressConnector *connector,
                                              const char *friendid,
                                              const void *data, size_t size,
                                              uint32_t msgid)
{
    uint8_t shared_key[SYMMETRIC_KEY_BYTES];
    uint8_t *crypted_data;
    ssize_t crypted_sz;
    int rc;

    rc = compute_friend_sharedkey(connector->carrier, friendid, shared_key);
    if (rc < 0) {
        vlogE("Express: compute shared key error(%x)", rc);
        return rc;
    }

    crypted_data = (uint8_t *)calloc(1, size + NONCE_BYTES + ZERO_BYTES);
    if (!crypted_data) {
        vlogW("Express: post receipt message failed.");
        return CARRIER_EXPRESS_ERROR(ERROR_OUT_OF_MEMORY);
    }
    rc = encrypt_data(shared_key, (uint8_t *)data, size, crypted_data);
    if(rc < 0) {
        free(crypted_data);
        vlogE("Express: encrypt last tiime failed.(%x)", rc);
        return rc;
    }
    crypted_sz = rc;

    rc = enqueue_post_tasklet(connector, friendid, EXPRESS_FRIEND_MESSAGE, crypted_data, crypted_sz, msgid);
    free(crypted_data);
    if (rc < 0) {
        vlogE("Express: enqueue post tasklet error(%x)", rc);
        return rc;
    }

    return 0;
}

/*
 * Ask for a pull of every node. Any thread. Requests are coalesced: while a
 * pull is waiting in the queue another request adds nothing (that pull has
 * not started, so it will see whatever the caller wants picked up); while a
 * pull is running, one more is queued behind it. So there is never more than
 * the pull in progress plus one queued, however often this is called.
 */
int express_enqueue_pull_messages(ExpressConnector *connector)
{
    ExpPullTasklet *task;

    assert(connector);

    pthread_mutex_lock(&connector->lock);
    if (connector->pulls_queued > 0) {
        pthread_mutex_unlock(&connector->lock);
        return 0;
    }
    pthread_mutex_unlock(&connector->lock);

    task = (ExpPullTasklet *)rc_zalloc(sizeof(ExpPullTasklet), NULL);
    if (!task)
        return CARRIER_EXPRESS_ERROR(ERROR_OUT_OF_MEMORY);

    task->base.entry.data = task;
    task->base.connector = connector;
    task->base.runner = pullmsgs_runner;

    pthread_mutex_lock(&connector->lock);
    if (connector->pulls_queued > 0) {      // another thread got in first
        pthread_mutex_unlock(&connector->lock);
        deref(task);
        return 0;
    }
    connector->pulls_queued++;
    linked_list_push_head(connector->task_list, &task->base.entry);
    pthread_mutex_unlock(&connector->lock);

    deref(task);
    pthread_cond_signal(&connector->cond);

    return 0;
}

/* Called when a sender becomes a friend (carrier_accept_friend): deliver the
 * offline messages held for them, on the laundry thread. */
int express_enqueue_hold_release(ExpressConnector *connector)
{
    assert(connector);
    return enqueue_hold_release_tasklet(connector);
}
