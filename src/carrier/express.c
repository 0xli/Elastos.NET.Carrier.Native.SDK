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

    uint32_t magic_num;

    int express_nodes_size;
    ExpNode express_nodes[0];
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
        } else {
            continue;
        }

        node->last_timestamp = last_ts;
        node->pending_del = pending_ts;
        vlogD("Express: loaded state node=%s:%u last=%" PRIu64 ", pending_del=%" PRIu64,
              node->ipv4, (unsigned)node->port, node->last_timestamp, node->pending_del);
    }
    fclose(fp);
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
        return CARRIER_EXPRESS_ERROR(ERROR_ENCRYPT);

    return rc + NONCE_BYTES;
}

static ssize_t decrypt_data(uint8_t *key,
                                uint8_t *crypted_data, size_t crypted_len,
                                uint8_t *plain_data)
{
    ssize_t rc;
    uint8_t *nonce_ptr = crypted_data;
    uint8_t *data_ptr = crypted_data + NONCE_BYTES;
    int data_sz = crypted_len - NONCE_BYTES;

    rc = crypto_decrypt(key, nonce_ptr, data_ptr, data_sz, plain_data);
    if (rc < 0)
        return CARRIER_EXPRESS_ERROR(ERROR_ENCRYPT);

    return rc;
}

static int process_message(ExpressConnector *connector,
                           uint8_t *data, size_t size,
                           uint64_t last_ts, uint64_t *timestamp)
{
    int rc;
    PacketPullMsg pmsg;

    rc = packet_decode_pullmsg(data, &pmsg);
    if (rc < 0) {
        vlogE("Express: decode pullmsg flatbuffer failed.(%x)", rc);
        return CARRIER_EXPRESS_ERROR(ERROR_BAD_FLATBUFFER);
    }

    if (last_ts >= pmsg.timestamp) {
        *timestamp = last_ts;
        vlogW("Express: duplicate/old offline message dropped. ts=%" PRIu64 ", last=%" PRIu64,
              pmsg.timestamp, last_ts);
        return 0;
    }

    if (pmsg.type == 'M') {
        uint8_t shared_key[SYMMETRIC_KEY_BYTES];
        uint8_t *plain_data;
        int plain_size;

        vlogV("Express: recieved offline message at time %llu", pmsg.timestamp);
        if (!carrier_is_friend(connector->carrier, pmsg.from)) {
            vlogE("Express: Friend message not frond friend, dropped.");
            return -1;
        }

        rc = compute_friend_sharedkey(connector->carrier, pmsg.from, shared_key);
        if (rc < 0) {
            vlogE("Express: compute message shared key error(%x)", rc);
            return rc;
        }

        plain_data = (uint8_t *)calloc(1, pmsg.payload_sz);
        if (!plain_data) {
            vlogW("Express: parse message failed.");
            return CARRIER_EXPRESS_ERROR(ERROR_OUT_OF_MEMORY);
        }

        rc = decrypt_data(shared_key, (uint8_t *)pmsg.payload, pmsg.payload_sz,
                          plain_data);
        if (rc < 0) {
            vlogW("Express: decrypt message data failed: (%x).", rc);
            free(plain_data);
            return rc;
        }
        plain_size = rc;
        *timestamp = pmsg.timestamp;

        connector->on_msg_cb(connector->carrier, pmsg.from,
                             plain_data, plain_size,
                             pmsg.timestamp);
        free(plain_data);
    } else if (pmsg.type == 'R') {
        vlogV("Express: recieved offline request at time %llu", pmsg.timestamp);
        char addr[CARRIER_MAX_ADDRESS_LEN + 1];
        carrier_get_address(connector->carrier, addr, sizeof(addr));
        if (strcmp(pmsg.address, addr) != 0) {
            vlogE("Express: Friend request with unmatched address, dropped.");
            return -1;
        }
        *timestamp = pmsg.timestamp;

        connector->on_req_cb(connector->carrier, pmsg.from,
                             pmsg.payload, pmsg.payload_sz,
                             pmsg.timestamp);
    } else {
        vlogW("Express: recieved offline unknown type at time %llu", pmsg.timestamp);
        return -1;
    }

    return 0;
}

static int parse_msg(ExpressConnector *connector, uint8_t *shared_key,
                     uint8_t *crypted_data, size_t crypted_size,
                     uint64_t last_ts, uint64_t *timestamp)
{
    int rc;
    uint8_t *plain_data;
    int plain_size;

    plain_data = (uint8_t *)calloc(1, crypted_size);
    if (!plain_data) {
        vlogW("Express: parse message failed.");
        return CARRIER_EXPRESS_ERROR(ERROR_OUT_OF_MEMORY);
    }

    rc = decrypt_data(shared_key,
                      crypted_data, crypted_size,
                      plain_data);
    if (rc < 0) {
        vlogW("Express: decrypt message data failed: (%x).", rc);
        free(plain_data);
        return rc;
    }
    plain_size = rc;

    rc = process_message(connector, plain_data, plain_size, last_ts, timestamp);
    if (rc < 0) {
        vlogW("Express: process message failed: (%x).", rc);
        free(plain_data);
        return rc;
    }

    free(plain_data);
    return 0;
}

typedef struct {
    uint32_t magic;
    uint32_t len;
    uint8_t  msg[0];
} exp_resp_t;

static int parse_exp_resp(ExpressConnector *connector, uint8_t *shared_key,
                          const uint8_t *data, size_t size, uint64_t last_ts, uint64_t *timestamp)
{
    exp_resp_t *resp = (exp_resp_t *) data;
    uint32_t len;
    int rc;

    if (size < sizeof(exp_resp_t))
        return 0;

    if (resp->magic != connector->magic_num)
        return CARRIER_EXPRESS_ERROR(ERROR_UNKNOWN);

    len = ntohl(resp->len);
    if (!len)
        return sizeof(exp_resp_t);

    if (sizeof(exp_resp_t) + len > size)
        return 0;

    rc = parse_msg(connector, shared_key, resp->msg, len, last_ts, timestamp);
    if (rc < 0) {
        vlogW("Express: response body parse message failed: (%x).", rc);
        return rc;
    }

    return sizeof(exp_resp_t) + len;
}

size_t http_read_data(char *buffer, size_t size, size_t nitems, void *userdata)
{
    int rc;
    ExpPullTasklet *task = (ExpPullTasklet *)userdata;
    ExpressConnector *connector = task->base.connector;
    size_t buf_sz = size * nitems;
    uint64_t ts;
    size_t parsed;

    if (!task->data)
        task->data = rc_alloc(buf_sz, NULL);
    else
        task->data = rc_realloc(task->data, task->len + buf_sz);
    memcpy(task->data + task->len, buffer, buf_sz);
    task->len += buf_sz;

    for (parsed = 0;
         (rc = parse_exp_resp(connector, task->shared_key_cache,
                              task->data + parsed, task->len - parsed,
                              task->last_timestamp, &ts)) > 0;
         parsed += rc, task->last_timestamp = ts) ;
    if (rc < 0) {
        carrier_set_error(rc);
        deref(task->data);
        task->data = NULL;
        task->len = 0;
        return -1;
    }

    if (task->len -= parsed)
        memmove(task->data, task->data + parsed, task->len);
    else {
        deref(task->data);
        task->data = NULL;
    }

    return buf_sz;
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

    encoded_sz = crypted_sz * 1.4f + 1;
    encoded_data = alloca(encoded_sz);
    encoded_data = base58_encode(crypted_data, crypted_sz,
                                encoded_data, &encoded_sz);
    if(encoded_data == NULL || encoded_sz <= 0) {
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

static int pullmsgs_runner(ExpTasklet *base)
{
    int rc;
    ExpPullTasklet *task = (ExpPullTasklet *)base;
    ExpressConnector *connector = task->base.connector;
    const char *path;
    int idx;

    int pulled = 0;
    int last_rc = 0;

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

    rc = enqueue_speedmeter_tasklet(connector);
    if (rc < 0) {
        deref(connector);
        carrier_set_error(rc);
        return NULL;
    }

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

int express_enqueue_pull_messages(ExpressConnector *connector)
{
    ExpPullTasklet *task;

    assert(connector);

    task = (ExpPullTasklet *)rc_zalloc(sizeof(ExpPullTasklet), NULL);
    if (!task)
        return CARRIER_EXPRESS_ERROR(ERROR_OUT_OF_MEMORY);

    task->base.entry.data = task;
    task->base.connector = connector;
    task->base.runner = pullmsgs_runner;

    pthread_mutex_lock(&connector->lock);
    linked_list_push_head(connector->task_list, &task->base.entry);
    pthread_mutex_unlock(&connector->lock);

    deref(task);
    pthread_cond_signal(&connector->cond);

    return 0;
}
