/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2017-2021. All rights reserved.
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

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/kthread.h>

#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>

#include <linux/vmalloc.h>
#include <linux/atomic.h>
#include <linux/proc_fs.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <asm/page.h>

#include <linux/ip.h>

#include "veth_hb.h"
#include "securec.h"

#define GET_QUEUE_STAT(node, stat) ((node) ? ((char *)(node) + (stat)->stat_offset) : NULL)

#define GET_SHM_QUEUE_STAT(node, stat) \
    (((node) && (node)->pshmqhd_v) ? ((char *)(node)->pshmqhd_v + (stat)->stat_offset) : NULL)

#define GET_STATS_VALUE(ptr, pstat) \
    ((ptr) ? (((pstat)->sizeof_stat == sizeof(u64)) ? (*(u64 *)(ptr)) : (*(u32 *)(ptr))) : 0)

#define GET_DMA_DIRECTION(type) (((type) == BSPVETH_RX) ? BMC_TO_HOST : HOST_TO_BMC)

#define CHECK_DMA_QUEUE_EMPTY(type, queue)                                             \
    (((type) == BSPVETH_RX && (queue)->pshmqhd_v->head == (queue)->pshmqhd_v->tail) || \
     ((type) != BSPVETH_RX && (queue)->head == (queue)->tail))

#define CHECK_DMA_RXQ_FAULT(queue, type, cnt) \
    ((queue)->dmal_cnt > 1 && (cnt) < ((queue)->work_limit / 2) && (type) == BSPVETH_RX)

static u32 veth_ethtool_get_link(struct net_device *dev);

int debug; /* debug switch */
module_param_call(debug, &edma_param_set_debug, &param_get_int, &debug, 0644);

MODULE_PARM_DESC(debug, "Debug switch (0=close debug, 1=open debug)");

#ifdef __UT_TEST
u32 g_testdma;

u32 g_testlbk;

#endif

struct bspveth_device g_bspveth_dev = {};

/* g_shutdown_flag用于防止veth_shutdown_task被veth_dma_tx_timer_do_H抢占的标记位，1代表不可抢占，0代表可抢占 */
static int g_shutdown_flag = 0;

static int veth_int_handler(struct notifier_block *pthis, unsigned long ev, void *unuse);

static struct notifier_block g_veth_int_nb = {
    .notifier_call = veth_int_handler,
};

static const struct veth_stats veth_gstrings_stats[] = {
    { "rx_packets", NET_STATS, VETH_STAT_SIZE(stats.rx_packets), VETH_STAT_OFFSET(stats.rx_packets) },
    { "rx_bytes", NET_STATS, VETH_STAT_SIZE(stats.rx_bytes), VETH_STAT_OFFSET(stats.rx_bytes) },
    { "rx_dropped", NET_STATS, VETH_STAT_SIZE(stats.rx_dropped), VETH_STAT_OFFSET(stats.rx_dropped) },
    { "rx_head", QUEUE_RX_STATS, QUEUE_TXRX_STAT_SIZE(head), QUEUE_TXRX_STAT_OFFSET(head) },
    { "rx_tail", QUEUE_RX_STATS, QUEUE_TXRX_STAT_SIZE(tail), QUEUE_TXRX_STAT_OFFSET(tail) },
    { "rx_next_to_fill", QUEUE_RX_STATS, QUEUE_TXRX_STAT_SIZE(next_to_fill), QUEUE_TXRX_STAT_OFFSET(next_to_fill) },
    { "rx_shmq_head", SHMQ_RX_STATS, SHMQ_TXRX_STAT_SIZE(head), SHMQ_TXRX_STAT_OFFSET(head) },
    { "rx_shmq_tail", SHMQ_RX_STATS, SHMQ_TXRX_STAT_SIZE(tail), SHMQ_TXRX_STAT_OFFSET(tail) },
    { "rx_shmq_next_to_free", SHMQ_RX_STATS, SHMQ_TXRX_STAT_SIZE(next_to_free), SHMQ_TXRX_STAT_OFFSET(next_to_free) },
    { "rx_queue_full", QUEUE_RX_STATS, QUEUE_TXRX_STAT_SIZE(s.q_full), QUEUE_TXRX_STAT_OFFSET(s.q_full) },
    { "rx_dma_busy", QUEUE_RX_STATS, QUEUE_TXRX_STAT_SIZE(s.dma_busy), QUEUE_TXRX_STAT_OFFSET(s.dma_busy) },
    { "rx_dma_failed", QUEUE_RX_STATS, QUEUE_TXRX_STAT_SIZE(s.dma_failed), QUEUE_TXRX_STAT_OFFSET(s.dma_failed) },

    { "tx_packets", NET_STATS, VETH_STAT_SIZE(stats.tx_packets), VETH_STAT_OFFSET(stats.tx_packets) },
    { "tx_bytes", NET_STATS, VETH_STAT_SIZE(stats.tx_bytes), VETH_STAT_OFFSET(stats.tx_bytes) },
    { "tx_dropped", NET_STATS, VETH_STAT_SIZE(stats.tx_dropped), VETH_STAT_OFFSET(stats.tx_dropped) },

    { "tx_head", QUEUE_TX_STATS, QUEUE_TXRX_STAT_SIZE(head), QUEUE_TXRX_STAT_OFFSET(head) },
    { "tx_tail", QUEUE_TX_STATS, QUEUE_TXRX_STAT_SIZE(tail), QUEUE_TXRX_STAT_OFFSET(tail) },
    { "tx_next_to_free", QUEUE_TX_STATS, QUEUE_TXRX_STAT_SIZE(next_to_free), QUEUE_TXRX_STAT_OFFSET(next_to_free) },
    { "tx_shmq_head", SHMQ_TX_STATS, SHMQ_TXRX_STAT_SIZE(head), SHMQ_TXRX_STAT_OFFSET(head) },
    { "tx_shmq_tail", SHMQ_TX_STATS, SHMQ_TXRX_STAT_SIZE(tail), SHMQ_TXRX_STAT_OFFSET(tail) },
    { "tx_shmq_next_to_free", SHMQ_TX_STATS, SHMQ_TXRX_STAT_SIZE(next_to_free), SHMQ_TXRX_STAT_OFFSET(next_to_free) },

    { "tx_queue_full", QUEUE_TX_STATS, QUEUE_TXRX_STAT_SIZE(s.q_full), QUEUE_TXRX_STAT_OFFSET(s.q_full) },
    { "tx_dma_busy", QUEUE_TX_STATS, QUEUE_TXRX_STAT_SIZE(s.dma_busy), QUEUE_TXRX_STAT_OFFSET(s.dma_busy) },
    { "tx_dma_failed", QUEUE_TX_STATS, QUEUE_TXRX_STAT_SIZE(s.dma_failed), QUEUE_TXRX_STAT_OFFSET(s.dma_failed) },

    { "recv_int", VETH_STATS, VETH_STAT_SIZE(recv_int), VETH_STAT_OFFSET(recv_int) },
    { "tobmc_int", VETH_STATS, VETH_STAT_SIZE(tobmc_int), VETH_STAT_OFFSET(tobmc_int) },
};

static int print_queue_stat(char *buf, int len, int type, int queue_num)
{
    struct bspveth_rxtx_q *pqueue = NULL;

    if (type == BSPVETH_RX) {
        pqueue = g_bspveth_dev.prx_queue[queue_num];
        len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "=============RXQUEUE STATIS============\r\n");
    } else {
        pqueue = g_bspveth_dev.ptx_queue[queue_num];
        len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "=============TXQUEUE STATIS============\r\n");
    }
    if (!pqueue) {
        len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "NULL\r\n");
        return len;
    }

    if (pqueue->pshmqhd_v == NULL) {
        return BSP_ERR_NULL_POINTER;
    }

    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[pkt            ] :%lld\r\n", queue_num,
                     pqueue->s.pkt);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[pktbyte        ] :%lld\r\n", queue_num,
                     pqueue->s.pktbyte);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[refill         ] :%lld\r\n", queue_num,
                     pqueue->s.refill);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[freetx         ] :%lld\r\n", queue_num,
                     pqueue->s.freetx);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[dmapkt         ] :%lld\r\n", queue_num,
                     pqueue->s.dmapkt);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[dmapktbyte     ] :%lld\r\n", queue_num,
                     pqueue->s.dmapktbyte);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[next_to_fill   ] :%d\r\n", queue_num,
                     pqueue->next_to_fill);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[next_to_free   ] :%d\r\n", queue_num,
                     pqueue->next_to_free);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[head           ] :%d\r\n", queue_num,
                     pqueue->head);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[tail           ] :%d\r\n", queue_num,
                     pqueue->tail);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[work_limit     ] :%d\r\n", queue_num,
                     pqueue->work_limit);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "=================SHARE=================\r\n");
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[next_to_fill   ] :%d\r\n", queue_num,
                     pqueue->pshmqhd_v->next_to_fill);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[next_to_free   ] :%d\r\n", queue_num,
                     pqueue->pshmqhd_v->next_to_free);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[head           ] :%d\r\n", queue_num,
                     pqueue->pshmqhd_v->head);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[tail           ] :%d\r\n", queue_num,
                     pqueue->pshmqhd_v->tail);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "=======================================\r\n");
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[dropped_pkt    ] :%d\r\n", queue_num,
                     pqueue->s.dropped_pkt);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[netifrx_err    ] :%d\r\n", queue_num,
                     pqueue->s.netifrx_err);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[null_point     ] :%d\r\n", queue_num,
                     pqueue->s.null_point);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[retry_err      ] :%d\r\n", queue_num,
                     pqueue->s.retry_err);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[allocskb_err   ] :%d\r\n", queue_num,
                     pqueue->s.allocskb_err);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[q_full         ] :%d\r\n", queue_num,
                     pqueue->s.q_full);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[q_emp          ] :%d\r\n", queue_num,
                     pqueue->s.q_emp);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[need_fill      ] :%d\r\n", queue_num,
                     pqueue->s.need_fill);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[need_free      ] :%d\r\n", queue_num,
                     pqueue->s.need_free);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[type_err       ] :%d\r\n", queue_num,
                     pqueue->s.type_err);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[shm_full       ] :%d\r\n", queue_num,
                     pqueue->s.shm_full);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[shm_emp        ] :%d\r\n", queue_num,
                     pqueue->s.shm_emp);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[shmretry_err   ] :%d\r\n", queue_num,
                     pqueue->s.shmretry_err);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[shmqueue_noinit] :%d\r\n", queue_num,
                     pqueue->s.shmqueue_noinit);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[dma_busy       ] :%d\r\n", queue_num,
                     pqueue->s.dma_busy);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[dma_mapping_err] :%d\r\n", queue_num,
                     pqueue->s.dma_mapping_err);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[dma_failed      ] :%d\r\n", queue_num,
                     pqueue->s.dma_failed);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[dma_burst      ] :%d\r\n", queue_num,
                     pqueue->s.dma_burst);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[lbk_cnt        ] :%d\r\n", queue_num,
                     pqueue->s.lbk_cnt);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[dma_need_offset] :%d\r\n", queue_num,
                     pqueue->s.dma_need_offset);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "QUEUE[%d]--[lbk_txerr      ] :%d\r\n", queue_num,
                     pqueue->s.lbk_txerr);

    return len;
}

#define VETH_GLOBAL_STATS_LEN (sizeof(veth_gstrings_stats) / sizeof(struct veth_stats))

#if defined(LINUX_VERSION_CODE) && (KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE)
static int veth_param_get_statics(char *buf, struct kernel_param *kp)
#else
static int veth_param_get_statics(char *buf, const struct kernel_param *kp)
#endif
{
    int len = 0;
    int i = 0;
    __kernel_time_t running_time = 0;

    if (!buf) {
        return 0;
    }

    /* The sysfs attribute exists as soon as the module is loaded, but the netdev is
       only created later; without this guard -> netif_running(NULL) dereferences NULL. */
    if (g_bspveth_dev.pnetdev == NULL) {
        return 0;
    }

    GET_SYS_SECONDS(running_time);

    running_time -= g_bspveth_dev.init_time;

    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1,
                     "==========================VETH INFO======================\r\n");
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "[version     ]:" VETH_VERSION "\n");
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "[link state  ]:%d\n",
                     veth_ethtool_get_link(g_bspveth_dev.pnetdev));
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "[running_time]:%luD %02lu:%02lu:%02lu\n",
                     running_time / (SECONDS_PER_DAY), running_time % (SECONDS_PER_DAY) / SECONDS_PER_HOUR,
                     running_time % SECONDS_PER_HOUR / SECONDS_PER_MINUTE, running_time % SECONDS_PER_MINUTE);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1,
                     "[bspveth_dev ]:MAX_QUEUE_NUM :0x%-16x    MAX_QUEUE_BDNUM :0x%-16x\r\n", MAX_QUEUE_NUM,
                     MAX_QUEUE_BDNUM);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1,
                     "[bspveth_dev ]:pnetdev       :0x%-16p    ppcidev         :0x%-16p\r\n", g_bspveth_dev.pnetdev,
                     g_bspveth_dev.ppcidev);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1,
                     "[bspveth_dev ]:pshmpool_p    :0x%-16p    pshmpool_v      :0x%-16p\r\n"
                     "[bspveth_dev]:shmpoolsize   :0x%-16x    g_veth_dbg_lv       :0x%-16x\r\n",
                     g_bspveth_dev.pshmpool_p, g_bspveth_dev.pshmpool_v, g_bspveth_dev.shmpoolsize, debug);

    for (i = 0; i < MAX_QUEUE_NUM; i++) {
        len = print_queue_stat(buf, len, BSPVETH_RX, i);
        len = print_queue_stat(buf, len, BSPVETH_TX, i);
    }

    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "=============BSPVETH STATIS===========\r\n");
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "[bspveth_dev]:run_dma_rx_task :0x%-8x(%d)\r\n",
                     g_bspveth_dev.run_dma_rx_task, g_bspveth_dev.run_dma_rx_task);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "[bspveth_dev]:run_dma_tx_task :0x%-8x(%d)\r\n",
                     g_bspveth_dev.run_dma_tx_task, g_bspveth_dev.run_dma_tx_task);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "[bspveth_dev]:run_skb_rx_task :0x%-8x(%d)\r\n",
                     g_bspveth_dev.run_skb_rx_task, g_bspveth_dev.run_skb_rx_task);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "[bspveth_dev]:run_skb_fr_task :0x%-8x(%d)\r\n",
                     g_bspveth_dev.run_skb_fr_task, g_bspveth_dev.run_skb_fr_task);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "[bspveth_dev]:recv_int        :0x%-8x(%d)\r\n",
                     g_bspveth_dev.recv_int, g_bspveth_dev.recv_int);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "[bspveth_dev]:tobmc_int       :0x%-8x(%d)\r\n",
                     g_bspveth_dev.tobmc_int, g_bspveth_dev.tobmc_int);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "[bspveth_dev]:shutdown_cnt    :0x%-8x(%d)\r\n",
                     g_bspveth_dev.shutdown_cnt, g_bspveth_dev.shutdown_cnt);

    return len;
}

module_param_call(statistics, NULL, veth_param_get_statics, &debug, 0444);

MODULE_PARM_DESC(statistics, "Statistics info of veth driver,readonly");

static void veth_reset_dma(int type)
{
    if (type == BSPVETH_RX) {
        bma_intf_reset_dma(BMC_TO_HOST);
    } else if (type == BSPVETH_TX) {
        bma_intf_reset_dma(HOST_TO_BMC);
    } else {
        return;
    }
}

static s32 bspveth_setup_tx_resources(struct bspveth_device *pvethdev, struct bspveth_rxtx_q *ptx_queue)
{
    unsigned int size;

    if (!pvethdev || !ptx_queue) {
        return BSP_ERR_NULL_POINTER;
    }

    ptx_queue->count = MAX_QUEUE_BDNUM;

    size = sizeof(struct bspveth_bd_info) * ptx_queue->count;
    ptx_queue->pbdinfobase_v = (struct bspveth_bd_info *)vmalloc(size);
    if (!ptx_queue->pbdinfobase_v) {
        goto alloc_failed;
    }

    (void)memset_s(ptx_queue->pbdinfobase_v, size, 0, size);

    /* round up to nearest 4K */
    ptx_queue->size = ptx_queue->count * sizeof(struct bspveth_bd_info);
    /* 4096 表示以最大4k页长对齐 */
    ptx_queue->size = ALIGN(ptx_queue->size, 4096);

    /* prepare  4096 send buffer */
    ptx_queue->pbdbase_v = (struct bspveth_dma_bd *)kmalloc(ptx_queue->size, GFP_KERNEL);
    if (!ptx_queue->pbdbase_v) {
        VETH_LOG(DLOG_ERROR, "Unable to allocate memory for the receive descriptor ring\n");

        vfree(ptx_queue->pbdinfobase_v);
        ptx_queue->pbdinfobase_v = NULL;

        goto alloc_failed;
    }

    ptx_queue->pbdbase_p = (u8 *)(__pa((unsigned long)(ptx_queue->pbdbase_v)));

    ptx_queue->next_to_fill = 0;
    ptx_queue->next_to_free = 0;
    ptx_queue->head = 0;
    ptx_queue->tail = 0;
    ptx_queue->work_limit = BSPVETH_WORK_LIMIT;

    (void)memset_s(&ptx_queue->s, sizeof(struct bspveth_rxtx_statis), 0, sizeof(struct bspveth_rxtx_statis));

    return 0;

alloc_failed:
    return -ENOMEM;
}

static void bspveth_free_tx_resources(struct bspveth_device *pvethdev, struct bspveth_rxtx_q *ptx_queue)
{
    unsigned int i;
    unsigned long size;
    struct bspveth_bd_info *pbdinfobase_v = NULL;
    struct sk_buff *skb = NULL;

    if (!ptx_queue || !pvethdev) {
        return;
    }

    pbdinfobase_v = ptx_queue->pbdinfobase_v;
    if (!pbdinfobase_v) {
        return;
    }

    for (i = 0; i < ptx_queue->count; i++) {
        skb = pbdinfobase_v[i].pdma_v;
        if (skb) {
            dev_kfree_skb_any(skb);
        }

        pbdinfobase_v[i].pdma_v = NULL;
    }

    size = sizeof(struct bspveth_bd_info) * ptx_queue->count;
    (void)memset_s(ptx_queue->pbdinfobase_v, size, 0, size);
    (void)memset_s(ptx_queue->pbdbase_v, ptx_queue->size, 0, ptx_queue->size);

    ptx_queue->next_to_fill = 0;
    ptx_queue->next_to_free = 0;
    ptx_queue->head = 0;
    ptx_queue->tail = 0;

    vfree(ptx_queue->pbdinfobase_v);
    ptx_queue->pbdinfobase_v = NULL;

    kfree(ptx_queue->pbdbase_v);
    ptx_queue->pbdbase_v = NULL;

    VETH_LOG(DLOG_DEBUG, "bspveth free tx resources ok, count=%d\n", ptx_queue->count);
}

static s32 bspveth_setup_all_tx_resources(struct bspveth_device *pvethdev)
{
    int qid;
    int i;
    int err = 0;
    struct bspveth_shmq_hd *shmq_head = NULL;

    if (!pvethdev) {
        return BSP_ERR_NULL_POINTER;
    }

    for (qid = 0; qid < MAX_QUEUE_NUM; qid++) {
        pvethdev->ptx_queue[qid] = (struct bspveth_rxtx_q *)kmalloc(sizeof(*pvethdev->ptx_queue[qid]), GFP_KERNEL);
        if (!pvethdev->ptx_queue[qid]) {
            VETH_LOG(DLOG_ERROR, "kmalloc failed for ptx_queue[%d]\n", qid);
            err = -1;
            goto failed;
        }
        (void)memset_s(pvethdev->ptx_queue[qid], sizeof(struct bspveth_rxtx_q), 0, sizeof(struct bspveth_rxtx_q));
        shmq_head = (struct bspveth_shmq_hd *)(pvethdev->pshmpool_v + MAX_SHAREQUEUE_SIZE * (qid));
        set_rxtx_pvethdev(pvethdev, shmq_head, qid, BSPVETH_TX);

        (void)memset_s(pvethdev->ptx_queue[qid]->pdmalbase_v, MAX_SHMDMAL_SIZE, 0, MAX_SHMDMAL_SIZE);

        err = bspveth_setup_tx_resources(pvethdev, pvethdev->ptx_queue[qid]);
        if (err != 0) {
            pvethdev->ptx_queue[qid]->pshmqhd_v = NULL;
            VETH_LOG(DLOG_ERROR, "Allocation for Tx Queue %u failed\n", qid);

            goto failed;
        }
    }

    return 0;
failed:
    for (i = 0; i < MAX_QUEUE_NUM; i++) {
        bspveth_free_tx_resources(pvethdev, pvethdev->ptx_queue[i]);
        if (pvethdev->ptx_queue[i] != NULL) {
            kfree(pvethdev->ptx_queue[i]);
            pvethdev->ptx_queue[i] = NULL;
        }
    }

    return err;
}

static void bspveth_free_all_tx_resources(struct bspveth_device *pvethdev)
{
    int i;

    if (!pvethdev) {
        return;
    }

    for (i = 0; i < MAX_QUEUE_NUM; i++) {
        if (pvethdev->ptx_queue[i]) {
            bspveth_free_tx_resources(pvethdev, pvethdev->ptx_queue[i]);
        }

        kfree(pvethdev->ptx_queue[i]);
        pvethdev->ptx_queue[i] = NULL;
    }
}

static s32 veth_alloc_one_rx_skb(struct bspveth_rxtx_q *prx_queue, int idx)
{
    dma_addr_t dma = 0;
    struct sk_buff *skb;
    struct bspveth_bd_info *pbdinfobase_v = NULL;
    struct bspveth_dma_bd *pbdbase_v = NULL;

    pbdinfobase_v = prx_queue->pbdinfobase_v;
    pbdbase_v = prx_queue->pbdbase_v;
    if (pbdinfobase_v == NULL || pbdbase_v == NULL || g_bspveth_dev.ppcidev == NULL) {
        return BSP_ERR_NULL_POINTER;
    }

    skb = netdev_alloc_skb(g_bspveth_dev.pnetdev, BSPVETH_SKB_SIZE + BSPVETH_CACHELINE_SIZE);
    if (!skb) {
        VETH_LOG(DLOG_ERROR, "netdev_alloc_skb failed\n");
        return -ENOMEM;
    }

    /* advance the data pointer to the next cache line */
    skb_reserve(skb, PTR_ALIGN(skb->data, BSPVETH_CACHELINE_SIZE) - skb->data);

    dma = dma_map_single(&g_bspveth_dev.ppcidev->dev, skb->data, BSPVETH_SKB_SIZE, DMA_FROM_DEVICE);
    if (dma_mapping_error(&g_bspveth_dev.ppcidev->dev, dma)) {
        VETH_LOG(DLOG_ERROR, "dma_mapping_error failed\n");
        dev_kfree_skb_any(skb);
        return -EFAULT;
    }

    pbdinfobase_v[idx].pdma_v = skb;
    pbdinfobase_v[idx].len = BSPVETH_SKB_SIZE;

    pbdbase_v[idx].dma_p = dma;
    pbdbase_v[idx].len = BSPVETH_SKB_SIZE;

    return 0;
}

static s32 veth_refill_rxskb(struct bspveth_rxtx_q *prx_queue, int queue)
{
    int i, work_limit;
    unsigned int next_to_fill, tail;
    int ret = BSP_OK;

    if (!prx_queue) {
        return BSP_ERR_AGAIN;
    }

    work_limit = (int)prx_queue->work_limit;
    next_to_fill = prx_queue->next_to_fill;
    tail = prx_queue->tail;

    for (i = 0; i < work_limit; i++) {
        if (!JUDGE_RX_QUEUE_SPACE(next_to_fill, tail, 1)) {
            break;
        }

        ret = veth_alloc_one_rx_skb(prx_queue, next_to_fill);
        if (ret != 0) {
            break;
        }

        INC_STATIS_RX(queue, refill, 1);
        next_to_fill = (next_to_fill + 1) & BSPVETH_POINT_MASK;
    }

    mb();   /* memory barriers. */
    prx_queue->next_to_fill = next_to_fill;

    tail = prx_queue->tail;
    if (JUDGE_RX_QUEUE_SPACE(next_to_fill, tail, 1)) {
        VETH_LOG(DLOG_DEBUG, "next_to_fill(%d) != tail(%d)\n", next_to_fill, tail);

        return BSP_ERR_AGAIN;
    }

    return 0;
}

static s32 bspveth_setup_rx_skb(struct bspveth_device *pvethdev, struct bspveth_rxtx_q *prx_queue)
{
    u32 idx;
    int ret = 0;

    if (!pvethdev || !prx_queue) {
        return BSP_ERR_NULL_POINTER;
    }

    VETH_LOG(DLOG_DEBUG, "waite setup rx skb ,count=%d\n", prx_queue->count);

    for (idx = 0; idx < prx_queue->count - 1; idx++) {
        ret = veth_alloc_one_rx_skb(prx_queue, idx);
        if (ret) {
            break;
        }
    }

    if (!idx) {
        /* Can't alloc even one packets */
        return -EFAULT;
    }

    mb();   /* memory barriers. */
    prx_queue->next_to_fill = idx;

    VETH_LOG(DLOG_DEBUG, "prx_queue->next_to_fill=%d\n", prx_queue->next_to_fill);

    VETH_LOG(DLOG_DEBUG, "setup rx skb ok, count=%d\n", prx_queue->count);

    return BSP_OK;
}

static void bspveth_free_rx_skb(struct bspveth_device *pvethdev, struct bspveth_rxtx_q *prx_queue)
{
    u32 i = 0;
    struct bspveth_bd_info *pbdinfobase_v = NULL;
    struct bspveth_dma_bd *pbdbase_v = NULL;
    struct sk_buff *skb = NULL;

    if (!pvethdev || !prx_queue) {
        return;
    }

    pbdinfobase_v = prx_queue->pbdinfobase_v;
    pbdbase_v = prx_queue->pbdbase_v;
    if (!pbdinfobase_v || !pbdbase_v || !g_bspveth_dev.ppcidev) {
        return;
    }

    /* Free all the Rx ring pages */
    for (i = 0; i < prx_queue->count; i++) {
        skb = pbdinfobase_v[i].pdma_v;
        if (!skb) {
            continue;
        }

        dma_unmap_single(&g_bspveth_dev.ppcidev->dev, pbdbase_v[i].dma_p, BSPVETH_SKB_SIZE, DMA_FROM_DEVICE);
        dev_kfree_skb_any(skb);

        pbdinfobase_v[i].pdma_v = NULL;
    }

    prx_queue->next_to_fill = 0;
}

static s32 bspveth_setup_all_rx_skb(struct bspveth_device *pvethdev)
{
    int qid, i, err = BSP_OK;

    if (!pvethdev) {
        return BSP_ERR_NULL_POINTER;
    }

    for (qid = 0; qid < MAX_QUEUE_NUM; qid++) {
        err = bspveth_setup_rx_skb(pvethdev, pvethdev->prx_queue[qid]);
        if (err != 0) {
            VETH_LOG(DLOG_ERROR, "queue[%d]setup RX skb failed\n", qid);
            goto failed;
        }

        VETH_LOG(DLOG_DEBUG, "queue[%d] bspveth_setup_rx_skb ok\n", qid);
    }

    return 0;

failed:
    for (i = 0; i < MAX_QUEUE_NUM; i++) {
        bspveth_free_rx_skb(pvethdev, pvethdev->prx_queue[i]);
    }

    return err;
}

static void bspveth_free_all_rx_skb(struct bspveth_device *pvethdev)
{
    int qid;

    if (!pvethdev) {
        return;
    }

    /* Free all the Rx ring pages */
    for (qid = 0; qid < MAX_QUEUE_NUM; qid++) {
        bspveth_free_rx_skb(pvethdev, pvethdev->prx_queue[qid]);
    }
}

static s32 bspveth_setup_rx_resources(struct bspveth_device *pvethdev, struct bspveth_rxtx_q *prx_queue)
{
    int size;

    if (!pvethdev || !prx_queue) {
        return BSP_ERR_NULL_POINTER;
    }

    prx_queue->count = MAX_QUEUE_BDNUM;
    size = (int)(sizeof(*prx_queue->pbdinfobase_v) * prx_queue->count);
    prx_queue->pbdinfobase_v = (struct bspveth_bd_info *)vmalloc(size);
    if (!prx_queue->pbdinfobase_v) {
        VETH_LOG(DLOG_ERROR, "Unable to vmalloc buffer memory for the receive descriptor ring\n");

        goto alloc_failed;
    }

    (void)memset_s(prx_queue->pbdinfobase_v, size, 0, size);

    /* Round up to nearest 4K */
    prx_queue->size = prx_queue->count * sizeof(*prx_queue->pbdbase_v);
    /* 4096 表示以4k页长对齐 */
    prx_queue->size = ALIGN(prx_queue->size, 4096);
    prx_queue->pbdbase_v = (struct bspveth_dma_bd *)kmalloc(prx_queue->size, GFP_ATOMIC);
    if (!prx_queue->pbdbase_v) {
        VETH_LOG(DLOG_ERROR, "Unable to allocate memory for the receive descriptor ring\n");

        vfree(prx_queue->pbdinfobase_v);
        prx_queue->pbdinfobase_v = NULL;

        goto alloc_failed;
    }

    prx_queue->pbdbase_p = (u8 *)__pa((unsigned long)(prx_queue->pbdbase_v));

    prx_queue->next_to_fill = 0;
    prx_queue->next_to_free = 0;
    prx_queue->head = 0;
    prx_queue->tail = 0;

    prx_queue->work_limit = BSPVETH_WORK_LIMIT;

    (void)memset_s(&prx_queue->s, sizeof(struct bspveth_rxtx_statis), 0, sizeof(struct bspveth_rxtx_statis));

    return 0;

alloc_failed:
    return -ENOMEM;
}

static void bspveth_free_rx_resources(struct bspveth_device *pvethdev, struct bspveth_rxtx_q *prx_queue)
{
    unsigned long size;
    struct bspveth_bd_info *pbdinfobase_v = NULL;

    if (!pvethdev || !prx_queue) {
        return;
    }

    pbdinfobase_v = prx_queue->pbdinfobase_v;
    if (!pbdinfobase_v) {
        return;
    }

    if (!prx_queue->pbdbase_v) {
        return;
    }

    size = sizeof(struct bspveth_bd_info) * prx_queue->count;
    (void)memset_s(prx_queue->pbdinfobase_v, size, 0, size);

    /* Zero out the descriptor ring */
    (void)memset_s(prx_queue->pbdbase_v, prx_queue->size, 0, prx_queue->size);

    vfree(prx_queue->pbdinfobase_v);
    prx_queue->pbdinfobase_v = NULL;

    kfree(prx_queue->pbdbase_v);
    prx_queue->pbdbase_v = NULL;

    VETH_LOG(DLOG_DEBUG, "bspveth free rx resources ok!!count=%d\n", prx_queue->count);
}

void set_rxtx_pvethdev(struct bspveth_device *pvethdev, struct bspveth_shmq_hd *shmq_head, int qid, int type)
{
    u8 *shmq_head_p = NULL;
    int isRX = (type == BSPVETH_RX) ? 1 : 0;
    struct bspveth_rxtx_q *ptxrx_queue = NULL;
    int ret = 0;
    phys_addr_t veth_address = 0;

    ret = bma_intf_get_map_address(TYPE_VETH_ADDR, &veth_address);
    if (ret != 0) {
        return;
    }

    if (!pvethdev) {
        return;
    }

    if (type == BSPVETH_TX) {
        ptxrx_queue = g_bspveth_dev.ptx_queue[qid];
    } else {
        ptxrx_queue = g_bspveth_dev.prx_queue[qid];
    }

    ptxrx_queue->pshmqhd_v = shmq_head;
    shmq_head_p = pvethdev->pshmpool_p + MAX_SHAREQUEUE_SIZE * (qid + isRX);
    ptxrx_queue->pshmqhd_p = shmq_head_p;
    ptxrx_queue->pshmbdbase_v = (struct bspveth_dma_shmbd *)((unsigned long)(shmq_head) + BSPVETH_SHMBDBASE_OFFSET);
    ptxrx_queue->pshmbdbase_p = (u8 *)((unsigned long)(shmq_head_p) + BSPVETH_SHMBDBASE_OFFSET);
    ptxrx_queue->pdmalbase_v = (struct bspveth_dmal *)((unsigned long)(shmq_head) + SHMDMAL_OFFSET);
    ptxrx_queue->pdmalbase_p = (u8 *)(u64)(veth_address + MAX_SHAREQUEUE_SIZE * (qid + isRX) + SHMDMAL_OFFSET);
}

static s32 bspveth_setup_all_rx_resources(struct bspveth_device *pvethdev)
{
    int qid, i, err = 0;
    struct bspveth_shmq_hd *shmq_head = NULL;

    if (!pvethdev) {
        return BSP_ERR_NULL_POINTER;
    }

    for (qid = 0; qid < MAX_QUEUE_NUM; qid++) {
        pvethdev->prx_queue[qid] = (struct bspveth_rxtx_q *)kmalloc(sizeof(*pvethdev->prx_queue[qid]), GFP_KERNEL);
        if (!pvethdev->prx_queue[qid]) {
            VETH_LOG(DLOG_ERROR, "kmalloc failed for prx_queue[%d]\n", qid);

            goto failed;
        }

        (void)memset_s(pvethdev->prx_queue[qid], sizeof(struct bspveth_rxtx_q), 0, sizeof(struct bspveth_rxtx_q));

        shmq_head = (struct bspveth_shmq_hd *)(pvethdev->pshmpool_v + MAX_SHAREQUEUE_SIZE * (qid + 1));

        set_rxtx_pvethdev(pvethdev, shmq_head, qid, BSPVETH_RX);

        (void)memset_s(pvethdev->prx_queue[qid]->pdmalbase_v, MAX_SHMDMAL_SIZE, 0, MAX_SHMDMAL_SIZE);

        err = bspveth_setup_rx_resources(pvethdev, pvethdev->prx_queue[qid]);
        if (err != 0) {
            VETH_LOG(DLOG_ERROR, "Allocation for Rx Queue %u failed\n", qid);
            goto failed;
        }
    }

    return 0;
failed:
    for (i = 0; i < MAX_QUEUE_NUM; i++) {
        bspveth_free_rx_resources(pvethdev, pvethdev->prx_queue[i]);
        kfree(pvethdev->prx_queue[i]);
        pvethdev->prx_queue[i] = NULL;
    }
    return err;
}

static void bspveth_free_all_rx_resources(struct bspveth_device *pvethdev)
{
    int i;

    if (!pvethdev) {
        return;
    }

    for (i = 0; i < MAX_QUEUE_NUM; i++) {
        if (pvethdev->prx_queue[i]) {
            bspveth_free_rx_resources(pvethdev, pvethdev->prx_queue[i]);
        }

        kfree(pvethdev->prx_queue[i]);
        pvethdev->prx_queue[i] = NULL;
    }
}

static s32 bspveth_dev_install(void)
{
    int err;

    err = bspveth_setup_all_rx_resources(&g_bspveth_dev);
    if (err != BSP_OK) {
        err = -1;
        goto err_setup_rx;
    }

    err = bspveth_setup_all_tx_resources(&g_bspveth_dev);
    if (err != BSP_OK) {
        err = -1;
        goto err_setup_tx;
    }

    err = bspveth_setup_all_rx_skb(&g_bspveth_dev);
    if (err != BSP_OK) {
        err = -1;
        goto err_setup_rx_skb;
    }

    return BSP_OK;

err_setup_rx_skb:
    bspveth_free_all_tx_resources(&g_bspveth_dev);

err_setup_tx:
    bspveth_free_all_rx_resources(&g_bspveth_dev);

err_setup_rx:

    return err;
}

static s32 bspveth_dev_uninstall(void)
{
    int err = BSP_OK;

    /* Free all the Rx ring pages */
    bspveth_free_all_rx_skb(&g_bspveth_dev);

    bspveth_free_all_tx_resources(&g_bspveth_dev);

    VETH_LOG(DLOG_DEBUG, "bspveth_free_all_tx_resources ok\n");

    bspveth_free_all_rx_resources(&g_bspveth_dev);

    VETH_LOG(DLOG_DEBUG, "bspveth_free_all_rx_resources ok\n");

    return err;
}

static s32 veth_open(struct net_device *pstr_dev)
{
    s32 ret = BSP_OK;

    if (!pstr_dev) {
        return -1;
    }

    if (!g_bspveth_dev.pnetdev) {
        g_bspveth_dev.pnetdev = pstr_dev;
    }

    ret = bspveth_dev_install();
    if (ret != BSP_OK) {
        ret = -1;
        goto failed1;
    }

    veth_skbtimer_init();

    veth_dmatimer_init_H();

    ret = bma_intf_register_int_notifier(&g_veth_int_nb);
    if (ret != BSP_OK) {
        ret = -1;
        goto failed2;
    }

    bma_intf_set_open_status(g_bspveth_dev.bma_priv, DEV_OPEN);

    g_bspveth_dev.prx_queue[0]->pshmqhd_v->tail = g_bspveth_dev.prx_queue[0]->pshmqhd_v->head;

    bma_intf_int_to_bmc(g_bspveth_dev.bma_priv);

    netif_start_queue(g_bspveth_dev.pnetdev);
    netif_carrier_on(pstr_dev);

    return BSP_OK;

failed2:
    (void)veth_dmatimer_close_H();

    (void)veth_skbtimer_close();

    (void)bspveth_dev_uninstall();

failed1:
    return ret;
}

static s32 veth_close(struct net_device *pstr_dev)
{
    (void)bma_intf_unregister_int_notifier(&g_veth_int_nb);

    netif_carrier_off(pstr_dev);

    bma_intf_set_open_status(g_bspveth_dev.bma_priv, DEV_CLOSE);

    netif_stop_queue(g_bspveth_dev.pnetdev);

    (void)veth_dmatimer_close_H();
    (void)veth_skbtimer_close();

    (void)bspveth_dev_uninstall();

    return BSP_OK;
}

static s32 veth_config(struct net_device *pstr_dev, struct ifmap *pstr_map)
{
    if (!pstr_dev || !pstr_map) {
        return BSP_ERR_NULL_POINTER;
    }

    /* can't act on a running interface */
    if (pstr_dev->flags & IFF_UP) {
        return -EBUSY;
    }

    /* Don't allow changing the I/O address */
    if (pstr_map->base_addr != pstr_dev->base_addr) {
        return -EOPNOTSUPP;
    }

    /* ignore other fields */
    return BSP_OK;
}

static s32 veth_ioctl(struct net_device *pstr_dev, struct ifreq *pifr, s32 l_cmd)
{
    return -EFAULT;
}

static struct net_device_stats *veth_stats(struct net_device *pstr_dev)
{
    return &g_bspveth_dev.stats;
}

static s32 veth_mac_set(struct net_device *pstr_dev, void *p_mac)
{
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)) && (!defined(CONFIG_SUSE_KERNEL) || \
    LINUX_VERSION_CODE < KERNEL_VERSION(5, 14, 21))
    uint32_t i;
#endif
    struct sockaddr *str_addr = NULL;
    u8 *puc_mac = NULL;

    if (!pstr_dev || !p_mac) {
        return BSP_ERR_NULL_POINTER;
    }

    str_addr = (struct sockaddr *)p_mac;
    puc_mac = (u8 *)str_addr->sa_data;
    /* only kernel version greater or equal to 5.15, or suse 5.14.21 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)) || (defined(CONFIG_SUSE_KERNEL) && \
    LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 21))
    eth_hw_addr_set(pstr_dev, puc_mac);
#else
    /* 6 表示MAC地址一共由6部分组成 */
    for (i = 0; i < 6; i++) {
        pstr_dev->dev_addr[i] = puc_mac[i];
    }
#endif

    return BSP_OK;
}

static u32 veth_ethtool_get_link(struct net_device *dev)
{
    if (!bma_intf_is_link_ok() || !netif_running(g_bspveth_dev.pnetdev)) {
        return 0;
    }

    if (g_bspveth_dev.ptx_queue[0] && g_bspveth_dev.ptx_queue[0]->pshmqhd_v) {
        return (u32)((BSPVETH_SHMQUEUE_INITOK == g_bspveth_dev.ptx_queue[0]->pshmqhd_v->init) && netif_carrier_ok(dev));
    }

    return 0;
}

static void veth_ethtool_get_drvinfo(struct net_device *dev, struct ethtool_drvinfo *info)
{
#if defined(LINUX_VERSION_CODE) && (KERNEL_VERSION(6, 8, 0) > LINUX_VERSION_CODE)
    strlcpy(info->driver, MODULE_NAME, sizeof(info->driver));
    strlcpy(info->version, VETH_VERSION, sizeof(info->version));
#else
    strscpy(info->driver, MODULE_NAME, sizeof(info->driver));
    strscpy(info->version, VETH_VERSION, sizeof(info->version));
#endif

    info->n_stats = VETH_GLOBAL_STATS_LEN;
}

static void veth_ethtool_get_stats(struct net_device *netdev, struct ethtool_stats *tool_stats, u64 *data)
{
    unsigned int i = 0;
    char *p = NULL;
    const struct veth_stats *p_stat = veth_gstrings_stats;
    struct bspveth_rxtx_q *ptx_node = g_bspveth_dev.ptx_queue[0];
    struct bspveth_rxtx_q *prx_node = g_bspveth_dev.prx_queue[0];
    char *const pstat_map[] = {
        /* QUEUE TX STATS */
        GET_QUEUE_STAT(ptx_node, p_stat),
        /* QUEUE RX STATS */
        GET_QUEUE_STAT(prx_node, p_stat),
        /* VETH STATS */
        (char *)&g_bspveth_dev + p_stat->stat_offset,
        /* SHMQ TX STATS */
        GET_SHM_QUEUE_STAT(ptx_node, p_stat),
        /* SHMQ RX STATS */
        GET_SHM_QUEUE_STAT(prx_node, p_stat),
        /* NET STATS */
        (char *)&g_bspveth_dev + p_stat->stat_offset
    };

    if (!data || !netdev || !tool_stats) {
        return;
    }

    for (i = 0; i < VETH_GLOBAL_STATS_LEN; i++) {
        p = NULL;

        if (p_stat->type > NET_STATS) {
            break;
        }

        p = pstat_map[p_stat->type];

        data[i] = GET_STATS_VALUE(p, p_stat);

        p_stat++;
    }
}

static void veth_get_strings(struct net_device *netdev, u32 stringset, u8 *data)
{
    u8 *p = data;
    unsigned int i;

    if (!p) {
        return;
    }

    if (stringset == ETH_SS_STATS) {
        for (i = 0; i < VETH_GLOBAL_STATS_LEN; i++) {
            (void)memcpy_s(p, ETH_GSTRING_LEN, veth_gstrings_stats[i].stat_string, ETH_GSTRING_LEN);

            p += ETH_GSTRING_LEN;
        }
    }
}

static int veth_get_sset_count(struct net_device *netdev, int sset)
{
    switch (sset) {
        case ETH_SS_STATS:
            return VETH_GLOBAL_STATS_LEN;

        default:
            return -EOPNOTSUPP;
    }
}

const struct ethtool_ops veth_ethtool_ops = {
    .get_drvinfo = veth_ethtool_get_drvinfo,
    .get_link = veth_ethtool_get_link,
    .get_ethtool_stats = veth_ethtool_get_stats,
    .get_strings = veth_get_strings,
    .get_sset_count = veth_get_sset_count,

};

/* veth_tx() returns int while .ndo_start_xmit requires netdev_tx_t. Modern kbuild
 * compiles with -Werror=incompatible-pointer-types, so route the entry through a
 * correctly typed wrapper. veth_tx() itself (and its return value) is unchanged. */
static netdev_tx_t veth_tx_ndo(struct sk_buff *skb, struct net_device *pstr_dev)
{
    return (netdev_tx_t)veth_tx(skb, pstr_dev);
}

static const struct net_device_ops veth_ops = {
    .ndo_open = veth_open,
    .ndo_stop = veth_close,
    .ndo_set_config = veth_config,
    .ndo_start_xmit = veth_tx_ndo,
    .ndo_do_ioctl = veth_ioctl,
    .ndo_get_stats = veth_stats,
    .ndo_set_mac_address = veth_mac_set,
};

static void veth_netdev_func_init(struct net_device *dev)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)) || (defined(CONFIG_SUSE_KERNEL) && \
    LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 21))
    unsigned char veth_mac[ETH_ALEN] = {0x9C, 0x7D, 0xA3, 0x28, 0x6F, 0xF9};
#endif
    struct tag_pcie_comm_priv *priv = (struct tag_pcie_comm_priv *)netdev_priv(dev);
    u32 host_number = 0;
    int ret = 0;

    VETH_LOG(DLOG_DEBUG, "eth init start\n");

    ether_setup(dev);

    ret = bma_intf_get_host_number(&host_number);
    if (ret < 0) {
        VETH_LOG(DLOG_ERROR, "bma_intf_get_host_number failed!\n");
        return;
    }
    dev->netdev_ops = &veth_ops;

    dev->watchdog_timeo = BSPVETH_NET_TIMEOUT;
    dev->mtu = BSPVETH_MTU_MAX;
    dev->flags = IFF_BROADCAST;
    dev->tx_queue_len = BSPVETH_MAX_QUE_DEEP;
    dev->ethtool_ops = &veth_ethtool_ops;

    /* Then, initialize the priv field. This encloses the statistics
     * and a few private fields.
     */
    (void)memset_s(priv, sizeof(struct tag_pcie_comm_priv), 0, sizeof(struct tag_pcie_comm_priv));
    (void)strncpy_s(priv->net_type, NET_TYPE_LEN, MODULE_NAME, NET_TYPE_LEN - 1);

    /* 9C:7D:A3:28:6F:F9 for host0 */
    /* 9C:7D:A3:28:6F:FB for host1 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)) || (defined(CONFIG_SUSE_KERNEL) && \
    LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 21))
    veth_mac[ETH_ALEN - 1] = (host_number == 0 ? 0xF9 : 0xFB);
    eth_hw_addr_set(dev, veth_mac);
#else
    dev->dev_addr[0x00] = 0x9c;
    dev->dev_addr[0x01] = 0x7d;
    dev->dev_addr[0x02] = 0xa3;
    dev->dev_addr[0x03] = 0x28;
    dev->dev_addr[0x04] = 0x6f;
    dev->dev_addr[0x05] = (host_number == 0 ? 0xf9 : 0xfb);
#endif

    VETH_LOG(DLOG_DEBUG, "set veth MAC addr OK\n");
}

static s32 veth_send_one_pkt(struct sk_buff *skb, int queue)
{
    u32 off, head, next_to_free;
    dma_addr_t dma = 0;
    int ret = 0;
    struct bspveth_rxtx_q *ptx_queue = g_bspveth_dev.ptx_queue[queue];

    if (!skb || !ptx_queue || !ptx_queue->pbdinfobase_v || !ptx_queue->pbdbase_v || !ptx_queue->pshmqhd_v) {
        INC_STATIS_RXTX(queue, null_point, 1, BSPVETH_TX);
        return BSP_ERR_NULL_POINTER;
    }

    if (!bma_intf_is_link_ok() || ptx_queue->pshmqhd_v->init != BSPVETH_SHMQUEUE_INITOK) {
        return -1;
    }

    head = ptx_queue->head;
    next_to_free = ptx_queue->next_to_free;

    /* stop to send pkt when queue is going to full when the rest space of ptx_queue is smaller than 3 */
    if (!JUDGE_TX_QUEUE_SPACE(head, next_to_free, 3)) {
        netif_stop_subqueue(g_bspveth_dev.pnetdev, queue);
        VETH_LOG(DLOG_DEBUG, "going to full, head: %d, nex to free: %d\n", head, next_to_free);
    }

    if (!JUDGE_TX_QUEUE_SPACE(head, next_to_free, 1)) {
        return BSP_NETDEV_TX_BUSY;
    }

    if (skb_shinfo(skb) && skb_shinfo(skb)->nr_frags) {
        /* We don't support frags */
        ret = skb_linearize(skb);
        if (ret != 0) {
            return -ENOMEM;
        }
    }

    dma = dma_map_single(&g_bspveth_dev.ppcidev->dev, skb->data, skb->len, DMA_TO_DEVICE);

    ret = dma_mapping_error(&g_bspveth_dev.ppcidev->dev, dma);
    if (ret != BSP_OK) {
        INC_STATIS_TX(queue, dma_mapping_err, 1);
        return BSP_ERR_DMA_ERR;
    }

    off = dma & 0x3;
    if (off) {
        INC_STATIS_TX(queue, dma_need_offset, 1);
    }

    ptx_queue->pbdinfobase_v[head].pdma_v = skb;
    ptx_queue->pbdbase_v[head].dma_p = dma & (~((u64)0x3));
    ptx_queue->pbdbase_v[head].off = off;
    ptx_queue->pbdbase_v[head].len = skb->len;

    mb();   /* memory barriers. */
    head = (head + 1) & BSPVETH_POINT_MASK;
    ptx_queue->head = head;

    VETH_LOG(DLOG_DEBUG, "[send]:oridma=0x%llx,skb->len=%d,head=%d,off=%d,alidma0x%llx\n", (u64)dma, skb->len, head,
             off, (u64)(dma & (~((u64)0x3))));

    return BSP_OK;
}

int veth_tx(struct sk_buff *skb, struct net_device *pstr_dev)
{
    u32 ul_ret = 0;
    int queue = 0;

    /* netdev_tx_t contract: a non-OK value here (the old code returned the raw driver
       error 0x0FFFF002) confuses the TX path, and the skb would be leaked. We drop the
       packet and report it as consumed, like the !skb/!dev path below does.
       NB: no INC_STATIS_TX() here - that macro dereferences ptx_queue[queue], which is
       exactly what is NULL in this branch. */
    if (g_bspveth_dev.ptx_queue[queue] == NULL) {
        if (skb != NULL) {
            dev_kfree_skb_any(skb);
        }
        return NETDEV_TX_OK;
    }

    VETH_LOG(DLOG_DEBUG, "===============enter==================\n");

    if (!skb || !pstr_dev) {
        INC_STATIS_TX(queue, null_point, 1);
        return NETDEV_TX_OK;
    }

    VETH_LOG(DLOG_DEBUG, "skb->len=%d\n", skb->len);

    ul_ret = (u32)veth_send_one_pkt(skb, queue);
    if (ul_ret == BSP_OK) {
        INC_STATIS_TX_TONETSTATS(queue, pkt, 1, tx_packets);
        INC_STATIS_TX_TONETSTATS(queue, pktbyte, skb->len, tx_bytes);

#ifndef USE_TASKLET
        (void)mod_timer(&g_bspveth_dev.dmatimer, jiffies_64);
#else
        tasklet_hi_schedule(&g_bspveth_dev.dma_task);
#endif
    } else {
        VETH_LOG(DLOG_DEBUG, "=======exit ret = %d=======\n", ul_ret);
        INC_STATIS_TX_TONETSTATS(queue, dropped_pkt, 1, tx_dropped);
        dev_kfree_skb_any(skb);
    }

    return NETDEV_TX_OK;
}

s32 veth_free_txskb(struct bspveth_rxtx_q *ptx_queue, int queue)
{
    int i, work_limit;
    unsigned int tail, next_to_free;
    struct bspveth_bd_info *ptx_bdinfo_v = NULL;
    struct sk_buff *skb = NULL;
    struct bspveth_dma_bd *pbd_v = NULL;

    if (ptx_queue == NULL || ptx_queue->pbdinfobase_v == NULL || ptx_queue->pbdbase_v == NULL
     || g_bspveth_dev.ptx_queue[queue] == NULL || g_bspveth_dev.ppcidev == NULL) {
        return BSP_ERR_AGAIN;
    }

    work_limit = (int)ptx_queue->work_limit;
    tail = ptx_queue->tail;
    next_to_free = ptx_queue->next_to_free;

    for (i = 0; i < work_limit; i++) {
        if (next_to_free == tail) {
            break;
        }

        ptx_bdinfo_v = &ptx_queue->pbdinfobase_v[next_to_free];

        pbd_v = &ptx_queue->pbdbase_v[next_to_free];

        skb = ptx_bdinfo_v->pdma_v;

        dma_unmap_single(&g_bspveth_dev.ppcidev->dev, pbd_v->dma_p | pbd_v->off, pbd_v->len, DMA_TO_DEVICE);

        if (skb) {
            dev_kfree_skb_any(skb);
        } else {
            VETH_LOG(DLOG_ERROR, "skb is NULL,tail=%d next_to_free=%d\n", tail, next_to_free);
        }

        ptx_bdinfo_v->pdma_v = NULL;
        INC_STATIS_TX(queue, freetx, 1);

        next_to_free = (next_to_free + 1) & BSPVETH_POINT_MASK;
    }

    mb();   /* memory barriers. */
    ptx_queue->next_to_free = next_to_free;
    tail = ptx_queue->tail;

    if (next_to_free != tail) {
        VETH_LOG(DLOG_DEBUG, "next_to_free(%d) != tail(%d)\n", next_to_free, tail);

        return BSP_ERR_AGAIN;
    }

    return BSP_OK;
}

static void set_veth_recv_pkt(struct bspveth_rxtx_q *prx_queue, u32 *ptail, int work_limit, int queue)
{
    int i;
    struct bspveth_bd_info *prx_bdinfo_v = NULL;
    struct sk_buff *skb = NULL;
    struct bspveth_dma_bd *pbd_v = NULL;
    u32 off = 0;
    if (prx_queue == NULL || ptail == NULL || prx_queue->pbdinfobase_v == NULL || prx_queue->pbdbase_v == NULL
     || g_bspveth_dev.ptx_queue[queue] == NULL || g_bspveth_dev.ppcidev == NULL) {
        return;
    }

    if (!prx_queue) {
        return;
    }

    for (i = 0; i < work_limit; i++) {
        if (*ptail == prx_queue->head) {
            break;
        }

        prx_bdinfo_v = &prx_queue->pbdinfobase_v[*ptail];

        skb = prx_bdinfo_v->pdma_v;
        if (!skb) {
            *ptail = (*ptail + 1) & BSPVETH_POINT_MASK;
            continue;
        }

        prx_bdinfo_v->pdma_v = NULL;
        pbd_v = &prx_queue->pbdbase_v[*ptail];

        off = pbd_v->off;
        if (off) {
            skb_reserve(skb, off);
        }

        dma_unmap_single(&g_bspveth_dev.ppcidev->dev, pbd_v->dma_p, BSPVETH_SKB_SIZE, DMA_FROM_DEVICE);

        *ptail = (*ptail + 1) & BSPVETH_POINT_MASK;

        skb_put(skb, pbd_v->len);

        skb->protocol = eth_type_trans(skb, g_bspveth_dev.pnetdev);
        skb->ip_summed = CHECKSUM_NONE;

        VETH_LOG(DLOG_DEBUG, "skb->len=%d,skb->protocol=%d\n", skb->len, skb->protocol);

        VETH_LOG(DLOG_DEBUG, "dma_p=0x%llx,dma_map=0x00,skb->len=%d,tail=%d,shm_off=%d\n", pbd_v->dma_p, skb->len,
                 *ptail, off);

        INC_STATIS_RX_TONETSTATS(queue, pkt, 1, rx_packets);
        INC_STATIS_RX_TONETSTATS(queue, pktbyte, skb->len, rx_bytes);

        if (netif_rx(skb) == NET_RX_DROP) {
            INC_STATIS_RX_TONETSTATS(queue, netifrx_err, 1, rx_errors);

            VETH_LOG(DLOG_DEBUG, "netif_rx failed\n");
        }
    }
}

s32 veth_recv_pkt(struct bspveth_rxtx_q *prx_queue, int queue)
{
    int ret;
    int work_limit;
    u32 tail, head;

    if (!prx_queue) {
        return BSP_ERR_AGAIN;
    }

    work_limit = (int)prx_queue->work_limit;
    tail = prx_queue->tail;

    set_veth_recv_pkt(prx_queue, &tail, work_limit, queue);

    mb();   /* memory barriers. */
    prx_queue->tail = tail;
    head = prx_queue->head;

    ret = veth_refill_rxskb(prx_queue, queue);
    if (ret != BSP_OK) {
        VETH_LOG(DLOG_DEBUG, "veth_refill_rxskb failed\n");
    }

    if (tail != head) {
        VETH_LOG(DLOG_DEBUG, "tail(%d) != head(%d)\n", tail, head);

        return BSP_ERR_AGAIN;
    }

    return BSP_OK;
}

#if !defined(USE_TASKLET) && defined(HAVE_TIMER_SETUP)
static void veth_skbtrtimer_do(struct timer_list *t)
#else
static void veth_skbtrtimer_do(unsigned long data)
#endif
{
    int ret = 0;

    ret = veth_skb_tr_task();
    if (ret == BSP_ERR_AGAIN) {
#ifndef USE_TASKLET
        (void)mod_timer(&g_bspveth_dev.skbtrtimer, jiffies_64);
#else
        tasklet_hi_schedule(&g_bspveth_dev.skb_task);
#endif
    }
}

s32 veth_skbtimer_close(void)
{
#ifndef USE_TASKLET
    (void)del_timer_sync(&g_bspveth_dev.skbtrtimer);
#else
    tasklet_kill(&g_bspveth_dev.skb_task);
#endif

    VETH_LOG(DLOG_DEBUG, "veth skbtimer close ok\n");

    return 0;
}

void veth_skbtimer_init(void)
{
#ifndef USE_TASKLET
#ifdef HAVE_TIMER_SETUP
    timer_setup(&g_bspveth_dev.skbtrtimer, veth_skbtrtimer_do, 0);
#else
    setup_timer(&g_bspveth_dev.skbtrtimer, veth_skbtrtimer_do, (unsigned long)&g_bspveth_dev);
#endif
    (void)mod_timer(&g_bspveth_dev.skbtrtimer, jiffies_64 + BSPVETH_SKBTIMER_INTERVAL);
#else
    tasklet_init(&g_bspveth_dev.skb_task, veth_skbtrtimer_do, (unsigned long)&g_bspveth_dev);
#endif

    VETH_LOG(DLOG_DEBUG, "veth skbtimer init OK\n");
}

static void veth_netdev_exit(void)
{
    if (g_bspveth_dev.pnetdev) {
        netif_stop_queue(g_bspveth_dev.pnetdev);
        unregister_netdev(g_bspveth_dev.pnetdev);
        free_netdev(g_bspveth_dev.pnetdev);

        VETH_LOG(DLOG_DEBUG, "veth netdev exit OK.\n");
    } else {
        VETH_LOG(DLOG_DEBUG, "veth_dev.pnetdev NULL.\n");
    }
}

static void veth_shutdown_task(struct work_struct *work)
{
    struct net_device *netdev = g_bspveth_dev.pnetdev;

    g_shutdown_flag = 1;

    VETH_LOG(DLOG_ERROR, "veth is going down, please restart it manual\n");

    g_bspveth_dev.shutdown_cnt++;

    if (netif_carrier_ok(netdev) != 0) {
        (void)bma_intf_unregister_int_notifier(&g_veth_int_nb);

        netif_carrier_off(netdev);

        bma_intf_set_open_status(g_bspveth_dev.bma_priv, DEV_CLOSE);

        /* can't transmit any more */
        netif_stop_queue(g_bspveth_dev.pnetdev);

        (void)veth_skbtimer_close();

        (void)veth_dmatimer_close_H();
    }
    g_shutdown_flag = 0;
}

static s32 veth_netdev_init(void)
{
    s32 l_ret = 0;
    struct net_device *netdev = NULL;

#if defined(LINUX_VERSION_CODE) && (KERNEL_VERSION(4, 1, 0) < LINUX_VERSION_CODE)
    netdev = alloc_netdev_mq(sizeof(struct tag_pcie_comm_priv), BSPVETH_DEV_NAME, NET_NAME_UNKNOWN,
                             veth_netdev_func_init, 1);
#else
    netdev = alloc_netdev_mq(sizeof(struct tag_pcie_comm_priv), BSPVETH_DEV_NAME, veth_netdev_func_init, 1);
#endif
    if (!netdev) {
        VETH_LOG(DLOG_ERROR, "alloc_netdev_mq failed!\n");
        return -ENOMEM;
    }

    /* register netdev */
    l_ret = register_netdev(netdev);
    if (l_ret < 0) {
        VETH_LOG(DLOG_ERROR, "register_netdev failed!ret=%d\n", l_ret);
        free_netdev(netdev);

        return -ENODEV;
    }

    g_bspveth_dev.pnetdev = netdev;

    VETH_LOG(DLOG_DEBUG, "veth netdev init OK\n");

    INIT_WORK(&g_bspveth_dev.shutdown_task, veth_shutdown_task);

    netif_carrier_off(netdev);

    return BSP_OK;
}

int veth_skb_tr_task(void)
{
    int rett = BSP_OK;
    int retr = BSP_OK;
    int i = 0;
    int task_state = BSP_OK;
    struct bspveth_rxtx_q *ptx_queue = NULL;
    struct bspveth_rxtx_q *prx_queue = NULL;

    for (i = 0; i < MAX_QUEUE_NUM; i++) {
        prx_queue = g_bspveth_dev.prx_queue[i];
        if (prx_queue) {
            g_bspveth_dev.run_skb_rx_task++;
            retr = veth_recv_pkt(prx_queue, i);
        }

        ptx_queue = g_bspveth_dev.ptx_queue[i];
        if (ptx_queue) {
            g_bspveth_dev.run_skb_fr_task++;
            rett = veth_free_txskb(ptx_queue, i);
            if (__netif_subqueue_stopped(g_bspveth_dev.pnetdev, i) &&
                JUDGE_TX_QUEUE_SPACE(ptx_queue->head, ptx_queue->next_to_free, 5)) { /* 5 代表判断ptx_queue大小是否不少于5 */
                netif_wake_subqueue(g_bspveth_dev.pnetdev, i);
                VETH_LOG(DLOG_DEBUG, "queue is free, head: %d, nex to free: %d\n", ptx_queue->head,
                         ptx_queue->next_to_free);
            }
        }

        if (rett == BSP_ERR_AGAIN || retr == BSP_ERR_AGAIN) {
            task_state = BSP_ERR_AGAIN;
        }
    }

    return task_state;
}

static int veth_int_handler(struct notifier_block *pthis, unsigned long ev, void *unuse)
{
    g_bspveth_dev.recv_int++;

    if (netif_running(g_bspveth_dev.pnetdev)) {
#ifndef USE_TASKLET
        (void)mod_timer(&g_bspveth_dev.dmatimer, jiffies_64);
#else
        tasklet_schedule(&g_bspveth_dev.dma_task);

#endif
    } else {
        VETH_LOG(DLOG_DEBUG, "netif is not running\n");
    }

    return IRQ_HANDLED;
}

#if !defined(USE_TASKLET) && defined(HAVE_TIMER_SETUP)
static void veth_dma_tx_timer_do_H(struct timer_list *t)
#else
static void veth_dma_tx_timer_do_H(unsigned long data)
#endif
{
    int txret, rxret;

    txret = veth_dma_task_H(BSPVETH_TX);

    rxret = veth_dma_task_H(BSPVETH_RX);
    if ((txret == BSP_ERR_AGAIN || rxret == BSP_ERR_AGAIN) && (g_shutdown_flag == 0)) {
#ifndef USE_TASKLET
        (void)mod_timer(&g_bspveth_dev.dmatimer, jiffies_64);
#else
        tasklet_hi_schedule(&g_bspveth_dev.dma_task);
#endif
    }
}

s32 veth_dmatimer_close_H(void)
{
#ifndef USE_TASKLET
    (void)del_timer_sync(&g_bspveth_dev.dmatimer);
#else
    tasklet_kill(&g_bspveth_dev.dma_task);
#endif

    VETH_LOG(DLOG_DEBUG, "bspveth_dmatimer_close RXTX TIMER ok\n");

    return 0;
}

void veth_dmatimer_init_H(void)
{
#ifndef USE_TASKLET
#ifdef HAVE_TIMER_SETUP
    timer_setup(&g_bspveth_dev.dmatimer, veth_dma_tx_timer_do_H, 0);
#else
    setup_timer(&g_bspveth_dev.dmatimer, veth_dma_tx_timer_do_H, (unsigned long)&g_bspveth_dev);
#endif
    (void)mod_timer(&g_bspveth_dev.dmatimer, jiffies_64 + BSPVETH_DMATIMER_INTERVAL);
#else
    tasklet_init(&g_bspveth_dev.dma_task, veth_dma_tx_timer_do_H, (unsigned long)&g_bspveth_dev);
#endif

    VETH_LOG(DLOG_DEBUG, "bspveth_dmatimer_init RXTX TIMER OK\n");
}

static s32 dmacmp_err_deal(struct bspveth_rxtx_q *prxtx_queue, u32 queue, u32 type)
{
    prxtx_queue->dmacmperr = 0;
    prxtx_queue->start_dma = 0;

    (void)veth_reset_dma(type);

    if (type == BSPVETH_RX) {
        VETH_LOG(DLOG_DEBUG, "bmc to host dma time out,dma count:%d,work_limit:%d\n", prxtx_queue->dmal_cnt,
                 prxtx_queue->work_limit);

        INC_STATIS_RX(queue, dma_failed, 1);
    } else {
        VETH_LOG(DLOG_DEBUG, "host to bmc dma time out,dma count:%d,work_limit:%d\n", prxtx_queue->dmal_cnt,
                 prxtx_queue->work_limit);

        INC_STATIS_TX(queue, dma_failed, 1);
    }

    if (prxtx_queue->dmal_cnt > 1) {
        prxtx_queue->work_limit = (prxtx_queue->dmal_cnt >> 1);
    }

    prxtx_queue->dma_overtime++;
    if (prxtx_queue->dma_overtime > BSPVETH_MAX_QUE_DEEP) {
        schedule_work(&g_bspveth_dev.shutdown_task);

        return -EFAULT;
    }

    return BSP_OK;
}

static s32 veth_check_dma_status(struct bspveth_rxtx_q *prxtx_queue, u32 queue, u32 type)
{
    int i = 0;
    enum dma_direction_e dir;

    dir = GET_DMA_DIRECTION(type);

    for (i = 0; i < BSPVETH_CHECK_DMA_STATUS_TIMES; i++) {
        if (bma_intf_check_dma_status(dir) == BSPVETH_DMA_OK) {
            break;
        }

        cpu_relax();

        /* 超过20次后，每次循环延时5ns */
        if (i > 20) {
            /* 延迟 5微妙 */
            udelay(5);
        }
    }

    if (i >= BSPVETH_CHECK_DMA_STATUS_TIMES) {
        INC_STATIS_RXTX(queue, dma_busy, 1, type);
        prxtx_queue->dmacmperr++;

        return -EFAULT;
    }

    return BSP_OK;
}

static s32 check_dmacmp_H(struct bspveth_rxtx_q *prxtx_queue, u32 queue, u32 type)
{
    s32 ret;
    u32 cnt;
    u32 host_head = 0;
    u32 host_tail = 0;
    u32 shm_head = 0;
    u32 shm_tail = 0;
    struct bspveth_shmq_hd *pshmq_head = NULL;

    if (!prxtx_queue || !prxtx_queue->pshmqhd_v) {
        return BSP_ERR_NULL_POINTER;
    }

    pshmq_head = prxtx_queue->pshmqhd_v;
    if (prxtx_queue->start_dma == 0) {
        return BSP_OK;
    }

    /* 当出错次数超过BD个数的4分之一时 */
    if (prxtx_queue->dmacmperr > BSPVETH_WORK_LIMIT / 4) {
        return dmacmp_err_deal(prxtx_queue, queue, type);
    }

    ret = veth_check_dma_status(prxtx_queue, queue, type);
    if (ret != BSP_OK) {
        return ret;
    }

    prxtx_queue->start_dma = 0;
    prxtx_queue->dma_overtime = 0;

    cnt = prxtx_queue->dmal_cnt;
    if (type == BSPVETH_RX) {
        host_head = prxtx_queue->head;
        shm_tail = pshmq_head->tail;

        pshmq_head->tail = (shm_tail + cnt) & BSPVETH_POINT_MASK;
        prxtx_queue->head = (host_head + cnt) & BSPVETH_POINT_MASK;

        INC_STATIS_RX(queue, dmapkt, cnt);
        INC_STATIS_RX(queue, dmapktbyte, prxtx_queue->dmal_byte);
    } else {
        host_tail = prxtx_queue->tail;
        shm_head = pshmq_head->head;

        prxtx_queue->tail = (host_tail + cnt) & BSPVETH_POINT_MASK;
        pshmq_head->head = (shm_head + cnt) & BSPVETH_POINT_MASK;

        INC_STATIS_TX(queue, dmapkt, cnt);
        INC_STATIS_TX(queue, dmapktbyte, prxtx_queue->dmal_byte);
    }

#ifndef USE_TASKLET
    (void)mod_timer(&g_bspveth_dev.skbtrtimer, jiffies_64);
#else
    tasklet_hi_schedule(&g_bspveth_dev.skb_task);
#endif

    (void)bma_intf_int_to_bmc(g_bspveth_dev.bma_priv);

    g_bspveth_dev.tobmc_int++;

    return BSP_OK;
}

static s32 checkspace_H(struct bspveth_rxtx_q *prxtx_queue, u32 queue, u32 type, u32 *pcnt)
{
    u32 host_head, host_tail, host_nextfill;
    u32 shm_head, shm_tail, shm_nextfill;
    u32 shm_cnt, host_cnt, cnt_tmp, cnt;

    if (!prxtx_queue || !prxtx_queue->pshmqhd_v) {
        return BSP_ERR_NULL_POINTER;
    }

    host_head = prxtx_queue->head;
    host_tail = prxtx_queue->tail;
    host_nextfill = prxtx_queue->next_to_fill;
    shm_head = prxtx_queue->pshmqhd_v->head;
    shm_tail = prxtx_queue->pshmqhd_v->tail;
    shm_nextfill = prxtx_queue->pshmqhd_v->next_to_fill;

    switch (type) {
        case BSPVETH_RX:
            if (shm_tail == shm_head) {
                INC_STATIS_RXTX(queue, shm_emp, 1, type);
                return BSP_ERR_NOT_TO_HANDLE;
            }

            if (!JUDGE_RX_QUEUE_SPACE(host_head, host_nextfill, 1)) {
                return -EFAULT;
            }

            shm_cnt = (shm_head - shm_tail) & BSPVETH_POINT_MASK;
            cnt_tmp = min(shm_cnt, prxtx_queue->work_limit);

            host_cnt = (host_nextfill - host_head) & BSPVETH_POINT_MASK;
            cnt = min(cnt_tmp, host_cnt);

            break;

        case BSPVETH_TX:
            if (host_tail == host_head) {
                INC_STATIS_RXTX(queue, q_emp, 1, type);
                return BSP_ERR_NOT_TO_HANDLE;
            }

            if (!JUDGE_TX_QUEUE_SPACE(shm_head, shm_nextfill, 1)) {
                return -EFAULT;
            }

            host_cnt = (host_head - host_tail) & BSPVETH_POINT_MASK;
            cnt_tmp = min(host_cnt, prxtx_queue->work_limit);
            shm_cnt = (shm_nextfill - (shm_head + 1)) & BSPVETH_POINT_MASK;
            cnt = min(cnt_tmp, shm_cnt);

            break;

        default:
            INC_STATIS_RXTX(queue, type_err, 1, type);
            return -EFAULT;
    }

    /* 当使用空降超过总空间 7 / 8时 */
    if (cnt > (BSPVETH_DMABURST_MAX * 7 / 8)) {
        INC_STATIS_RXTX(queue, dma_burst, 1, type);
    }

    *pcnt = cnt;

    return BSP_OK;
}

static int make_dmalistbd_h2b_H(struct bspveth_rxtx_q *prxtx_queue, u32 cnt, u32 type)
{
    u32 i = 0;
    u32 len = 0;
    u32 host_tail = 0;
    u32 shm_head = 0;
    u32 off = 0;
    struct bspveth_dmal *pdmalbase_v = NULL;
    struct bspveth_shmq_hd *pshmq_head = NULL;
    struct bspveth_bd_info *pbdinfobase_v = NULL;
    struct bspveth_dma_bd *pbdbase_v = NULL;
    struct bspveth_dma_shmbd *pshmbdbase_v = NULL;

    if (prxtx_queue == NULL) {
        return BSP_ERR_NULL_POINTER;
    }

    pdmalbase_v = prxtx_queue->pdmalbase_v;
    pshmq_head = prxtx_queue->pshmqhd_v;
    pbdinfobase_v = prxtx_queue->pbdinfobase_v;
    pbdbase_v = prxtx_queue->pbdbase_v;
    pshmbdbase_v = prxtx_queue->pshmbdbase_v;
    if (!pdmalbase_v || !pshmq_head || !pbdinfobase_v || !pbdbase_v || !pshmbdbase_v) {
        return BSP_ERR_NULL_POINTER;
    }

    host_tail = prxtx_queue->tail;
    shm_head = pshmq_head->head;

    for (i = 0; i < cnt; i++) {
        off = pbdbase_v[QUEUE_MASK(host_tail + i)].off;

        pdmalbase_v[i].chl = (i == cnt - 1) ? 0x9 : 0x0000001;

        if (pbdinfobase_v[QUEUE_MASK(host_tail + i)].pdma_v == NULL) {
            return BSP_ERR_NULL_POINTER;
        }
        pdmalbase_v[i].len = (pbdinfobase_v[QUEUE_MASK(host_tail + i)].pdma_v)->len;
        pdmalbase_v[i].slow = lower_32_bits(pbdbase_v[QUEUE_MASK(host_tail + i)].dma_p);
        pdmalbase_v[i].shi = upper_32_bits(pbdbase_v[QUEUE_MASK(host_tail + i)].dma_p);
        pdmalbase_v[i].dlow = lower_32_bits(pshmbdbase_v[QUEUE_MASK(shm_head + i)].dma_p);
        pdmalbase_v[i].dhi = 0;

        pshmbdbase_v[QUEUE_MASK(shm_head + i)].len = pdmalbase_v[i].len;

        pdmalbase_v[i].len += off;

        pshmbdbase_v[QUEUE_MASK(shm_head + i)].off = off;

        len += pdmalbase_v[i].len;
    }

    pdmalbase_v[i].chl = 0x7;
    pdmalbase_v[i].len = 0x0;
    pdmalbase_v[i].slow = lower_32_bits((u64)prxtx_queue->pdmalbase_p);
    pdmalbase_v[i].shi = upper_32_bits((u64)prxtx_queue->pdmalbase_p);
    pdmalbase_v[i].dlow = 0;
    pdmalbase_v[i].dhi = 0;

    prxtx_queue->dmal_cnt = cnt;
    prxtx_queue->dmal_byte = len;

    return 0;
}

static int make_dmalistbd_b2h_H(struct bspveth_rxtx_q *prxtx_queue, u32 cnt, u32 type)
{
    u32 i, len = 0, host_head, shm_tail, off;
    struct bspveth_dmal *pdmalbase_v = NULL;
    struct bspveth_shmq_hd *pshmq_head = NULL;
    struct bspveth_bd_info *pbdinfobase_v = NULL;
    struct bspveth_dma_bd *pbdbase_v = NULL;
    struct bspveth_dma_shmbd *pshmbdbase_v = NULL;

    if (!prxtx_queue) {
        VETH_LOG(DLOG_ERROR, "[END][makebd-B2H]:prxtx_queue NULL!!!\n");
        return BSP_ERR_NULL_POINTER;
    }

    pdmalbase_v = prxtx_queue->pdmalbase_v;
    pshmq_head = prxtx_queue->pshmqhd_v;
    pbdinfobase_v = prxtx_queue->pbdinfobase_v;
    pbdbase_v = prxtx_queue->pbdbase_v;
    pshmbdbase_v = prxtx_queue->pshmbdbase_v;

    if (!pdmalbase_v || !pshmq_head || !pbdinfobase_v || !pbdbase_v || !pshmbdbase_v) {
        VETH_LOG(DLOG_ERROR, "[END][makebd-B2H]:pdmalbase_v NULL!!!\n");
        return BSP_ERR_NULL_POINTER;
    }

    host_head = prxtx_queue->head;
    shm_tail = pshmq_head->tail;

    for (i = 0; i < cnt; i++) {
        off = pshmbdbase_v[QUEUE_MASK(shm_tail + i)].off;
        if (i == (cnt - 1)) {
            pdmalbase_v[i].chl = 0x9;
        } else {
            pdmalbase_v[i].chl = 0x0000001;
        }

        pdmalbase_v[i].len = pshmbdbase_v[QUEUE_MASK(shm_tail + i)].len;
        pdmalbase_v[i].slow = lower_32_bits(pshmbdbase_v[QUEUE_MASK(shm_tail + i)].dma_p);
        pdmalbase_v[i].shi = 0;
        pdmalbase_v[i].dlow = lower_32_bits(pbdbase_v[QUEUE_MASK(host_head + i)].dma_p);
        pdmalbase_v[i].dhi = upper_32_bits(pbdbase_v[QUEUE_MASK(host_head + i)].dma_p);
        pdmalbase_v[i].len += off;

        pbdbase_v[QUEUE_MASK(host_head + i)].off = off;
        pbdbase_v[QUEUE_MASK(host_head + i)].len = pdmalbase_v[i].len;

        len += pdmalbase_v[i].len;
    }

    pdmalbase_v[i].chl = 0x0000007;
    pdmalbase_v[i].len = 0x0;
    pdmalbase_v[i].slow = lower_32_bits((u64)prxtx_queue->pdmalbase_p);
    pdmalbase_v[i].shi = upper_32_bits((u64)prxtx_queue->pdmalbase_p);
    pdmalbase_v[i].dlow = 0;
    pdmalbase_v[i].dhi = 0;

    prxtx_queue->dmal_cnt = cnt;
    prxtx_queue->dmal_byte = len;

    return 0;
}

static s32 start_dmalist_H(struct bspveth_rxtx_q *prxtx_queue, u32 cnt, u32 type)
{
    int ret = BSP_OK;
    struct bma_dma_transfer_s dma_transfer;
    (void)memset_s(&dma_transfer, sizeof(struct bma_dma_transfer_s), 0, sizeof(struct bma_dma_transfer_s));

    if (!prxtx_queue) {
        return -1;
    }

    switch (type) {
        case BSPVETH_RX:
            ret = make_dmalistbd_b2h_H(prxtx_queue, cnt, type);
            if (ret) {
                goto failed;
            }
            dma_transfer.dir = BMC_TO_HOST;

            break;

        case BSPVETH_TX:
            ret = make_dmalistbd_h2b_H(prxtx_queue, cnt, type);
            if (ret) {
                goto failed;
            }
            dma_transfer.dir = HOST_TO_BMC;

            break;

        default:
            ret = -1;
            goto failed;
    }

    dma_transfer.type = DMA_LIST;
    dma_transfer.transfer.list.dma_addr = (dma_addr_t)prxtx_queue->pdmalbase_p;
    dma_transfer.pdmalbase_v = prxtx_queue->pdmalbase_v;
    dma_transfer.dmal_cnt = prxtx_queue->dmal_cnt;

    ret = bma_intf_start_dma(g_bspveth_dev.bma_priv, &dma_transfer);
    if (ret < 0) {
        goto failed;
    }

    prxtx_queue->start_dma = 1;

    return BSP_OK;

failed:
    return ret;
}

static int check_dma_queue_fault(struct bspveth_rxtx_q *prxtx_queue, u32 queue, u32 type, u32 *pcnt)
{
    int ret = BSP_OK;
    u32 cnt = 0;

    if (prxtx_queue->dma_overtime > BSPVETH_MAX_QUE_DEEP) {
        return -EFAULT;
    }

    ret = check_dmacmp_H(prxtx_queue, queue, type);
    if (ret != BSP_OK) {
        return -EFAULT;
    }

    ret = checkspace_H(prxtx_queue, queue, type, &cnt);
    if (ret != BSP_OK) {
        return -EFAULT;
    }

    if (CHECK_DMA_RXQ_FAULT(prxtx_queue, type, cnt)) {
        /* 延迟50微秒 */
        udelay(50);
        prxtx_queue->dmal_cnt--;

        return -EFAULT;
    }

    *pcnt = cnt;

    return BSP_OK;
}

s32 dma_rxtx_H(struct bspveth_rxtx_q *prxtx_queue, u32 queue, u32 type)
{
    int ret = BSP_OK;
    u32 cnt = 0;
    u32 shm_init;
    struct bspveth_shmq_hd *pshmq_head = NULL;

    if (!prxtx_queue || !prxtx_queue->pshmqhd_v) {
        return BSP_ERR_NULL_POINTER;
    }

    pshmq_head = prxtx_queue->pshmqhd_v;
    shm_init = pshmq_head->init;
    if (shm_init != BSPVETH_SHMQUEUE_INITOK) {
        INC_STATIS_RXTX(queue, shmqueue_noinit, 1, type);
        return -EFAULT;
    }

    if (CHECK_DMA_QUEUE_EMPTY(type, prxtx_queue)) {
        return BSP_OK;
    }

    ret = check_dma_queue_fault(prxtx_queue, queue, type, &cnt);
    if (ret != BSP_OK) {
        return -EFAULT;
    }

    ret = start_dmalist_H(prxtx_queue, cnt, type);
    if (ret != BSP_OK) {
        return -EFAULT;
    }

    /* 当剩余空间不超过16时 */
    if (cnt <= 16) {
        ret = check_dmacmp_H(prxtx_queue, queue, type);
        if (ret != BSP_OK) {
            return -EFAULT;
        }
    }

    return BSP_OK;
}

int veth_dma_task_H(u32 type)
{
    int i;
    struct bspveth_rxtx_q *prxtx_queue = NULL;

    for (i = 0; i < MAX_QUEUE_NUM; i++) {
        if (type == BSPVETH_RX) {
            g_bspveth_dev.run_dma_rx_task++;
            prxtx_queue = g_bspveth_dev.prx_queue[i];
        } else {
            g_bspveth_dev.run_dma_tx_task++;
            prxtx_queue = g_bspveth_dev.ptx_queue[i];
        }

        if (prxtx_queue != NULL && prxtx_queue->pshmqhd_v != NULL) {
            struct bspveth_shmq_hd *pshmq_head = prxtx_queue->pshmqhd_v;
            (void)dma_rxtx_H(prxtx_queue, i, type);
            if ((type == BSPVETH_RX && pshmq_head->head != pshmq_head->tail) ||
                (type == BSPVETH_TX && prxtx_queue->head != prxtx_queue->tail)) {
                return BSP_ERR_AGAIN;
            }
        }
    }

    return BSP_OK;
}

#ifdef __UT_TEST

s32 atu_config_H(struct pci_dev *pdev, unsigned int region, unsigned int hostaddr_h, unsigned int hostaddr_l,
                 unsigned int bmcaddr_h, unsigned int bmcaddr_l, unsigned int len)
{
    (void)pci_write_config_dword(pdev, 0x900, 0x80000000 + (region & 0x00000007));
    (void)pci_write_config_dword(pdev, 0x90c, hostaddr_l);
    (void)pci_write_config_dword(pdev, 0x910, hostaddr_h);
    (void)pci_write_config_dword(pdev, 0x914, hostaddr_l + len - 1);
    (void)pci_write_config_dword(pdev, 0x918, bmcaddr_l);
    (void)pci_write_config_dword(pdev, 0x91c, bmcaddr_h);
    /*  atu ctrl1 reg   */
    (void)pci_write_config_dword(pdev, 0x904, 0x00000000);
    /*  atu ctrl2 reg   */
    (void)pci_write_config_dword(pdev, 0x908, 0x80000000);

    return 0;
}

void bspveth_atu_config_H(void)
{
    (void)atu_config_H(g_bspveth_dev.ppcidev, 0x01, (sizeof(unsigned long) == SIZEOF_UL_IN_64BIT) ?
                       /* 32 用于适配不同操作系统，取高位值 */
                       ((u64)(g_bspveth_dev.phostrtc_p) >> 32) : 0, ((u64)(g_bspveth_dev.phostrtc_p) & 0xffffffff),
                       0, HOSTRTC_REG_BASE, HOSTRTC_REG_SIZE);

    (void)atu_config_H(g_bspveth_dev.ppcidev, 0x02, (sizeof(unsigned long) == SIZEOF_UL_IN_64BIT) ?
                       /* 32 用于适配不同操作系统，取高位值 */
                       ((u64)(g_bspveth_dev.pshmpool_p) >> 32) : 0, ((u64)(g_bspveth_dev.pshmpool_p) & 0xffffffff),
                       0, VETH_SHAREPOOL_BASE_INBMC, VETH_SHAREPOOL_SIZE);
}

void bspveth_pcie_free_H(void)
{
    struct pci_dev *pdev = g_bspveth_dev.ppcidev;

    if (pdev) {
        pci_disable_device(pdev);
    } else {
        VETH_LOG(DLOG_ERROR, "bspveth_dev.ppcidev  IS NULL\n");
    }

    VETH_LOG(DLOG_DEBUG, "bspveth_pcie_exit_H ok\n");
}

#endif

static void bspveth_host_exit_H(void)
{
    int ret = 0;

    ret = bma_intf_unregister_type((void **)&g_bspveth_dev.bma_priv);
    if (ret < 0) {
        VETH_LOG(DLOG_ERROR, "bma_intf_unregister_type failed\n");

        return;
    }

    VETH_LOG(DLOG_DEBUG, "bspveth host exit H OK\n");
}

static s32 bspveth_host_init_H(void)
{
    int ret = 0;
    struct bma_priv_data_s *bma_priv = NULL;

    ret = bma_intf_register_type(TYPE_VETH, 0, INTR_ENABLE, (void **)&bma_priv);
    if (ret != 0) {
        ret = -1;
        goto failed;
    }

    if (!bma_priv) {
        VETH_LOG(DLOG_ERROR, "bma_priv is NULL\n");
        return -1;
    }

    g_bspveth_dev.bma_priv = bma_priv;
    g_bspveth_dev.ppcidev = bma_priv->specific.veth.pdev;

    g_bspveth_dev.pshmpool_p = (u8 *)bma_priv->specific.veth.veth_swap_phy_addr;
    g_bspveth_dev.pshmpool_v = (u8 *)bma_priv->specific.veth.veth_swap_addr;
    g_bspveth_dev.shmpoolsize = bma_priv->specific.veth.veth_swap_len;

    VETH_LOG(DLOG_DEBUG, "bspveth host init H OK\n");

    return BSP_OK;

failed:
    return ret;
}

static int __init veth_init(void)
{
    int ret = BSP_OK;
    int buf_len = 0;

    if (!bma_intf_check_edma_supported()) {
        return -ENXIO;
    }

    (void)memset_s(&g_bspveth_dev, sizeof(g_bspveth_dev), 0, sizeof(g_bspveth_dev));

    buf_len = snprintf_s(g_bspveth_dev.name, NET_NAME_LEN, NET_NAME_LEN - 1, "%s", BSPVETH_DEV_NAME);
    if (buf_len < 0 || ((u32)buf_len >= (NET_NAME_LEN))) {
        VETH_LOG(DLOG_ERROR, "BSP_SNPRINTF lRet =0x%x\n", buf_len);
        return BSP_ERR_INVALID_STR;
    }

    ret = bspveth_host_init_H();
    if (ret != BSP_OK) {
        ret = -1;
        goto failed1;
    }

    ret = veth_netdev_init();
    if (ret != BSP_OK) {
        ret = -1;
        goto failed2;
    }

    GET_SYS_SECONDS(g_bspveth_dev.init_time);

    return BSP_OK;

failed2:
    bspveth_host_exit_H();

failed1:

    return ret;
}

static void __exit veth_exit(void)
{
    veth_netdev_exit();

    bspveth_host_exit_H();
}

MODULE_AUTHOR("HUAWEI TECHNOLOGIES CO., LTD.");
MODULE_DESCRIPTION("HUAWEI VETH DRIVER");
MODULE_LICENSE("GPL");
MODULE_VERSION(VETH_VERSION);

module_init(veth_init);
module_exit(veth_exit);
