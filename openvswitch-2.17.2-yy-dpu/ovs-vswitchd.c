/* Copyright (c) 2008, 2009, 2010, 2011, 2012, 2013, 2014, 2015, 2016 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_MLOCKALL
#include <sys/mman.h>
#endif

#include "bridge.h"
#include "command-line.h"
#include "compiler.h"
#include "daemon.h"
#include "dirs.h"
#include "dpif.h"
#include "dummy.h"
#include "fatal-signal.h"
#include "memory.h"
#include "netdev.h"
#include "openflow/openflow.h"
#include "ovsdb-idl.h"
#include "ovs-rcu.h"
#include "ovs-router.h"
#include "ovs-thread.h"
#include "openvswitch/poll-loop.h"
#include "simap.h"
#include "stream-ssl.h"
#include "stream.h"
#include "svec.h"
#include "timeval.h"
#include "unixctl.h"
#include "util.h"
#include "openvswitch/usdt-probes.h"
#include "openvswitch/vconn.h"
#include "openvswitch/vlog.h"
#include "lib/vswitch-idl.h"
#include "lib/dns-resolve.h"

#include <rte_malloc.h>
#include <rte_mempool.h>
#include "../lib/dpif-netdev.h"
#include <unistd.h>

VLOG_DEFINE_THIS_MODULE(vswitchd);

/* --mlockall: If set, locks all process memory into physical RAM, preventing
 * the kernel from paging any of its memory to disk. */
static bool want_mlockall;

static unixctl_cb_func ovs_vswitchd_exit;

static char *parse_options(int argc, char *argv[], char **unixctl_path);
OVS_NO_RETURN static void usage(void);

struct ovs_vswitchd_exit_args {
    bool *exiting;
    bool *cleanup;
};

/* ----------------- HOPA CP start -----------------*/

#define IPV4_ADDR(a, b, c, d) (((a & 0xff) << 24) | ((b & 0xff) << 16) | ((c & 0xff) << 8) | (d & 0xff))

#define NUM_MBUFS (8191)
#define MBUF_CACHE_SIZE (512)
#define RX_RING_SIZE (1024)
#define TX_RING_SIZE (1024)

/* Number of paths */
#define PATH_NB (4)

/* MAC addr */
#define DST_MAC                            \
    {                                      \
        0x08, 0xc0, 0xeb, 0xbf, 0xef, 0x9b \
    }
#define SRC_MAC                           \
    {                                      \
        0x08, 0xc0, 0xeb, 0xbf, 0xef, 0x83 \
    }

/* IP addr */
#define DST_IP IPV4_ADDR(192, 168, 201, 2)
#define SRC_IP IPV4_ADDR(192, 168, 201, 1)

/* UDP port and path */
#define SRC_PORT (4444)
#define DST_PORT (8880)

bool hopa_cp_has_init = false;

pthread_t hopa_cp_thread_send;
pthread_t hopa_cp_thread_progress;

uint64_t all_paths_delay_list[PATH_NB] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};

time_t start_time;

static void hopa_cp_init(void);
static void *hopa_cp_send(void* arg);
static void *hopa_cp_progress(void* arg);

/* encode packet */
static struct rte_mbuf *encode_probe_pkt(uint8_t path_id);
static struct rte_mbuf *encode_repath_pkt(uint8_t repath_id);

/* packet progress */
static void hopa_cp_probe_pkt_progress(struct hopa_cp_hdr *hopa_cp_hdr);
static void hopa_cp_repath_pkt_progress(struct hopa_cp_hdr *hopa_cp_hdr);
static void hopa_cp_repath_ack_pkt_progress(struct hopa_cp_hdr *hopa_cp_hdr);
static void hopa_dp_ts_pkt_progress(struct hopa_dp_hdr *hopa_dp_hdr);

/* utils */
static uint8_t get_min_delay_path_id(uint64_t *all_paths_delay_list, uint8_t length);

/* ------------------ HOPA CP end ------------------*/

int
main(int argc, char *argv[])
{
    char *unixctl_path = NULL;
    struct unixctl_server *unixctl;
    char *remote;
    bool exiting, cleanup;
    struct ovs_vswitchd_exit_args exit_args = {&exiting, &cleanup};
    int retval;

    set_program_name(argv[0]);
    ovsthread_id_init();

    dns_resolve_init(true);
    ovs_cmdl_proctitle_init(argc, argv);
    service_start(&argc, &argv);
    remote = parse_options(argc, argv, &unixctl_path);
    fatal_ignore_sigpipe();

    daemonize_start(true);

    if (want_mlockall) {
#ifdef HAVE_MLOCKALL
        if (mlockall(MCL_CURRENT | MCL_FUTURE)) {
            VLOG_ERR("mlockall failed: %s", ovs_strerror(errno));
        } else {
            set_memory_locked();
        }
#else
        VLOG_ERR("mlockall not supported on this system");
#endif
    }

    retval = unixctl_server_create(unixctl_path, &unixctl);
    if (retval) {
        exit(EXIT_FAILURE);
    }
    unixctl_command_register("exit", "[--cleanup]", 0, 1,
                             ovs_vswitchd_exit, &exit_args);

    bridge_init(remote);
    free(remote);

    start_time = time(NULL); // HOPA

    exiting = false;
    cleanup = false;
    while (!exiting) {
        OVS_USDT_PROBE(main, run_start);
        memory_run();
        if (memory_should_report()) {
            struct simap usage;

            simap_init(&usage);
            bridge_get_memory_usage(&usage);
            memory_report(&usage);
            simap_destroy(&usage);
        }
        bridge_run();
        unixctl_server_run(unixctl);
        netdev_run();

        hopa_cp_init(); // HOPA_CP Init. Only once! delay 1s!

        memory_wait();
        bridge_wait();
        unixctl_server_wait(unixctl);
        netdev_wait();
        if (exiting) {
            poll_immediate_wake();
        }
        OVS_USDT_PROBE(main, poll_block);
        poll_block();
        if (should_service_stop()) {
            exiting = true;
        }
    }
    bridge_exit(cleanup);
    unixctl_server_destroy(unixctl);
    service_stop();
    vlog_disable_async();
    ovsrcu_exit();
    dns_resolve_destroy();

    return 0;
}

static char *
parse_options(int argc, char *argv[], char **unixctl_pathp)
{
    enum {
        OPT_PEER_CA_CERT = UCHAR_MAX + 1,
        OPT_MLOCKALL,
        OPT_UNIXCTL,
        VLOG_OPTION_ENUMS,
        OPT_BOOTSTRAP_CA_CERT,
        OPT_ENABLE_DUMMY,
        OPT_DISABLE_SYSTEM,
        OPT_DISABLE_SYSTEM_ROUTE,
        DAEMON_OPTION_ENUMS,
        OPT_DPDK,
        SSL_OPTION_ENUMS,
        OPT_DUMMY_NUMA,
    };
    static const struct option long_options[] = {
        {"help",        no_argument, NULL, 'h'},
        {"version",     no_argument, NULL, 'V'},
        {"mlockall",    no_argument, NULL, OPT_MLOCKALL},
        {"unixctl",     required_argument, NULL, OPT_UNIXCTL},
        DAEMON_LONG_OPTIONS,
        VLOG_LONG_OPTIONS,
        STREAM_SSL_LONG_OPTIONS,
        {"peer-ca-cert", required_argument, NULL, OPT_PEER_CA_CERT},
        {"bootstrap-ca-cert", required_argument, NULL, OPT_BOOTSTRAP_CA_CERT},
        {"enable-dummy", optional_argument, NULL, OPT_ENABLE_DUMMY},
        {"disable-system", no_argument, NULL, OPT_DISABLE_SYSTEM},
        {"disable-system-route", no_argument, NULL, OPT_DISABLE_SYSTEM_ROUTE},
        {"dpdk", optional_argument, NULL, OPT_DPDK},
        {"dummy-numa", required_argument, NULL, OPT_DUMMY_NUMA},
        {NULL, 0, NULL, 0},
    };
    char *short_options = ovs_cmdl_long_options_to_short_options(long_options);

    for (;;) {
        int c;

        c = getopt_long(argc, argv, short_options, long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'h':
            usage();

        case 'V':
            ovs_print_version(0, 0);
            print_dpdk_version();
            exit(EXIT_SUCCESS);

        case OPT_MLOCKALL:
            want_mlockall = true;
            break;

        case OPT_UNIXCTL:
            *unixctl_pathp = optarg;
            break;

        VLOG_OPTION_HANDLERS
        DAEMON_OPTION_HANDLERS
        STREAM_SSL_OPTION_HANDLERS

        case OPT_PEER_CA_CERT:
            stream_ssl_set_peer_ca_cert_file(optarg);
            break;

        case OPT_BOOTSTRAP_CA_CERT:
            stream_ssl_set_ca_cert_file(optarg, true);
            break;

        case OPT_ENABLE_DUMMY:
            dummy_enable(optarg);
            break;

        case OPT_DISABLE_SYSTEM:
            dp_disallow_provider("system");
            break;

        case OPT_DISABLE_SYSTEM_ROUTE:
            ovs_router_disable_system_routing_table();
            break;

        case '?':
            exit(EXIT_FAILURE);

        case OPT_DPDK:
            ovs_fatal(0, "Using --dpdk to configure DPDK is not supported.");
            break;

        case OPT_DUMMY_NUMA:
            ovs_numa_set_dummy(optarg);
            break;

        default:
            abort();
        }
    }
    free(short_options);

    argc -= optind;
    argv += optind;

    switch (argc) {
    case 0:
        return xasprintf("unix:%s/db.sock", ovs_rundir());

    case 1:
        return xstrdup(argv[0]);

    default:
        VLOG_FATAL("at most one non-option argument accepted; "
                   "use --help for usage");
    }
}

static void
usage(void)
{
    printf("%s: Open vSwitch daemon\n"
           "usage: %s [OPTIONS] [DATABASE]\n"
           "where DATABASE is a socket on which ovsdb-server is listening\n"
           "      (default: \"unix:%s/db.sock\").\n",
           program_name, program_name, ovs_rundir());
    stream_usage("DATABASE", true, false, true);
    daemon_usage();
    vlog_usage();
    printf("\nDPDK options:\n"
           "Configuration of DPDK via command-line is removed from this\n"
           "version of Open vSwitch. DPDK is configured through ovsdb.\n"
          );
    printf("\nOther options:\n"
           "  --unixctl=SOCKET          override default control socket name\n"
           "  -h, --help                display this help message\n"
           "  -V, --version             display version information\n");
    exit(EXIT_SUCCESS);
}

static void
ovs_vswitchd_exit(struct unixctl_conn *conn, int argc,
                  const char *argv[], void *exit_args_)
{
    struct ovs_vswitchd_exit_args *exit_args = exit_args_;
    *exit_args->exiting = true;
    *exit_args->cleanup = argc == 2 && !strcmp(argv[1], "--cleanup");
    unixctl_command_reply(conn, NULL);
}

static void hopa_cp_init(void){

    if(hopa_cp_has_init)
        return;

    if((time(NULL) - start_time) < 1)
        return;

    /* Creates a new mempool in memory to hold the mbufs. */
	hopa_cp_mp = rte_pktmbuf_pool_create("HOPA_CP_MP", NUM_MBUFS, MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, 0);
    
    if (hopa_cp_mp == NULL)
		VLOG_ERR("Cannot create hopa_cp_mp");
    else
        VLOG_INFO("hopa_cp_mp success");
    
    m_hopa_cp_in_out_ring = rte_malloc("in_out ring", sizeof(struct hopa_cp_in_out_ring), 0);
	memset(m_hopa_cp_in_out_ring, 0, sizeof(struct hopa_cp_in_out_ring));
    
    m_hopa_cp_in_out_ring->hopa_cp_in_ring = rte_ring_create("hopa_cp in ring", RX_RING_SIZE, 0, RING_F_SP_ENQ | RING_F_SC_DEQ);
	m_hopa_cp_in_out_ring->hopa_cp_out_ring = rte_ring_create("hopa_cp out ring", TX_RING_SIZE, 0, RING_F_SP_ENQ | RING_F_SC_DEQ);

    if(m_hopa_cp_in_out_ring != NULL)
        if((m_hopa_cp_in_out_ring->hopa_cp_in_ring == NULL) || (m_hopa_cp_in_out_ring->hopa_cp_out_ring== NULL))
            VLOG_ERR("Cannot create m_hopa_cp_in_out_ring");
        else
            VLOG_INFO("m_hopa_cp_in_out_ring success");
    else
         VLOG_INFO("m_hopa_cp_in_out_ring NULL");

    pthread_create(&hopa_cp_thread_send, NULL, hopa_cp_send, NULL);
    pthread_create(&hopa_cp_thread_progress, NULL, hopa_cp_progress, NULL);

    VLOG_INFO("hopa_cp_thread create, rte_socket_id_hopa = %d",rte_socket_id());

    hopa_cp_has_init = true;
}

static void *
hopa_cp_send(void* arg)
{
    VLOG_INFO("hopa_cp_thread_send start");
    (void *)arg;

    struct rte_mbuf *mbuf;

    while (1)
    {
        sleep(5);
        
        mbuf = encode_repath_pkt(2); // test 换路 path2

        int count = rte_ring_mp_enqueue_burst(m_hopa_cp_in_out_ring->hopa_cp_out_ring, (void **)&mbuf, 1, NULL);
        VLOG_INFO("rte_ring_mp_enqueue_burst repath_pkt count:[%d]", count);

    }

    return 0;
}

static void *
hopa_cp_progress(void* arg)
{
    struct hopa_cp_hdr *hopa_cp_hdrs[32];
	uint16_t nb_rx;
	uint16_t i;

    VLOG_INFO("hopa_cp_thread_progress start");
    while (1)
	{
        nb_rx = rte_ring_mc_dequeue_burst(m_hopa_cp_in_out_ring->hopa_cp_in_ring, (void **)hopa_cp_hdrs, 32, NULL);
        for (i = 0; i < nb_rx; i++)
        {
            if (hopa_cp_hdrs[i]->flag == HOPA_CP)
			{
                switch (hopa_cp_hdrs[i]->cp_flag)
                {
                    case PROBE:
                        hopa_cp_probe_pkt_progress(hopa_cp_hdrs[i]);  // receiver
                        break;

                    case REPATH:
                        hopa_cp_repath_pkt_progress(hopa_cp_hdrs[i]);  // sender
                        VLOG_INFO("repath");
                        break;

                    case REPATH_ACK:
                        hopa_cp_repath_ack_pkt_progress(hopa_cp_hdrs[i]);  // receiver
                        VLOG_INFO("repath_ack");
                        break;
                    
                    default:
                        break;
                }
            }

            rte_free(hopa_cp_hdrs[i]);
        }
    }

    return 0;
}

static struct rte_mbuf *encode_probe_pkt(uint8_t path_id)
{
	struct rte_mbuf *mbuf;
    struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *udp_hdr;
	struct hopa_cp_hdr *hopa_cp_hdr;
    struct timespec ts;
	uint64_t sender_ts;

    if (hopa_cp_mp == NULL)
	{
		VLOG_INFO("hopa_cp_mp null");
		return NULL;
	}

	mbuf = rte_pktmbuf_alloc(hopa_cp_mp);

	if (!mbuf){
		VLOG_INFO("Error with rte_pktmbuf_alloc()");
        return NULL;
    }

    //mbuf->pkt_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(struct hopa_cp_hdr);
    //mbuf->data_len = sizeof(struct hopa_cp_hdr);

    /*  ETH  */
    eth_hdr = (struct rte_ether_hdr *)rte_pktmbuf_append(mbuf, sizeof(struct rte_ether_hdr));
	eth_hdr->src_addr = (struct rte_ether_addr){SRC_MAC};
	eth_hdr->dst_addr = (struct rte_ether_addr){DST_MAC};
	eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    /*  IPv4  */
	ipv4_hdr = (struct rte_ipv4_hdr *)rte_pktmbuf_append(mbuf, sizeof(struct rte_ipv4_hdr));
	ipv4_hdr->version_ihl = (4 << 4) + 5;   // ipv4 version , length 5 (*4 bytes)
	ipv4_hdr->type_of_service = 0;  // No Diffserv
	ipv4_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(struct hopa_cp_hdr)); // IP bytes
	ipv4_hdr->packet_id = rte_cpu_to_be_16(5462);   // set random
	ipv4_hdr->fragment_offset = rte_cpu_to_be_16(0);
	ipv4_hdr->time_to_live = 64;
	ipv4_hdr->next_proto_id = IPPROTO_UDP;
	ipv4_hdr->src_addr = rte_cpu_to_be_32(SRC_IP);
	ipv4_hdr->dst_addr = rte_cpu_to_be_32(DST_IP);
	ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);

    /*  UDP  */
	udp_hdr = (struct rte_udp_hdr *)rte_pktmbuf_append(mbuf, sizeof(struct rte_udp_hdr));
    udp_hdr->src_port = rte_cpu_to_be_16(SRC_PORT);
	udp_hdr->dst_port = rte_cpu_to_be_16(DST_PORT + path_id);
	udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + sizeof(struct hopa_cp_hdr));

    /*  HOPA CP  */
    hopa_cp_hdr = (struct hopa_cp_hdr *)rte_pktmbuf_append(mbuf, sizeof(struct hopa_cp_hdr));
    hopa_cp_hdr->flag = HOPA_CP;
	hopa_cp_hdr->cp_flag = PROBE;
    hopa_cp_hdr->probe_path_id = path_id;
    hopa_cp_hdr->repath_id = 0;
    hopa_cp_hdr->rsvd = 0;
    hopa_cp_hdr->seq = 0;
	hopa_cp_hdr->ack = 0;
    clock_gettime(0, &ts);
    sender_ts = (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
	hopa_cp_hdr->ts = rte_cpu_to_be_64(sender_ts);

    VLOG_INFO("current ts (ns) : [%" PRIu64 "]", sender_ts);

	return mbuf;
}

static struct rte_mbuf *encode_repath_pkt(uint8_t repath_id)
{
	struct rte_mbuf *mbuf;
    struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *udp_hdr;
	struct hopa_cp_hdr *hopa_cp_hdr;
    struct timespec ts;
	uint64_t sender_ts;

    if (hopa_cp_mp == NULL)
	{
		VLOG_INFO("hopa_cp_mp null");
		return NULL;
	}

	mbuf = rte_pktmbuf_alloc(hopa_cp_mp);

	if (!mbuf){
		VLOG_INFO("Error with rte_pktmbuf_alloc()");
        return NULL;
    }

    //mbuf->pkt_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(struct hopa_cp_hdr);
    //mbuf->data_len = sizeof(struct hopa_cp_hdr);

    /*  ETH  */
    eth_hdr = (struct rte_ether_hdr *)rte_pktmbuf_append(mbuf, sizeof(struct rte_ether_hdr));
	eth_hdr->src_addr = (struct rte_ether_addr){SRC_MAC};
	eth_hdr->dst_addr = (struct rte_ether_addr){DST_MAC};
	eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    /*  IPv4  */
	ipv4_hdr = (struct rte_ipv4_hdr *)rte_pktmbuf_append(mbuf, sizeof(struct rte_ipv4_hdr));
	ipv4_hdr->version_ihl = (4 << 4) + 5;   // ipv4 version , length 5 (*4 bytes)
	ipv4_hdr->type_of_service = 0;  // No Diffserv
	ipv4_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(struct hopa_cp_hdr)); // IP bytes
	ipv4_hdr->packet_id = rte_cpu_to_be_16(5462);   // set random
	ipv4_hdr->fragment_offset = rte_cpu_to_be_16(0);
	ipv4_hdr->time_to_live = 64;
	ipv4_hdr->next_proto_id = IPPROTO_UDP;
	ipv4_hdr->src_addr = rte_cpu_to_be_32(SRC_IP);
	ipv4_hdr->dst_addr = rte_cpu_to_be_32(DST_IP);
	ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);

    /*  UDP  */
	udp_hdr = (struct rte_udp_hdr *)rte_pktmbuf_append(mbuf, sizeof(struct rte_udp_hdr));
    udp_hdr->src_port = rte_cpu_to_be_16(SRC_PORT);
	udp_hdr->dst_port = rte_cpu_to_be_16(DST_PORT + repath_id);
	udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + sizeof(struct hopa_cp_hdr));

    /*  HOPA CP  */
    hopa_cp_hdr = (struct hopa_cp_hdr *)rte_pktmbuf_append(mbuf, sizeof(struct hopa_cp_hdr));
    hopa_cp_hdr->flag = HOPA_CP;
	hopa_cp_hdr->cp_flag = REPATH;
    hopa_cp_hdr->probe_path_id = 65535; // 非探测报文
    hopa_cp_hdr->repath_id = repath_id;
    hopa_cp_hdr->rsvd = 0;
    hopa_cp_hdr->seq = 0;
	hopa_cp_hdr->ack = 0;
	hopa_cp_hdr->ts = 0;

    VLOG_INFO("repath_pkt");

	return mbuf;
}

static void hopa_cp_probe_pkt_progress(struct hopa_cp_hdr *hopa_cp_hdr)
{
    uint64_t sender_ts;
	uint64_t receiver_ts;
    uint8_t path_id = hopa_cp_hdr->probe_path_id;
    sender_ts = rte_be_to_cpu_64(hopa_cp_hdr->ts);
    struct timespec ts;
    clock_gettime(0, &ts);
    receiver_ts = (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;

	all_paths_delay_list[path_id] = 1000000000 + receiver_ts - sender_ts;

    VLOG_INFO("path id : [%d] , receiver_ts : [%" PRIu64 "], sender_ts : [%" PRIu64 "], delay : [%" PRIu64 "]", path_id, receiver_ts, sender_ts, all_paths_delay_list[path_id]);

    best_path_id = get_min_delay_path_id(all_paths_delay_list, PATH_NB);

	VLOG_INFO("opt_path_id : [%d]", best_path_id);
}

static void hopa_cp_repath_pkt_progress(struct hopa_cp_hdr *hopa_cp_hdr)
{

    best_path_id = hopa_cp_hdr->repath_id;
    // 1、触发换路(通知数据面 DP)  TODO

    // 2、回复repath_ack
	/*
    struct rte_mbuf *repath_ack_mbuf;
	repath_ack_mbuf = encode_repath_ack_pkt();
	rte_ring_mp_enqueue_burst(m_hopa_cp_in_out_ring->hopa_cp_out_ring, (void **)&repath_ack_mbuf, 1, NULL);
    */
	// 3、启动定时器  TODO   发端初始化ACK定时器
	// rte_timer_reset(&retran_timer, rte_get_timer_hz() * 2, SINGLE, rte_lcore_id(), timer_cb, NULL);
}

static void hopa_cp_repath_ack_pkt_progress(struct hopa_cp_hdr *hopa_cp_hdr)
{
	// TODO   收到ack, 确认上一个repath已经收到, 停止定时器
	// rte_timer_stop(&retran_timer);
}

static void hopa_dp_ts_pkt_progress(struct hopa_dp_hdr *hopa_dp_hdr)
{
	bool is_repath = 1;
	if (is_repath)
	{
		struct rte_mbuf *repath_mbuf;
		repath_mbuf = encode_repath_pkt((uint8_t)best_path_id);
		rte_ring_mp_enqueue_burst(m_hopa_cp_in_out_ring->hopa_cp_out_ring, (void **)&repath_mbuf, 1, NULL);
	}
}

static uint8_t get_min_delay_path_id(uint64_t *all_paths_delay_list, uint8_t length)
{
	uint8_t path_id = 0;

	for (uint8_t i = 1; i < length; i++)
		if (all_paths_delay_list[i] < all_paths_delay_list[path_id])
			path_id = i;

	return path_id;
}