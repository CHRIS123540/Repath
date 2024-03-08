#include <stdint.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <unistd.h>
#include <stdlib.h> 
#include <time.h>

#define DEF_SENDER 1

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

/* Number of paths */
#define PATH_NB (4)

#define IPV4_ADDR(a, b, c, d) (((a & 0xff) << 24) | ((b & 0xff) << 16) | ((c & 0xff) << 8) | (d & 0xff))

#define PRINT_IP_ADDR(ip_addr) printf("IP: %d.%d.%d.%d\n", \
    (int)((ip_addr) >> 24) & 0xFF, \
    (int)((ip_addr) >> 16) & 0xFF, \
    (int)((ip_addr) >> 8) & 0xFF, \
    (int)(ip_addr) & 0xFF)

/* MAC addr */
#define SRC_MAC {0x08, 0xc0, 0xeb, 0xbf, 0xef, 0x9a}
#define DST_MAC {0x08, 0xc0, 0xeb, 0xbf, 0xef, 0x82}

/* IP addr */
#define SRC_IP IPV4_ADDR(192, 168, 200, 2)
#define DST_IP IPV4_ADDR(192, 168, 200, 1)

/* UDP port and path */
#define SRC_PORT 1234
#define DST_PORT_PATH_1 5678

/* P0 port id */
#define PORT_P0 0

/* Mempool */
struct rte_mempool *mbuf_pool;

/* timestamp */
struct timespec ts;
uint64_t sender_ts;
uint64_t receiver_ts;

/* HOPA parameters */
struct hopa_param
{
    int is_sender; /* 1 -> sender. 0 -> receiver. */
};

/* 4 paths delta time */
struct path_delta_time{
    uint64_t sender_ts[PATH_NB];
    uint64_t receiver_ts[PATH_NB];
    uint64_t delta_time_ts[PATH_NB];
};

struct path_delta_time *path_delta_time;

/* HOPA CP Header */
struct hopa_cp_hdr
{
	uint8_t flag; /**< HOPA flag. 0 -> control plane . 1 -> data plane */
    uint8_t tlv; /**< unknow */
    rte_be64_t ts; /**< timestamp */
    uint8_t rsvd; /**< reserved field */
};

/* HOPA DP Header */
struct hopa_dp_hdr
{
	uint8_t flag; /**< HOPA flag. 0 -> control plane . 1 -> data plane */
    rte_be64_t ts; /**< timestamp */
    uint8_t rsvd; /**< reserved field */
    rte_be32_t seq_nb; /**< timestamp */
    uint8_t seg_end; /**< reserved field */
};

/* Function definition */
static void parse_args(struct hopa_param *user_param, int argc, char *argv[]);
static void usage();
static void print_hopa_param(struct hopa_param *user_param);
static inline int port_init(uint16_t port, struct rte_mempool *mbuf_pool);

/* encode packet */
static void fill_eth_header(struct rte_ether_hdr *eth_hdr);
static void fill_ipv4_header(struct rte_ipv4_hdr *ipv4_hdr);
static void fill_udp_header(struct rte_ipv4_hdr *ipv4_hdr, struct rte_udp_hdr *udp_hdr, uint8_t path_id);
static void fill_hopa_header(struct hopa_cp_hdr *hopa_hdr);

/* lcore funcation */
static int lcore_probe(__rte_unused void *arg);
static int lcore_stats(__rte_unused void *arg);