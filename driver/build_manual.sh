#!/bin/bash
#******************************************************************************
# Copyright (C), 2020-2021, Huawei Tech. Co., Ltd.
# File : build_manual.sh
# 1.Build driver ko files and form them into a rpm pkg.
#
#  History       :
#  1.Date        : 2020/11/25
#    Modification: Add description
#  2.Date        : 2022/06/25
#    Modification: Build with a non-root user
#******************************************************************************
set -e

export DRV_VERSION=0.4.0
export KMOD_NAME=ibmadriver

build_ko()
{
    cd ${BIN_DIR}
    rm -f *.ko

    modules=("edma_drv" "cdev_drv" "veth_drv" "kbox_drv" "cdev_veth_drv")

    # cdev module should not be supported by euleros
    if [[ "$1" =~ "euleros" ]]; then
        unset modules[4]
    fi

    for module in "${modules[@]}"; do
        cd ${module} || return 1
        if ! make_ret=$(make -j); then
            echo "Build ${module} failed, message:${make_ret}."
            return 1
        fi
        mv *.ko ..
        cd ..
    done

    strip --strip-debug *.ko

    echo "Build driver ko successfully."
    return 0
}
cd $(dirname ${0})
BIN_DIR=$(pwd)
ARCH_TYPE=$(uname -m)
BEP_ENV_CONF_FILE="bep_env.conf"

uname_r_version=$(uname -r)
kernel_ver=$2
if [ -n "${kernel_ver}" ]; then
    export KERNEL_VERSION=${kernel_ver}
    export BUILD_PWD=${BIN_DIR}
    KERNEL_VERSION=${kernel_ver}
else
    export KERNEL_VERSION=${uname_r_version}
    export BUILD_PWD=${BIN_DIR}
    KERNEL_VERSION=${uname_r_version}
fi

echo "kernel version: ${KERNEL_VERSION}"

FLAVOR=$(echo ${KERNEL_VERSION} |awk -F- '{print $NF}')
kerv_array=(${KERNEL_VERSION})

UNSAFE_COMPILATION_SYSTEM_ARRAY=("kylin" "uos" "neokylin")
DRIVER_KO_DIR_ARRAY=(cdev_drv cdev_veth_drv edma_drv kbox_drv veth_drv)

set_env()
{
    PUBLISHER=$(echo "$1" | cut -d '-' -f 1)
    OS_VERSION=$(echo "$1" | cut -d '-' -f 2)
    if [ -f /etc/os-release ] && [ -n "$(grep -woEi 'Ubuntu|Debian|uos|Linx' /etc/os-release)" ] && which dpkg >/dev/null 2>&1; then
        PACKAGE_TYPE="deb"
    else
        PACKAGE_TYPE="rpm"
    fi

    if [[ "$1" =~ "euleros" ]]; then
        sed -i "s/cdev_veth_drv//g" ${BIN_DIR}/Makefile
        sed -i "/Source4/d" ${BIN_DIR}/linux_pack/rpm/iBMA_driver.spec
        sed -i "/SOURCE4/d" ${BIN_DIR}/linux_pack/rpm/iBMA_driver.spec
        sed -i "s/\"cdev_veth_drv\"//g" ${BIN_DIR}/linux_pack/rpm/iBMA_driver.spec
    fi

    # remove the "-fstack-protector-all" option in Makefile if the "-fstack-protector-all" option is not supported by this os
    for os_type in ${UNSAFE_COMPILATION_SYSTEM_ARRAY[@]}
    do
        if [[ "$1" =~ "${os_type}" ]]; then
            for driver_dir in ${DRIVER_KO_DIR_ARRAY[@]}
            do
                sed -i 's/EXTRA_CFLAGS += -Wformat=0 -fstack-protector-all/EXTRA_CFLAGS += -Wformat=0/g' ${BIN_DIR}/${driver_dir}/Makefile
            done
            break
        fi
    done
}

build_driver_rpm()
{
    echo "Start build rpm..."

    mkdir -p $HOME/rpmbuild

    cd $HOME
    echo "%_topdir $HOME/rpmbuild" > .rpmmacros
    rm -rf ./rpmbuild/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

    if ! ret_mkdir=$(mkdir -pv -m 750 ./rpmbuild/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS} 2>&1); then
        "mkdir return ${ret_mkdir}."
        return 1
    fi

    cp -f $BIN_DIR/*.ko   $HOME/rpmbuild/SOURCES/

    if [ "$PUBLISHER" = "sles" ]; then
        cp -f $BIN_DIR/linux_pack/rpm/fileslist  $HOME/rpmbuild/SOURCES/fileslist
    fi

    cp -f $BIN_DIR/linux_pack/rpm/iBMA_driver.spec  $HOME/rpmbuild/SPECS/iBMA_driver.spec
    chmod 640 $HOME/rpmbuild/SPECS/iBMA_driver.spec

    if [ "$PUBLISHER" = "sles" ] || [ "$PUBLISHER" = "citrix" ]; then
        echo "Start build ${KERNEL_VERSION}"

        cd $HOME/rpmbuild
        if ! ret_driver_build=$(rpmbuild -bb --define "version_var $DRV_VERSION" \
                                --define 'kmod_name iBMA_driver' \
                                --define "kernel $KERNEL_VERSION" \
                                --define "os_type $PUBLISHER" \
                                --define "os_version $OS_VERSION" \
                                --define "os_flavor $FLAVOR" SPECS/iBMA_driver.spec 2>&1); then
            echo "Build rpm failed, message:${ret_driver_build}."
            return 1
        else
            echo "Build rpm successfully."
        fi
    else
        for ((i=0;i<${#kerv_array[@]};i++))
        do
            echo "Start build ${kerv_array[$i]}"
            cd $HOME/rpmbuild
            if ! rpm_ret=$(rpmbuild -bb --define "version_var $DRV_VERSION" \
                           --define 'kmod_name iBMA_driver' \
                           --define "kernel ${kerv_array[$i]}" \
                           --define "os_type $PUBLISHER" \
                           --define "os_version $OS_VERSION" \
                           --define "os_flavor $FLAVOR" SPECS/iBMA_driver.spec 2>&1); then
                echo "Build ${kerv_array[$i]} failed, message:${rpm_ret}."
                return 1
            else
                echo "Build ${kerv_array[$i]} successfully."
            fi
        done
    fi

    mv $HOME/rpmbuild/RPMS/${ARCH_TYPE}/*.rpm $BIN_DIR
    return 0
}

build_driver_deb()
{
    echo "Start build deb..."

    rm -rf $BIN_DIR/linux_pack/deb/$KMOD_NAME/lib/modules/*
    mkdir -p $BIN_DIR/linux_pack/deb/$KMOD_NAME/lib/modules/$KERNEL_VERSION/updates/$KMOD_NAME

    cp -f $BIN_DIR/*.ko   $BIN_DIR/linux_pack/deb/$KMOD_NAME/lib/modules/$KERNEL_VERSION/updates/$KMOD_NAME/

    architecture=$(dpkg --print-architecture)
    sed -i "/^Version/c Version: $DRV_VERSION"  $BIN_DIR/linux_pack/deb/$KMOD_NAME/DEBIAN/control
    sed -i "/^Architecture/c Architecture: $architecture"  $BIN_DIR/linux_pack/deb/$KMOD_NAME/DEBIAN/control
    chmod -R 775 $BIN_DIR/linux_pack/deb/$KMOD_NAME/DEBIAN/
    cd $BIN_DIR/linux_pack/deb || return 1

    if ! dpkg_ret=$(dpkg-deb -Zxz --uniform-compression -b $KMOD_NAME $KMOD_NAME-$KERNEL_VERSION-$DRV_VERSION-$PUBLISHER$OS_VERSION.$architecture.deb 2>&1); then
        echo "Build deb failed, message:${dpkg_ret}."
        return 1
    else
        echo "Build deb successfully."
    fi

    mv $BIN_DIR/linux_pack/deb/$KMOD_NAME-$KERNEL_VERSION-$DRV_VERSION-$PUBLISHER$OS_VERSION.$architecture.deb  $BIN_DIR
    return 0
}

build_driver_sles()
{
    echo "Start build sles driver..."
    local exclude_flavor="xen"
    local tmp_top_dir="${BIN_DIR}/tmp_top_dir"

    if [ -d "${tmp_top_dir}" ]; then
        rm -rf "${tmp_top_dir}"
    fi

    mkdir -p "${tmp_top_dir}"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

    # need to compile xen kernel package on UVP
    if [ "xen" = "${FLAVOR}" ];then
        exclude_flavor="default"
    fi

    rm -rf iBMA_driver-${DRV_VERSION}

    # make sles build sources
    mkdir iBMA_driver-${DRV_VERSION}
    cp -rf edma_drv/ kbox_drv/ veth_drv/ cdev_drv/ cdev_veth_drv/ secure/ iBMA_driver-${DRV_VERSION}
    if [[ -d "../include" ]]; then
        cp -rf ../include iBMA_driver-${DRV_VERSION}
    else
        cp -rf include iBMA_driver-${DRV_VERSION}
    fi
    tar --sort=name --format=gnu -jcf iBMA_driver-${DRV_VERSION}.tar.bz2 iBMA_driver-${DRV_VERSION}

    cp -f "${BIN_DIR}/iBMA_driver-${DRV_VERSION}.tar.bz2" "${tmp_top_dir}/SOURCES/"
    cp -f "${BIN_DIR}/linux_pack/rpm/iBMA_driver.preamble" "${tmp_top_dir}/SOURCES/"
    cp -f "${BIN_DIR}/linux_pack/rpm/SLES_iBMA_driver.spec" "${tmp_top_dir}/SPECS/"

    if ! ret=$(rpmbuild -ba --define "exclude_flavor $exclude_flavor" --define "_topdir ${tmp_top_dir}" "${tmp_top_dir}/SPECS/SLES_iBMA_driver.spec" 2>&1); then
        echo "Build sles driver failed, message:${ret}."
        return 1
    fi

    driver_path="$(echo "${ret}" | grep "iBMA_driver-kmp-.*.rpm" | grep "${FLAVOR}" | awk '{print $2}')"
    if [ -z "${driver_path}" ]; then
        echo "Not found the driver files in ${tmp_top_dir}/RPMS/$(uname -p)/."
        return 1
    fi

    # the driver add sles info
    driver_name=${driver_path##*/}
    # delete last two .
    driver_name=${driver_name%.*.*}
    new_driver_name="${driver_name}-${1}.$(uname -p).rpm"

    mv "${driver_path}" "${BIN_DIR}/${new_driver_name}"

    echo "Build sles driver successfully."

    return 0
}

function enable_bep()
{
    if ls /opt/buildtools/secBepkit-*/bep_env.sh 1> /dev/null 2>&1; then
        sec_bep_kit_path=$(ls /opt/buildtools/secBepkit-*/bep_env.sh)
        source "${sec_bep_kit_path}" -s "${BEP_ENV_CONF_FILE}"
        echo "Enable bep successfully. "
        return 0
    fi
}

main()
{
    local drv_info=$1
    local build_function

    if [ "${drv_info}" == "" ] || [ "$(echo \"${drv_info}\" | grep - | wc -l)" == "0" ] || [ "${drv_info}" == "-h" ]; then
        echo "Usage: ./build_manual.sh os_type-os_version [kernel_version]"
        echo "Example: ./build_manual.sh centos-8.0"
        return 1
    fi

    if [ ! -d "secure/src/" ]; then
        echo "secure libraries does not exit, please check"
        return 1
    fi

    enable_bep

    if echo "${drv_info}" | grep "sles" -q; then
        build_driver_sles "${drv_info}"
        return $?
    fi

    set_env ${drv_info}

    echo "Start build driver..."

    if ! build_ko $1; then
        echo "Build driver ko failed."
        return 1
    fi

    if [ "$PACKAGE_TYPE" = "rpm" ];then
        build_function="build_driver_rpm"
    elif [ "$PACKAGE_TYPE" = "deb" ];then
        build_function="build_driver_deb"
    else
        echo "The operating system is not supported."
        return 1
    fi

    if ! ${build_function}; then
        echo "Build driver failed."
        return 1
    else
        echo "Build driver successfully."
    fi

    return 0
}

main $1
