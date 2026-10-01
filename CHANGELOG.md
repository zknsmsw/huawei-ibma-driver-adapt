# Changelog

## v0.4.0-pve2 (2026-10)

第二轮：修掉代码审计发现的**真实缺陷**（不改协议/业务逻辑）。

**运行时缺陷修复**

1. `veth_tx()`：发送队列未就绪时返回 `BSP_ERR_NULL_POINTER`（非法的 `netdev_tx_t` 返回值）且**不释放 skb**
   → 改为释放 skb 并返回 `NETDEV_TX_OK`（丢弃语义）。
   注意：该分支**不能**调用 `INC_STATIS_TX()`，因为该宏会解引用正好为 NULL 的 `ptx_queue[queue]`。
2. `veth_netdev_init()`：`alloc_netdev_mq()` 返回值未判空即 `register_netdev()`
   → 增加判空并返回 `-ENOMEM`；`register_netdev()` 失败时补 `free_netdev()`，避免 netdev 泄漏。
3. sysfs `statistics` 属性在 netdev 初始化完成前被读取，会经 `netif_running(NULL)` 触发空指针崩溃
   → 增加 `pnetdev == NULL` 提前返回。
4. `kbox_printk_exit()`：先 `kfree()` 缓冲区、后 `unregister_console()`，存在 use-after-free 窗口
   → 改为**先注销控制台（并同步）再释放**，并把 `g_printk_init_ok` 置回 false。
5. `kbox_ram_op.c`：`VM_RESERVED` 自 Linux 3.11 起已被删除且该位被复用（现代内核 `0x00080000` 是
   `VM_LOCKONFAULT`），旧的本地兜底定义会写错标志位
   → 改用 `VM_IO | VM_DONTEXPAND | VM_DONTDUMP`。
6. `/proc/kbox` 是"kbox 已加载"的标记，但卸载时从未删除 → 卸载路径与初始化失败路径补
   `remove_proc_entry()`，避免下次 `insmod` 误判为"已加载"而跳过首次初始化。

**编译告警清理**

7. 删除 `edma_host.h` 中一段**从未被引用**的 `VM_*` 重定义（消除 `"VM_ARCH_1" redefined` 等告警）。
8. `wait_done_dma_queue()` 改为 `static`（消除 `-Wmissing-prototypes`）。

**有意保留、未改动**（需要硬件/流量验证，或属于上游设计取舍）

* `kbox` 在 panic/NMI 上下文里做时间读取 + `udelay` + MMIO 访问；MMIO 通过 `int*` 传递；
  `/proc/kbox` 的读写权限模型 —— 改动风险高于收益。
* `veth` 设置了 `watchdog_timeo` 但未实现 `ndo_tx_timeout` —— 正确实现需要真实流量复现。
* `USE_DMA` 未定义导致 `DMA_NOT_LIST` 分支为空实现 —— 上游默认行为，改动会影响数据传输路径。

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
