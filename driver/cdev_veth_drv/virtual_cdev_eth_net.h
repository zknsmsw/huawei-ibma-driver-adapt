/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2019-2021. All rights reserved.
 * Huawei iBMA driver.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef VETH_CDEV_NET_H
#define VETH_CDEV_NET_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/dma-mapping.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/delay.h>

#include "bma_include.h"
#include "bma_ker_intf.h"
#include "securec.h"

#define BSP_OK (0)
#define BSP_ERR (0xFFFFFFFF)
#define BSP_NETDEV_TX_BUSY (1)
#define BSP_ERR_INIT_ERR (BSP_NETDEV_TX_BUSY)
#define BSP_ETH_ERR_BASE (0x0FFFF000)
#define BSP_ERR_OUT_OF_MEM (BSP_ETH_ERR_BASE + 1)    /* Insufficient of memory */
#define BSP_ERR_NULL_POINTER (BSP_ETH_ERR_BASE + 2)  /* Null pointer detected */
#define BSP_ERR_INVALID_STR (BSP_ETH_ERR_BASE + 3)   /* Invalid param string detected */
#define BSP_ERR_INVALID_PARAM (BSP_ETH_ERR_BASE + 4) /* Invalid param value detected */
#define BSP_ERR_INVALID_DATA (BSP_ETH_ERR_BASE + 5)  /* ioctl operation fail */
#define BSP_ERR_OUT_OF_RANGE (BSP_ETH_ERR_BASE + 6)  /* Value out of range */
#define BSP_ERR_INVALID_CARD (BSP_ETH_ERR_BASE + 7)
#define BSP_ERR_INVALID_GRP (BSP_ETH_ERR_BASE + 8)
#define BSP_ERR_INVALID_ETH (BSP_ETH_ERR_BASE + 9)
#define BSP_ERR_SEND_ERR (BSP_ETH_ERR_BASE + 10)
#define BSP_ERR_DMA_ERR (BSP_ETH_ERR_BASE + 11)
#define BSP_ERR_RECV_ERR (BSP_ETH_ERR_BASE + 12)
#define BSP_ERR_SKB_ERR (BSP_ETH_ERR_BASE + 13)
#define BSP_ERR_DMA_ADDR_ERR (BSP_ETH_ERR_BASE + 14)
#define BSP_ERR_IOREMAP_ERR (BSP_ETH_ERR_BASE + 15)
#define BSP_ERR_LEN_ERR (BSP_ETH_ERR_BASE + 16)
#define BSP_ERR_STAT_ERR (BSP_ETH_ERR_BASE + 17)
#define BSP_ERR_AGAIN (BSP_ETH_ERR_BASE + 18)
#define BSP_ERR_NOT_TO_HANDLE (BSP_ETH_ERR_BASE + 19)

#define VETH_SHAREPOOL_SIZE (0xdf000)
#define VETH_SHAREPOOL_OFFSET (0x10000)
#define MAX_SHAREQUEUE_SIZE (0x20000)

#define BSPVETH_DMABURST_MAX (64)
#define BSPVETH_SHMBDBASE_OFFSET (0x80)
#define SHMDMAL_OFFSET (0x10000)
#define MAX_SHMDMAL_SIZE (BSPVETH_DMABURST_MAX * 32)
#define MAX_QUEUE_NUM (1)
#define MAX_QUEUE_BDNUM (128)
#define BSPVETH_MAX_QUE_DEEP (MAX_QUEUE_BDNUM)
#define BSPVETH_POINT_MASK (MAX_QUEUE_BDNUM - 1)
#define BSPVETH_WORK_LIMIT (64)
#define BSPVETH_CHECK_DMA_STATUS_TIMES (512)

#define BSPPACKET_MTU_MAX (1500)

#define BSPVETH_DMA_OK (1)
#define BSPVETH_DMA_BUSY (0)
#define BSPVETH_RX (2)
#define BSPVETH_TX (3)
#define BSPVETH_SHMQUEUE_INITOK (0x12)
#define BSPVETH_SHMQUEUE_INITOK_V2 (0x16)

#define MAX_PACKET_LEN (128 * BSPPACKET_MTU_MAX)
#define MAX_RXTX_PACKET_LEN 64

/* device name. */
#define CDEV_VETH_NAME "net_cdev"

#define FREE_AND_NULL(ptr) do { \
    if (NULL != (ptr)) { \
        kfree(ptr);      \
        (ptr) = NULL;    \
    }                    \
} while (0)

typedef struct {
    u32 len;
    u8 *packet;
} edma_packet_node_s;

typedef struct {
    u32 token;
    u32 number;
    u32 cut_packet_len;
    u8 cut_packet[BSPPACKET_MTU_MAX];
    u8 resv[24];
} edma_cut_packet_node_s;

#define TK_MIDDLE_PACKET 0
#define TK_START_PACKET 1
#define TK_END_PACKET 2
#define TK_START_END 3

/* EDMA transfer requires an alignment of 4. */
#define EDMA_ADDR_ALIGNMENT (4UL)
#define EDMA_ADDR_ALIGN_MASK (EDMA_ADDR_ALIGNMENT - 1)

#define NODE_SIZE (sizeof(edma_cut_packet_node_s))
#define NODE_PER_PAGE (PAGE_SIZE / (NODE_SIZE))

typedef struct {
    struct miscdevice dev;
    void *priv;
} cdev_dev_s;

typedef struct {
    u64 dmapkt;
    u64 dmapktbyte;

    u32 q_empty;
    u32 shm_empty;
    u32 dma_busy;
    u32 type_err;

    u32 dma_need_offset;
    u32 dma_failed;
    u32 dma_burst;
} edma_rxtx_statistics;

typedef struct {
    u8 *pdma_v;
    dma_addr_t dma_p;
    u32 len;
    u32 off;
} edma_bd_info_s;

typedef struct {
    u32 dma_p;
    u32 len;
    u32 off;
} edma_dma_shmbd_s;

typedef struct {
    u32 count;
    u32 total;
    u32 next_to_fill;
    u32 next_to_free;
    u32 resv1;
    u32 resv2;
    u32 init;
    u32 head;
    u32 tail;
} edma_shmq_hd_s;

typedef struct {
    u32 chl;
    u32 len;
    u32 slow;
    u32 shi;
    u32 dlow;
    u32 dhi;
} edma_dmal_s;

typedef struct {
    edma_bd_info_s *pbdinfobase_v;

    edma_shmq_hd_s *pshmqhd_v;
    u8 *pshmqhd_p;

    edma_dma_shmbd_s *pshmbdbase_v;
    u8 *pshmbdbase_p;

    edma_dmal_s *pdmalbase_v;
    u8 *pdmalbase_p;

    u32 dmal_cnt;
    u32 dmal_byte;

    u32 count;
    u32 size;

    u32 head;
    u32 tail;

    u16 start_dma;
    u16 dmacmperr;
    u16 dma_overtime;

    u32 work_limit;

    edma_rxtx_statistics s;
} edma_rxtx_q_s;

typedef struct {
    edma_rxtx_q_s *ptx_queue;
    edma_rxtx_q_s *prx_queue;

    edma_packet_node_s *rx_packet;
    spinlock_t rx_queue_lock;

    u32 rx_packet_head;
    u32 rx_packet_tail;

    unsigned long pages_tx;
    unsigned long pages_rx;

    u8 *pshmpool_p;
    u8 *pshmpool_v;
    u32 shmpoolsize;

    u32 recv_int;
    u32 tobmc_int;
    u32 run_dmaTXtask;
    u32 run_dmaRXtask;
    u32 run_skbRXtask;

    struct tasklet_struct skb_task;
    struct tasklet_struct dma_task;

    cdev_dev_s cdev;
    __kernel_time_t init_time;

    void *edma_priv;
} edma_eth_dev_s;

#ifndef LOG
#define LOG(level, fmt, ...) do { \
    if (debug >= level) {                                                               \
        printk(KERN_ERR "[%s,%d] -> " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__); \
    }                                                                                   \
} while (0)
#endif

#define BD_QUEUE_MASK(p) ((p) & (BSPVETH_POINT_MASK))

#define GET_BD_RING_QUEUE_COUNT(head, tail) ((BSPVETH_MAX_QUE_DEEP + (head) - (tail)) & BSPVETH_POINT_MASK)
#define GET_BD_RING_QUEUE_SPACE(head, tail) ((BSPVETH_MAX_QUE_DEEP - 1 + (tail) - (head)) & BSPVETH_POINT_MASK)
#define JUDGE_RING_QUEUE_SPACE(head, tail, len) (GET_BD_RING_QUEUE_SPACE(head, tail) >= (len))

#define CHECK_DMA_QUEUE_EMPTY(type, queue)                                             \
    (((type) == BSPVETH_RX && (queue)->pshmqhd_v->head == (queue)->pshmqhd_v->tail) || \
     ((type) == BSPVETH_TX && (queue)->head == (queue)->tail))

#define CHECK_DMA_RXQ_FAULT(queue, type, cnt) \
    ((type) == BSPVETH_RX && (queue)->dmal_cnt > 1 && (cnt) < ((queue)->work_limit / 2))

#define GET_DMA_DIRECTION(type) (((type) == BSPVETH_RX) ? BMC_TO_HOST : HOST_TO_BMC)

/* ****** rate limit ******** */
#define RL_MAX_PACKET 10
#define RL_STRESS_LOW 50
#define RL_STRESS_HIGH 80
#define RL_DELAY_MS_LOW 20
#define RL_DELAY_MS_HIGH 100

void veth_dma_task_H(u32 type);
void veth_skbtimer_close(void);
int veth_skbtimer_init(void);
int veth_dmatimer_close_H(void);
int veth_dmatimer_init_H(void);
int veth_skb_tr_task(unsigned long data);

#endif
