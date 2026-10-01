# Changelog

## v0.4.0-pve1 (2026-10)

首个适配版本：在华为 `iBMA_Driver 0.4.0`（DKMS 源码）基础上适配 Linux 6.15+/6.17/7.0 内核。

**编译阻断修复（6 项）**

1. `EXTRA_CFLAGS` → `ccflags-y`（5 个 Makefile）。kbuild 自 6.15 起移除了 `EXTRA_CFLAGS`
   兼容映射，导致驱动的全部 `-I`/`-D` 被丢弃、`securec.h`/`bma_include.h` 找不到。
2. `from_timer` → `timer_container_of` 改名，使 `HAVE_TIMER_SETUP` 探测失效、进而编译已被删除的
   `setup_timer()`。改为同时探测两个宏名，并显式包含 `<linux/timer.h>`。
3. `hrtimer_init()` → `hrtimer_setup()`（新内核实现在 6.15+，旧名在 6.17+/7.0 被移除）。
4. `del_timer_sync()` → `timer_delete_sync()`（6.15+ 新名，6.17+/7.0 移除旧名）。
5. `rdmsrl_safe()` → `rdmsrq_safe()`（x86 MSR 安全读在新内核改名）。
6. `int veth_tx()` 赋给 `struct net_device_ops.ndo_start_xmit`（类型 `netdev_tx_t`）触发
   `-Werror=incompatible-pointer-types`；改为通过类型正确的静态包装注册，**行为不变**。

**其它**

* `stdarg.h` 探测路径增强，避免 `-nostdinc` 环境下回退到 `<stdarg.h>`。
* 版本判断改为"直接探测目标内核头文件"，因此对发行版 backport 同样正确
  （例如 Proxmox VE 6.14 已 backport `timer_delete_sync`）。
* 全部改动保留旧 API 分支，**4.15 – 6.14 内核仍可编译**。

**真机验证**

* Proxmox VE 9.2 / `7.0.14-20-pve` / gcc 14 / x86_64：5 个模块全部编译通过，
  `host_edma_drv` 成功绑定 `19e5:1710`，`/dev/hwibmc0..3` 生成，iBMA 2.20.0 用户态注册、
  心跳、事件上报正常。
