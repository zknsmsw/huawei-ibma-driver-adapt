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
#include <linux/utsname.h> /* system_utsname */
#include <linux/rtc.h>     /* struct rtc_time */
#include "kbox_include.h"
#include "kbox_main.h"
#include "kbox_printk.h"
#include "kbox_ram_image.h"
#include "kbox_ram_op.h"
#include "kbox_panic.h"

#include "kbox_dump.h"

#ifdef CONFIG_X86
#include "kbox_mce.h"
#endif

#define THREAD_TMP_BUF_SIZE 256

#if (KERNEL_VERSION(3, 0, 0) < LINUX_VERSION_CODE)
static DEFINE_SPINLOCK(g_dump_lock);
#else
static spinlock_t g_dump_lock = SPIN_LOCK_UNLOCKED;
#endif

static const char g_day_in_month[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static void kbox_show_kernel_version(void)
{
    (void)kbox_dump_painc_info("\nOS : %s,\nRelease : %s,\nVersion : %s,\nMachine : %s,\nNodename : %s\n",
                               init_uts_ns.name.sysname, init_uts_ns.name.release, init_uts_ns.name.version,
                               init_uts_ns.name.machine, init_uts_ns.name.nodename);
}

static void kbox_show_version(void)
{
    (void)kbox_dump_painc_info("\nKBOX_VERSION         : %s\n", KBOX_VERSION);
}

void kbox_dump_event(enum kbox_error_type_e type, unsigned long event, const char *msg)
{
    if (!spin_trylock(&g_dump_lock)) {
        return;
    }

    (void)kbox_dump_painc_info("\n====kbox begin dumping...====\n");

    switch (type) {
#ifdef CONFIG_X86
        case KBOX_MCE_EVENT:

            kbox_handle_mce_dump(msg);

            break;
#endif

        case KBOX_OPPS_EVENT:

            break;
        case KBOX_PANIC_EVENT:
            if (kbox_handle_panic_dump(msg) == KBOX_FALSE) {
                goto end;
            }

            break;
        default:
            break;
    }

    kbox_show_kernel_version();

    kbox_show_version();

    (void)kbox_dump_painc_info("\n====kbox end dump====\n");

    kbox_output_syslog_info();
    kbox_output_printk_info();

end:
    spin_unlock(&g_dump_lock);
}
