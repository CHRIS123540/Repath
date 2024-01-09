/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2015 Intel Corporation
 */

#include <stdint.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <unistd.h>
#include <stdlib.h> 

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

#define IPV4_ADDR(a, b, c, d)(((a & 0xff) << 24) | ((b & 0xff) << 16) | \
		((c & 0xff) << 8) | (d & 0xff))

#define PRINT_IP_ADDR(ip_addr) printf("IP: %d.%d.%d.%d\n", \
    (int)((ip_addr) >> 24) & 0xFF, \
    (int)((ip_addr) >> 16) & 0xFF, \
    (int)((ip_addr) >> 8) & 0xFF, \
    (int)(ip_addr) & 0xFF)

static const struct rte_eth_conf port_conf_default = {
	.rxmode = {
		.max_rx_pkt_len = RTE_ETHER_MAX_LEN,
	},
};

/* basicfwd.c: Basic DPDK skeleton forwarding example. */

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */


struct rte_mempool *mbuf_pool;

static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf = port_conf_default;
	const uint16_t rx_rings = 1, tx_rings = 2;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf txconf;

	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	retval = rte_eth_dev_info_get(port, &dev_info);
	if (retval != 0) {
		printf("Error during getting device (port %u) info: %s\n",
				port, strerror(-retval));
		return retval;
	}

	if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |=
			DEV_TX_OFFLOAD_MBUF_FAST_FREE;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
				rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	txconf = dev_info.default_txconf;
	txconf.offloads = port_conf.txmode.offloads;
	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
				rte_eth_dev_socket_id(port), &txconf);
		if (retval < 0)
			return retval;
	}

	/* Start the Ethernet port. */
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	struct rte_ether_addr addr;
	retval = rte_eth_macaddr_get(port, &addr);
	if (retval != 0)
		return retval;

	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			port,
			addr.addr_bytes[0], addr.addr_bytes[1],
			addr.addr_bytes[2], addr.addr_bytes[3],
			addr.addr_bytes[4], addr.addr_bytes[5]);

	/* Enable RX in promiscuous mode for the Ethernet device. */
	retval = rte_eth_promiscuous_enable(port);
	if (retval != 0)
		return retval;

	return 0;
}


static void
fill_ethernet_header(struct rte_ether_hdr *hdr)
{
	struct rte_ether_addr s_addr = {{0x08, 0xc0, 0xeb, 0xbf, 0xef, 0x9a}};
	struct rte_ether_addr d_addr = {{0x02, 0xb1, 0x04, 0x57, 0x76, 0x77}};
	//08:c0:eb:bf:ef:9a
	//02:b1:04:57:76:77
	hdr->s_addr = s_addr;
	hdr->d_addr = d_addr;
	hdr->ether_type = rte_cpu_to_be_16(0x0800);
}

static void
fill_ipv4_header(struct rte_ipv4_hdr *hdr)
{
	hdr->version_ihl = (4 << 4) + 5;		  // ipv4, length 5 (*4)
	hdr->type_of_service = 0;				  // No Diffserv
	hdr->total_length = rte_cpu_to_be_16(40); //
	hdr->packet_id = rte_cpu_to_be_16(5462);  // set random
	hdr->fragment_offset = rte_cpu_to_be_16(0);
	hdr->time_to_live = 64;
	hdr->next_proto_id = 17;
	hdr->hdr_checksum = rte_cpu_to_be_16(25295);
	hdr->src_addr = rte_cpu_to_be_32(IPV4_ADDR(192, 168, 200, 2)); // 192.168.200.2
	hdr->dst_addr = rte_cpu_to_be_32(IPV4_ADDR(192, 168, 200, 24)); // 192.168.200.24
}

static void
fill_udp_header(struct rte_udp_hdr *hdr)
{
	hdr->src_port = rte_cpu_to_be_16(1234);
	hdr->dst_port = rte_cpu_to_be_16(5678);
	hdr->dgram_len = rte_cpu_to_be_16(20);
	hdr->dgram_cksum = rte_cpu_to_be_16(0xffff);
}



static int
lcore_probe(__rte_unused void *arg){

while(1)
{
	unsigned  i;
	struct rte_eth_dev_tx_buffer *buffer;
	struct rte_mbuf *m;

	struct rte_ether_hdr *ether_h;
	struct rte_ipv4_hdr *ipv4_h;
	struct rte_udp_hdr *udp_h;

	for (i = 0; i < 4; i++)
	{
		m = rte_pktmbuf_alloc(mbuf_pool);

		ether_h = (struct rte_ether_hdr *)rte_pktmbuf_append(m, sizeof(struct rte_ether_hdr));
		fill_ethernet_header(ether_h);

		ipv4_h = (struct rte_ipv4_hdr *)rte_pktmbuf_append(m, sizeof(struct rte_ipv4_hdr));
		fill_ipv4_header(ipv4_h);
		//PRINT_IP_ADDR(rte_be_to_cpu_32(ipv4_h->src_addr));
		udp_h = (struct rte_udp_hdr *)rte_pktmbuf_append(m, sizeof(struct rte_udp_hdr));
		fill_udp_header(udp_h);
		udp_h->dst_port = rte_cpu_to_be_16(5678+i);
		rte_eth_tx_burst(0, 1, &m, 1);//p0
		sleep(1);
		rte_pktmbuf_free(m);

	}
	
}
}
/*
 * The lcore main. This is the main thread that does the work, reading from
 * an input port and writing to an output port.
 */
static int
lcore_main(__rte_unused void *arg)
{
	uint16_t port;
    struct rte_ipv4_hdr *ipv4;
	
	/*
	 * Check that the port is on the same NUMA node as the polling thread
	 * for best performance.
	 */
	RTE_ETH_FOREACH_DEV(port)
		if (rte_eth_dev_socket_id(port) >= 0 &&
				rte_eth_dev_socket_id(port) !=
						(int)rte_socket_id())
			printf("WARNING, port %u is on remote NUMA node to "
					"polling thread.\n\tPerformance will "
					"not be optimal.\n", port);

	printf("\nCore %u forwarding packets. [Ctrl+C to quit]\n",
			rte_lcore_id());

	/* Run until the application is quit or killed. */
	for (;;) {
		/*
		 * Receive packets on a port and forward them on the paired
		 * port. The mapping is 0 -> 1, 1 -> 0, 2 -> 3, 3 -> 2, etc.
		 */
		
		
		for(port = 0;port < 2;port++) {

			/* Get burst of RX packets, from first port of pair. */
			struct rte_mbuf *bufs[BURST_SIZE];
			const uint16_t nb_rx = rte_eth_rx_burst(port, 0,
					bufs, BURST_SIZE);

			uint16_t buf;
			for (buf = 0; buf < nb_rx; buf++){

				ipv4 = (struct rte_ipv4_hdr *)(rte_pktmbuf_mtod(bufs[buf], struct rte_ether_hdr *) + 1);
				
					//printf("lenth %d\n",bufs[buf]->pkt_len);
					//PRINT_IP_ADDR(rte_be_to_cpu_32(ipv4->src_addr));
					//PRINT_IP_ADDR(rte_be_to_cpu_32(ipv4->dst_addr));		
				}


			/* Send burst of TX packets, to second port of pair. */
			const uint16_t nb_tx = rte_eth_tx_burst(port ^ 1, 0,
					bufs, nb_rx);

			/* Free any unsent packets. */
			if (unlikely(nb_tx < nb_rx)) {
				uint16_t buf;
				for (buf = nb_tx; buf < nb_rx; buf++)
					rte_pktmbuf_free(bufs[buf]);
			}
		}
		
	}
}

/*
 * The main function, which does initialization and calls the per-lcore
 * functions.
 */
int
main(int argc, char *argv[])
{
	
	unsigned nb_ports;
	uint16_t portid;

	/* Initialize the Environment Abstraction Layer (EAL). */
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	argc -= ret;
	argv += ret;

	/* Check that there is an even number of ports to send/receive on. */
	nb_ports = rte_eth_dev_count_avail();
	printf("NUM PORT %d\n",nb_ports);
	if (nb_ports < 2 || (nb_ports & 1))
	//	rte_exit(EXIT_FAILURE, "Error: number of ports must be even\n");

	/* Creates a new mempool in memory to hold the mbufs. */
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	/* Initialize all ports. */
	RTE_ETH_FOREACH_DEV(portid)
		if (port_init(portid, mbuf_pool) != 0)
			rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu16 "\n",
					portid);

	printf("core %d\n",rte_lcore_count());
	if (rte_lcore_count() > 1)
		printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");

	/* Call lcore_main on the main core only. */
	rte_eal_remote_launch(lcore_probe, NULL, 2);	
	rte_eal_remote_launch(lcore_main, NULL, 1);
	while(1)
	{
		sleep(1);
	}
	/* clean up the EAL */
	rte_eal_cleanup();

	return 0;
}
