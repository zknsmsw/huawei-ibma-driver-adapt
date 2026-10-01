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

#include <linux/spinlock.h>
#include <linux/console.h> /* struct console */
#include <linux/slab.h>
#include <linux/err.h>
#include "kbox_include.h"
#include "kbox_main.h"
#include "kbox_ram_image.h"
#include "kbox_ram_op.h"

#include "kbox_printk.h"

#define TMP_BUF_SIZE 256

static int g_printk_init_ok = KBOX_FALSE;

static char *g_printk_info_buf;
static char *g_printk_info_buf_tmp;

static struct printk_ctrl_block_tmp_s g_printk_ctrl_block_tmp = {};

#if (KERNEL_VERSION(3, 0, 0) < LINUX_VERSION_CODE)
static DEFINE_SPINLOCK(g_printk_buf_lock);
#else
static spinlock_t g_printk_buf_lock = SPIN_LOCK_UNLOCKED;
#endif

static void kbox_printk_info_write(struct console *console, const char *printk_buf, unsigned int buf_len);

static struct console g_printk_console = {
    .name = "k_prtk",
    .flags = CON_ENABLED | CON_PRINTBUFFER,
    .write = kbox_printk_info_write,
};

static int kbox_printk_format_is_order(struct printk_info_ctrl_block_s *printk_ctrl_blk_first,
                                       struct printk_info_ctrl_block_s *printk_ctrl_blk_second)
{
    if (!printk_ctrl_blk_first || !printk_ctrl_blk_second) {
        return KBOX_FALSE;
    }

    if (memcmp(printk_ctrl_blk_first->flag, PRINTK_CURR_FLAG, PRINTK_FLAG_LEN) == 0 &&
        memcmp(printk_ctrl_blk_second->flag, PRINTK_LAST_FLAG, PRINTK_FLAG_LEN) == 0) {
        return KBOX_TRUE;
    }

    return KBOX_FALSE;
}

static void kbox_printk_format(struct printk_info_ctrl_block_s *printk_ctrl_blk, const unsigned int len,
                               const char *flag)
{
    int ret;
    if (!printk_ctrl_blk || !flag) {
        return;
    }

    (void)memset_s(printk_ctrl_blk, len, 0, len);
    ret = memcpy_s(printk_ctrl_blk->flag, PRINTK_FLAG_LEN, flag, PRINTK_FLAG_LEN);
    if (ret != KBOX_TRUE) {
        KBOX_MSG("memcpy flag failed.\n");
        return;
    }
}

static void kbox_printk_init_info_first(struct image_super_block_s *kbox_super_block)
{
    KBOX_MSG("\n");
    if (kbox_printk_format_is_order(&kbox_super_block->printk_ctrl_blk[0], &kbox_super_block->printk_ctrl_blk[1]) ==
        KBOX_TRUE) {
        (void)memcpy_s(kbox_super_block->printk_ctrl_blk[0].flag, PRINTK_FLAG_LEN, PRINTK_LAST_FLAG, PRINTK_FLAG_LEN);
        (void)memcpy_s(kbox_super_block->printk_ctrl_blk[1].flag, PRINTK_FLAG_LEN, PRINTK_CURR_FLAG, PRINTK_FLAG_LEN);

        kbox_super_block->printk_ctrl_blk[1].len = 0;
        g_printk_ctrl_block_tmp.printk_region = 1;
        g_printk_ctrl_block_tmp.section = KBOX_SECTION_PRINTK2;
        (void)kbox_clear_region(KBOX_SECTION_PRINTK2);
    } else if (kbox_printk_format_is_order(&kbox_super_block->printk_ctrl_blk[1],
                                           &kbox_super_block->printk_ctrl_blk[0]) == KBOX_TRUE) {
        (void)memcpy_s(kbox_super_block->printk_ctrl_blk[1].flag, PRINTK_FLAG_LEN, PRINTK_LAST_FLAG, PRINTK_FLAG_LEN);
        (void)memcpy_s(kbox_super_block->printk_ctrl_blk[0].flag, PRINTK_FLAG_LEN, PRINTK_CURR_FLAG, PRINTK_FLAG_LEN);

        kbox_super_block->printk_ctrl_blk[0].len = 0;
        g_printk_ctrl_block_tmp.printk_region = 0;
        g_printk_ctrl_block_tmp.section = KBOX_SECTION_PRINTK1;
        (void)kbox_clear_region(KBOX_SECTION_PRINTK1);
    } else {
        kbox_printk_format(kbox_super_block->printk_ctrl_blk, sizeof(struct printk_info_ctrl_block_s),
                           PRINTK_CURR_FLAG);
        kbox_printk_format(kbox_super_block->printk_ctrl_blk + 1, sizeof(struct printk_info_ctrl_block_s),
                           PRINTK_LAST_FLAG);
        g_printk_ctrl_block_tmp.printk_region = 0;
        g_printk_ctrl_block_tmp.section = KBOX_SECTION_PRINTK1;
        (void)kbox_clear_region(KBOX_SECTION_PRINTK1);
        (void)kbox_clear_region(KBOX_SECTION_PRINTK2);
    }

    g_printk_ctrl_block_tmp.start = 0;
    g_printk_ctrl_block_tmp.end = 0;
    g_printk_ctrl_block_tmp.valid_len = 0;
}

static void kbox_printk_init_info_not_first(struct image_super_block_s *kbox_super_block)
{
    KBOX_MSG("\n");
    if (kbox_printk_format_is_order(&kbox_super_block->printk_ctrl_blk[0], &kbox_super_block->printk_ctrl_blk[1]) ==
        KBOX_TRUE) {
        g_printk_ctrl_block_tmp.printk_region = 0;
        g_printk_ctrl_block_tmp.section = KBOX_SECTION_PRINTK1;
    } else if (kbox_printk_format_is_order(&kbox_super_block->printk_ctrl_blk[1],
                                           &kbox_super_block->printk_ctrl_blk[0]) == KBOX_TRUE) {
        g_printk_ctrl_block_tmp.printk_region = 1;
        g_printk_ctrl_block_tmp.section = KBOX_SECTION_PRINTK2;
    } else {
        kbox_printk_format(kbox_super_block->printk_ctrl_blk, sizeof(struct printk_info_ctrl_block_s),
                           PRINTK_CURR_FLAG);
        kbox_printk_format(kbox_super_block->printk_ctrl_blk + 1, sizeof(struct printk_info_ctrl_block_s),
                           PRINTK_LAST_FLAG);
        g_printk_ctrl_block_tmp.printk_region = 0;
        g_printk_ctrl_block_tmp.section = KBOX_SECTION_PRINTK1;
        (void)kbox_clear_region(KBOX_SECTION_PRINTK1);
        (void)kbox_clear_region(KBOX_SECTION_PRINTK2);
    }

    g_printk_ctrl_block_tmp.start = 0;
}

static int kbox_printk_init_info(int kbox_proc_exist)
{
    struct image_super_block_s kbox_super_block = {};
    unsigned int read_len = 0;
    unsigned int write_len = 0;

    read_len = (unsigned int)kbox_read_from_ram(SECTION_KERNEL_OFFSET, (unsigned int)sizeof(struct image_super_block_s),
        (char *)&kbox_super_block, KBOX_SECTION_KERNEL);
    if (read_len != sizeof(struct image_super_block_s)) {
        KBOX_MSG("fail to get superblock data!\n");
        return KBOX_FALSE;
    }

    if (kbox_proc_exist != 0) {
        kbox_printk_init_info_not_first(&kbox_super_block);
        if (kbox_read_printk_info(g_printk_info_buf, &g_printk_ctrl_block_tmp) != KBOX_TRUE) {
            g_printk_ctrl_block_tmp.end = 0;
            g_printk_ctrl_block_tmp.valid_len = 0;
        }
    } else {
        kbox_printk_init_info_first(&kbox_super_block);
    }

    kbox_super_block.checksum = 0;
    kbox_super_block.checksum =
        ~((unsigned char)kbox_checksum((char *)&kbox_super_block, (unsigned int)sizeof(kbox_super_block))) + 1;
    write_len = (unsigned int)kbox_write_to_ram(SECTION_KERNEL_OFFSET, (unsigned int)sizeof(struct image_super_block_s),
        (char *)&kbox_super_block, KBOX_SECTION_KERNEL);
    if (write_len == 0) {
        KBOX_MSG("fail to write superblock data!\n");
        return KBOX_FALSE;
    }

    return KBOX_TRUE;
}

void kbox_output_printk_info(void)
{
    unsigned int start_tmp = 0;
    unsigned int end_tmp = 0;
    unsigned int len_tmp = 0;
    unsigned long flags = 0;
    int ret;

    if (unlikely(!g_printk_info_buf || !g_printk_info_buf_tmp)) {
        return;
    }

    if (g_printk_init_ok != KBOX_TRUE) {
        return;
    }

    spin_lock_irqsave(&g_printk_buf_lock, flags);
    if (g_printk_ctrl_block_tmp.valid_len == 0) {
        spin_unlock_irqrestore(&g_printk_buf_lock, flags);
        return;
    }

    start_tmp = (g_printk_ctrl_block_tmp.start % SECTION_PRINTK_LEN);
    end_tmp = ((g_printk_ctrl_block_tmp.end - 1) % SECTION_PRINTK_LEN);
    len_tmp = g_printk_ctrl_block_tmp.valid_len;

    if (start_tmp > end_tmp && len_tmp >= start_tmp) {
        ret = memcpy_s(g_printk_info_buf_tmp, SECTION_PRINTK_LEN, g_printk_info_buf + start_tmp, len_tmp - start_tmp);
        if (ret != KBOX_TRUE) {
            spin_unlock_irqrestore(&g_printk_buf_lock, flags);
            return;
        }

        ret = memcpy_s(g_printk_info_buf_tmp + len_tmp - start_tmp, SECTION_PRINTK_LEN - len_tmp + start_tmp,
                       g_printk_info_buf, end_tmp + 1);
        if (ret != KBOX_TRUE) {
            spin_unlock_irqrestore(&g_printk_buf_lock, flags);
            return;
        }
    } else {
        ret = memcpy_s(g_printk_info_buf_tmp, SECTION_PRINTK_LEN, g_printk_info_buf + start_tmp, len_tmp);
        if (ret != KBOX_TRUE) {
            spin_unlock_irqrestore(&g_printk_buf_lock, flags);
            return;
        }
    }

    spin_unlock_irqrestore(&g_printk_buf_lock, flags);

    (void)kbox_write_printk_info(g_printk_info_buf_tmp, &g_printk_ctrl_block_tmp);
}

static void kbox_emit_printk_char(const char c)
{
    if (unlikely(!g_printk_info_buf)) {
        return;
    }

    *(g_printk_info_buf + (g_printk_ctrl_block_tmp.end % SECTION_PRINTK_LEN)) = c;
    g_printk_ctrl_block_tmp.end++;

    if (g_printk_ctrl_block_tmp.end > SECTION_PRINTK_LEN) {
        g_printk_ctrl_block_tmp.start++;
    }

    if (g_printk_ctrl_block_tmp.end < SECTION_PRINTK_LEN) {
        g_printk_ctrl_block_tmp.valid_len++;
    }
}

static int kbox_duplicate_printk_info(const char *printk_buf, unsigned int buf_len)
{
    unsigned int idx = 0;
    unsigned long flags = 0;
    const char *printk_buf_tmp = printk_buf;

    spin_lock_irqsave(&g_printk_buf_lock, flags);
    for (idx = 0; idx < buf_len; idx++) {
        kbox_emit_printk_char(*(printk_buf_tmp++));
    }

    spin_unlock_irqrestore(&g_printk_buf_lock, flags);

    return (int)buf_len;
}

int kbox_dump_printk_info(const char *fmt, ...)
{
    va_list args;
    int num = 0;
    char tmp_buf[TMP_BUF_SIZE] = {};

    if (g_printk_init_ok != KBOX_TRUE) {
        return 0;
    }

    va_start(args, fmt);

    num = vsnprintf_s(tmp_buf, sizeof(tmp_buf) - 1, sizeof(tmp_buf) - 1, fmt, args);
    if (num >= 0) {
        (void)kbox_duplicate_printk_info(tmp_buf, (unsigned int)num);
    }

    va_end(args);

    return num;
}

static void kbox_printk_info_write(struct console *pconsole, const char *printk_buf, unsigned int buf_len)
{
    UNUSED(pconsole);

    if (unlikely(!printk_buf)) {
        return;
    }

    (void)kbox_duplicate_printk_info(printk_buf, buf_len);
}

int kbox_printk_init(int kbox_proc_exist)
{
    int ret = KBOX_TRUE;

    g_printk_info_buf = (char *)kmalloc(SECTION_PRINTK_LEN, GFP_KERNEL);
    if (!g_printk_info_buf) {
        KBOX_MSG("kmalloc g_printk_info_buf fail!\n");
        ret = -ENOMEM;
        goto fail;
    }

    (void)memset_s(g_printk_info_buf, SECTION_PRINTK_LEN, 0, SECTION_PRINTK_LEN);

    g_printk_info_buf_tmp = (char *)kmalloc(SECTION_PRINTK_LEN, GFP_KERNEL);
    if (!g_printk_info_buf_tmp) {
        KBOX_MSG("kmalloc g_printk_info_buf_tmp fail!\n");
        ret = -ENOMEM;
        goto fail;
    }

    (void)memset_s(g_printk_info_buf_tmp, SECTION_PRINTK_LEN, 0, SECTION_PRINTK_LEN);

    ret = kbox_printk_init_info(kbox_proc_exist);
    if (ret != KBOX_TRUE) {
        KBOX_MSG("kbox_printk_init_info failed!\n");
        goto fail;
    }

    register_console(&g_printk_console);

    g_printk_init_ok = KBOX_TRUE;

    return ret;
fail:

    kfree(g_printk_info_buf);
    g_printk_info_buf = NULL;

    kfree(g_printk_info_buf_tmp);
    g_printk_info_buf_tmp = NULL;

    return ret;
}

void kbox_printk_exit(void)
{
    int ret = 0;

    if (g_printk_init_ok != KBOX_TRUE) {
        return;
    }

    kfree(g_printk_info_buf);
    g_printk_info_buf = NULL;

    kfree(g_printk_info_buf_tmp);
    g_printk_info_buf_tmp = NULL;

    ret = unregister_console(&g_printk_console);
    if (ret != 0) {
        KBOX_MSG("unregister_console failed!\n");
    }
}
