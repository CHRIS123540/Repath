#include "hopa_cp.h"

static const struct rte_eth_conf port_conf_default = {
	.rxmode = {
		.max_rx_pkt_len = RTE_ETHER_MAX_LEN,
	},
};

static void parse_args(struct hopa_param *user_param, int argc, char *argv[]) {
	for (int i = 1; i < argc; ++i) {
        if (strlen(argv[i]) == 2 && strcmp(argv[i], "-s") == 0) {
            if (i+1 < argc){
                user_param->is_sender = strtoull(argv[i+1], NULL, 10);
                i++;
            }
            else{
                usage();
                exit(EXIT_FAILURE);
            }
        }
		else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-h") == 0){
            usage();
            exit(EXIT_SUCCESS);
		}
    	else{
			printf("invalid option: %s\n", argv[i]);
			usage();
			exit(EXIT_FAILURE);
    	}
	}
}

static void usage(){
	printf("Options:\n");
    printf(" -h <help>            Help information\n");
    printf(" -s <sender>          Sender is 1, and Receiver is 0. (default %d)\n", DEF_SENDER);
}

static void print_hopa_param(struct hopa_param *user_param){
	printf("-s is :        %d \n", user_param->is_sender);
}

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
		printf("Error during getting device (port %u) info: %s\n", port, strerror(-retval));
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

	printf("Used Port %u MAC: %02"PRIx8":%02"PRIx8":%02"PRIx8":%02"PRIx8":%02"PRIx8":%02"PRIx8"\n",
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
fill_eth_header(struct rte_ether_hdr *eth_hdr)
{
	struct rte_ether_addr s_addr = {SRC_MAC};
	struct rte_ether_addr d_addr = {DST_MAC};

	eth_hdr->s_addr = s_addr;
	eth_hdr->d_addr = d_addr;
	eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
}

static void
fill_ipv4_header(struct rte_ipv4_hdr *ipv4_hdr)
{
	ipv4_hdr->version_ihl = (4 << 4) + 5;			// ipv4 version , length 5 (*4 bytes)
	ipv4_hdr->type_of_service = 0;			// No Diffserv
	ipv4_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(struct hopa_cp_hdr)); // IP 52 bytes
	ipv4_hdr->packet_id = rte_cpu_to_be_16(5462); // set random
	ipv4_hdr->fragment_offset = rte_cpu_to_be_16(0);
	ipv4_hdr->time_to_live = 64;
	ipv4_hdr->next_proto_id = IPPROTO_UDP;
	ipv4_hdr->src_addr = rte_cpu_to_be_32(SRC_IP);
	ipv4_hdr->dst_addr = rte_cpu_to_be_32(DST_IP);
	ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);
}

static void
fill_udp_header(struct rte_ipv4_hdr *ipv4_hdr, struct rte_udp_hdr *udp_hdr, uint8_t path_id)
{
	udp_hdr->src_port = rte_cpu_to_be_16(SRC_PORT);
	udp_hdr->dst_port = rte_cpu_to_be_16(DST_PORT_PATH_1 - 1 + path_id);
	udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + sizeof(struct hopa_cp_hdr));
	udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, udp_hdr);
}

static void
fill_hopa_header(struct hopa_cp_hdr *hopa_cp_hdr)
{
	hopa_cp_hdr->flag = 0;
	hopa_cp_hdr->tlv = 0;

	if(clock_gettime(0, &ts) == 0) {
        sender_ts = (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
        printf("current ts (ns) : %" PRIu64 "\n", sender_ts);
    } else {
        rte_exit(EXIT_FAILURE, "Error with clock_gettime\n");
    }
	hopa_cp_hdr->ts = rte_cpu_to_be_64(sender_ts);

	hopa_cp_hdr->rsvd = 0;
}

static int
lcore_probe(__rte_unused void *arg){

	unsigned  i;
	struct rte_eth_dev_tx_buffer *buffer;
	struct rte_mbuf *mbufs[PATH_NB];

	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *udp_hdr;
	struct hopa_cp_hdr *hopa_cp_hdr;

	while(1){
		
		for (i = 0; i < PATH_NB; i++)
		{
			mbufs[i] = rte_pktmbuf_alloc(mbuf_pool);

			if(mbufs[i] == NULL) rte_exit(EXIT_FAILURE, "Error with rte_pktmbuf_alloc()\n");

			eth_hdr = (struct rte_ether_hdr *)rte_pktmbuf_append(mbufs[i], sizeof(struct rte_ether_hdr));
			fill_eth_header(eth_hdr);

			ipv4_hdr = (struct rte_ipv4_hdr *)rte_pktmbuf_append(mbufs[i], sizeof(struct rte_ipv4_hdr));
			fill_ipv4_header(ipv4_hdr);

			udp_hdr = (struct rte_udp_hdr *)rte_pktmbuf_append(mbufs[i], sizeof(struct rte_udp_hdr));
			fill_udp_header(ipv4_hdr, udp_hdr, i + 1);

			hopa_cp_hdr = (struct hopa_cp_hdr *)rte_pktmbuf_append(mbufs[i], sizeof(struct hopa_cp_hdr));
			fill_hopa_header(hopa_cp_hdr);
		}

		rte_eth_tx_burst(PORT_P0, 1, mbufs, PATH_NB);

		for (i = 0; i < PATH_NB; i++){
			rte_pktmbuf_free(mbufs[i]);
		}
		
		sleep(1);
	}

}

static int
lcore_stats(__rte_unused void *arg){

	struct rte_mbuf *bufs[BURST_SIZE];
	uint16_t nb_rx;
	uint16_t i;

	path_delta_time = (struct path_delta_time *)malloc(sizeof(struct path_delta_time));

	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *udp_hdr;
	struct hopa_cp_hdr *hopa_cp_hdr;

	while (1)
	{
		nb_rx = rte_eth_rx_burst(PORT_P0, 0, bufs, BURST_SIZE);

		for (i = 0; i < nb_rx; i++){
			eth_hdr = rte_pktmbuf_mtod_offset(bufs[i], struct rte_ether_hdr *, 0);
			// ipv4_hdr = (struct rte_ipv4_hdr *)(rte_pktmbuf_mtod(bufs[i], struct rte_ether_hdr *) + 1);
			ipv4_hdr = rte_pktmbuf_mtod_offset(bufs[i], struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
			if (ipv4_hdr->next_proto_id == IPPROTO_UDP){
				udp_hdr = (struct rte_udp_hdr *)(ipv4_hdr + 1);

				if(rte_be_to_cpu_16(udp_hdr->src_port) == SRC_PORT){ // HOPA
					hopa_cp_hdr = (struct hopa_cp_hdr *)(udp_hdr + 1);

					if(hopa_cp_hdr->flag == 0){ // HOPA CP
						int path_id = rte_be_to_cpu_16(udp_hdr->dst_port) - DST_PORT_PATH_1;

						sender_ts = rte_be_to_cpu_64(hopa_cp_hdr->ts);
						printf("sender ts (ns) : %" PRIu64 "\n", sender_ts);

						if(clock_gettime(0, &ts) == 0) {
							receiver_ts = (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
							printf("receiver ts (ns) : %" PRIu64 "\n", receiver_ts);
						} else {
							rte_exit(EXIT_FAILURE, "Error with clock_gettime\n");
						}

						if(receiver_ts > sender_ts)
							path_delta_time->delta_time_ts[path_id] = receiver_ts - sender_ts;
						else
							path_delta_time->delta_time_ts[path_id] = sender_ts - receiver_ts;
						
						printf("path id : %d , sub (ns) : %f\n", path_id + 1, path_delta_time->delta_time_ts[path_id]);
					}
				}
			}
		}

		for (i = 0; i < nb_rx; i++)
			rte_pktmbuf_free(bufs[i]);
	}
}

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

	struct hopa_param hopa_param = {0};
	parse_args(&hopa_param, argc, argv);
	print_hopa_param(&hopa_param);

	/* Check that there is an even number of ports to send/receive on. */
	nb_ports = rte_eth_dev_count_avail();
	printf("NUM PORT %d\n",nb_ports);

	nb_ports = 1; // one port (p0)

	/* Creates a new mempool in memory to hold the mbufs. */
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	/* Initialize all ports. */
	RTE_ETH_FOREACH_DEV(portid)
		if (port_init(portid, mbuf_pool) != 0)
			rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu16 "\n", portid);

	// printf("core %d\n",rte_lcore_count());

	/* Call lcore_main on the main core only. */
	if(hopa_param.is_sender){
		printf("-----------------sender-----------------\n");
		rte_eal_remote_launch(lcore_probe, NULL, 1);
	}
	else{
		printf("-----------------receiver-----------------\n");
		rte_eal_remote_launch(lcore_stats, NULL, 1);
	}
	
	while(1)
	{
		sleep(1);
	}
	/* clean up the EAL */
	rte_eal_cleanup();

	return 0;
}
