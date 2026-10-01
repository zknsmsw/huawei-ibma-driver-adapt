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

#include <linux/version.h>
#include <linux/notifier.h>
#if defined(LINUX_VERSION_CODE) && (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0))
#include <linux/panic_notifier.h>
#endif
#include "kbox_include.h"
#include "kbox_dump.h"
#include "kbox_hook.h"

int panic_notify(struct notifier_block *pthis, unsigned long event, void *msg);

static int die_notify(struct notifier_block *self, unsigned long val, void *data);

static struct notifier_block g_panic_nb = {
    .notifier_call = panic_notify,
    .priority = 100,
};

static struct notifier_block g_die_nb = {
    .notifier_call = die_notify,
};

int panic_notify(struct notifier_block *pthis, unsigned long event, void *msg)
{
    UNUSED(pthis);
    UNUSED(event);

    kbox_dump_event(KBOX_PANIC_EVENT, DUMPSTATE_PANIC_RESET, (const char *)msg);

    return NOTIFY_OK;
}

static int die_notify(struct notifier_block *self, unsigned long val, void *data)
{
    struct kbox_die_args *args = (struct kbox_die_args *)data;

    if (!args) {
        return NOTIFY_OK;
    }

    switch (val) {
        case 1:
            break;
        case 5: /* 5 表示检查nmi消息 */
            if (args->str != NULL && strcmp(args->str, "nmi") == 0) {
                return NOTIFY_OK;
            }
#ifdef CONFIG_X86
            kbox_dump_event(KBOX_MCE_EVENT, DUMPSTATE_MCE_RESET, args->str);
#endif
            break;

        default:
            break;
    }

    return NOTIFY_OK;
}

int kbox_register_hook(void)
{
    int ret = 0;

    ret = atomic_notifier_chain_register(&panic_notifier_list, &g_panic_nb);
    if (ret != 0) {
        KBOX_MSG("atomic_notifier_chain_register g_panic_nb failed!\n");
    }

    ret = register_die_notifier(&g_die_nb);
    if (ret != 0) {
        KBOX_MSG("register_die_notifier g_die_nb failed!\n");
    }

    return ret;
}

void kbox_unregister_hook(void)
{
    int ret = 0;

    ret = atomic_notifier_chain_unregister(&panic_notifier_list, &g_panic_nb);
    if (ret < 0) {
        KBOX_MSG("atomic_notifier_chain_unregister g_panic_nb failed!\n");
    }

    ret = unregister_die_notifier(&g_die_nb);
    if (ret < 0) {
        KBOX_MSG("unregister_die_notifier g_die_nb failed!\n");
    }
}
