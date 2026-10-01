# 适配技术说明

本文说明：**为什么华为原版 `iBMA_Driver 0.4.0` 在较新内核上编不过**，以及本仓库每一处改动对应的内核侧证据。

## 分析方法

对比对象是**真实的内核构建树**（Debian/Proxmox 的 `*-headers-<ver>` 包，包含 `include/`、`arch/x86/include/`、
`scripts/Makefile*`、`.config`、`Module.symvers`）：

| 内核 | 用途 |
|---|---|
| `proxmox-headers-7.0.14-14-pve` / `-20-pve` | 目标（Proxmox VE 9.2） |
| `proxmox-headers-6.17.13-21-pve` | 目标（Proxmox VE 9.1） |
| `proxmox-headers-6.14.11-9-pve` | 对照（Proxmox VE 9.0 内核，旧 API 仍在） |
| `ubuntu-headers-6.8.0-100` | 对照（华为支持矩阵里的较新内核） |

方法：逐一核对驱动里用到的内核接口在该内核树中是否存在、签名是否兼容，并核对 kbuild 行为。
**最终以真机编译为准**：Proxmox VE 9.2 + `7.0.14-20-pve` + gcc 14 全部编译通过。

## 编译阻断 1：kbuild 不再支持 `EXTRA_CFLAGS`

* 证据：7.0.14 的 `scripts/Makefile.lib` 中**没有** `EXTRA_CFLAGS` 映射（合并 `ccflags-y` 的逻辑被删除）；
  而 6.14.11 的 `scripts/Makefile.lib:4` 仍有 `ccflags-y += $(EXTRA_CFLAGS)`。
  上游对应提交：*kbuild: remove EXTRA_\*FLAGS support*。
* 驱动把 4 个 `-I` 路径、`-D DRV_VERSION`、`-DCDEV_VETH_VERSION`、`-D HAVE_LINUX_STDARG_H` 全写在
  `EXTRA_CFLAGS` 里 → 全部被丢弃：`#include "bma_include.h"`、`"bma_ker_intf.h"`、`"securec.h"`
  直接 `No such file or directory`（引号包含只先搜本文件目录，不会搜父目录）。
* 修复：5 个模块 Makefile 中 `EXTRA_CFLAGS +=` 全部改成 `ccflags-y +=`（`ccflags-y` 自 2.6.36 起有效，
  对旧内核同样成立，无需版本判断）。

## 编译阻断 2：`from_timer` 改名导致 `setup_timer()` 死分支被编译

* `driver/edma_drv/bma_include.h` 用 `#if defined(timer_setup) && defined(from_timer)` 判定 `HAVE_TIMER_SETUP`。
* 7.0.14：`from_timer` 已改名为 `timer_container_of`（`include/linux/timer.h:132`）；6.14 仍是 `from_timer`。
  → 7.0.14 上 `HAVE_TIMER_SETUP` 未定义，于是编译 `#else` 分支里的 `setup_timer()`
  （`driver/edma_drv/edma_host.c`），而该函数**在 6.14 和 7.0 中都已被删除**。
* 另外 `-Werror=implicit-function-declaration` 是**始终生效**的硬错误
  （7.0.14 `scripts/Makefile.warn:13`），所以这不是"链接错误"而是直接编译失败。
* 修复：显式 `#include <linux/timer.h>`，探测条件改为
  `defined(timer_setup) && (defined(from_timer) || defined(timer_container_of))`。

## 编译阻断 3/4/5：定时器、hrtimer、MSR 三处改名

| API（旧 → 新） | 7.0.14 证据 | 6.14 对照 | 驱动调用点 | 修复方式 |
|---|---|---|---|---|
| `hrtimer_init` → `hrtimer_setup` | `include/linux/hrtimer.h:215` 只有新名 | 6.14 `:229` 两者都有 | `edma_host.c`（心跳定时器） | Makefile 探测 `hrtimer_setup` → `HAVE_HRTIMER_SETUP` 双分支 |
| `del_timer_sync` → `timer_delete_sync` | `include/linux/timer.h:166` 只有新名 | 6.14 `:183` 保留旧名内联包装（且已 backport 新名） | `edma_host.c`（超时定时器） | Makefile 探测 `timer_delete_sync` → `HAVE_TIMER_DELETE_SYNC` 双分支 |
| `rdmsrl_safe` → `rdmsrq_safe` | `arch/x86/include/asm/msr.h:218` 只有新名 | 6.14 `:283` 旧名 | `kbox_mce.c`（MCE 读 MSR） | Makefile 探测 `rdmsrq_safe` → `HAVE_MSR_Q_SAFE` 双分支 |

> 这三处**故意不用 `LINUX_VERSION_CODE` 硬编码版本号**，而是让 Makefile 去 `grep` 目标内核的头文件。
> 原因：发行版会 backport（例如 PVE 的 6.14.11 内核已经有 `timer_delete_sync`，但版本号仍是 6.14），
> 用版本号判断会误判。

## 编译阻断 6：`ndo_start_xmit` 返回类型

* `driver/veth_drv/veth_hb.c` 把 `int veth_tx(...)` 赋给 `struct net_device_ops.ndo_start_xmit`
  （类型 `netdev_tx_t (*)(...)`）。
* `-Werror=incompatible-pointer-types` 在这些内核上**默认生效**：
  * 7.0.14 `scripts/Makefile.warn:105`
  * 6.17.13 `scripts/Makefile.extrawarn:102`
  * 6.14.11 `scripts/Makefile.extrawarn:97`
  * Ubuntu 6.8.0-100 `scripts/Makefile.extrawarn:89`
  （5.15 及更早没有该开关 —— 这解释了为什么老系统能编过。）
* 修复：新增类型正确的静态包装并注册到 `.ndo_start_xmit`，`veth_tx()` 本体与返回值**完全不变**。

## 附带修复：`stdarg.h` 探测

`driver/secure/include/securec.h` 在未定义 `HAVE_LINUX_STDARG_H` 时会回退到 `#include <stdarg.h>`，
而内核编译使用 `-nostdinc`（7.0.14 `Makefile:1095`）且不给 x86 加 `-isystem`，会失败。
本仓库把 Makefile 里的探测路径补全为 `/lib/modules/*/build/include/linux/stdarg.h`、
`/usr/src/kernels/*/...`、`/usr/src/*/...`，保证安装过头文件的环境能命中。

## 设计原则

1. **双分支**：所有 API 改动都保留旧内核分支，因此 4.15–6.14 与 6.15+ 都能编译。
2. **最小改动**：不动业务逻辑与协议，只解决编译、类型与明确的缺陷。
3. **尽量不引入无法验证的行为改动**：见文末"有意保留"。

## v0.4.0-pve2：缺陷修复（非编译问题）

这些是代码审计发现、且**无需硬件即可判定为错**的问题：

| 位置 | 问题 | 修复 |
|---|---|---|
| `veth_drv/veth_hb.c` `veth_tx()` | 发送队列未就绪时返回 `BSP_ERR_NULL_POINTER`（非法 `netdev_tx_t`）且 skb 泄漏 | 释放 skb 并返回 `NETDEV_TX_OK`；该分支不可调用 `INC_STATIS_TX()`（宏会解引用 NULL 的 `ptx_queue[queue]`） |
| `veth_drv/veth_hb.c` `veth_netdev_init()` | `alloc_netdev_mq()` 未判空即 `register_netdev()`；注册失败泄漏 netdev | 判空返回 `-ENOMEM`；失败路径补 `free_netdev()` |
| `veth_drv/veth_hb.c` `veth_param_get_statics()` | netdev 未初始化时 sysfs 读取经 `netif_running(NULL)` 空指针 | 增加 `pnetdev == NULL` 提前返回 |
| `kbox_drv/kbox_printk.c` `kbox_printk_exit()` | 先 `kfree` 再 `unregister_console`，use-after-free 窗口 | 先注销控制台再释放，并复位 `g_printk_init_ok` |
| `kbox_drv/kbox_ram_op.c` mmap | `VM_RESERVED`（3.11 起删除、该位被复用为 `VM_LOCKONFAULT`）写错标志位 | 改用 `VM_IO \| VM_DONTEXPAND \| VM_DONTDUMP` |
| `kbox_drv/kbox_main.c` | `/proc/kbox`（"已加载"标记）卸载时不清理 | 卸载与初始化失败路径补 `remove_proc_entry()` |
| `edma_drv/edma_host.h` | 一段从未被引用的 `VM_*` 重定义 | 删除（消除 `redefined` 告警） |
| `edma_drv/edma_queue.c` | `wait_done_dma_queue()` 无前置声明 | 改为 `static` |

## 有意保留（未修改）

以下几项**已知但故意不动**，因为它们要么需要真实硬件/流量验证，要么属于上游设计取舍：

* `veth_tx()` 之后的发包主路径、DMA 环操作、`cdev_veth` 的共享内存环发布顺序 —— 缺少硬件验证不宜改。
* `veth` 设了 `watchdog_timeo` 但没有 `ndo_tx_timeout` —— 正确实现需要复现"发送队列卡死"。
* `USE_DMA` 未定义，`host_dma_transfer_without_list()` 为空实现 —— 上游默认行为，改动会影响数据传输。
* `kbox` 在 panic/NMI 上下文做时间读取 + `udelay` + MMIO；MMIO 经 `int*` 传递；`/proc/kbox` 的权限模型。
* `secure/`（libboundscheck）内部的实现细节。

