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
2. **最小改动**：不动任何业务逻辑，不"顺手"重构；只解决编译与类型问题。
3. **故意留下的无害告警**（未修复，避免扩大改动面）：
   * `VM_ARCH_1` / `VM_DONTDUMP` / `VM_MERGEABLE` 宏重定义 —— `edma_host.h` 里那段是死代码（无引用）。
   * `wait_done_dma_queue` 无前置声明（`-Wmissing-prototypes`）。
   两者都只是警告（内核未开 `CONFIG_WERROR`），不影响加载与运行。

## 已知的运行时（非本次移植引入）问题

仅作提示，本仓库**未**修改行为：

* `veth_tx()` 在发送队列为空时返回 `BSP_ERR_NULL_POINTER`（不是合法的 `netdev_tx_t`），且不释放 skb。
* `kbox`（黑匣子）用 `VM_RESERVED 0x00080000`（实际是 `VM_LOCKONFAULT` 位），并在 panic/NMI 上下文里
  做时间读取 + `udelay` + MMIO。该模块对应 iBMA 的"黑匣子"功能，默认不加载（`iBMA.ini` 中 `iBMA_kbox=false`）。
