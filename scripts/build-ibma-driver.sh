#!/bin/bash
#==============================================================================
# Huawei iBMA kernel driver - build & install helper
#
#   Builds the (adapted) iBMA_Driver 0.4.0 out-of-tree kernel modules against the
#   running kernel, and optionally installs them.
#
#   Usage (run on the target machine):
#     ./scripts/build-ibma-driver.sh                  # build only -> ./out
#     sudo ./scripts/build-ibma-driver.sh --direct    # build + copy .ko to /lib/modules
#     sudo ./scripts/build-ibma-driver.sh --dkms      # build + register as DKMS module
#     sudo ./scripts/build-ibma-driver.sh --deb       # build + build/install vendor .deb
#     sudo ./scripts/build-ibma-driver.sh --deb --ibma-dir /root/iBMA2.0
#                                                     # ... and drop it into
#                                                     # <iBMA2.0>/drivers/Debian/ so the
#                                                     # official iBMA installer uses it
#
#   Options:
#     --src <dir>      driver source tree (default: <repo>/driver)
#     --kernel <ver>   kernel version to build for (default: uname -r)
#
#   Exit codes: 0 ok, 1 error, 2 preflight failed (missing packages/headers)
#==============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SRC_DIR="${REPO_ROOT}/driver"
BUILD_ROOT="${REPO_ROOT}/.build"
OUT_DIR="${REPO_ROOT}/out"
KVER="$(uname -r)"
DRV_VERSION="0.4.0"
MODE="build"
IBMA_DIR=""

RED=$'\e[31m'; GRN=$'\e[32m'; YEL=$'\e[33m'; NC=$'\e[0m'
info() { echo "${GRN}[iBMA]${NC} $*"; }
warn() { echo "${YEL}[warn]${NC} $*"; }
err()  { echo "${RED}[FAIL]${NC} $*" >&2; }

while [ $# -gt 0 ]; do
    case "$1" in
        --deb)      MODE="deb" ;;
        --dkms)     MODE="dkms" ;;
        --direct)   MODE="direct" ;;
        --src)      shift; SRC_DIR="${1:-}" ;;
        --kernel)   shift; KVER="${1:-}" ;;
        --ibma-dir) shift; IBMA_DIR="${1:-}" ;;
        -h|--help)  sed -n '2,25p' "$0"; exit 0 ;;
        *) err "unknown argument: $1"; exit 1 ;;
    esac
    shift
done

KDIR="/lib/modules/${KVER}/build"

#--------------------------------------------------------------- preflight ---
if [ "${MODE}" != "build" ] && [ "$(id -u)" != "0" ]; then
    err "install modes (--deb/--dkms/--direct) must run as root"
    exit 1
fi
[ -d "${SRC_DIR}" ] || { err "driver source not found: ${SRC_DIR}"; exit 1; }
[ -f "${SRC_DIR}/Makefile" ] || { err "${SRC_DIR} does not look like the iBMA_Driver tree"; exit 1; }

MISSING=0
for t in gcc make tar strip; do
    command -v "$t" >/dev/null 2>&1 || { warn "missing tool: $t"; MISSING=1; }
done
if [ ! -d "${KDIR}" ]; then
    warn "kernel build tree not found: ${KDIR}"
    echo  "    Proxmox VE : apt install -y proxmox-headers-${KVER}"
    echo  "    Debian     : apt install -y linux-headers-${KVER}"
    MISSING=1
fi
if [ "${MODE}" = "deb" ]; then
    command -v dpkg-deb >/dev/null 2>&1 || { warn "missing dpkg-deb (apt install dpkg-dev)"; MISSING=1; }
fi
if [ "${MODE}" = "dkms" ]; then
    command -v dkms >/dev/null 2>&1 || { warn "missing dkms (apt install dkms)"; MISSING=1; }
fi
if [ "${MISSING}" = "1" ]; then
    err "preflight failed. On Proxmox VE / Debian:"
    echo "    apt update"
    echo "    apt install -y build-essential dpkg-dev binutils dwarves dkms pciutils ipmitool \\"
    echo "                   proxmox-headers-${KVER}   # 或 linux-headers-${KVER}"
    exit 2
fi

# Secure Boot check (only warn; unsigned modules are accepted unless SB/lockdown is on)
SB_STATE="$(mokutil --sb-state 2>/dev/null || true)"
case "${SB_STATE}" in
    *[Ee]nabled*)
        warn "Secure Boot is ENABLED: an unsigned module will NOT load."
        warn "Disable Secure Boot, or enroll the DKMS MOK key (mokutil --import)." ;;
esac

info "kernel            : ${KVER}"
info "kernel build tree : $(readlink -f "${KDIR}")"
info "gcc               : $(gcc -dumpversion 2>/dev/null)"

#------------------------------------------------------------------------------
# Build in a scratch copy so the source tree stays clean.
#------------------------------------------------------------------------------
WORK="${BUILD_ROOT}/iBMA_Driver-${DRV_VERSION}"
rm -rf "${BUILD_ROOT}" "${OUT_DIR}"
mkdir -p "${BUILD_ROOT}" "${OUT_DIR}"
cp -a "${SRC_DIR}" "${WORK}"

info "building modules for ${KVER} ..."
if ! make -C "${WORK}" all KERNEL_VERSION="${KVER}" STACK_PROTECT=false -j"$(nproc)"; then
    err "build failed. Re-run verbosely:"
    echo "    make -C ${WORK} all KERNEL_VERSION=${KVER} STACK_PROTECT=false V=1 2>&1 | tee /tmp/ibma-build.log"
    exit 1
fi

KO_LIST="host_edma_drv host_cdev_drv host_veth_drv cdev_veth_drv host_kbox_drv"
for m in ${KO_LIST}; do
    [ -f "${WORK}/${m}.ko" ] || { err "expected module not built: ${m}.ko"; exit 1; }
    vm="$(modinfo -F vermagic "${WORK}/${m}.ko" 2>/dev/null)"
    case "${vm}" in
        "${KVER}"*) : ;;
        *) warn "${m}.ko vermagic mismatch: '${vm}' (expected ${KVER})" ;;
    esac
done
cp -f "${WORK}"/*.ko "${OUT_DIR}/"
KO_COUNT="$(printf '%s\n' ${KO_LIST} | wc -l)"
info "built ${KO_COUNT} modules -> ${OUT_DIR}"

#------------------------------------------------ optional: vendor .deb -----
build_vendor_deb() {
    info "building vendor-format driver .deb ..."
    chmod +x "${WORK}/build_manual.sh" 2>/dev/null || true
    ( cd "${WORK}" && ./build_manual.sh debian-13 ) || return 1
    local deb
    deb="$(ls -1t "${WORK}"/ibmadriver-*.deb 2>/dev/null | head -n1)"
    [ -n "${deb}" ] || return 1
    cp -f "${deb}" "${OUT_DIR}/"
    info "driver package: $(basename "${deb}")"
}

#----------------------------------------------------------------- install ---
case "${MODE}" in
build)
    build_vendor_deb || warn "vendor .deb build failed (optional); the plain .ko files are in ${OUT_DIR}"
    info "build only. To install, re-run with one of: --direct | --dkms | --deb"
    ;;

deb)
    build_vendor_deb || { err "could not build the driver .deb"; exit 1; }
    deb="$(ls -1t "${OUT_DIR}"/ibmadriver-*.deb | head -n1)"
    info "installing $(basename "${deb}") ..."
    dpkg -i "${deb}" || { err "dpkg -i failed"; exit 1; }
    if [ -n "${IBMA_DIR}" ]; then
        mkdir -p "${IBMA_DIR}/drivers/Debian"
        cp -f "${deb}" "${IBMA_DIR}/drivers/Debian/"
        info "driver package placed in ${IBMA_DIR}/drivers/Debian/"
    fi
    ;;

dkms)
    DKMS_SRC="/usr/src/iBMA_Driver-${DRV_VERSION}"
    info "installing DKMS source tree to ${DKMS_SRC} ..."
    rm -rf "${DKMS_SRC}"
    cp -a "${WORK}" "${DKMS_SRC}"
    dkms remove -m iBMA_Driver -v "${DRV_VERSION}" --all >/dev/null 2>&1
    dkms add     -m iBMA_Driver -v "${DRV_VERSION}"        || exit 1
    dkms build   -m iBMA_Driver -v "${DRV_VERSION}"        || { err "dkms build failed"; exit 1; }
    dkms install -m iBMA_Driver -v "${DRV_VERSION}" --force || { err "dkms install failed"; exit 1; }
    dkms status  -m iBMA_Driver -v "${DRV_VERSION}"
    ;;

direct)
    DEST="/lib/modules/${KVER}/updates/iBMA_driver"
    info "installing modules to ${DEST} ..."
    mkdir -p "${DEST}"
    cp -f "${OUT_DIR}"/*.ko "${DEST}/"
    depmod -a "${KVER}" || exit 1
    ;;
esac

#------------------------------------------------------------------ verify ---
info "verifying module database ..."
for m in ${KO_LIST}; do
    p="$(modinfo -n "${m}" 2>/dev/null || true)"
    if [ -n "${p}" ]; then echo "    ${m} -> ${p}"; else warn "${m}: not in depmod database"; fi
done

# NOTE: no `... | grep -q` here - with `set -o pipefail` grep exiting early sends
# SIGPIPE to lspci (141) and the pipeline would be reported as failed.
PCIE_INFO="$(lspci -nn 2>/dev/null | grep -iE '19e5:(1710|1712)')"
if [ -n "${PCIE_INFO}" ]; then
    info "Huawei iBMA PCIe device present:"
    printf '    %s\n' "${PCIE_INFO}"
    info "loading host_edma_drv / host_cdev_drv ..."
    modprobe host_edma_drv 2>&1 | sed 's/^/    /'
    modprobe host_cdev_drv 2>&1 | sed 's/^/    /'
    sleep 1
    lsmod | grep -E 'host_edma_drv|host_cdev_drv' | sed 's/^/    /' || true
    ls -l /dev/hwibmc* 2>/dev/null | sed 's/^/    /' || warn "/dev/hwibmc* not created (check dmesg)"
else
    warn "PCIe device 19e5:1710/1712 not visible on this machine."
    warn "On iBMC Web UI: Diagnose -> Black Box -> enable, then REBOOT;"
    warn "after reboot verify with:  lspci -nn | grep 19e5"
fi

cat <<EOF

$(info "done.")

Next step - install the iBMA userspace package (download it from Huawei):
  1) download "iBMA_<ver>_Software_Linux_x86_64.zip" from Huawei Support-E
  2) unzip iBMA_*.zip && tar -xzf iBMA-Linux-*.tar.gz
  3) cd iBMA2.0 && sudo bash install.sh -s
  4) verify: systemctl status iBMA ; lspci -nnk -s <bus>
EOF
