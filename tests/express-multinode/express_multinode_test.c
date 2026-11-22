/*
 * express_multinode_test — drive the Carrier Express connector against
 * several Express nodes without a DHT.
 *
 * Modes (one per invocation, all take --data <dir> and zero or more
 * --node host:port:base58pk, in the order the SDK should see them):
 *
 *   init   create (or open) the carrier identity in <dir>, print
 *          "address: ..." and "userid: ...", exit.
 *   send   post an offline friend request through the connector
 *          (express_enqueue_post_request, the same path carrier_add_friend
 *          takes when the friend is offline) to --to <address> with
 *          --hello <text>; wait --wait seconds for the laundry thread, exit.
 *   pull   run the carrier loop, which pulls from every node right away,
 *          for --wait seconds; print "REQUEST from=<userid> hello=<text>"
 *          for each offline friend request delivered, then print the
 *          persisted per-node state as "STATE <line>" lines.
 *
 * The carrier is created with no bootstrap nodes, so nothing here touches
 * the network except the Express nodes given on the command line. The test
 * uses friend requests rather than friend messages because a friend message
 * is only delivered to an existing friend, and making friends needs the DHT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <getopt.h>

#include <crystal.h>
#include <dirent.h>
#include "carrier.h"
#include "carrier_impl.h"
#include "packet.h"
#include "express.h"

#define MAX_NODES 8

static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_accept_requests;   // --accept: accept every friend request as it arrives

/* FNV-1a 32 of a message body, so a test can check a large payload by hash. */
static uint32_t fnv1a(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t h = 0x811c9dc5u;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

static void on_friend_message(Carrier *w, const char *from, const void *msg, size_t len,
                              int64_t timestamp, bool offline, void *context)
{
    (void)w; (void)context;
    pthread_mutex_lock(&out_lock);
    printf("MESSAGE from=%s len=%zu fnv=%08x ts=%lld offline=%d\n", from, len, fnv1a(msg, len),
           (long long)timestamp, offline ? 1 : 0);
    fflush(stdout);
    pthread_mutex_unlock(&out_lock);
}

static int count_held(const char *data_dir)
{
    char path[1024];
    DIR *d;
    struct dirent *de;
    int n = 0;
    snprintf(path, sizeof(path), "%s/express_hold", data_dir);
    d = opendir(path);
    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL)
        if (strstr(de->d_name, ".held"))
            n++;
    closedir(d);
    return n;
}

static void on_friend_request(Carrier *w, const char *userid,
                              const CarrierUserInfo *info,
                              const char *hello, void *context)
{
    (void)info; (void)context;
    pthread_mutex_lock(&out_lock);
    printf("REQUEST from=%s hello=%s\n", userid, hello ? hello : "");
    fflush(stdout);
    pthread_mutex_unlock(&out_lock);
    if (g_accept_requests) {
        int rc = carrier_accept_friend(w, userid);
        pthread_mutex_lock(&out_lock);
        printf("ACCEPTED %s rc=%d\n", userid, rc);
        fflush(stdout);
        pthread_mutex_unlock(&out_lock);
    }
}

static void *run_thread(void *arg)
{
    Carrier *w = (Carrier *)arg;
    int rc = carrier_run(w, 50);
    if (rc < 0)
        fprintf(stderr, "carrier_run failed: 0x%x\n", carrier_get_error());
    return NULL;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: express_multinode_test init|send|sendmsg|pull --data DIR [--node host:port:pk]...\n"
        "       [--to ADDRESS|USERID --hello TEXT] [--size BYTES] [--wait SECONDS] [--loglevel N]\n"
        "       [--friend USERID]... [--accept]\n"
        "  send     post an offline friend request to --to ADDRESS\n"
        "  sendmsg  post an offline friend message to --to USERID (--hello text, or --size bytes)\n"
        "  pull     pull; --friend adds friends first, --accept accepts requests as they arrive\n");
}

static void print_state(const char *data_dir)
{
    char path[1024];
    char line[2048];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/express_state.dat", data_dir);
    fp = fopen(path, "r");
    if (!fp) {
        printf("STATE <none>\n");
        return;
    }
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = '\0';
        printf("STATE %s\n", line);
    }
    fclose(fp);
}

int main(int argc, char *argv[])
{
    const char *mode;
    const char *data_dir = NULL;
    const char *to = NULL;
    const char *hello = "hello";
    int wait_sec = 5;
    int loglevel = CarrierLogLevel_Info;
    long size = 0;
    char *friends[MAX_NODES];
    int nfriends = 0;
    char *node_specs[MAX_NODES];
    char *node_host[MAX_NODES];
    char *node_port[MAX_NODES];
    char *node_pk[MAX_NODES];
    ExpressNode nodes[MAX_NODES];
    int nnodes = 0;
    CarrierOptions opts;
    CarrierCallbacks cbs;
    Carrier *w;
    pthread_t tid;
    char address[CARRIER_MAX_ADDRESS_LEN + 1];
    char userid[CARRIER_MAX_ID_LEN + 1];
    int i, opt;

    static struct option long_opts[] = {
        { "data",     required_argument, NULL, 'd' },
        { "node",     required_argument, NULL, 'n' },
        { "to",       required_argument, NULL, 't' },
        { "hello",    required_argument, NULL, 'h' },
        { "wait",     required_argument, NULL, 'w' },
        { "loglevel", required_argument, NULL, 'l' },
        { "size",     required_argument, NULL, 's' },
        { "friend",   required_argument, NULL, 'f' },
        { "accept",   no_argument,       NULL, 'A' },
        { NULL, 0, NULL, 0 }
    };

    if (argc < 2) {
        usage();
        return 2;
    }
    mode = argv[1];
    optind = 2;

    while ((opt = getopt_long(argc, argv, "d:n:t:h:w:l:s:f:A", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's': size = atol(optarg); break;
        case 'f':
            if (nfriends >= MAX_NODES) { usage(); return 2; }
            friends[nfriends++] = optarg;
            break;
        case 'A': g_accept_requests = 1; break;
        case 'd': data_dir = optarg; break;
        case 'n':
            if (nnodes >= MAX_NODES) { usage(); return 2; }
            node_specs[nnodes++] = strdup(optarg);
            break;
        case 't': to = optarg; break;
        case 'h': hello = optarg; break;
        case 'w': wait_sec = atoi(optarg); break;
        case 'l': loglevel = atoi(optarg); break;
        default: usage(); return 2;
        }
    }

    if (!data_dir) {
        usage();
        return 2;
    }
    if ((strcmp(mode, "send") == 0 || strcmp(mode, "sendmsg") == 0) && (!to || nnodes == 0)) {
        fprintf(stderr, "%s needs --to and at least one --node\n", mode);
        return 2;
    }

    for (i = 0; i < nnodes; i++) {
        char *spec = node_specs[i];
        char *p1 = strchr(spec, ':');
        char *p2 = p1 ? strchr(p1 + 1, ':') : NULL;
        if (!p1 || !p2) {
            fprintf(stderr, "bad --node %s (want host:port:pk)\n", spec);
            return 2;
        }
        *p1 = '\0';
        *p2 = '\0';
        node_host[i] = spec;
        node_port[i] = p1 + 1;
        node_pk[i] = p2 + 1;

        memset(&nodes[i], 0, sizeof(nodes[i]));
        nodes[i].ipv4 = node_host[i];
        nodes[i].ipv6 = NULL;
        nodes[i].port = node_port[i];
        nodes[i].public_key = node_pk[i];
    }

    memset(&opts, 0, sizeof(opts));
    opts.persistent_location = data_dir;
    opts.udp_enabled = true;
    opts.log_level = loglevel;
    opts.bootstraps_size = 0;
    opts.bootstraps = NULL;
    opts.express_nodes_size = nnodes;
    opts.express_nodes = nnodes ? nodes : NULL;

    memset(&cbs, 0, sizeof(cbs));
    cbs.friend_request = on_friend_request;
    cbs.friend_message = on_friend_message;

    w = carrier_new(&opts, &cbs, NULL);
    if (!w) {
        fprintf(stderr, "carrier_new failed: 0x%x\n", carrier_get_error());
        return 1;
    }

    carrier_get_address(w, address, sizeof(address));
    carrier_get_userid(w, userid, sizeof(userid));
    printf("address: %s\n", address);
    printf("userid: %s\n", userid);
    fflush(stdout);

    // No DHT here, so carrier_accept_friend's readiness check is satisfied by hand.
    w->is_ready = true;
    for (i = 0; i < nfriends; i++) {
        int rc = carrier_accept_friend(w, friends[i]);
        printf("FRIEND %s rc=%d%s\n", friends[i], rc,
               rc < 0 && carrier_get_error() == CARRIER_GENERAL_ERROR(ERROR_ALREADY_EXIST) ? " (already)" : "");
    }
    fflush(stdout);

    if (strcmp(mode, "init") == 0) {
        carrier_kill(w);
        return 0;
    }

    if (pthread_create(&tid, NULL, run_thread, w) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        carrier_kill(w);
        return 1;
    }

    if (strcmp(mode, "send") == 0) {
        Packet *cp;
        uint8_t *data;
        size_t len;
        int rc;

        cp = packet_create(PACKET_TYPE_FRIEND_REQUEST, NULL);
        if (!cp) {
            fprintf(stderr, "packet_create failed\n");
            carrier_kill(w);
            return 1;
        }
        packet_set_name(cp, "express-multinode-test");
        packet_set_descr(cp, "");
        packet_set_hello(cp, hello);
        data = packet_encode(cp, &len);
        packet_free(cp);
        if (!data) {
            fprintf(stderr, "packet_encode failed\n");
            carrier_kill(w);
            return 1;
        }

        rc = express_enqueue_post_request(w->connector, to, data, len);
        free(data);
        if (rc < 0) {
            fprintf(stderr, "express_enqueue_post_request failed: 0x%x\n", rc);
            carrier_kill(w);
            return 1;
        }
        printf("POSTED to=%s hello=%s\n", to, hello);
        fflush(stdout);
    } else if (strcmp(mode, "sendmsg") == 0) {
        Packet *cp;
        uint8_t *data;
        size_t len;
        char *body;
        size_t body_len;
        int rc;

        if (size > 0) {
            long k;
            body_len = (size_t)size;
            body = malloc(body_len);
            for (k = 0; k < size; k++)
                body[k] = (char)('a' + (k * 7 + k / 251) % 26);
        } else {
            body_len = strlen(hello);
            body = strdup(hello);
        }
        cp = packet_create(PACKET_TYPE_MESSAGE, NULL);
        packet_set_raw_data(cp, body, body_len);
        data = packet_encode(cp, &len);
        packet_free(cp);
        if (!data) {
            fprintf(stderr, "packet_encode failed\n");
            carrier_kill(w);
            return 1;
        }
        rc = express_enqueue_post_message_with_receipt(w->connector, to, data, len, 1);
        free(data);
        if (rc < 0) {
            fprintf(stderr, "express_enqueue_post_message_with_receipt failed: 0x%x\n", rc);
            carrier_kill(w);
            return 1;
        }
        printf("POSTEDMSG to=%s len=%zu fnv=%08x\n", to, body_len, fnv1a(body, body_len));
        fflush(stdout);
        free(body);
    } else if (strcmp(mode, "pull") == 0) {
        /* carrier_run pulls on its first iteration (do_express_expire);
         * one more explicit pull makes the test independent of that. */
        if (w->connector)
            express_enqueue_pull_messages(w->connector);
    } else {
        usage();
        carrier_kill(w);
        return 2;
    }

    sleep(wait_sec);

    carrier_kill(w);
    pthread_join(tid, NULL);

    if (strcmp(mode, "pull") == 0) {
        print_state(data_dir);
        printf("HELD %d\n", count_held(data_dir));
    }

    return 0;
}
