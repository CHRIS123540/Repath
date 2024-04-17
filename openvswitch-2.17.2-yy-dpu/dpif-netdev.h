/*
 * Copyright (c) 2008, 2009, 2010, 2011, 2012, 2013, 2015 Nicira, Inc.
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

#ifndef DPIF_NETDEV_H
#define DPIF_NETDEV_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dpif.h"
#include "openvswitch/types.h"
#include "dp-packet.h"
#include "packets.h"

#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_ethdev.h>

#ifdef  __cplusplus
extern "C" {
#endif

/* Enough headroom to add a vlan tag, plus an extra 2 bytes to allow IP
 * headers to be aligned on a 4-byte boundary.  */
enum { DP_NETDEV_HEADROOM = 2 + VLAN_HEADER_LEN };

bool dpif_is_netdev(const struct dpif *);

/* HOPA CP start */

struct rte_mempool *hopa_cp_mp;

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

/* HOPA CP Header */
struct hopa_cp_hdr
{
    uint8_t flag;      /**< HOPA flag. 0 -> control plane . 1 -> data plane */
    uint8_t cp_flag;   /**< CP flag. 0 -> perbe. 1 -> repath. 2 -> repath_ack. */
    uint8_t probe_path_id; /**< probe path id */
    uint8_t repath_id; /**< repath id */
    uint8_t rsvd; /**< reserved field */
    rte_be64_t seq;
    rte_be64_t ack;
    rte_be64_t ts;     /**< timestamp */
};

/* HOPA DP Header */
struct hopa_dp_hdr
{
    uint8_t flag;      /**< HOPA flag. 0 -> control plane . 1 -> data plane */
    uint8_t rsvd;      /**< reserved field */
    uint8_t seg_end;   /**< seg_end */
    rte_be32_t seq_nb; /**< seq_nb */
    rte_be64_t ts;     /**< timestamp */
};

struct hopa_cp_in_out_ring
{
    struct rte_ring *hopa_cp_in_ring;
    struct rte_ring *hopa_cp_out_ring;
};

struct hopa_cp_in_out_ring *m_hopa_cp_in_out_ring;

struct rte_mbuf *hopa_cp_send_mbuf[32];
struct hopa_cp_hdr *hopa_cp_recv_cp_hdr[32];

/* 最优路径ID */
uint8_t best_path_id; 

/* HOPA CP end */

#define NR_QUEUE   1
#define NR_PMD_THREADS 1

#ifdef  __cplusplus
}
#endif

#endif /* netdev.h */
