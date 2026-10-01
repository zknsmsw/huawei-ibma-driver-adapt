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

#include <linux/io.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/miscdevice.h>
#include "bma_include.h"
#include "bma_ker_intf.h"
#include "securec.h"

#define CDEV_NAME_PREFIX "hwibmc"

#ifdef DRV_VERSION
#define CDEV_VERSION MICRO_TO_STR(DRV_VERSION)
#else
#define CDEV_VERSION "0.4.0"
#endif

#define CDEV_DEFAULT_NUM 4
#define CDEV_MAX_NUM 8

#define CDEV_NAME_MAX_LEN 32
#define CDEV_INVALID_ID (0xffffffff)

struct cdev_statistics_s {
    unsigned int recv_bytes;
    unsigned int send_bytes;
    unsigned int send_pkgs;
    unsigned int recv_pkgs;
    unsigned int send_failed_count;
    unsigned int recv_failed_count;
    unsigned int open_status;
};

struct cdev_dev {
    struct miscdevice dev_struct;
    struct cdev_statistics_s s;
    char dev_name[CDEV_NAME_MAX_LEN];
    dev_t dev_id;
    void *dev_data;
    atomic_t open;
    int type;
};

struct cdev_dev_set {
    struct cdev_dev dev_list[CDEV_MAX_NUM];
    int dev_num;
    unsigned int init_time;
};

int dev_num = CDEV_DEFAULT_NUM; /* the dev num want to create */
int debug = DLOG_ERROR;         /* debug switch */
module_param(dev_num, int, 0640);
MODULE_PARM_DESC(dev_num, "cdev num you want");
MODULE_PARM_DESC(debug, "Debug switch (0=close debug, 1=open debug)");

#define CDEV_LOG(level, fmt, args...) do { \
    if (debug >= (level)) {                                                             \
        printk(KERN_NOTICE "edma_cdev: %s, %d, " fmt "\n", __func__, __LINE__, ##args); \
    }                                                                                   \
} while (0)

static int cdev_open(struct inode *inode, struct file *filp);
static int cdev_release(struct inode *inode, struct file *filp);
static unsigned int cdev_poll(struct file *file, poll_table *wait);
static ssize_t cdev_read(struct file *filp, char __user *data, size_t count, loff_t *ppos);
static ssize_t cdev_write(struct file *filp, const char __user *data, size_t count, loff_t *ppos);

struct cdev_dev_set g_cdev_set;

#define INC_CDEV_STATS(pdev, name, count) (((struct cdev_dev *)(pdev))->s.name += (count))

module_param_call(debug, &edma_param_set_debug, &param_get_int, &debug, 0644);

#if defined(LINUX_VERSION_CODE) && (KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE)
static int cdev_param_get_statics(char *buf, struct kernel_param *kp)
#else
static int cdev_param_get_statics(char *buf, const struct kernel_param *kp)
#endif
{
    int len = 0;
    int i = 0;
    __kernel_time_t running_time = 0;

    if (!buf) {
        return 0;
    }

    GET_SYS_SECONDS(running_time);
    running_time -= (__kernel_time_t)g_cdev_set.init_time;
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1,
                     "============================CDEV_DRIVER_INFO=======================\n");
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "version      :%s\n", CDEV_VERSION);
    len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "running_time :%luD %02lu:%02lu:%02lu\n",
                     running_time / (SECONDS_PER_DAY), running_time % (SECONDS_PER_DAY) / SECONDS_PER_HOUR,
                     running_time % SECONDS_PER_HOUR / SECONDS_PER_MINUTE, running_time % SECONDS_PER_MINUTE);

    for (i = 0; i < g_cdev_set.dev_num; i++) {
        len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1,
                         "===================================================\n");
        len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "name      :%s\n", g_cdev_set.dev_list[i].dev_name);
        len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "dev_id    :%08x\n", g_cdev_set.dev_list[i].dev_id);
        len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "type      :%u\n", g_cdev_set.dev_list[i].type);
        len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "status    :%s\n",
                         g_cdev_set.dev_list[i].s.open_status == 1 ? "open" : "close");
        len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "send_pkgs :%u\n",
                         g_cdev_set.dev_list[i].s.send_pkgs);
        len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "send_bytes:%u\n",
                         g_cdev_set.dev_list[i].s.send_bytes);
        len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "send_failed_count:%u\n",
                         g_cdev_set.dev_list[i].s.send_failed_count);
        len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "recv_pkgs :%u\n",
                         g_cdev_set.dev_list[i].s.recv_pkgs);
        len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "recv_bytes:%u\n",
                         g_cdev_set.dev_list[i].s.recv_bytes);
        len += sprintf_s(buf + len, STATISCITS_BUF_SIZE - len - 1, "recv_failed_count:%u\n",
                         g_cdev_set.dev_list[i].s.recv_failed_count);
    }

    return len;
}
module_param_call(statistics, NULL, cdev_param_get_statics, &debug, 0444);
MODULE_PARM_DESC(statistics, "Statistics info of cdev driver,readonly");

const struct file_operations g_bma_cdev_fops = {
    .owner = THIS_MODULE,
    .open = cdev_open,
    .release = cdev_release,
    .poll = cdev_poll,
    .read = cdev_read,
    .write = cdev_write,
};

static int __init bma_cdev_init(void)
{
    int i = 0;

    int ret = 0;
    int err_count = 0;
    __kernel_time_t init_time = 0;

    if (!bma_intf_check_edma_supported()) {
        return -ENXIO;
    }

    if (dev_num <= 0 || dev_num > CDEV_MAX_NUM) {
        return -EINVAL;
    }

    (void)memset_s(&g_cdev_set, sizeof(struct cdev_dev_set), 0, sizeof(struct cdev_dev_set));
    g_cdev_set.dev_num = dev_num;

    for (i = 0; i < dev_num; i++) {
        struct cdev_dev *pdev = &g_cdev_set.dev_list[i];

        if (sprintf_s(pdev->dev_name, CDEV_NAME_MAX_LEN - 1, "%s%d", CDEV_NAME_PREFIX, i) < 0) {
            err_count++;
            continue;
        }

        pdev->dev_struct.name = pdev->dev_name;
        pdev->dev_struct.minor = MISC_DYNAMIC_MINOR;
        pdev->dev_struct.fops = &g_bma_cdev_fops;

        pdev->dev_id = CDEV_INVALID_ID;

        ret = misc_register(&pdev->dev_struct);

        if (ret) {
            CDEV_LOG(DLOG_DEBUG, "misc_register failed %d", i);
            err_count++;
            continue;
        }

        pdev->dev_id = MKDEV(MISC_MAJOR, pdev->dev_struct.minor);

        ret = bma_intf_register_type(TYPE_CDEV + i, 0, INTR_DISABLE, &pdev->dev_data);

        if (ret) {
            CDEV_LOG(DLOG_ERROR, "cdev %d open failed ,result = %d", i, ret);
            misc_deregister(&pdev->dev_struct);
            pdev->dev_id = CDEV_INVALID_ID;
            err_count++;
            continue;
        }
        pdev->type = TYPE_CDEV + i;
        atomic_set(&pdev->open, 1);

        CDEV_LOG(DLOG_DEBUG, "%s id is %08x", pdev->dev_struct.name, pdev->dev_id);
    }

    if (err_count == dev_num) {
        CDEV_LOG(DLOG_ERROR, "init cdev failed!");
        return -EFAULT;
    }
    GET_SYS_SECONDS(init_time);
    g_cdev_set.init_time = (unsigned int)init_time;
    return 0;
}

static void __exit bma_cdev_exit(void)
{
    while (dev_num--) {
        struct cdev_dev *pdev = &g_cdev_set.dev_list[dev_num];

        if (pdev->dev_id != CDEV_INVALID_ID) {
            if (pdev->dev_data && pdev->type != 0) {
                (void)bma_intf_unregister_type(&pdev->dev_data);
            }

            (void)misc_deregister(&g_cdev_set.dev_list[dev_num].dev_struct);
        }
    }
}

int cdev_open(struct inode *inode_prt, struct file *filp)
{
    int i = 0;
    struct cdev_dev *pdev = NULL;

    if (!inode_prt) {
        return -EFAULT;
    }

    if (!filp) {
        return -EFAULT;
    }

    if (dev_num <= 0 || dev_num > CDEV_MAX_NUM) {
        CDEV_LOG(DLOG_ERROR, "dev_num error");
        return -EFAULT;
    }

    for (i = 0; i < dev_num; i++) {
        pdev = &g_cdev_set.dev_list[i];

        if (pdev == NULL) {
            return -ENODEV;
        }

        if (pdev->dev_id == inode_prt->i_rdev) {
            break;
        }
    }

    if (i == dev_num) {
        CDEV_LOG(DLOG_ERROR, "can not find dev id %08x", inode_prt->i_rdev);
        return -ENODEV;
    }
    /* each device can be opened only once */
    if (atomic_dec_and_test(&pdev->open) == 0) {
        CDEV_LOG(DLOG_DEBUG, "%s is already opened", pdev->dev_name);
        atomic_inc(&pdev->open);
        return -EBUSY; /* already opened */
    }

    filp->private_data = &g_cdev_set.dev_list[i];
    bma_intf_set_open_status(pdev->dev_data, DEV_OPEN);
    INC_CDEV_STATS(filp->private_data, open_status, 1);

    return 0;
}

int cdev_release(struct inode *inode_prt, struct file *filp)
{
    struct cdev_dev *pdev = NULL;

    if (!filp) {
        return 0;
    }

    pdev = (struct cdev_dev *)filp->private_data;
    if (pdev) {
        INC_CDEV_STATS(filp->private_data, open_status, -1);
        bma_intf_set_open_status(pdev->dev_data, DEV_CLOSE);
        atomic_inc(&pdev->open);
        filp->private_data = NULL;
    }

    return 0;
}

unsigned int cdev_poll(struct file *filp, poll_table *wait)
{
    unsigned int mask = 0;
    wait_queue_head_t *queue_head = NULL;

    if (filp == NULL || filp->private_data == NULL) {
        return 0;
    }
    queue_head = (wait_queue_head_t *)bma_cdev_get_wait_queue(((struct cdev_dev *)(filp->private_data))->dev_data);
    if (!queue_head) {
        return 0;
    }

    poll_wait(filp, queue_head, wait);

    if (bma_cdev_check_recv(((struct cdev_dev *)(filp->private_data))->dev_data)) {
        mask |= (POLLIN | POLLRDNORM);
    }

    CDEV_LOG(DLOG_DEBUG, "poll return %08x", mask);

    return mask;
}

ssize_t cdev_read(struct file *filp, char __user *data, size_t count, loff_t *ppos)
{
    int ret = 0;

    CDEV_LOG(DLOG_DEBUG, "data is %pK,count is %u", data, (unsigned int)count);

    if (filp == NULL || data == NULL || count == 0 || filp->private_data == NULL) {
        return -EFAULT;
    }

    ret = bma_cdev_recv_msg(((struct cdev_dev *)(filp->private_data))->dev_data, data, count);

    if (ret > 0) {
        INC_CDEV_STATS(filp->private_data, recv_bytes, (unsigned int)ret);
        INC_CDEV_STATS(filp->private_data, recv_pkgs, 1);
    } else {
        INC_CDEV_STATS(filp->private_data, recv_failed_count, 1);
    }

    return ret;
}

ssize_t cdev_write(struct file *filp, const char __user *data, size_t count, loff_t *ppos)
{
    int ret = 0;

    if (filp == NULL || data == NULL || count == 0 || filp->private_data == NULL) {
        return -EFAULT;
    }

    CDEV_LOG(DLOG_DEBUG, "data is %pK,count is %u", data, (unsigned int)count);
    ret = bma_cdev_add_msg(((struct cdev_dev *)(filp->private_data))->dev_data, data, count);

    if (ret > 0) {
        INC_CDEV_STATS(filp->private_data, send_bytes, (unsigned int)ret);
        INC_CDEV_STATS(filp->private_data, send_pkgs, 1);
    } else {
        INC_CDEV_STATS(filp->private_data, send_failed_count, 1);
    }

    return ret;
}

MODULE_AUTHOR("HUAWEI TECHNOLOGIES CO., LTD.");
MODULE_DESCRIPTION("Hi171x Intelligent Management system chip CDEV driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(CDEV_VERSION);

module_init(bma_cdev_init);
module_exit(bma_cdev_exit);
