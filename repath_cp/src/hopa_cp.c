#include "hopa_cp.h"
#include "hopa_log.h"

struct rte_mempool *mbuf_pool = NULL;
struct hopa_in_out_ring *hopa_in_out_ring_ins = NULL;
uint64_t all_paths_delay_list[PATH_NB] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
struct cur_path_info *cur_path_info;
uint8_t opt_path_id = 0;
struct rte_timer retran_timer;

static struct hopa_in_out_ring *get_ring_instance(void)
{
	if (hopa_in_out_ring_ins == NULL)
	{
		hopa_in_out_ring_ins = rte_malloc("in_out ring", sizeof(struct hopa_in_out_ring), 0);
		memset(hopa_in_out_ring_ins, 0, sizeof(struct hopa_in_out_ring));
	}

	return hopa_in_out_ring_ins;
}

static void parse_args(struct hopa_param *user_param, int argc, char *argv[])
{
	for (int i = 1; i < argc; ++i)
	{
		if (strlen(argv[i]) == 2 && strcmp(argv[i], "-s") == 0)
		{
			if (i + 1 < argc)
			{
				user_param->is_sender = strtoull(argv[i + 1], NULL, 10);
				i++;
			}
			else
			{
				usage();
				exit(EXIT_FAILURE);
			}
		}
		else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-h") == 0)
		{
			usage();
			exit(EXIT_SUCCESS);
		}
		else
		{
			printf("invalid option: %s\n", argv[i]);
			usage();
			exit(EXIT_FAILURE);
		}
	}
}

static void usage()
{
	printf("Options:\n");
	printf(" -h <help>            Help information\n");
	printf(" -s <sender>          Sender is 1, and Receiver is 0. (default %d)\n", DEF_SENDER);
}

static void print_hopa_param(struct hopa_param *user_param)
{
	printf("-s is :        %d \n", user_param->is_sender);
}

static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf = {
		.rxmode = {
			.max_rx_pkt_len = RTE_ETHER_MAX_LEN,
		},
	};
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
	if (retval != 0)
	{
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
	for (q = 0; q < rx_rings; q++)
	{
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
										rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	txconf = dev_info.default_txconf;
	txconf.offloads = port_conf.txmode.offloads;
	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++)
	{
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

	printf("Used Port %u MAC: %02" PRIx8 ":%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8 "\n",
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
	eth_hdr->s_addr = (struct rte_ether_addr){SRC_MAC};
	eth_hdr->d_addr = (struct rte_ether_addr){DST_MAC};
	eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
}

static void
fill_ipv4_header(struct rte_ipv4_hdr *ipv4_hdr)
{
	ipv4_hdr->version_ihl = (4 << 4) + 5;																							  // ipv4 version , length 5 (*4 bytes)
	ipv4_hdr->type_of_service = 0;																									  // No Diffserv
	ipv4_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(struct hopa_cp_hdr)); // IP 52 bytes
	ipv4_hdr->packet_id = rte_cpu_to_be_16(5462);																					  // set random
	ipv4_hdr->fragment_offset = rte_cpu_to_be_16(0);
	ipv4_hdr->time_to_live = 64;
	ipv4_hdr->next_proto_id = IPPROTO_UDP;
	ipv4_hdr->src_addr = rte_cpu_to_be_32(SRC_IP);
	ipv4_hdr->dst_addr = rte_cpu_to_be_32(DST_IP);
	ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);
}

static void
fill_udp_header(struct rte_ipv4_hdr *ipv4_hdr, struct rte_udp_hdr *udp_hdr, uint16_t dst_port)
{
	udp_hdr->src_port = rte_cpu_to_be_16(SRC_PORT);
	udp_hdr->dst_port = rte_cpu_to_be_16(dst_port);
	udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + sizeof(struct hopa_cp_hdr));
	udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, udp_hdr);
}

static struct rte_mbuf *encode_udp_pkt(uint16_t dst_port)
{
	struct rte_mbuf *mbuf;
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *udp_hdr;

	if (!mbuf_pool)
	{
		printf("mbuf_pool null\n");
		return NULL;
	}
	mbuf = rte_pktmbuf_alloc(mbuf_pool);
	if (!mbuf)
		rte_exit(EXIT_FAILURE, "Error with rte_pktmbuf_alloc()\n");

	eth_hdr = (struct rte_ether_hdr *)rte_pktmbuf_append(mbuf, sizeof(struct rte_ether_hdr));
	fill_eth_header(eth_hdr);

	ipv4_hdr = (struct rte_ipv4_hdr *)rte_pktmbuf_append(mbuf, sizeof(struct rte_ipv4_hdr));
	fill_ipv4_header(ipv4_hdr);

	udp_hdr = (struct rte_udp_hdr *)rte_pktmbuf_append(mbuf, sizeof(struct rte_udp_hdr));
	fill_udp_header(ipv4_hdr, udp_hdr, dst_port);

	return mbuf;
}

static struct rte_mbuf *encode_probe_pkt(uint8_t path_id)
{
	struct rte_mbuf *mbuf;
	struct hopa_cp_hdr *hopa_cp_hdr;
	uint64_t sender_ts;

	mbuf = encode_udp_pkt(DST_PORT_PATH_1 - 1 + path_id);

	hopa_cp_hdr = (struct hopa_cp_hdr *)rte_pktmbuf_append(mbuf, sizeof(struct hopa_cp_hdr));

	hopa_cp_hdr->flag = HOPA_CP;
	hopa_cp_hdr->cp_flag = PROBE;
	hopa_cp_hdr->rsvd = 0;
	hopa_cp_hdr->repath_id = 0;
	hopa_cp_hdr->seq = 0;
	hopa_cp_hdr->ack = 0;

	struct timespec ts;
	if (clock_gettime(0, &ts) == 0)
	{
		sender_ts = (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
		printf("current ts (ns) : %" PRIu64 "\n", sender_ts);
	}
	else
		rte_exit(EXIT_FAILURE, "Error with clock_gettime\n");
	hopa_cp_hdr->ts = rte_cpu_to_be_64(sender_ts);

	return mbuf;
}

static struct rte_mbuf *encode_repath_pkt(uint8_t repath_id)
{
	struct rte_mbuf *mbuf;
	struct hopa_cp_hdr *hopa_cp_hdr;

	mbuf = encode_udp_pkt(DST_PORT_PATH_1 - 1 + opt_path_id); // 暂定从最优路径发送

	hopa_cp_hdr = (struct hopa_cp_hdr *)rte_pktmbuf_append(mbuf, sizeof(struct hopa_cp_hdr));
	hopa_cp_hdr->flag = HOPA_CP;
	hopa_cp_hdr->cp_flag = REPATH;
	hopa_cp_hdr->rsvd = 0;
	hopa_cp_hdr->repath_id = repath_id;
	hopa_cp_hdr->seq = 0; // TODO
	hopa_cp_hdr->ack = 0;

	return mbuf;
}

static struct rte_mbuf *encode_repath_ack_pkt()
{
	struct rte_mbuf *mbuf;
	struct hopa_cp_hdr *hopa_cp_hdr;

	mbuf = encode_udp_pkt(DST_PORT_PATH_1 - 1 + opt_path_id); // 暂定从最优路径发送

	hopa_cp_hdr = (struct hopa_cp_hdr *)rte_pktmbuf_append(mbuf, sizeof(struct hopa_cp_hdr));
	hopa_cp_hdr->flag = HOPA_CP;
	hopa_cp_hdr->cp_flag = REPATH_ACK;
	hopa_cp_hdr->rsvd = 0;
	hopa_cp_hdr->repath_id = 0;
	hopa_cp_hdr->seq = 0;
	hopa_cp_hdr->ack = 0; // TODO

	return mbuf;
}

static void timer_cb(__rte_unused struct rte_timer *timer, __rte_unused void *arg)
{
	printf("timer out\n");
	rte_timer_reset(&retran_timer, rte_get_timer_hz() * 0.1, SINGLE, rte_lcore_id(), timer_cb, arg);
}

static void hopa_cp_probe_pkt_progress(struct rte_mbuf *hopa_cp_mbuf)
{
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *udp_hdr;
	struct hopa_cp_hdr *hopa_cp_hdr;
	uint64_t sender_ts;
	uint64_t receiver_ts;

	ipv4_hdr = rte_pktmbuf_mtod_offset(hopa_cp_mbuf, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
	udp_hdr = (struct rte_udp_hdr *)(ipv4_hdr + 1);
	hopa_cp_hdr = (struct hopa_cp_hdr *)(udp_hdr + 1);

	uint8_t path_id = rte_be_to_cpu_16(udp_hdr->dst_port) - DST_PORT_PATH_1;

	sender_ts = rte_be_to_cpu_64(hopa_cp_hdr->ts);

	struct timespec ts;
	if (clock_gettime(0, &ts) == 0)
	{
		receiver_ts = (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
	}
	else
		rte_exit(EXIT_FAILURE, "Error with clock_gettime\n");

	all_paths_delay_list[path_id] = receiver_ts - sender_ts + 1000000000;

	// double delay = ((double)all_paths_delay_list[path_id] / 1000.0) / 1000.0;

	HOPA_LOG_TRACE("path id : %d , delay (us) : %" PRIu64 "", path_id, all_paths_delay_list[path_id]);

	opt_path_id = get_min_delay_path_id(all_paths_delay_list, PATH_NB);

	HOPA_LOG_INFO("opt_path_id = %d", opt_path_id);
}

static void hopa_cp_repath_pkt_progress(struct rte_mbuf *hopa_cp_mbuf)
{
	// 1、触发换路(通知数据面 DP)  TODO

	// 2、回复repath_ack
	struct rte_mbuf *repath_ack_mbuf;
	repath_ack_mbuf = encode_repath_ack_pkt();
	rte_ring_mp_enqueue_burst(hopa_in_out_ring_ins->hopa_out_ring, (void **)&repath_ack_mbuf, 1, NULL);

	// 3、启动定时器  TODO   收端初始化定时器
	rte_timer_reset(&retran_timer, rte_get_timer_hz() * 2, SINGLE, rte_lcore_id(), timer_cb, NULL);
}

static void hopa_cp_repath_ack_pkt_progress(struct rte_mbuf *hopa_cp_mbuf)
{
	// TODO   收到ack, 确认上一个repath已经收到, 停止定时器
	rte_timer_stop(&retran_timer);
}

static void hopa_dp_ts_pkt_progress(struct rte_mbuf *hopa_cp_mbuf)
{
	uint8_t is_repath = one_path_check(1);
	if (is_repath)
	{
		struct rte_mbuf *repath_mbuf;
		repath_mbuf = encode_repath_pkt((uint8_t)opt_path_id);
		rte_ring_mp_enqueue_burst(hopa_in_out_ring_ins->hopa_out_ring, (void **)&repath_mbuf, 1, NULL);
	}
}

static bool one_path_check()
{
	// TODO   随路检测算法 --->  换路时机
	cur_path_info->delta_t = cur_path_info->cur_ts - cur_path_info->last_ts;

	/* 计算 max_dt    min_dt */
	if (cur_path_info->cur_dt > cur_path_info->max_dt)
		cur_path_info->max_dt = cur_path_info->cur_dt;
	if (cur_path_info->cur_dt < cur_path_info->min_dt)
		cur_path_info->min_dt = cur_path_info->cur_dt;

	/* 计算 绝对取值 */
	cur_path_info->thres_dt = cur_path_info->min_dt + R * (cur_path_info->max_dt - cur_path_info->min_dt);

	/* 计算 相对取值 */
	cur_path_info->cur_dt_grad = (cur_path_info->cur_dt - cur_path_info->last_1_dt) / cur_path_info->delta_t;

	bool b2 = 2 * cur_path_info->last_3_dt >= (cur_path_info->thres_dt - cur_path_info->min_dt);
	bool b3 = cur_path_info->last_1_dt_grad > 0 && cur_path_info->last_2_dt_grad > 0 && cur_path_info->last_3_dt_grad > 0;
	bool b4 = cur_path_info->cur_dt_grad >= (cur_path_info->thres_dt - cur_path_info->min_dt) / (2 * cur_path_info->delta_t);
	bool is_try_repath = (b2 && b3) || b4;

	bool is_force_repath = cur_path_info->cur_dt >= cur_path_info->thres_dt;

	return is_try_repath || is_force_repath;
}

static uint8_t get_min_delay_path_id(uint64_t *all_paths_delay_list, int length)
{
	uint8_t path_id = 0;

	for (int i = 1; i < length; i++)
	{
		if (all_paths_delay_list[i] < all_paths_delay_list[path_id])
		{
			path_id = i;
		}
	}

	return path_id;
}

static int
lcore_probe(__rte_unused void *arg)
{

	unsigned i;
	struct rte_mbuf *mbufs[PATH_NB];

	while (1)
	{
		for (i = 0; i < PATH_NB; i++)
			mbufs[i] = encode_probe_pkt(i + 1);

		rte_ring_mp_enqueue_burst(hopa_in_out_ring_ins->hopa_out_ring, (void **)&mbufs, PATH_NB, NULL);

		// usleep(PROBE_GAP * 1000);
		sleep(2); // for test
	}
}

static int
lcore_stats(__rte_unused void *arg)
{
	struct rte_mbuf *bufs[BURST_SIZE];
	uint16_t nb_rx;
	uint16_t i;

	/* timer init */
	rte_timer_subsystem_init();
	rte_timer_init(&retran_timer);
	srand(time(NULL));

	cur_path_info = (struct cur_path_info *)malloc(sizeof(struct cur_path_info));

	// struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *udp_hdr;
	struct hopa_cp_hdr *hopa_cp_hdr;

	uint64_t prev_tsc = 0, cur_tsc, diff_tsc;
	uint64_t timer_resolution_cycles = rte_get_timer_hz() * 10 / 1000;

	while (1)
	{
		// timer
		cur_tsc = rte_get_timer_cycles();
		diff_tsc = cur_tsc - prev_tsc;
		if (diff_tsc > timer_resolution_cycles)
		{
			rte_timer_manage();
			prev_tsc = cur_tsc;
		}

		// nb_rx = rte_eth_rx_burst(PORT_P0, 0, bufs, BURST_SIZE);
		nb_rx = rte_ring_mc_dequeue_burst(hopa_in_out_ring_ins->hopa_in_ring, (void **)bufs, BURST_SIZE, NULL);

		for (i = 0; i < nb_rx; i++)
		{
			// eth_hdr = rte_pktmbuf_mtod_offset(bufs[i], struct rte_ether_hdr *, 0);
			ipv4_hdr = rte_pktmbuf_mtod_offset(bufs[i], struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
			if (ipv4_hdr->next_proto_id == IPPROTO_UDP)
			{
				udp_hdr = (struct rte_udp_hdr *)(ipv4_hdr + 1);

				if (rte_be_to_cpu_16(udp_hdr->src_port) == SRC_PORT) // HOPA
				{
					hopa_cp_hdr = (struct hopa_cp_hdr *)(udp_hdr + 1);

					if (hopa_cp_hdr->flag == HOPA_CP)
					{
						switch (hopa_cp_hdr->cp_flag)
						{
						case PROBE:
							hopa_cp_probe_pkt_progress(bufs[i]);
							break;
						case REPATH:
							hopa_cp_repath_pkt_progress(bufs[i]);
							break;
						case REPATH_ACK:
							hopa_cp_repath_ack_pkt_progress(bufs[i]);
							break;

						default:
							printf("hopa_cp_flag unknow error.\n");
							break;
						}
					}
					else if (hopa_cp_hdr->flag == HOPA_DP)
						hopa_dp_ts_pkt_progress(bufs[i]);
				}
			}
		}

		for (i = 0; i < nb_rx; i++)
			rte_pktmbuf_free(bufs[i]);
	}
}

int main(int argc, char *argv[])
{
	struct hopa_in_out_ring *m_hopa_in_out_ring;
	struct rte_mbuf *recv_mbuf[BURST_SIZE] = {NULL};
	struct rte_mbuf *send_mbuf[BURST_SIZE] = {NULL};

	unsigned nb_ports;
	uint16_t portid;

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
	printf("NUM PORT %d\n", nb_ports);

	nb_ports = 1; // one port (p0) !!!

	/* Creates a new mempool in memory to hold the mbufs. */
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
										MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	/* Initialize P0 port. */
	if (port_init(PORT_P0, mbuf_pool) != 0)
		rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu16 "\n", portid);

	/* ring buf */
	m_hopa_in_out_ring = get_ring_instance();
	if (m_hopa_in_out_ring == NULL)
		rte_exit(EXIT_FAILURE, "ring buffer init failed\n");

	m_hopa_in_out_ring->hopa_in_ring = rte_ring_create("in ring", RX_RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
	m_hopa_in_out_ring->hopa_out_ring = rte_ring_create("out ring", TX_RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);

	/* TODO */
	if (hopa_param.is_sender)
	{
		printf("-----------------sender-----------------\n");
		rte_eal_remote_launch(lcore_probe, NULL, 1);
	}
	else
	{
		printf("-----------------receiver-----------------\n");
		rte_eal_remote_launch(lcore_stats, NULL, 1);
	}

	int rx_num;
	int total_num;
	int offset;
	int tx_num;

	while (1)
	{
		rte_timer_manage();
		// rx
		rx_num = rte_eth_rx_burst(PORT_P0, 0, recv_mbuf, BURST_SIZE);
		if (rx_num > 0)
			rte_ring_sp_enqueue_burst(m_hopa_in_out_ring->hopa_in_ring, (void **)recv_mbuf, rx_num, NULL);

		// tx
		total_num = rte_ring_sc_dequeue_burst(m_hopa_in_out_ring->hopa_out_ring, (void **)send_mbuf, BURST_SIZE, NULL);
		if (total_num > 0)
		{
			offset = 0;
			while (offset < total_num)
			{
				tx_num = rte_eth_tx_burst(PORT_P0, 0, &send_mbuf[offset], total_num - offset);
				if (tx_num > 0)
				{
					offset += tx_num;
				}
			}
		}
	}

	/* clean up the EAL */
	rte_eal_cleanup();

	return 0;
}
