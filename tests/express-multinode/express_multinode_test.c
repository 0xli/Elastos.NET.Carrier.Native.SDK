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
#include "carrier.h"
#include "carrier_impl.h"
#include "packet.h"
#include "express.h"

#define MAX_NODES 8

static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;

static void on_friend_request(Carrier *w, const char *userid,
                              const CarrierUserInfo *info,
                              const char *hello, void *context)
{
    (void)w; (void)info; (void)context;
    pthread_mutex_lock(&out_lock);
    printf("REQUEST from=%s hello=%s\n", userid, hello ? hello : "");
    fflush(stdout);
    pthread_mutex_unlock(&out_lock);
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
        "usage: express_multinode_test init|send|pull --data DIR [--node host:port:pk]...\n"
        "       [--to ADDRESS --hello TEXT] [--wait SECONDS] [--loglevel N]\n");
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
        { NULL, 0, NULL, 0 }
    };

    if (argc < 2) {
        usage();
        return 2;
    }
    mode = argv[1];
    optind = 2;

    while ((opt = getopt_long(argc, argv, "d:n:t:h:w:l:", long_opts, NULL)) != -1) {
        switch (opt) {
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
    if (strcmp(mode, "send") == 0 && (!to || nnodes == 0)) {
        fprintf(stderr, "send needs --to and at least one --node\n");
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

    if (strcmp(mode, "pull") == 0)
        print_state(data_dir);

    return 0;
}
