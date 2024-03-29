#include <stdint.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_ring.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_timer.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

#define IPV4_ADDR(a, b, c, d) (((a & 0xff) << 24) | ((b & 0xff) << 16) | ((c & 0xff) << 8) | (d & 0xff))

#define PRINT_IP_ADDR(ip_addr) printf("IP: %d.%d.%d.%d\n",           \
                                      (int)((ip_addr) >> 24) & 0xFF, \
                                      (int)((ip_addr) >> 16) & 0xFF, \
                                      (int)((ip_addr) >> 8) & 0xFF,  \
                                      (int)(ip_addr) & 0xFF)

/* DPDK param */

#define RX_RING_SIZE (1024)
#define TX_RING_SIZE (1024)
#define NUM_MBUFS (8191)
#define MBUF_CACHE_SIZE (250)
#define BURST_SIZE (32)

/* HOPA param */

#define DEF_SENDER (1)

#define PROBE_GAP (300)

/* MAC addr */
#define SRC_MAC                            \
    {                                      \
        0x08, 0xc0, 0xeb, 0xbf, 0xef, 0x9a \
    }
#define DST_MAC                            \
    {                                      \
        0x08, 0xc0, 0xeb, 0xbf, 0xef, 0x82 \
    }

/* IP addr */
#define SRC_IP IPV4_ADDR(192, 168, 200, 2)
#define DST_IP IPV4_ADDR(192, 168, 200, 1)

/* Number of paths */
#define PATH_NB (4)

/* UDP port and path */
#define SRC_PORT (1234)
#define DST_PORT_PATH_1 (5678)

/* P0 port id */
#define PORT_P0 (0)

/* 预设比例 r */
#define R (0.7)

enum hopa_module
{
    HOPA_CP,
    HOPA_DP
};

enum hopa_cp_flag
{
    PROBE,
    REPATH,
    REPATH_ACK
};

struct hopa_in_out_ring
{
    struct rte_ring *hopa_in_ring;
    struct rte_ring *hopa_out_ring;
};

/* HOPA cli parameters */
struct hopa_param
{
    int is_sender; /* 1 -> sender. 0 -> receiver. */
};

/* current path */
struct cur_path_info
{
    uint8_t path_id;

    uint64_t max_dt;
    uint64_t min_dt;

    uint64_t last_3_dt_grad;
    uint64_t last_2_dt_grad;
    uint64_t last_1_dt_grad;
    uint64_t cur_dt_grad;

    uint64_t last_3_dt;
    uint64_t last_2_dt;
    uint64_t last_1_dt;
    uint64_t cur_dt;

    uint64_t cur_ts;
    uint64_t last_ts;

    uint64_t thres_dt;

    uint64_t delta_t;
};

/* HOPA CP Header */
struct hopa_cp_hdr
{
    uint8_t flag;      /**< HOPA flag. 0 -> control plane . 1 -> data plane */
    uint8_t cp_flag;   /**< CP flag. 0 -> perbe. 1 -> repath. 2 -> repath_ack. */
    rte_be64_t ts;     /**< timestamp */
    uint8_t repath_id; /**< repath id */
    rte_be64_t seq;
    rte_be64_t ack;
    uint8_t rsvd; /**< reserved field */
};

/* HOPA DP Header */
struct hopa_dp_hdr
{
    uint8_t flag;      /**< HOPA flag. 0 -> control plane . 1 -> data plane */
    rte_be64_t ts;     /**< timestamp */
    uint8_t rsvd;      /**< reserved field */
    rte_be32_t seq_nb; /**< timestamp */
    uint8_t seg_end;   /**< reserved field */
};

/* Function definition */
/* init */
static void parse_args(struct hopa_param *user_param, int argc, char *argv[]);
static void usage();
static void print_hopa_param(struct hopa_param *user_param);
static inline int port_init(uint16_t port, struct rte_mempool *mbuf_pool);

/* encode packet */
static void fill_eth_header(struct rte_ether_hdr *eth_hdr);
static void fill_ipv4_header(struct rte_ipv4_hdr *ipv4_hdr);
static void fill_udp_header(struct rte_ipv4_hdr *ipv4_hdr, struct rte_udp_hdr *udp_hdr, uint16_t dst_port);
static struct rte_mbuf *encode_udp_pkt(uint16_t dst_port);
static struct rte_mbuf *encode_probe_pkt(uint8_t path_id);
static struct rte_mbuf *encode_repath_pkt(uint8_t repath_id);
static struct rte_mbuf *encode_repath_ack_pkt();

/* packet progress */
static void hopa_cp_probe_pkt_progress(struct rte_mbuf *hopa_cp_mbuf);
static void hopa_cp_repath_pkt_progress(struct rte_mbuf *hopa_cp_mbuf);
static void hopa_cp_repath_ack_pkt_progress(struct rte_mbuf *hopa_cp_mbuf);
static void hopa_dp_ts_pkt_progress(struct rte_mbuf *hopa_cp_mbuf);

/*  */
static bool one_path_check();

static uint8_t get_min_delay_path_id(uint64_t *all_paths_delay_list, int length);

/* timer */
static void timer_cb(__rte_unused struct rte_timer *timer, __rte_unused void *arg);

/* lcore funcation */
static int lcore_probe(__rte_unused void *arg);
static int lcore_stats(__rte_unused void *arg);