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

#ifndef KBOX_PANIC_HOOK_H
#define KBOX_PANIC_HOOK_H

struct kbox_die_args {
    struct pt_regs *regs;
    const char *str;
    long err;
    int trapnr;
    int signr;
};

extern int register_die_notifier(struct notifier_block *nb);
extern int unregister_die_notifier(struct notifier_block *nb);

int kbox_register_hook(void);
void kbox_unregister_hook(void);

#endif
