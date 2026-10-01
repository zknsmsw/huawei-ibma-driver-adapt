# 华为 iBMA 内核驱动 · 新版内核适配版

> 让华为服务器（iBMC）的带内管理组件 **iBMA** 的内核驱动，能在 **Linux 6.15 / 6.17 / 7.0**
> （含 **Proxmox VE 9.1 / 9.2**）上正常编译与运行。

**⚠️ 本仓库只包含开源的内核驱动源码（GPL-2.0-or-later + Mulan PSL v2）。**
iBMA **用户态软件**（`iBMA2.0` 安装包、二进制、文档）**不在本仓库**，请到华为官方支持网站自行下载 —— 见 [第 5 节](#5-安装-ibma-用户态软件华为官网下载)。

**本项目是社区适配版，不是华为官方发布物，与华为无隶属关系。**

---

## 目录

- [1. 这是什么 / 为什么要适配](#1-这是什么--为什么要适配)
- [2. 支持范围](#2-支持范围)
- [3. 快速开始（TL;DR）](#3-快速开始tldr)
- [4. 第一步：安装内核驱动](#4-第一步安装内核驱动)
- [5. 第二步：安装 iBMA 用户态软件（华为官网下载）](#5-安装-ibma-用户态软件华为官网下载)
- [6. 端到端验证](#6-端到端验证)
- [7. 卸载 / 回滚](#7-卸载--回滚)
- [8. 内核升级之后](#8-内核升级之后)
- [9. 常见问题（FAQ）](#9-常见问题faq)
- [10. 已知的无害告警](#10-已知的无害告警)
- [11. 仓库结构、改动与许可](#11-仓库结构改动与许可)
- [English Quick Start](#english-quick-start)

---

## 1. 这是什么 / 为什么要适配

华为服务器的带内管理依赖 **iBMA**，它由两部分组成：

| 部分 | 说明 | 从哪来 |
|---|---|---|
| **内核驱动** | `iBMA_Driver`（`host_edma_drv` / `host_cdev_drv` / `host_veth_drv` / `cdev_veth_drv` / `host_kbox_drv`），通过专用 PCIe 通道与 iBMC 通信 | iBMA 软件包里附带的 **DKMS 源码**（本仓库） |
| **用户态服务** | `Manager` / `Monitor` / `iBMA_RedfishMain`、Redfish 服务等 | iBMA 软件包（**华为官网**） |

华为随包发布的 DKMS 源码版本是 `0.4.0`（2025-12）。它**在 Linux 6.15 及之后无法编译**，
因此 Proxmox VE 9.1（6.17 内核）/ 9.2（7.0 内核）上装不上 —— 典型报错是头文件找不到，
或者 `setup_timer` / `hrtimer_init` / `del_timer_sync` / `rdmsrl_safe` 未定义。
本仓库用**最小改动**修好了这些点（9 个文件，不动业务逻辑），并且**旧内核仍然能编**。

原版编不过的 6 个原因（详细证据见 [`docs/ADAPTATION.md`](docs/ADAPTATION.md)）：

| # | 问题 | 一句话原因 |
|---|---|---|
| 1 | `EXTRA_CFLAGS` 失效 | kbuild 自 6.15 起删除了 `EXTRA_CFLAGS` 兼容映射，驱动所有 `-I`/`-D` 被丢弃 → 头文件找不到 |
| 2 | `setup_timer()` 被编译 | `from_timer` 改名 `timer_container_of` 导致 `HAVE_TIMER_SETUP` 探测失效，走进已被删除的旧分支 |
| 3 | `hrtimer_init()` 不存在 | 6.15+ 改名 `hrtimer_setup()`，6.17/7.0 移除旧名 |
| 4 | `del_timer_sync()` 不存在 | 6.15+ 新名 `timer_delete_sync()`，6.17/7.0 移除旧名 |
| 5 | `rdmsrl_safe()` 不存在 | x86 MSR 安全读改名 `rdmsrq_safe()` |
| 6 | `ndo_start_xmit` 返回类型 | `int` 赋给 `netdev_tx_t (*)`，在 `-Werror=incompatible-pointer-types` 下直接编译失败（6.2+ 内核默认开启） |

---

## 2. 支持范围

| 项目 | 说明 |
|---|---|
| **内核** | **4.15 – 6.14**（走旧 API 分支，原版行为）/ **6.15、6.16** / **6.17** / **7.0**（新 API 分支） |
| **已实测** | Proxmox VE **9.2** + `7.0.14-20-pve` + gcc 14（x86_64）：5 个模块全部编译成功，驱动绑定设备，iBMA 2.20.0 用户态注册/心跳/事件上报正常 |
| **发行版** | Debian 13 / Proxmox VE 9.x、Ubuntu 24.04+、RHEL/CentOS/openEuler 等（凡是能装内核头文件+GCC 的都行） |
| **硬件** | iBMC 暴露 PCI 设备 **`19e5:1710`** 或 **`19e5:1712`** 的华为服务器（实测 RH2288H V3 → `1710`） |
| **iBMA 用户态** | 2.20.0（华为官网下载，见第 5 节） |
| 架构 | x86_64（本仓库源码同时保留 arm64 分支，但未实测） |

> 判断你的机器能不能用：`lspci -nn | grep -i 19e5`，能看到 `[19e5:1710]` 或 `[19e5:1712]` 就可以。

---

## 3. 快速开始（TL;DR）

```bash
# 0) 依赖 + 内核头文件（Proxmox VE）
sudo apt update
sudo apt install -y build-essential dpkg-dev binutils dwarves dkms pciutils ipmitool proxmox-headers-$(uname -r)

# 1) 安装内核驱动（DKMS 方式，内核升级自动重建）
git clone https://github.com/zknsmsw/huawei-ibma-driver-adapt.git
cd huawei-ibma-driver-adapt
sudo ./scripts/build-ibma-driver.sh --dkms

# 2) 验证驱动
lspci -nnk -s <bus>            # Kernel driver in use: edma_drv
ls -l /dev/hwibmc*

# 3) 安装用户态软件（从华为官网下载 iBMA 软件包）
unzip iBMA_*_Software_Linux_x86_64.zip
tar -xzf iBMA-Linux-*.tar.gz
cd iBMA2.0 && sudo bash install.sh -s
```

---

## 4. 第一步：安装内核驱动

### 4.0 前置条件

1. **iBMC 侧通道要打开**：iBMC Web → **诊断 → 黑匣子 → 启用**（V3/V5 服务器默认可能是关闭的，
   启用/关闭后需要**重启服务器**）。重启后在 OS 里确认设备出现：
   ```bash
   lspci -nn | grep -iE '19e5:(1710|1712)'
   # 例：09:00.0 Signal processing controller [1180]: Huawei ... iBMA Virtual Network Adapter [19e5:1710]
   ```
   看不到设备时，驱动装上了也不会工作（iBMA 靠这个 PCIe 通道与 iBMC 通信）。
2. **Secure Boot**：本驱动是自编译模块。若 Secure Boot 打开，未 enroll 密钥的模块会被拒绝加载。
   ```bash
   mokutil --sb-state        # 建议 enabled 时先关闭 Secure Boot，或 enroll DKMS 的 MOK
   ```
3. **磁盘/权限**：root 权限；建议先卸载旧版 iBMA 驱动（如果有）。

### 4.1 安装编译依赖

```bash
# Proxmox VE
sudo apt update
sudo apt install -y build-essential dpkg-dev binutils dwarves dkms pciutils ipmitool \
                    proxmox-headers-$(uname -r)

# Debian / Ubuntu
sudo apt install -y build-essential dpkg-dev binutils dwarves dkms pciutils ipmitool \
                    linux-headers-$(uname -r)

# RHEL / CentOS / openEuler 系
sudo dnf install -y gcc make kernel-devel-$(uname -r) kernel-headers-$(uname -r) dkms pciutils ipmitool rpm-build
```

确认头文件到位：`ls -d /lib/modules/$(uname -r)/build`。

### 4.2 安装驱动（三种方式选一个）

#### 方式 A：一键脚本（推荐）

```bash
sudo ./scripts/build-ibma-driver.sh --dkms      # 注册为 DKMS 模块（以后升级内核自动重建）
sudo ./scripts/build-ibma-driver.sh --direct    # 直接把 .ko 拷到 /lib/modules/$(uname -r)/updates/
sudo ./scripts/build-ibma-driver.sh --deb       # 生成并安装华为格式的驱动 .deb
```
脚本会：检查依赖/头文件 → 在 `.build/` 里编译 → 校验 `vermagic` → 按所选方式安装 → 若发现
`19e5:1710/1712` 设备就自动 `modprobe` 并检查 `/dev/hwibmc*`。

配合官方 iBMA 安装包时，还可以把生成的驱动 .deb 放进安装包的 `drivers/Debian/` 目录
（这是华为《iBMA 用户指南》4.12 节的标准做法）：

```bash
sudo ./scripts/build-ibma-driver.sh --deb --ibma-dir /root/iBMA2.0
```

#### 方式 B：手工编译 + depmod（最直观）

```bash
make -C driver all KERNEL_VERSION="$(uname -r)" STACK_PROTECT=false -j"$(nproc)"

sudo mkdir -p /lib/modules/$(uname -r)/updates/iBMA_driver
sudo cp driver/*.ko /lib/modules/$(uname -r)/updates/iBMA_driver/
sudo depmod -a
sudo modprobe host_edma_drv host_cdev_drv
```

#### 方式 C：手工 DKMS

```bash
sudo cp -r driver /usr/src/iBMA_Driver-0.4.0
sudo dkms add     -m iBMA_Driver -v 0.4.0
sudo dkms build   -m iBMA_Driver -v 0.4.0
sudo dkms install -m iBMA_Driver -v 0.4.0 --force
```

> 官方 DKMS 源码包 `ibmasrc-dkms-0.4.0.amd64.deb` 也可以用，但那个包里的源码是**未适配**的原版；
> 要用本仓库的源码，请用方式 A/C（或把 `driver/` 覆盖到 `/usr/src/iBMA_Driver-0.4.0/` 后 `dkms build`）。

### 4.3 验证驱动

```bash
# 1) 模块已注册、版本信息正确
modinfo -F vermagic host_edma_drv        # 必须显示你的内核版本，例如 7.0.14-20-pve
modinfo host_edma_drv | head -5

# 2) 设备已绑定
lspci -nnk -s 09:00.0
#   Kernel driver in use: edma_drv
#   Kernel modules: host_edma_drv
ls -l /sys/bus/pci/devices/0000:09:00.0/driver      # -> .../drivers/edma_drv

# 3) 字符设备已生成（iBMA 用户态会用 /dev/hwibmc0）
ls -l /dev/hwibmc*
# crw------- 1 root root 10, 263 ... /dev/hwibmc0   （共 4 个）

# 4) 模块依赖关系
lsmod | grep -E 'host_edma_drv|host_cdev_drv|host_veth_drv'
# host_cdev_drv   ...  1
# host_edma_drv   ...  2 host_veth_drv,host_cdev_drv
```

---

## 5. 第二步：安装 iBMA 用户态软件（华为官网下载）

> 本仓库**不提供**用户态软件（它包含华为的闭源二进制、Redfish 实现、文档等）。
> 请到 **华为企业业务支持网站（Support-E）** 搜索 **iBMA**，在"软件下载"里选择与你的服务器/OS
> 匹配的版本（本文以 **2.20.0** 为例），下载 `iBMA_2.20.0_Software_Linux_x86_64.zip`。
> 也可以从 iBMC Web 的"系统管理 → 系统信息"或 iBMC 的软件包来源处获取最新版本。

```bash
unzip iBMA_2.20.0_Software_Linux_x86_64.zip
tar -xzf iBMA-Linux-2.20.0.tar.gz          # 得到 iBMA2.0/ 目录
cd iBMA2.0
sudo bash install.sh -s                    # -s 静默安装（推荐）；要交互配置用 bash install.sh
```

正常的输出会长这样（关键是那句 `already has a pre-installed iBMA driver` —— 说明它识别到了
第 4 步装好的内核驱动，于是**跳过**安装包里自带的驱动）：

```
Starting to install iBMA in silent mode.
System is Debian
Kernel version is 7.0.14-20-pve
Driver package version is 0.4.0
Your environment already has a pre-installed iBMA driver
Installing iBMA ...
iBMA installed successfully.
Starting iBMA service.
Start iBMA service successfully.
```

> 如果它报 `Kernel version ... does not support` 并退出，说明它**没找到**已安装的驱动，
> 回到 [4.3](#43-验证驱动) 检查 `modinfo host_edma_drv` 是否可用。

---

## 6. 端到端验证

```bash
# 服务状态
systemctl --no-pager status iBMA
systemctl is-enabled iBMA

# veth 通道（iBMA 与 iBMC 的通信接口，默认 IPv6 link-local）
ip -6 addr show veth
#   inet6 fe80::9e7d:a3ff:fe28:6ff9/64 scope link
cat /proc/sys/net/ipv6/conf/veth/disable_ipv6     # 期望 0

# 日志：最关键的一行是注册成功
grep -i -E 'register to iBMC successfully|Heart beat|Event report successfully' \
     /opt/huawei/ibma/log/common.log | tail -n 5

# 驱动侧
dmesg | grep -iE 'edma|bma|hwibmc' | tail -n 20
```

**iBMC Web 侧**：系统管理 → 系统信息，应能看到 `iBMA服务`（版本）、`iBMA运行状态`（运行中）、
`iBMA驱动`；监控页面（CPU / 内存 / 网口 / 硬盘）开始有数据。

实测通过时的日志片段（Proxmox VE 9.2 + iBMA 2.20.0）：

```
Register.py[sendRegCmd 923] regCmd[11]: 3, veth mode: True.
Register.py[processRegReply 855] iBMA register to iBMC successfully(FE80:...:9E7D:A3FF:FE28:6FFA/40443).
Register.py[checkIdleState 748] Heart beat is recovered.
EventRequest.py[validPostResponse 165] Event report successfully, destURL:/redfish/v1/EventClient/sms0 ...
```

---

## 7. 卸载 / 回滚

```bash
# 驱动
sudo dkms remove -m iBMA_Driver -v 0.4.0 --all          # DKMS 方式
sudo dpkg -r ibmadriver                                # deb 方式
sudo rm -rf /lib/modules/$(uname -r)/updates/iBMA_driver && sudo depmod -a   # 直接拷贝方式
sudo rmmod host_veth_drv cdev_veth_drv host_kbox_drv host_cdev_drv host_edma_drv 2>/dev/null

# 用户态
cd iBMA2.0 && sudo bash install.sh -u                  # 或按《iBMA 用户指南》卸载章节
```

---

## 8. 内核升级之后

* **DKMS 方式**：`dkms.conf` 里 `AUTOINSTALL="yes"`，新内核发布会自动重建 —— 但**前提是已装对应
  内核头文件**。若头文件装得晚，补一句即可：
  ```bash
  sudo apt install proxmox-headers-<新内核版本>
  sudo dkms autoinstall -k <新内核版本>
  sudo dkms status
  ```
* **`--direct` / `--deb` 方式**：模块的 `vermagic` 与新内核不匹配，升级内核后必须**重新编译安装**：
  ```bash
  cd huawei-ibma-driver-adapt && sudo ./scripts/build-ibma-driver.sh --direct
  ```

---

## 9. 常见问题（FAQ）

<details>
<summary><b>Q1. <code>lspci</code> 看不到 <code>19e5:1710/1712</code> 设备</b></summary>

iBMA 依赖 iBMC 暴露的专用 PCIe 通道。请到 iBMC Web → **诊断 → 黑匣子 → 启用**，然后**重启服务器**；
老固件（iBMC V3 早期版本）可能没有该功能或通道不可用，此时无法使用 iBMA 2.0。
</details>

<details>
<summary><b>Q2. <code>modprobe</code> 报 <code>required key not available</code> / 模块不加载</b></summary>

Secure Boot 打开时，未签名（或密钥未 enroll）的模块会被拒绝。两种做法：

```bash
mokutil --sb-state                       # 确认状态
# A) 关闭 Secure Boot（BIOS/固件里改）
# B) 或 enroll DKMS 自动生成的密钥：
sudo mokutil --import /var/lib/dkms/mok.pub    # 设一次性密码，重启后在 MOK 管理界面确认
```
</details>

<details>
<summary><b>Q3. dmesg 里 <code>module verification failed ... tainting kernel</code> 是错误吗？</b></summary>

不是。这是内核"模块签名未通过校验"的提示：模块仍然**正常加载**，只是给内核打上 taint 标记
（`/proc/sys/kernel/tainted`，位 13 = 8192）。原因通常是模块用 DKMS 自签密钥签名、而该密钥没有
enroll 进固件信任链。任何 DKMS 外挂模块（ZFS 等）在同样环境下都会打这条。只有在**开启 Secure Boot**
时它才会变成"拒绝加载"。
</details>

<details>
<summary><b>Q4. 编译报 <code>xxx.h: No such file or directory</code></b></summary>

十有八九是用了**未适配的原版源码**（`EXTRA_CFLAGS` 被 kbuild 忽略导致 `-I` 路径丢失）。
请确认你用的是本仓库的 `driver/`（或用 `patches/` 里的补丁打到原始源码上）。
</details>

<details>
<summary><b>Q5. <code>vermagic</code> 不匹配 / 升级内核后失效</b></summary>

模块必须针对**运行中的内核**编译。见[第 8 节](#8-内核升级之后)。
</details>

<details>
<summary><b>Q6. 黑匣子（kbox）模块没加载，正常吗？</b></summary>

正常。iBMA 默认配置 `iBMA.ini` 中 `iBMA_kbox=false`，`host_kbox_drv` 只在需要"黑匣子"功能时才加载。
</details>

<details>
<summary><b>Q7. 能不能只装驱动、不装用户态？</b></summary>

可以：驱动只是个 PCIe 通道驱动，装上后没有用户态服务就不会有任何数据上报，也不会破坏系统。
但**完整功能必须装用户态软件**。
</details>

---

## 10. 已知的无害提示

| 提示 | 说明 |
|---|---|
| `Skipping BTF generation for xxx.ko due to unavailability of vmlinux` | 外部模块缺少 vmlinux 时的正常提示，不影响加载。 |
| dmesg `module verification failed ... tainting kernel` | 见 [Q3](#9-常见问题faq)，仅 taint 标记，模块正常加载。 |

> **v0.4.0-pve2 起**：编译过程已无 `VM_* redefined`、`-Wmissing-prototypes` 等告警；
> 并额外修掉了 6 个运行时缺陷（`veth_tx` 非法返回值 + skb 泄漏、`alloc_netdev_mq` 未判空、
> sysfs 早读空指针、`kbox` 控制台注销顺序、`VM_RESERVED` 标志位、`/proc/kbox` 未清理）。
> 详见 [CHANGELOG.md](CHANGELOG.md)。

---

## 11. 仓库结构、改动与许可

```
.
├── driver/                       # 已适配的 iBMA_Driver 0.4.0 源码（从此处编译）
│   ├── edma_drv/  cdev_drv/  kbox_drv/  veth_drv/  cdev_veth_drv/  secure/  include/
│   ├── Makefile                  # make all KERNEL_VERSION=$(uname -r)
│   ├── build_manual.sh           # 生成 rpm/deb（华为原脚本）
│   └── dkms.conf
├── patches/
│   └── ibma-driver-0.4.0-kernel7.0.patch   # 相对华为原版的统一 diff（可直接 patch）
├── scripts/
│   └── build-ibma-driver.sh      # 一键编译/安装（--direct | --dkms | --deb）
├── docs/
│   └── ADAPTATION.md             # 适配技术说明（含内核源码行号证据）
├── CHANGELOG.md
├── NOTICE.md                     # 来源、改动、非官方声明、GPL 合规说明
├── LICENSE                       # GPL-2.0（driver/ 主体）
└── LICENSE-MulanPSL-2.0.txt      # Mulan PSL v2（driver/secure/）
```

**改动清单**（14 个文件，详见 [`docs/ADAPTATION.md`](docs/ADAPTATION.md) 与 [`patches/`](patches/)）：

1. 5 个 `Makefile`：`EXTRA_CFLAGS` → `ccflags-y`，`stdarg.h` 探测路径补全；
2. `edma_drv/Makefile`、`kbox_drv/Makefile`：新增"按目标内核头文件探测 API"的宏
   （`HAVE_TIMER_DELETE_SYNC` / `HAVE_HRTIMER_SETUP` / `HAVE_MSR_Q_SAFE`）；
3. `edma_drv/bma_include.h`：`HAVE_TIMER_SETUP` 兼容 `timer_container_of`，显式包含 `<linux/timer.h>`；
4. `edma_drv/edma_host.c`：`hrtimer_init` / `del_timer_sync` 新旧 API 双分支；
5. `kbox_drv/kbox_mce.c`：`rdmsrl_safe` → `rdmsrq_safe` 双分支；
6. `veth_drv/veth_hb.c`：新增类型正确的 `netdev_tx_t veth_tx_ndo()` 包装（行为不变）；
7. **缺陷修复**（v0.4.0-pve2）：`veth_tx` 非法返回值 + skb 泄漏、`alloc_netdev_mq` 未判空、
   sysfs 早读空指针、`kbox` 控制台注销顺序（use-after-free）、`kbox` 的 `VM_RESERVED` 标志位、
   `/proc/kbox` 未清理；同时删除 `edma_host.h` 的死代码 `VM_*` 段、`wait_done_dma_queue` 改 `static`。

**许可**

* `driver/` 主体：**GPL-2.0-or-later**（华为，见各目录 `copyright.txt` / `License.txt`）
* `driver/secure/`（Huawei libboundscheck）：**Mulan PSL v2**
* 本仓库的适配改动与文档：同样按上述许可分发，**不提供任何担保**（NO WARRANTY）
* 非华为官方项目，商标归各自所有者；如权利人认为不妥，请开 issue。

---

## English Quick Start

This repo contains **only the open-source kernel driver** of Huawei's iBMA (v0.4.0, DKMS source),
minimally adapted to build on modern kernels (**6.15 / 6.17 / 7.0**, incl. **Proxmox VE 9.1/9.2**).
The iBMA **userspace** package is proprietary-ish and must be downloaded from Huawei's support site.

```bash
# 1) deps + kernel headers (Proxmox VE example)
sudo apt update && sudo apt install -y build-essential dpkg-dev binutils dwarves dkms pciutils ipmitool proxmox-headers-$(uname -r)

# 2) install the kernel driver
git clone https://github.com/zknsmsw/huawei-ibma-driver-adapt.git && cd huawei-ibma-driver-adapt
sudo ./scripts/build-ibma-driver.sh --dkms        # or --direct / --deb

# 3) verify
modinfo -F vermagic host_edma_drv
lspci -nnk -s <bus>            # expect: Kernel driver in use: edma_drv
ls -l /dev/hwibmc*

# 4) then install the userspace package downloaded from Huawei
unzip iBMA_*_Software_Linux_x86_64.zip && tar -xzf iBMA-Linux-*.tar.gz
cd iBMA2.0 && sudo bash install.sh -s
```

Prerequisites: the iBMC must expose PCI device `19e5:1710` or `19e5:1712`
(enable *Black Box* in the iBMC Web UI and reboot). See [`docs/ADAPTATION.md`](docs/ADAPTATION.md)
for the technical rationale (with kernel-source references).

Licenses: GPL-2.0-or-later (driver) and Mulan PSL v2 (`driver/secure/`). Not affiliated with Huawei.
