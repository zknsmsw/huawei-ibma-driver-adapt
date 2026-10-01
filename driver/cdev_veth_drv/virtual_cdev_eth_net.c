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

#include "virtual_cdev_eth_net.h"
#include <linux/delay.h>

static edma_eth_dev_s g_eth_edmaprivate;
static edma_packet_node_s g_edma_recv_packet_tmp = { 0, NULL };
static edma_cut_packet_node_s *g_edma_send_cut_packet = NULL;
static unsigned int g_last_token = TK_START_END;
static unsigned int g_last_number = 0;
static unsigned int g_device_opened = 0;
static unsigned int g_read_pos = 0;

static unsigned int g_peer_not_ready = 0;

static const int NO_SPACE_RETRY = 10;
static const int NO_SPACE_WAIT_MS = 2000;
static const int CUT_PKG_SLEEP_MS = 1;
static const int CUT_PKG_LIMIT_COUNT = 30;
static const int SEND_INT_PKG_COUNT = 50;

static int cdev_open(struct inode *inode_ptr, struct file *filp);
static int cdev_release(struct inode *inode_ptr, struct file *filp);
static unsigned int cdev_poll(struct file *file, poll_table *wait);
static ssize_t cdev_read(struct file *filp, char __user *data, size_t count, loff_t *ppos);
static ssize_t cdev_write(struct file *filp, const char __user *data, size_t count, loff_t *ppos);

int debug = DLOG_ERROR;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Debug switch (0=close debug, 1=open debug)");

struct file_operations g_eth_edma_cdev_fops = {
    .owner = THIS_MODULE,
    .open = cdev_open,
    .release = cdev_release,
    .poll = cdev_poll,
    .read = cdev_read,
    .write = cdev_write,
};

static void dump_global_info(void)
{
    edma_shmq_hd_s *pshmqhd_v = NULL;

    if (debug == 0) {
        return;
    }

    LOG(DLOG_DEBUG, "\r\n==========================VETH INFO======================\r\n");

    pshmqhd_v = g_eth_edmaprivate.ptx_queue->pshmqhd_v;
    LOG(DLOG_DEBUG, "TX head/tail: %u/%u ------------", pshmqhd_v->head, pshmqhd_v->tail);

    pshmqhd_v = g_eth_edmaprivate.prx_queue->pshmqhd_v;
    LOG(DLOG_DEBUG, "RX head/tail: %u/%u ------------", pshmqhd_v->head, pshmqhd_v->tail);
}

static inline int edma_is_queue_ready(edma_rxtx_q_s *prxtx_queue)
{
    if (prxtx_queue == NULL) {
        return 0;
    }

    return (prxtx_queue->pshmqhd_v->init == BSPVETH_SHMQUEUE_INITOK_V2);
}

static inline void edma_veth_host_addr_init(struct bma_priv_data_s *priv)
{
    struct bma_priv_data_s *edma_priv = priv;

    g_eth_edmaprivate.pshmpool_p = (u8 *)edma_priv->specific.veth.veth_swap_phy_addr;
    g_eth_edmaprivate.pshmpool_v = (u8 *)edma_priv->specific.veth.veth_swap_addr;
    g_eth_edmaprivate.shmpoolsize = (u32)edma_priv->specific.veth.veth_swap_len;
}

static void edma_veth_free_tx_resources(edma_rxtx_q_s *ptx_queue)
{
    edma_bd_info_s *pbdinfobase_v = NULL;

    if ((ptx_queue == NULL) || (ptx_queue->pbdinfobase_v == NULL)) {
        return;
    }

    pbdinfobase_v = ptx_queue->pbdinfobase_v;
    ptx_queue->pbdinfobase_v = NULL;
    ptx_queue->pshmqhd_v = NULL;

    vfree(pbdinfobase_v);

    LOG(DLOG_DEBUG, "edma_veth_free_tx_resources ok. count=%d", ptx_queue->count);
}

static void edma_veth_free_all_tx_resources(edma_eth_dev_s *edma_eth)
{
    if ((edma_eth != NULL) && (edma_eth->ptx_queue != NULL)) {
        edma_veth_free_tx_resources(edma_eth->ptx_queue);
        kfree(edma_eth->ptx_queue);
        edma_eth->ptx_queue = NULL;
    }
}

static int edma_veth_setup_tx_resources(edma_rxtx_q_s *ptx_queue)
{
    int size;

    if (!ptx_queue) {
        return -EFAULT;
    }

    ptx_queue->count = MAX_QUEUE_BDNUM;
    size = sizeof(edma_bd_info_s) * ptx_queue->count;

    ptx_queue->pbdinfobase_v = (edma_bd_info_s *)vmalloc(size);
    if (!ptx_queue->pbdinfobase_v) {
        LOG(DLOG_ERROR, "Failed to alloc memory for the TX queue.");
        return -ENOMEM;
    }

    (void)memset_s(ptx_queue->pbdinfobase_v, size, 0, size);

    /* round up to nearest 4K */
    size = sizeof(edma_dma_shmbd_s) * ptx_queue->count;
    /* 4096 表示以最大4k页长对齐 */
    ptx_queue->size = ALIGN(size, 4096);

    ptx_queue->work_limit = BSPVETH_WORK_LIMIT;

    return 0;
}

static int edma_veth_setup_all_tx_resources(edma_eth_dev_s *edma_eth)
{
    int err = 0;
    u8 *shmq_head = NULL;
    u8 *shmq_head_p = NULL;
    edma_rxtx_q_s *tx_queue = NULL;
    int ret = 0;
    phys_addr_t veth_address = 0;

    ret = bma_intf_get_map_address(TYPE_VETH_ADDR, &veth_address);
    if (ret != 0) {
        return -EFAULT;
    }

    if (!edma_eth) {
        return -EFAULT;
    }

    tx_queue = (edma_rxtx_q_s *)kmalloc(sizeof(edma_rxtx_q_s), GFP_KERNEL);
    if (!tx_queue) {
        LOG(DLOG_ERROR, "Failed to alloc TX queue.");
        return -ENOMEM;
    }

    (void)memset_s(tx_queue, sizeof(edma_rxtx_q_s), 0, sizeof(edma_rxtx_q_s));

    shmq_head = edma_eth->pshmpool_v + (MAX_SHAREQUEUE_SIZE * 0);
    shmq_head_p = edma_eth->pshmpool_p + (MAX_SHAREQUEUE_SIZE * 0);

    tx_queue->pshmqhd_v = (edma_shmq_hd_s *)shmq_head;
    tx_queue->pshmqhd_p = shmq_head_p;

    tx_queue->pshmbdbase_v = (edma_dma_shmbd_s *)(shmq_head + BSPVETH_SHMBDBASE_OFFSET);
    tx_queue->pshmbdbase_p = shmq_head_p + BSPVETH_SHMBDBASE_OFFSET;

    tx_queue->pdmalbase_v = (edma_dmal_s *)(shmq_head + SHMDMAL_OFFSET);
    tx_queue->pdmalbase_p = (u8 *)(veth_address + (MAX_SHAREQUEUE_SIZE * 0) + SHMDMAL_OFFSET);

    (void)memset_s(tx_queue->pdmalbase_v, MAX_SHMDMAL_SIZE, 0, MAX_SHMDMAL_SIZE);

    err = edma_veth_setup_tx_resources(tx_queue);
    if (err != 0) {
        kfree(tx_queue);
        return err;
    }

    edma_eth->ptx_queue = tx_queue;

    return 0;
}

static int edma_veth_setup_rx_resources(edma_rxtx_q_s *prx_queue)
{
    int size;

    if (!prx_queue) {
        return -EFAULT;
    }

    prx_queue->count = MAX_QUEUE_BDNUM;
    size = sizeof(edma_bd_info_s) * prx_queue->count;

    prx_queue->pbdinfobase_v = (edma_bd_info_s *)vmalloc(size);
    if (!prx_queue->pbdinfobase_v) {
        LOG(DLOG_ERROR, "Failed to alloc memory for the RX queue.");
        return -ENOMEM;
    }

    (void)memset_s(prx_queue->pbdinfobase_v, size, 0, size);

    /* Round up to nearest 4K */
    size = sizeof(edma_dma_shmbd_s) * prx_queue->count;
    /* 4096 表示以最大页长4k对齐 */
    prx_queue->size = ALIGN(size, 4096);

    prx_queue->work_limit = BSPVETH_WORK_LIMIT;

    return 0;
}

static int edma_veth_setup_all_rx_resources(edma_eth_dev_s *edma_eth)
{
    int err = 0;
    u8 *shmq_head = NULL;
    u8 *shmq_head_p = NULL;
    edma_rxtx_q_s *rx_queue = NULL;
    int ret = 0;
    phys_addr_t veth_address = 0;

    ret = bma_intf_get_map_address(TYPE_VETH_ADDR, &veth_address);
    if (ret != 0) {
        return -EFAULT;
    }

    if (!edma_eth) {
        return -EFAULT;
    }

    rx_queue = (edma_rxtx_q_s *)kmalloc(sizeof(edma_rxtx_q_s), GFP_KERNEL);
    if (!rx_queue) {
        LOG(DLOG_ERROR, "Failed to alloc RX queue.");
        return -ENOMEM;
    }

    (void)memset_s(rx_queue, sizeof(edma_rxtx_q_s), 0, sizeof(edma_rxtx_q_s));

    shmq_head = edma_eth->pshmpool_v + MAX_SHAREQUEUE_SIZE;
    shmq_head_p = edma_eth->pshmpool_p + MAX_SHAREQUEUE_SIZE;
    rx_queue->pshmqhd_v = (edma_shmq_hd_s *)shmq_head;
    rx_queue->pshmqhd_p = shmq_head_p;

    rx_queue->pshmbdbase_v = (edma_dma_shmbd_s *)(shmq_head + BSPVETH_SHMBDBASE_OFFSET);
    rx_queue->pshmbdbase_p = shmq_head_p + BSPVETH_SHMBDBASE_OFFSET;

    /* DMA address list (only used in host). */
    rx_queue->pdmalbase_v = (edma_dmal_s *)(shmq_head + SHMDMAL_OFFSET);
    rx_queue->pdmalbase_p = (u8 *)(veth_address + MAX_SHAREQUEUE_SIZE + SHMDMAL_OFFSET);
    (void)memset_s(rx_queue->pdmalbase_v, MAX_SHMDMAL_SIZE, 0, MAX_SHMDMAL_SIZE);

    err = edma_veth_setup_rx_resources(rx_queue);
    if (err != 0) {
        kfree(rx_queue);
        return err;
    }

    edma_eth->prx_queue = rx_queue;

    return 0;
}

static void edma_veth_free_rx_resources(edma_rxtx_q_s *prx_queue)
{
    edma_bd_info_s *pbdinfobase_v = NULL;

    if (!prx_queue || !prx_queue->pbdinfobase_v) {
        return;
    }

    pbdinfobase_v = prx_queue->pbdinfobase_v;
    prx_queue->pbdinfobase_v = NULL;
    prx_queue->pshmqhd_v = NULL;

    /* Free all the Rx ring pages */
    vfree(pbdinfobase_v);

    LOG(DLOG_DEBUG, "edma_veth_free_rx_resources ok. count=%d", prx_queue->count);
}

static void edma_veth_free_all_rx_resources(edma_eth_dev_s *edma_eth)
{
    if (edma_eth && edma_eth->prx_queue) {
        edma_veth_free_rx_resources(edma_eth->prx_queue);
        kfree(edma_eth->prx_queue);
        edma_eth->prx_queue = NULL;
    }
}

static int edma_veth_setup_all_tx_queue(edma_eth_dev_s *edma_eth)
{
    void *buf = NULL;

    unsigned int i;
    unsigned int j;

    dma_addr_t dmaaddr;
    edma_bd_info_s *pbdinfobase_v = NULL;

    edma_rxtx_q_s *ptx_queue = NULL;

    struct bma_priv_data_s *priv = NULL;
    struct device *dev = NULL;

    if (!edma_eth) {
        return -EFAULT;
    }

    priv = (struct bma_priv_data_s *)edma_eth->edma_priv;
    dev = &priv->specific.veth.pdev->dev;

    ptx_queue = edma_eth->ptx_queue;

    edma_eth->pages_tx = 0;

    pbdinfobase_v = ptx_queue->pbdinfobase_v;

    for (i = 0; i < MAX_QUEUE_BDNUM; i++) {
        buf = kmalloc(NODE_SIZE, GFP_KERNEL | GFP_DMA);
        if (!buf) {
            for (j = 0; j < i; j++) {
                kfree((void *)ptx_queue->pbdinfobase_v[j].pdma_v);
            }
            LOG(DLOG_ERROR, "Fail to alloc tx buf.");
            return -ENOMEM;
        }

        dmaaddr = dma_map_single(dev, buf, NODE_SIZE, DMA_TO_DEVICE);
        if (dma_mapping_error(dev, dmaaddr)) {
            LOG(DLOG_ERROR, "Failed to map tx DMA address.");
            for (j = 0; j < i; j++) {
                kfree((void *)ptx_queue->pbdinfobase_v[j].pdma_v);
            }
            kfree(buf);

            return -EIO;
        }

        (void)memset_s(buf, NODE_SIZE, 0xFF, NODE_SIZE);

        pbdinfobase_v[i].pdma_v = (u8 *)(buf);
        pbdinfobase_v[i].dma_p = dmaaddr;
        pbdinfobase_v[i].len = NODE_SIZE;
    }

    LOG(DLOG_DEBUG, "set tx done.");

    return 0;
}

static int edma_veth_setup_all_rx_queue(edma_eth_dev_s *edma_eth)
{
    void *buf = NULL;

    unsigned int i;
    unsigned int j;

    dma_addr_t dmaaddr;
    edma_bd_info_s *pbdinfobase_v = NULL;

    edma_rxtx_q_s *prx_queue = NULL;

    struct bma_priv_data_s *priv = NULL;
    struct device *dev = NULL;

    if (!edma_eth) {
        return -EFAULT;
    }

    priv = (struct bma_priv_data_s *)edma_eth->edma_priv;
    dev = &priv->specific.veth.pdev->dev;

    prx_queue = edma_eth->prx_queue;

    edma_eth->pages_rx = 0;

    pbdinfobase_v = prx_queue->pbdinfobase_v;

    for (i = 0; i < MAX_QUEUE_BDNUM; i++) {
        buf = kmalloc(NODE_SIZE, GFP_KERNEL | GFP_DMA);
        if (!buf) {
            for (j = 0; j < i; j++) {
                kfree((void *)prx_queue->pbdinfobase_v[j].pdma_v);
            }
            LOG(DLOG_ERROR, "Fail to alloc rx buf.");

            return -ENOMEM;
        }

        dmaaddr = dma_map_single(dev, buf, NODE_SIZE, DMA_FROM_DEVICE);
        if (dma_mapping_error(dev, dmaaddr)) {
            LOG(DLOG_ERROR, "Failed to map rx DMA address.");
            for (j = 0; j < i; j++) {
                kfree((void *)prx_queue->pbdinfobase_v[j].pdma_v);
            }
            kfree(buf);

            return -EIO;
        }

        (void)memset_s(buf, NODE_SIZE, 0xFF, NODE_SIZE);

        pbdinfobase_v[i].pdma_v = (u8 *)(buf);
        pbdinfobase_v[i].dma_p = dmaaddr;
        pbdinfobase_v[i].len = NODE_SIZE;
    }

    LOG(DLOG_DEBUG, "set rx done.");

    return 0;
}

static int edma_veth_setup_all_rxtx_queue(edma_eth_dev_s *edma_eth)
{
    int err;

    if (!edma_eth) {
        return -EFAULT;
    }

    err = edma_veth_setup_all_tx_queue(edma_eth);
    if (err < 0) {
        return err;
    }

    err = edma_veth_setup_all_rx_queue(edma_eth);
    if (err < 0) {
        return err;
    }

    return 0;
}

static void edma_veth_dump(void)
{
    edma_eth_dev_s *edma_eth = &g_eth_edmaprivate;
    edma_rxtx_q_s *ptx_queue = edma_eth->ptx_queue;
    edma_rxtx_q_s *prx_queue = edma_eth->prx_queue;
    edma_shmq_hd_s *pshmq_head = NULL;

    if (debug == 0 || prx_queue == NULL || prx_queue->pshmqhd_v == NULL) {
        return;
    }

    pshmq_head = prx_queue->pshmqhd_v;

    LOG(DLOG_DEBUG,
        "RX host_head:%u, host_tail:%u, shm_head:%u, shm_tail:%u, "
        "count: %u, total: %u, init: %u.",
        prx_queue->head, prx_queue->tail, pshmq_head->head, pshmq_head->tail, pshmq_head->count, pshmq_head->total,
        pshmq_head->init);

    if (ptx_queue == NULL || ptx_queue->pshmqhd_v == NULL) {
        return;
    }

    pshmq_head = ptx_queue->pshmqhd_v;

    LOG(DLOG_DEBUG,
        "TX host_head:%u, host_tail:%u, shm_head:%u, shm_tail:%u, "
        "count: %u, total: %u, init: %u.",
        ptx_queue->head, ptx_queue->tail, pshmq_head->head, pshmq_head->tail, pshmq_head->count, pshmq_head->total,
        pshmq_head->init);
}

static int edma_veth_setup_resource(edma_eth_dev_s *edma_eth)
{
    int err;

    if (!edma_eth) {
        return -EFAULT;
    }

    err = edma_veth_setup_all_rx_resources(edma_eth);
    if (err < 0) {
        return err;
    }

    err = edma_veth_setup_all_tx_resources(edma_eth);
    if (err < 0) {
        goto FREE_RX;
    }

    err = edma_veth_setup_all_rxtx_queue(edma_eth);
    if (err < 0) {
        goto FREE_TX;
    }

    return 0;

FREE_TX:
    edma_veth_free_all_tx_resources(edma_eth);
FREE_RX:
    edma_veth_free_all_rx_resources(edma_eth);

    return err;
}

static void edma_veth_free_rxtx_queue(edma_eth_dev_s *edma_eth)
{
    int i;
    edma_rxtx_q_s *ptx_queue = NULL;
    edma_rxtx_q_s *prx_queue = NULL;

    struct bma_priv_data_s *priv = NULL;
    struct device *dev = NULL;

    edma_bd_info_s *pbdinfobase_v = NULL;

    if (!edma_eth || !edma_eth->edma_priv) {
        return;
    }

    priv = (struct bma_priv_data_s *)edma_eth->edma_priv;
    if (priv->specific.veth.pdev == NULL) {
        return;
    }

    dev = &priv->specific.veth.pdev->dev;

    ptx_queue = edma_eth->ptx_queue;
    prx_queue = edma_eth->prx_queue;
    if (ptx_queue == NULL || prx_queue == NULL) {
        return;
    }

    pbdinfobase_v = ptx_queue->pbdinfobase_v;
    if (pbdinfobase_v == NULL) {
        return;
    }

    for (i = 0; i < MAX_QUEUE_BDNUM; i++) {
        dma_unmap_single(dev, pbdinfobase_v[i].dma_p, NODE_SIZE, DMA_TO_DEVICE);
        kfree(pbdinfobase_v[i].pdma_v);
    }

    pbdinfobase_v = prx_queue->pbdinfobase_v;
    if (pbdinfobase_v == NULL) {
        return;
    }

    for (i = 0; i < MAX_QUEUE_BDNUM; i++) {
        dma_unmap_single(dev, pbdinfobase_v[i].dma_p, NODE_SIZE, DMA_FROM_DEVICE);
        kfree(pbdinfobase_v[i].pdma_v);
    }
}

static void edma_veth_free_resource(edma_eth_dev_s *edma_eth)
{
    edma_veth_free_rxtx_queue(edma_eth);
    LOG(DLOG_DEBUG, "edma_veth_free_rxtx_queue done.");

    edma_veth_free_all_rx_resources(edma_eth);
    LOG(DLOG_DEBUG, "edma_veth_free_all_rx_resources done.");

    edma_veth_free_all_tx_resources(edma_eth);
    LOG(DLOG_DEBUG, "edma_veth_free_all_tx_resources done.");
}

static int edma_veth_send_one_pkt(edma_cut_packet_node_s *cut_packet_node)
{
    u32 head, tail;
    edma_bd_info_s *pbdinfo_v = NULL;
    edma_rxtx_q_s *ptx_queue = g_eth_edmaprivate.ptx_queue;
    struct bma_priv_data_s *priv = NULL;
    struct device *dev = NULL;
    unsigned int i;
    int ret;

    if (!cut_packet_node || !ptx_queue || !ptx_queue->pshmbdbase_v) {
        LOG(DLOG_ERROR, "Invalid packet node.");
        return -EFAULT;
    }

    priv = (struct bma_priv_data_s *)(g_eth_edmaprivate.edma_priv);
    dev = &priv->specific.veth.pdev->dev;

    if (bma_intf_is_link_ok() == 0) {
        LOG(DLOG_ERROR, "EDMA link is not ready.");
        return -EIO;
    }

    for (i = 0; i < NO_SPACE_RETRY; i++) {
        head = ptx_queue->head;
        tail = ptx_queue->tail;

        LOG(DLOG_DEBUG, "TX queue, before: head/tail: %u/%u", head, tail);

        if (JUDGE_RING_QUEUE_SPACE(head, tail, 1)) {
            break;
        }

        if (i == NO_SPACE_RETRY - 1) {
            LOG(DLOG_ERROR, "EDMA queue has no space.");
            return -EBUSY;
        }

        LOG(DLOG_DEBUG, "TX queue, before: i:%d head/tail: %u/%u", i, head, tail);
        tasklet_hi_schedule(&g_eth_edmaprivate.dma_task);
        msleep(NO_SPACE_WAIT_MS);
    }

    ptx_queue->head = (head + 1) & BSPVETH_POINT_MASK;

    pbdinfo_v = ptx_queue->pbdinfobase_v + head;

    /* 3 用于补充计算cut_packet_node中前三个u32类型的长度 */
    pbdinfo_v->len = cut_packet_node->cut_packet_len + 3 * sizeof(u32);
    ret = memcpy_s(pbdinfo_v->pdma_v, NODE_SIZE, cut_packet_node, pbdinfo_v->len);
    if (ret != BSP_OK) {
        return -EINVAL;
    }

    /* Force sync data from CPU to device. */
    dma_sync_single_for_device(dev, pbdinfo_v->dma_p, pbdinfo_v->len, DMA_TO_DEVICE);

    LOG(DLOG_DEBUG, "TX queue, after: head/tail: %u -> %u\n", ptx_queue->head, ptx_queue->tail);

    return 0;
}

static inline unsigned int edma_veth_get_ring_buf_count(unsigned int head, unsigned int tail, unsigned int size)
{
    return (tail + size - head) % size;
}

static inline void edma_veth_flush_ring_node(edma_packet_node_s *node, unsigned int ring_len)
{
    unsigned int i;

    for (i = 0; i < ring_len; i++) {
        FREE_AND_NULL(node[i].packet);
    }
}

static int get_peer_queue_stress(edma_rxtx_q_s *queue)
{
    static int write_count = 0;
    int stress;

    if (++write_count < RL_MAX_PACKET) {
        /* not enough packets, use the last delay. */
        return -1;
    }

    write_count = 0;

    /* check peer rx queue stress. */
    if ((NULL == queue) || (0 == queue->pshmqhd_v->total)) {
        /* no rate limit allowed. */
        return 0;
    }

    /* 100 用于百分比换算 */
    stress = (int)((queue->pshmqhd_v->count * 100) / queue->pshmqhd_v->total);

    return stress;
}

static void do_queue_rate_limit(edma_rxtx_q_s *queue)
{
    static unsigned int delay_ms = 0;
    unsigned long delay_jiffies;
    int stress = get_peer_queue_stress(queue);

    LOG(DLOG_DEBUG, "count: %u, total: %u, stress: %d", queue->pshmqhd_v->count, queue->pshmqhd_v->total, stress);

    if (stress >= RL_STRESS_HIGH) {
        delay_ms = RL_DELAY_MS_HIGH;
    } else if (stress >= RL_STRESS_LOW) {
        delay_ms = RL_DELAY_MS_LOW;
    } else if (stress >= 0) {
        delay_ms = 0;
    }

    if (delay_ms != 0) {
        delay_jiffies = (unsigned long)msecs_to_jiffies(delay_ms);
        schedule_timeout_killable(delay_jiffies);
    }
}

static int edma_veth_cut_tx_packet_send(edma_eth_dev_s *eth_dev, const char __user *data, size_t len)
{
    int ret = 0;
    edma_cut_packet_node_s *tx_cut_pkt = g_edma_send_cut_packet;
    unsigned int length = len;
    unsigned int already_read_len = 0;
    unsigned int count = 0;

    if (!tx_cut_pkt)
        return -EFAULT;

    do_queue_rate_limit(eth_dev->ptx_queue);

    while (length > 0) {
        LOG(DLOG_DEBUG, "length: %u/%u", length, len);

        if (length > BSPPACKET_MTU_MAX) {
            /* fragment. */
            if (copy_from_user(tx_cut_pkt->cut_packet, data + already_read_len, BSPPACKET_MTU_MAX)) {
                LOG(DLOG_DEBUG, "Failed to copy user data.");
                return -EFAULT;
            }
            tx_cut_pkt->number = count++;
            length = length - BSPPACKET_MTU_MAX;

            tx_cut_pkt->token = tx_cut_pkt->number == 0 ? TK_START_PACKET : TK_MIDDLE_PACKET;
            tx_cut_pkt->cut_packet_len = BSPPACKET_MTU_MAX;
        } else {
            if (copy_from_user(tx_cut_pkt->cut_packet, data + already_read_len, length)) {
                LOG(DLOG_DEBUG, "Failed to copy user data.");
                return -EFAULT;
            }
            tx_cut_pkt->number = count++;
            tx_cut_pkt->token = len > BSPPACKET_MTU_MAX ? TK_END_PACKET : TK_START_END;
            tx_cut_pkt->cut_packet_len = length;
            length = 0;
        }

        already_read_len += tx_cut_pkt->cut_packet_len;
        ret = edma_veth_send_one_pkt(tx_cut_pkt);
        if (ret < 0) {
            LOG(DLOG_DEBUG, "edma_veth_send_one_pkt failed, %d.", ret);
            return ret;
        }

        if (length > 0 && count > CUT_PKG_LIMIT_COUNT) {
            LOG(DLOG_DEBUG, "Goto middle package, count:%d, need sleep.", count);
            msleep(CUT_PKG_SLEEP_MS);
            /* send a interrupt to BMC for receive package */
            if (count % SEND_INT_PKG_COUNT == 0) {
                tasklet_hi_schedule(&g_eth_edmaprivate.dma_task);
            }
        }
    }

    LOG(DLOG_DEBUG, "send done, length: %u", length);

    return 0;
}

static int edma_veth_copy_full_packet(edma_eth_dev_s *eth_dev, u8 *packet, u32 len)
{
    unsigned int count = 0;
    unsigned long flags = 0;
    u8 *ptr = NULL;
    int ret;

    LOG(DLOG_DEBUG, "Recv full packet, len %u.", len);

    ptr = (u8 *)kmalloc(len, GFP_ATOMIC);
    if (ptr) {
        /* lock the queue. */
        spin_lock_irqsave(&eth_dev->rx_queue_lock, flags);

        count = edma_veth_get_ring_buf_count(eth_dev->rx_packet_head, eth_dev->rx_packet_tail, MAX_RXTX_PACKET_LEN);
        if (count >= (MAX_RXTX_PACKET_LEN - 1)) {
            LOG(DLOG_DEBUG, "The rx queue is full.");
            spin_unlock_irqrestore(&eth_dev->rx_queue_lock, flags);
            kfree(ptr);
            return -EBUSY;
        }

        ret = memcpy_s(ptr, len, packet, len);
        if (ret != BSP_OK) {
            LOG(DLOG_DEBUG, "memcpy packet failed.");
            spin_unlock_irqrestore(&eth_dev->rx_queue_lock, flags);
            kfree(ptr);
            return -EINVAL;
        }
        eth_dev->rx_packet[eth_dev->rx_packet_tail].packet = ptr;
        eth_dev->rx_packet[eth_dev->rx_packet_tail].len = len;
        eth_dev->rx_packet_tail = (eth_dev->rx_packet_tail + 1) % MAX_RXTX_PACKET_LEN;

        spin_unlock_irqrestore(&eth_dev->rx_queue_lock, flags);

        return 0;
    }

    return -ENOMEM;
}

static int assemble_packet(edma_cut_packet_node_s *node, edma_packet_node_s *g_packet, unsigned int *p_copy_back)
{
    int ret = 0;
    if ((g_last_token == TK_START_END) || (g_last_token == TK_END_PACKET)) {
        /* This should be a new packet. */
        if ((node->token == TK_START_PACKET) || (node->token == TK_START_END)) {
            ret = memcpy_s(g_packet->packet, MAX_PACKET_LEN, node->cut_packet, node->cut_packet_len);
            if (ret != BSP_OK) {
                return -EINVAL;
            }

            g_packet->len = node->cut_packet_len;

            if (node->token == TK_START_END) {
                /* A full packet, increase tail. */
                *p_copy_back = 1;
            }
        } else {
            LOG(DLOG_ERROR, "The rx packet is out-of-order, token: %d, len: %u, number: %u", node->token,
                node->cut_packet_len, node->number);
            return -EINVAL;
        }
    } else {
        /* packet number incorrect */
        if (g_last_number != (node->number - 1)) {
            LOG(DLOG_ERROR, "The number is not correct (%u/%u)", g_last_number, node->number);
            return -EINVAL;
        }

        /* packet token incorrect */
        if (node->token != TK_MIDDLE_PACKET && node->token != TK_END_PACKET) {
            LOG(DLOG_ERROR, "The token %u is not expected", node->token);
            return -EINVAL;
        }

        ret = memcpy_s(g_packet->packet + g_packet->len, MAX_PACKET_LEN - g_packet->len, node->cut_packet,
                       node->cut_packet_len);
        if (ret != BSP_OK) {
            return -EINVAL;
        }

        g_packet->len = g_packet->len + node->cut_packet_len;
        if (node->token == TK_END_PACKET) {
            *p_copy_back = 1;
        }
    }
    return ret;
}

static int edma_veth_cut_rx_packet_recv(edma_eth_dev_s *eth_dev, u8 *packet, u32 len)
{
    int ret;
    edma_cut_packet_node_s *node = (edma_cut_packet_node_s *)packet;
    edma_packet_node_s *g_packet = &g_edma_recv_packet_tmp;
    unsigned int copy_back = 0;

    /* 3 用于补充计算node中前三个u32类型的长度，得到node的实际长度 */
    if (node->cut_packet_len && (len > node->cut_packet_len + 3 * sizeof(u32))) {
        /* 3 用于补充计算node中前三个u32类型的长度，得到node的实际长度 */
        len = node->cut_packet_len + 3 * sizeof(u32);
    }

    LOG(DLOG_DEBUG, "cut_packet_len: %u, token: %u/%u, number: %u, real length: %u.", node->cut_packet_len, node->token,
        g_last_token, node->number, len);

    if ((node->cut_packet_len > BSPPACKET_MTU_MAX) || ((g_packet->len + node->cut_packet_len) > MAX_PACKET_LEN)) {
        LOG(DLOG_ERROR, "This packet is too long, packet length %u/%u", node->cut_packet_len, g_packet->len);
        ret = -EINVAL;
        goto fail;
    }

    ret = assemble_packet(node, g_packet, &copy_back);
    if (ret != BSP_OK) {
        goto fail;
    }

    if (copy_back != 0) {
        ret = edma_veth_copy_full_packet(eth_dev, g_packet->packet, g_packet->len);
        g_packet->len = 0;
    }

    g_last_token = node->token;
    g_last_number = node->number;

    LOG(DLOG_DEBUG, "rx_packet_head:%u, rx_packet_tail: %u", eth_dev->rx_packet_head, eth_dev->rx_packet_tail);

    return (int)copy_back;

fail:
    g_last_token = TK_START_END;
    g_last_number = 0;
    (void)memset_s(g_packet->packet, MAX_PACKET_LEN, 0, MAX_PACKET_LEN);
    g_packet->len = 0;

    return ret;
}

static int edma_veth_recv_pkt(edma_rxtx_q_s *prx_queue, struct bma_priv_data_s *priv)
{
    int ret = BSP_OK;

    u32 i, work_limit;
    u32 tail, head;

    edma_bd_info_s *prx_bdinfo_v = NULL;
    struct device *dev = NULL;

    u8 *packet = NULL;
    u32 len;
    u32 off;

    wait_queue_head_t *queue_head = NULL;
    u8 do_wake_up = 0;

    if (priv == NULL || prx_queue == NULL || priv->specific.veth.pdev == NULL) {
        return BSP_OK;
    }

    dev = &priv->specific.veth.pdev->dev;

    work_limit = prx_queue->work_limit;
    tail = prx_queue->tail;

    for (i = 0; i < work_limit; i++) {
        head = prx_queue->head;

        if (tail == head) {
            break;
        }

        LOG(DLOG_DEBUG, "========= enter ===== [%u/%u] ======", head, tail);
        prx_bdinfo_v = prx_queue->pbdinfobase_v + tail;

        len = prx_bdinfo_v->len;
        off = prx_bdinfo_v->off;
        packet = prx_bdinfo_v->pdma_v;

        LOG(DLOG_DEBUG, "off:%u, len: %u.", off, len);

        if (g_device_opened == 0) {
            LOG(DLOG_DEBUG, "Local char device is not opened, drop packet");
            tail = BD_QUEUE_MASK(tail + 1);
            continue;
        }

        dma_sync_single_for_cpu(dev, prx_bdinfo_v->dma_p, len + off, DMA_FROM_DEVICE);

        if (off) {
            packet += off;
        }

        ret = edma_veth_cut_rx_packet_recv(&g_eth_edmaprivate, packet, len);
        if (ret < 0) {
            LOG(DLOG_DEBUG, "edma_veth_cut_rx_packet_recv fail, ret: %d", ret);
        } else if (0 != ret) {
            do_wake_up = 1;
        }

        tail = BD_QUEUE_MASK(tail + 1);
    }

    prx_queue->tail = tail;
    head = prx_queue->head;

    if (tail != head) {
        /* check if more processing is needed. */
        return BSP_ERR_AGAIN;
    } else if (do_wake_up) {
        queue_head = (wait_queue_head_t *)bma_cdev_get_wait_queue(priv);
        if (queue_head && waitqueue_active(queue_head)) {
            /* wake up the waiting process. */
            LOG(DLOG_DEBUG, "Wake up queue.");
            wake_up(queue_head);
        }
    }

    return BSP_OK;
}

static void edma_task_do_packet_recv(unsigned long data)
{
    int ret = BSP_OK;
    edma_rxtx_q_s *prx_queue = NULL;
    struct bma_priv_data_s *priv = NULL;
    struct tasklet_struct *t = (struct tasklet_struct *)data;

    priv = (struct bma_priv_data_s *)g_eth_edmaprivate.edma_priv;
    prx_queue = g_eth_edmaprivate.prx_queue;

    if (prx_queue != NULL) {
        g_eth_edmaprivate.run_skbRXtask++;

        ret = edma_veth_recv_pkt(prx_queue, priv);
    }

    if (ret == BSP_ERR_AGAIN) {
        tasklet_hi_schedule(t);
    }
}

static inline void edma_veth_reset_dma(int type)
{
    bma_intf_reset_dma(GET_DMA_DIRECTION(type));
}

static int dmacmp_err_deal_2(edma_rxtx_q_s *prxtx_queue, u32 type)
{
    if (!prxtx_queue) {
        return -EFAULT;
    }

    prxtx_queue->dmacmperr = 0;
    prxtx_queue->start_dma = 0;

    (void)edma_veth_reset_dma(type);

    if (type == BSPVETH_RX) {
        LOG(DLOG_DEBUG, "bmc to host dma time out, dma count:%d, work_limit:%d\n", prxtx_queue->dmal_cnt,
            prxtx_queue->work_limit);
        prxtx_queue->s.dma_failed += 1;
    } else {
        LOG(DLOG_DEBUG, "host to bmc dma time out, dma count:%d, work_limit:%d\n", prxtx_queue->dmal_cnt,
            prxtx_queue->work_limit);

        prxtx_queue->s.dma_failed += 1;
    }

    if (prxtx_queue->dmal_cnt > 1) {
        prxtx_queue->work_limit = (prxtx_queue->dmal_cnt >> 1);
    }

    prxtx_queue->dma_overtime++;
    if (prxtx_queue->dma_overtime > BSPVETH_MAX_QUE_DEEP) {
        return -EFAULT;
    }

    return BSP_OK;
}

static int edma_veth_check_dma_status(edma_rxtx_q_s *prxtx_queue, u32 type)
{
    int i = 0;
    enum dma_direction_e dir = GET_DMA_DIRECTION(type);

    if (!prxtx_queue) {
        return -EFAULT;
    }

    for (i = 0; i < BSPVETH_CHECK_DMA_STATUS_TIMES; i++) {
        if (bma_intf_check_dma_status(dir) == BSPVETH_DMA_OK) {
            return BSP_OK;
        }

        cpu_relax();

        /* 当检查次数超过20次时，每次延迟5微妙 */
        if (i > 20) {
            /* 5 表示延迟5微秒 */
            udelay(5);
        }
    }

    prxtx_queue->s.dma_busy += 1;
    prxtx_queue->dmacmperr++;

    return -EFAULT;
}

static int check_dmacmp_H_2(edma_rxtx_q_s *prxtx_queue, u32 type)
{
    u32 cnt = 0;
    u32 host_head = 0;
    u32 host_tail = 0;
    u32 shm_head = 0;
    u32 shm_tail = 0;
    s32 ret = 0;
    edma_shmq_hd_s *pshmq_head = NULL;

    if ((prxtx_queue == NULL) || (prxtx_queue->pshmqhd_v == NULL)) {
        return BSP_ERR_NULL_POINTER;
    }

    if (prxtx_queue->start_dma == 0) {
        return BSP_OK;
    }

    pshmq_head = prxtx_queue->pshmqhd_v;

    /* 当出错次数超过BD个数的4分之一时 */
    if (prxtx_queue->dmacmperr > BSPVETH_WORK_LIMIT / 4) {
        return dmacmp_err_deal_2(prxtx_queue, type);
    }

    ret = edma_veth_check_dma_status(prxtx_queue, type);
    if (ret != BSP_OK) {
        return ret;
    }

    prxtx_queue->start_dma = 0;
    prxtx_queue->dma_overtime = 0;

    if (type == BSPVETH_RX) {
        cnt = prxtx_queue->dmal_cnt;

        host_head = prxtx_queue->head;
        shm_tail = pshmq_head->tail;

        pshmq_head->tail = BD_QUEUE_MASK(shm_tail + cnt);
        prxtx_queue->head = BD_QUEUE_MASK(host_head + cnt);

        LOG(DLOG_DEBUG, "RX, host_head:%u, host_tail:%u, shm_head:%u, shm_tail:%u, inc: %u.", prxtx_queue->head,
            prxtx_queue->tail, pshmq_head->head, pshmq_head->tail, cnt);

        prxtx_queue->s.dmapkt += cnt;
        prxtx_queue->s.dmapktbyte += prxtx_queue->dmal_byte;
    } else {
        cnt = prxtx_queue->dmal_cnt;

        host_tail = prxtx_queue->tail;
        shm_head = pshmq_head->head;

        prxtx_queue->tail = BD_QUEUE_MASK(host_tail + cnt);
        pshmq_head->head = BD_QUEUE_MASK(shm_head + cnt);

        LOG(DLOG_DEBUG, "TX, host_head:%u, host_tail:%u, shm_head:%u, shm_tail:%u, inc: %u.", prxtx_queue->head,
            prxtx_queue->tail, pshmq_head->head, pshmq_head->tail, cnt);

        prxtx_queue->s.dmapkt += cnt;
        prxtx_queue->s.dmapktbyte += prxtx_queue->dmal_byte;
    }

    tasklet_hi_schedule(&g_eth_edmaprivate.skb_task);

    (void)bma_intf_int_to_bmc(g_eth_edmaprivate.edma_priv);

    g_eth_edmaprivate.tobmc_int++;

    return BSP_OK;
}

static int checkspace_H_2(edma_rxtx_q_s *prxtx_queue, u32 type, u32 *pcnt)
{
    u32 host_head, host_tail;
    u32 shm_head, shm_tail;
    u32 shm_cnt, host_cnt, cnt_tmp, cnt;
    edma_shmq_hd_s *pshmq_head = NULL;

    if (prxtx_queue == NULL || prxtx_queue->pshmqhd_v == NULL || !pcnt) {
        return -EFAULT;
    }

    pshmq_head = prxtx_queue->pshmqhd_v;

    host_head = prxtx_queue->head;
    host_tail = prxtx_queue->tail;
    shm_head = pshmq_head->head;
    shm_tail = pshmq_head->tail;

    LOG(DLOG_DEBUG, "host_head:%u, host_tail:%u, shm_head:%u, shm_tail:%u.", host_head, host_tail, shm_head, shm_tail);

    switch (type) {
        case BSPVETH_RX:
            if (shm_head == shm_tail) {
                prxtx_queue->s.shm_empty += 1;
                return BSP_ERR_NOT_TO_HANDLE;
            }

            if (!JUDGE_RING_QUEUE_SPACE(host_head, host_tail, 1)) {
                return -EFAULT;
            }

            shm_cnt = GET_BD_RING_QUEUE_COUNT(shm_head, shm_tail);
            cnt_tmp = min(shm_cnt, prxtx_queue->work_limit);

            host_cnt = GET_BD_RING_QUEUE_SPACE(host_tail, host_head);
            cnt = min(cnt_tmp, host_cnt);

            LOG(DLOG_DEBUG, "RX, host_cnt: %u, shm_cnt: %u, cnt_tmp: %u, cnt: %u", host_cnt, shm_cnt, cnt_tmp, cnt);

            break;

        case BSPVETH_TX:
            if (host_tail == host_head) {
                prxtx_queue->s.q_empty += 1;
                return BSP_ERR_NOT_TO_HANDLE;
            }

            host_cnt = GET_BD_RING_QUEUE_COUNT(host_head, host_tail);
            cnt_tmp = min(host_cnt, prxtx_queue->work_limit);

            shm_cnt = GET_BD_RING_QUEUE_SPACE(shm_head, shm_tail);
            cnt = min(cnt_tmp, shm_cnt);

            LOG(DLOG_DEBUG, "TX, host_cnt: %u, shm_cnt: %u, cnt_tmp: %u, cnt: %u", host_cnt, shm_cnt, cnt_tmp, cnt);

            break;

        default:
            prxtx_queue->s.type_err += 1;
            return -EFAULT;
    }

    /* 当占用空间达到最大空间 7 / 8 时 */
    if (cnt > ((BSPVETH_DMABURST_MAX * 7) / 8)) {
        prxtx_queue->s.dma_burst += 1;
    }

    *pcnt = cnt;

    return BSP_OK;
}

static int make_dmalistbd_h2b_H_2(edma_rxtx_q_s *prxtx_queue, u32 cnt)
{
    u32 host_tail, shm_head, i = 0, len = 0, off = 0;
    unsigned long addr;

    edma_dmal_s *pdmalbase_v = NULL;
    edma_bd_info_s *pbdinfobase_v = NULL;
    edma_dma_shmbd_s *pshmbdbase_v = NULL;

    if (prxtx_queue == NULL || prxtx_queue->pshmqhd_v == NULL) {
        return -EFAULT;
    }

    if (cnt == 0) {
        return 0;
    }

    pdmalbase_v = prxtx_queue->pdmalbase_v;
    pbdinfobase_v = prxtx_queue->pbdinfobase_v;
    pshmbdbase_v = prxtx_queue->pshmbdbase_v;

    host_tail = prxtx_queue->tail;
    shm_head = prxtx_queue->pshmqhd_v->head;

    if (pdmalbase_v == NULL || pbdinfobase_v == NULL || pshmbdbase_v == NULL) {
        return -EFAULT;
    }

    for (i = 0; i < cnt; i++) {
        LOG(DLOG_DEBUG, "TX DMA, HOST: %u -> BMC: %u", host_tail, shm_head);

        pdmalbase_v[i].chl = 0x1;

        addr = ((unsigned long)pbdinfobase_v[host_tail].dma_p) & (~EDMA_ADDR_ALIGN_MASK);
        off = addr & EDMA_ADDR_ALIGN_MASK;

        /* src: veth_send_one_pkt. */
        pdmalbase_v[i].slow = lower_32_bits(addr);
        pdmalbase_v[i].shi = upper_32_bits(addr);

        /* dst: bmc dma, in shared memory. */
        pdmalbase_v[i].dlow = lower_32_bits(pshmbdbase_v[shm_head].dma_p);
        pdmalbase_v[i].dhi = 0;

        /* len: len + offset caused by alignment */
        pdmalbase_v[i].len = pbdinfobase_v[host_tail].len + off;

        LOG(DLOG_DEBUG, "TX DMA %08x%08x -> %08x%08x, off: %u, len: %u.", pdmalbase_v[i].shi, pdmalbase_v[i].slow,
            pdmalbase_v[i].dhi, pdmalbase_v[i].dlow, off, pbdinfobase_v[host_tail].len);

        pshmbdbase_v[shm_head].len = pbdinfobase_v[host_tail].len;
        pshmbdbase_v[shm_head].off = off;

        len += pdmalbase_v[i].len;

        /* ready for the next round. */
        host_tail = BD_QUEUE_MASK(host_tail + 1);
        shm_head = BD_QUEUE_MASK(shm_head + 1);
    }

    pdmalbase_v[i - 1].chl = 0x9;

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

static int make_dmalistbd_b2h_H_2(edma_rxtx_q_s *prxtx_queue, u32 cnt)
{
    u32 i, len = 0;

    edma_dmal_s *pdmalbase_v = NULL;
    edma_shmq_hd_s *pshmq_head = NULL;
    edma_bd_info_s *pbdinfobase_v = NULL;
    edma_dma_shmbd_s *pshmbdbase_v = NULL;

    u32 host_head;
    u32 shm_tail;

    if (prxtx_queue == NULL || cnt == 0) {
        return -EFAULT;
    }

    pdmalbase_v = prxtx_queue->pdmalbase_v;
    pbdinfobase_v = prxtx_queue->pbdinfobase_v;
    pshmbdbase_v = prxtx_queue->pshmbdbase_v;

    pshmq_head = prxtx_queue->pshmqhd_v;

    host_head = prxtx_queue->head;

    if (pdmalbase_v == NULL || pbdinfobase_v == NULL || pshmbdbase_v == NULL || pshmq_head == NULL) {
        return -EFAULT;
    }
    shm_tail = pshmq_head->tail;

    for (i = 0; i < cnt; i++) {
        LOG(DLOG_DEBUG, "RX DMA, BMC: %u -> HOST: %u", shm_tail, host_head);

        pbdinfobase_v[host_head].off = pshmbdbase_v[shm_tail].off;
        pbdinfobase_v[host_head].len = pshmbdbase_v[shm_tail].len;

        pdmalbase_v[i].chl = 0x1;

        /* src: bmc set in shared memory. */
        pdmalbase_v[i].slow = lower_32_bits(pshmbdbase_v[shm_tail].dma_p);
        pdmalbase_v[i].shi = 0;

        /* dst: edma_veth_setup_all_rxtx_queue. */
        pdmalbase_v[i].dlow = lower_32_bits(pbdinfobase_v[host_head].dma_p);
        pdmalbase_v[i].dhi = upper_32_bits(pbdinfobase_v[host_head].dma_p);

        pdmalbase_v[i].len = pshmbdbase_v[shm_tail].len + pshmbdbase_v[shm_tail].off;

        LOG(DLOG_DEBUG, "RX DMA %08x%08x -> %08x%08x, off: %u, len: %u, total: %u.", pdmalbase_v[i].shi,
            pdmalbase_v[i].slow, pdmalbase_v[i].dhi, pdmalbase_v[i].dlow, pshmbdbase_v[shm_tail].off,
            pshmbdbase_v[shm_tail].len, pdmalbase_v[i].len);

        len += pdmalbase_v[i].len;

        /* ready for the next round. */
        host_head = BD_QUEUE_MASK(host_head + 1);
        shm_tail = BD_QUEUE_MASK(shm_tail + 1);
    }

    pdmalbase_v[i - 1].chl = 0x9;

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

static int start_dmalist_H_2(edma_rxtx_q_s *prxtx_queue, u32 type, u32 cnt)
{
    int ret = BSP_OK;
    struct bma_dma_transfer_s dma_transfer;
    edma_shmq_hd_s *pshmq_head = NULL;

    if (prxtx_queue == NULL || prxtx_queue->pshmqhd_v == NULL) {
        return -1;
    }

    (void)memset_s(&dma_transfer, sizeof(dma_transfer), 0, sizeof(dma_transfer));

    pshmq_head = prxtx_queue->pshmqhd_v;

    LOG(DLOG_DEBUG, "before -> %u/%u/%u/%u.", prxtx_queue->head, prxtx_queue->tail, pshmq_head->head, pshmq_head->tail);

    if (type == BSPVETH_RX) {
        dma_transfer.dir = BMC_TO_HOST;
        ret = make_dmalistbd_b2h_H_2(prxtx_queue, cnt);
    } else {
        dma_transfer.dir = HOST_TO_BMC;
        ret = make_dmalistbd_h2b_H_2(prxtx_queue, cnt);
    }

    if (ret < 0) {
        return ret;
    }

    dma_transfer.type = DMA_LIST;
    dma_transfer.transfer.list.dma_addr = (dma_addr_t)prxtx_queue->pdmalbase_p;
    dma_transfer.pdmalbase_v = (struct bspveth_dmal *)prxtx_queue->pdmalbase_v;
    dma_transfer.dmal_cnt = prxtx_queue->dmal_cnt;

    ret = bma_intf_start_dma(g_eth_edmaprivate.edma_priv, &dma_transfer);
    LOG(DLOG_DEBUG, "after -> %u/%u/%u/%u, ret: %d", prxtx_queue->head, prxtx_queue->tail, pshmq_head->head,
        pshmq_head->tail, ret);

    if (ret < 0) {
        return ret;
    }

    prxtx_queue->start_dma = 1;

    return BSP_OK;
}

static int check_dma_queue_fault_2(edma_rxtx_q_s *prxtx_queue, u32 type, u32 *pcnt)
{
    int ret = BSP_OK;
    u32 cnt = 0;

    if (!prxtx_queue || !pcnt) {
        return -EFAULT;
    }

    if (prxtx_queue->dma_overtime > BSPVETH_MAX_QUE_DEEP) {
        return -EFAULT;
    }

    ret = check_dmacmp_H_2(prxtx_queue, type);
    if (ret != BSP_OK) {
        return -EFAULT;
    }

    ret = checkspace_H_2(prxtx_queue, type, &cnt);
    if (ret != BSP_OK) {
        return -EFAULT;
    }

    if (CHECK_DMA_RXQ_FAULT(prxtx_queue, type, cnt)) {
        /* 延时50微秒 */
        udelay(50);

        prxtx_queue->dmal_cnt--;

        return -EFAULT;
    }

    *pcnt = cnt;

    return BSP_OK;
}

static int dma_rxtx_H_2(edma_rxtx_q_s *prxtx_queue, u32 type)
{
    int ret = BSP_OK;
    u32 cnt = 0;

    if (!prxtx_queue || !prxtx_queue->pshmqhd_v) {
        return -EFAULT;
    }

    if (CHECK_DMA_QUEUE_EMPTY(type, prxtx_queue)) {
        LOG(DLOG_DEBUG, "Queue (type: %u) is empty.", type);
        return BSP_OK;
    }

    ret = check_dma_queue_fault_2(prxtx_queue, type, &cnt);
    if (ret != BSP_OK) {
        LOG(DLOG_DEBUG, "check_dma_queue_fault_2 (ret: %d).", ret);
        return -EFAULT;
    }

    if (cnt == 0) {
        return BSP_OK;
    }

    ret = start_dmalist_H_2(prxtx_queue, type, cnt);
    if (ret != BSP_OK) {
        LOG(DLOG_DEBUG, "start_dmalist_H_2 returns %d", ret);
        return -EFAULT;
    }

    /* 当剩余空间少于16时 */
    if (cnt <= 16) {
        ret = check_dmacmp_H_2(prxtx_queue, type);
        if (ret != BSP_OK) {
            LOG(DLOG_DEBUG, "check_dmacmp_H_2 returns %d", ret);
            return -EFAULT;
        }
    }

    return BSP_OK;
}

static int veth_dma_task_H_2(u32 type)
{
    edma_rxtx_q_s *prxtx_queue = NULL;

    if (type == BSPVETH_RX) {
        g_eth_edmaprivate.run_dmaRXtask++;
        prxtx_queue = g_eth_edmaprivate.prx_queue;
    } else {
        g_eth_edmaprivate.run_dmaTXtask++;
        prxtx_queue = g_eth_edmaprivate.ptx_queue;
    }

    if (prxtx_queue != NULL) {
        if (edma_is_queue_ready(prxtx_queue) == 0) {
            LOG(DLOG_DEBUG, "queue is not ready, init flag: %u.", prxtx_queue->pshmqhd_v->init);
            return BSP_OK;
        }

        (void)dma_rxtx_H_2(prxtx_queue, type);

        if (!CHECK_DMA_QUEUE_EMPTY(type, prxtx_queue)) {
            return BSP_ERR_AGAIN;
        }
    }

    return BSP_OK;
}

static void edma_task_do_data_transmit(unsigned long data)
{
    struct tasklet_struct *t = (struct tasklet_struct *)data;
    int txret, rxret;

    LOG(DLOG_DEBUG,
        "host_head/host_tail/shm_head/shm_tail - "
        "rx:%u/%u/%u/%u, tx:%u/%u/%u/%u.",
        g_eth_edmaprivate.prx_queue->head, g_eth_edmaprivate.prx_queue->tail,
        g_eth_edmaprivate.prx_queue->pshmqhd_v->head, g_eth_edmaprivate.prx_queue->pshmqhd_v->tail,
        g_eth_edmaprivate.ptx_queue->head, g_eth_edmaprivate.ptx_queue->tail,
        g_eth_edmaprivate.ptx_queue->pshmqhd_v->head, g_eth_edmaprivate.ptx_queue->pshmqhd_v->tail);

    txret = veth_dma_task_H_2(BSPVETH_TX);

    rxret = veth_dma_task_H_2(BSPVETH_RX);

    LOG(DLOG_DEBUG,
        "host_head/host_tail/shm_head/shm_tail - "
        "rx:%u/%u/%u/%u, tx:%u/%u/%u/%u.\n",
        g_eth_edmaprivate.prx_queue->head, g_eth_edmaprivate.prx_queue->tail,
        g_eth_edmaprivate.prx_queue->pshmqhd_v->head, g_eth_edmaprivate.prx_queue->pshmqhd_v->tail,
        g_eth_edmaprivate.ptx_queue->head, g_eth_edmaprivate.ptx_queue->tail,
        g_eth_edmaprivate.ptx_queue->pshmqhd_v->head, g_eth_edmaprivate.ptx_queue->pshmqhd_v->tail);

    if ((txret == BSP_ERR_AGAIN) || (rxret == BSP_ERR_AGAIN)) {
        /* restart transmission. */
        tasklet_hi_schedule(t);
    }
}

static int edma_tasklet_setup(edma_eth_dev_s *dev, u8 **rx_buf, edma_cut_packet_node_s **tx_cut_pkt_buf)
{
    u8 *rx_pkt_buf;
    edma_packet_node_s *rx_packet = NULL;
    edma_cut_packet_node_s *tx_cut_buf = NULL;
    size_t rx_size = sizeof(edma_packet_node_s) * MAX_RXTX_PACKET_LEN;

    if (!dev || !rx_buf || !tx_cut_pkt_buf) {
        return -EFAULT;
    }

    rx_pkt_buf = (u8 *)kmalloc(MAX_PACKET_LEN, GFP_KERNEL);
    if (!rx_pkt_buf) {
        return -ENOMEM;
    }

    tx_cut_buf = (edma_cut_packet_node_s *)kmalloc(sizeof(*tx_cut_buf), GFP_KERNEL);
    if (!tx_cut_buf) {
        kfree(rx_pkt_buf);
        return -ENOMEM;
    }

    rx_packet = (edma_packet_node_s *)kmalloc(rx_size, GFP_KERNEL);
    if (!rx_packet) {
        kfree(rx_pkt_buf);
        kfree(tx_cut_buf);
        return -ENOMEM;
    }

    (void)memset_s(rx_pkt_buf, MAX_PACKET_LEN, 0, MAX_PACKET_LEN);
    (void)memset_s(tx_cut_buf, sizeof(*tx_cut_buf), 0, sizeof(*tx_cut_buf));
    (void)memset_s(rx_packet, rx_size, 0, rx_size);

    *rx_buf = rx_pkt_buf;
    *tx_cut_pkt_buf = tx_cut_buf;
    dev->rx_packet = rx_packet;

    spin_lock_init(&dev->rx_queue_lock);

    tasklet_init(&dev->skb_task, edma_task_do_packet_recv, (unsigned long)&dev->skb_task);

    tasklet_init(&dev->dma_task, edma_task_do_data_transmit, (unsigned long)&dev->dma_task);

    return 0;
}

static void edma_tasklet_free(edma_eth_dev_s *dev, u8 **rx_buf, edma_cut_packet_node_s **tx_cut_pkt_buf)
{
    if (dev == NULL || rx_buf == NULL || tx_cut_pkt_buf == NULL || dev->rx_packet == NULL) {
        return;
    }

    /* stop task before releasing resource. */
    tasklet_kill(&dev->dma_task);
    tasklet_kill(&dev->skb_task);

    kfree(*rx_buf);
    kfree(*tx_cut_pkt_buf);

    /* flush the ring buf. */
    edma_veth_flush_ring_node(dev->rx_packet, MAX_RXTX_PACKET_LEN);
    kfree(dev->rx_packet);

    *rx_buf = NULL;
    *tx_cut_pkt_buf = NULL;
    dev->rx_packet = NULL;
}

static int edma_veth_int_handler(struct notifier_block *nb, unsigned long ev, void *unuse)
{
    g_eth_edmaprivate.recv_int++;

    if (g_eth_edmaprivate.dma_task.func) {
        tasklet_hi_schedule(&g_eth_edmaprivate.dma_task);
    }

    return IRQ_HANDLED;
}

static struct notifier_block g_edma_veth_int_nb = {
    .notifier_call = edma_veth_int_handler,
};

static int comm_init_dev(edma_eth_dev_s *edma, struct file_operations *fops)
{
    cdev_dev_s *dev = &edma->cdev;
    int ret = 0;

    dev->priv = edma->edma_priv;
    dev->dev.minor = MISC_DYNAMIC_MINOR;
    dev->dev.name = CDEV_VETH_NAME;
    dev->dev.fops = fops;

    ret = misc_register(&dev->dev);
    if (ret < 0) {
        LOG(DLOG_ERROR, "Failed to alloc major number, %d", ret);
        return ret;
    }

    return 0;
}

static inline void comm_cleanup_dev(edma_eth_dev_s *edma)
{
    cdev_dev_s *dev = &edma->cdev;

    misc_deregister(&dev->dev);
}

static int __init edma_cdev_init(void)
{
    int ret;
    LOG(DLOG_DEBUG, "Module init.");

    if (!bma_intf_check_edma_supported()) {
        return -ENXIO;
    }

    (void)memset_s(&g_eth_edmaprivate, sizeof(g_eth_edmaprivate), 0, sizeof(g_eth_edmaprivate));

    /* register EDMA sub-subyem. */
    ret = bma_intf_register_type(TYPE_VETH, 0, INTR_ENABLE, &g_eth_edmaprivate.edma_priv);
    if (ret < 0) {
        LOG(DLOG_ERROR, "Failed to register EDMA interface.");
        goto failed;
    }

    /* initialize host DMA address. */
    edma_veth_host_addr_init(g_eth_edmaprivate.edma_priv);

    /* setup TX/RX resource */
    ret = edma_veth_setup_resource(&g_eth_edmaprivate);
    if (ret < 0) {
        LOG(DLOG_ERROR, "Failed to setup resource.");
        goto failed1;
    }

    /* setup resource for user packets. */
    ret = edma_tasklet_setup(&g_eth_edmaprivate, &g_edma_recv_packet_tmp.packet, &g_edma_send_cut_packet);
    if (ret < 0) {
        goto failed2;
    }

    /* register char device. */
    ret = comm_init_dev(&g_eth_edmaprivate, &g_eth_edma_cdev_fops);
    if (ret != 0) {
        LOG(DLOG_ERROR, "Failed to register cdev device.");
        goto failed3;
    }

    /* register EDMA INT notifier. */
    ret = bma_intf_register_int_notifier(&g_edma_veth_int_nb);
    if (ret < 0) {
        LOG(DLOG_ERROR, "Failed to register INT notifier.");
        goto failed4;
    }

    dump_global_info();

    GET_SYS_SECONDS(g_eth_edmaprivate.init_time);

    return 0;

failed4:
    comm_cleanup_dev(&g_eth_edmaprivate);
failed3:
    edma_tasklet_free(&g_eth_edmaprivate, &g_edma_recv_packet_tmp.packet, &g_edma_send_cut_packet);
failed2:
    edma_veth_free_resource(&g_eth_edmaprivate);
failed1:
    (void)bma_intf_unregister_type(&g_eth_edmaprivate.edma_priv);
failed:
    return ret;
}

static void __exit edma_cdev_exit(void)
{
    LOG(DLOG_DEBUG, "Module exit.");

    bma_intf_unregister_int_notifier(&g_edma_veth_int_nb);

    comm_cleanup_dev(&g_eth_edmaprivate);

    edma_tasklet_free(&g_eth_edmaprivate, &g_edma_recv_packet_tmp.packet, &g_edma_send_cut_packet);

    edma_veth_free_resource(&g_eth_edmaprivate);

    bma_intf_unregister_type(&g_eth_edmaprivate.edma_priv);

    return;
}

static inline int cdev_check_ring_recv(void)
{
    unsigned int count;

    count = edma_veth_get_ring_buf_count(g_eth_edmaprivate.rx_packet_head, g_eth_edmaprivate.rx_packet_tail,
                                         MAX_RXTX_PACKET_LEN);
    return (count > 0 ? 1 : 0);
}

static ssize_t cdev_copy_packet_to_user(edma_eth_dev_s *dev, char __user *data, size_t count)
{
    unsigned char *packet = NULL;
    unsigned char *start = NULL;
    unsigned int free_packet = 0;
    unsigned long flags = 0;
    ssize_t length = (ssize_t)count;
    ssize_t left;

    LOG(DLOG_DEBUG, "rx_packet_head:%u, rx_packet_tail: %u", dev->rx_packet_head, dev->rx_packet_tail);

    spin_lock_irqsave(&dev->rx_queue_lock, flags);

    if (!cdev_check_ring_recv()) {
        spin_unlock_irqrestore(&dev->rx_queue_lock, flags);
        return -EAGAIN;
    }

    left = (ssize_t)(dev->rx_packet[dev->rx_packet_head].len) - g_read_pos;
    start = dev->rx_packet[dev->rx_packet_head].packet + g_read_pos;

    LOG(DLOG_DEBUG, "User needs %ld bytes, pos: %ld, total len: %u, left: %ld.", count, g_read_pos,
        dev->rx_packet[dev->rx_packet_head].len, left);
    if (left <= 0) {
        /* No more data in this message, retry. */
        length = -EAGAIN;
        free_packet = 1;
    } else if (length > left) {
        /* A full message is returned. */
        length = left;
        free_packet = 1;
    } else {
        /* Update pos. */
        g_read_pos += length;
    }

    if (free_packet) {
        g_read_pos = 0;
        packet = dev->rx_packet[dev->rx_packet_head].packet;
        dev->rx_packet[dev->rx_packet_head].packet = NULL;
        dev->rx_packet_head = (dev->rx_packet_head + 1) % MAX_RXTX_PACKET_LEN;
    }

    spin_unlock_irqrestore(&dev->rx_queue_lock, flags);

    if ((length > 0) && copy_to_user(data, start, length)) {
        LOG(DLOG_DEBUG, "Failed to copy data to userspace, skip this message.");
        length = -EFAULT;
        g_read_pos = 0;
    }

    LOG(DLOG_DEBUG, "Copied bytes: %ld, pos: %ld, buf len: %lu, free_packet: %d.", length, g_read_pos, count,
        free_packet);

    if (packet != NULL) {
        /* Free the packet as needed. */
        kfree(packet);
    }

    return length;
}

int cdev_open(struct inode *inode_ptr, struct file *filp)
{
    cdev_dev_s *dev = &g_eth_edmaprivate.cdev;

    LOG(DLOG_DEBUG, "Open device.");

    if ((inode_ptr == NULL) || (filp == NULL)) {
        return -EFAULT;
    }

    /* only one instance is allowed. */
    if (g_device_opened != 0) {
        return -EBUSY;
    }

    LOG(DLOG_DEBUG, "Init flag, rx: %d, tx:%d", g_eth_edmaprivate.prx_queue->pshmqhd_v->init,
        g_eth_edmaprivate.ptx_queue->pshmqhd_v->init);

    // save to private data.
    filp->private_data = dev;
    g_device_opened = 1;
    g_read_pos = 0;

    return 0;
}

int cdev_release(struct inode *inode_ptr, struct file *filp)
{
    LOG(DLOG_DEBUG, "Close device.");

    if (!filp) {
        return 0;
    }

    filp->private_data = NULL;

    g_device_opened = 0;

    return 0;
}

unsigned int cdev_poll(struct file *filp, poll_table *wait)
{
    unsigned int mask = 0;
    wait_queue_head_t *queue_head = NULL;

    if (filp == NULL || filp->private_data == NULL) {
        return 0;
    }

    edma_veth_dump();

    queue_head = (wait_queue_head_t *)bma_cdev_get_wait_queue(((cdev_dev_s *)(filp->private_data))->priv);
    if (queue_head == NULL) {
        return 0;
    }

    /* check or add to wait queue. */
    poll_wait(filp, queue_head, wait);

    if (!edma_is_queue_ready(g_eth_edmaprivate.prx_queue)) {
        return 0;
    }

    if (cdev_check_ring_recv() > 0) {
        mask = (POLLIN | POLLRDNORM);
    }

    return mask;
}

ssize_t cdev_read(struct file *filp, char __user *data, size_t count, loff_t *ppos)
{
    edma_eth_dev_s *dev = &g_eth_edmaprivate;
    ssize_t length = 0;

    if ((data == NULL) || (count >= MAX_PACKET_LEN)) {
        return -EFAULT;
    }

    LOG(DLOG_DEBUG, "read begin, count: %ld, pos: %u.", count, g_read_pos);

    length = cdev_copy_packet_to_user(dev, data, count);

    LOG(DLOG_DEBUG, "read done, length: %ld, pos: %u.", length, g_read_pos);

    return length;
}

ssize_t cdev_write(struct file *filp, const char __user *data, size_t count, loff_t *ppos)
{
    int ret = 0;
    edma_eth_dev_s *pdev = &g_eth_edmaprivate;

    if ((data == NULL) || (count == 0) || (count > MAX_PACKET_LEN)) {
        return -EINVAL;
    }

    if (!edma_is_queue_ready(pdev->ptx_queue)) {
        if ((g_peer_not_ready == 0) && (pdev->ptx_queue != NULL) && (pdev->ptx_queue->pshmqhd_v != NULL)) {
            LOG(DLOG_ERROR, "Peer rx queue is not ready (%u).", pdev->ptx_queue->pshmqhd_v->init);
            g_peer_not_ready = 1;
        }
        return -EPERM;
    } else if (g_peer_not_ready) {
        LOG(DLOG_ERROR, "Peer rx queue becomes ready.");
        g_peer_not_ready = 0;
    }

    if (pdev->ptx_queue != NULL && pdev->ptx_queue->pshmqhd_v != NULL) {
        LOG(DLOG_DEBUG, "data length is %lu, pos: %u (%u/%u)", count, g_read_pos, pdev->ptx_queue->pshmqhd_v->count,
            pdev->ptx_queue->pshmqhd_v->total);
    }

    ret = edma_veth_cut_tx_packet_send(pdev, data, count);
    if (ret < 0) {
        LOG(DLOG_ERROR, "Failed to send packet, return code: %d.", ret);
    } else {
        tasklet_hi_schedule(&g_eth_edmaprivate.dma_task);
        ret = (int)count;
    }

    return ret;
}

MODULE_VERSION(MICRO_TO_STR(CDEV_VETH_VERSION));
MODULE_AUTHOR("HUAWEI TECHNOLOGIES CO., LTD.");
MODULE_DESCRIPTION("Hi171x Intelligent Management system chip CDEV driver");
MODULE_LICENSE("GPL");

module_init(edma_cdev_init);
module_exit(edma_cdev_exit);
