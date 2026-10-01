# NOTICE / 来源与改动声明

## 这是什么

本仓库是**社区适配版**：在华为 **iBMA 2.0 用户态软件包**所附带的开源内核驱动
`iBMA_Driver 0.4.0`（DKMS 源码）基础上，做了让它可以编译/运行在**较新 Linux 内核**
（6.15 / 6.17 / 7.0，例如 Proxmox VE 9.1 / 9.2）上的最小改动。

* **本仓库只包含内核驱动源码**，不包含、也不再分发 iBMA 用户态软件（`iBMA2.0` 安装包、二进制、文档等）。
  用户态软件请到华为官方支持网站自行下载。
* 本仓库**不是**华为官方发布物，与华为技术有限公司**无隶属或背书关系**。
  "Huawei"、"iBMA"、"iBMC"、"FusionServer" 等商标/名称归其各自所有者。

## 上游来源

| 项目 | 值 |
|---|---|
| 上游组件 | 华为 iBMA 2.0 内核驱动（`iBMA_Driver`，DKMS 源码包 `ibmasrc-dkms-0.4.0.amd64.deb` / `iBMA_Driver-dkms-0.4.0-src.x86_64.rpm`） |
| 上游版本 | `0.4.0`（`PACKAGE_VERSION="3.0.0"`，`DRV_VERSION=0.4.0`） |
| 获取方式 | 华为企业业务支持网站（Support-E）搜索 "iBMA"，下载 `iBMA-*-dkms_0.4.0_*_src.tar.gz` |
| 许可证（`driver/` 主体） | GNU General Public License v2.0 **or later**（每个子目录含 `License.txt`、`copyright.txt`） |
| 许可证（`driver/secure/`，Huawei libboundscheck） | Mulan PSL v2 |

`driver/*/copyright.txt` 原文：

```
Copyright (C) Huawei Technologies Co., Ltd.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
```

## 本仓库做了哪些改动

改动只有 **9 个文件**，全部是为了适配新内核 API / kbuild 行为，**未改动任何业务逻辑**：

| 文件 | 改动 |
|---|---|
| `driver/{edma_drv,cdev_drv,kbox_drv,veth_drv,cdev_veth_drv}/Makefile` | `EXTRA_CFLAGS +=` → `ccflags-y +=`（kbuild 6.15 起删除 `EXTRA_CFLAGS` 兼容映射） |
| `driver/*/Makefile` | `stdarg.h` 探测增加 `/lib/modules/*/build/include/...` 与 `/usr/src/*/include/...` 路径 |
| `driver/edma_drv/Makefile`、`driver/kbox_drv/Makefile` | 新增"直接 grep 目标内核头文件"的能力探测：`HAVE_TIMER_DELETE_SYNC`、`HAVE_HRTIMER_SETUP`、`HAVE_MSR_Q_SAFE` |
| `driver/edma_drv/bma_include.h` | 显式 `#include <linux/timer.h>`；`HAVE_TIMER_SETUP` 探测兼容新名 `timer_container_of` |
| `driver/edma_drv/edma_host.c` | `hrtimer_init()` / `del_timer_sync()` 增加新旧 API 双分支 |
| `driver/kbox_drv/kbox_mce.c` | `rdmsrl_safe()` → 新内核用 `rdmsrq_safe()` |
| `driver/veth_drv/veth_hb.c` | 新增 `static netdev_tx_t veth_tx_ndo()` 包装，满足 `-Werror=incompatible-pointer-types` |

完整 diff 见 `patches/ibma-driver-0.4.0-kernel7.0.patch`；技术依据（含内核源码行号）见 `docs/ADAPTATION.md`。

## GPL 合规说明

* 本仓库保留上游全部版权头、`License.txt`、`copyright.txt`，未删除或修改任何许可声明。
* 修改内容与本声明一并提供，满足 GPL-2.0 "标明改动" 的要求。
* 依据 GPL-2.0 分发，**不提供任何担保**。
* 若你是上游权利人并认为本仓库有不妥之处，请开 issue，我们会配合调整或删除。
