#
# Makefile for the edma drivers.
#

obj-m := cdev_veth_drv.o

cdev_veth_drv-objs := virtual_cdev_eth_net.o

DRV_VERSION ?= 0.4.0

KERNEL_VERSION ?= $(shell uname -r)
BUILD_PWD ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../)

SUPPORT_SAFE_FLAG = $(shell grep -woEi 'kylin|uos|bclinux' /etc/os-release 2>/dev/null | wc -l)
ifneq ($(SUPPORT_SAFE_FLAG),0)
STACK_PROTECT ?= false
endif
STACK_PROTECT ?= true

STDARG_H_FLAG = $(wildcard /lib/modules/*/build/include/linux/stdarg.h \
                        /usr/src/kernels/*/include/linux/stdarg.h \
                        /usr/src/*/include/linux/stdarg.h)
ifeq ($(STDARG_H_FLAG),)
    HAVE_LINUX_STDARG_H_FLAG ?= false
else
    HAVE_LINUX_STDARG_H_FLAG ?= true
endif

KERNEL_DIR ?= /lib/modules/${KERNEL_VERSION}/build
SECURE_DIR ?= ../secure/src/

GCC_VERSION_GREATER_OR_EQUAL_7 ?= $(shell expr `gcc -dumpversion | cut -f1 -d.` \>= 7)

cdev_veth_drv-objs += $(SECURE_DIR)securecutil.o
cdev_veth_drv-objs += $(SECURE_DIR)secureinput_a.o
cdev_veth_drv-objs += $(SECURE_DIR)secureprintoutput_a.o
cdev_veth_drv-objs += $(SECURE_DIR)memset_s.o
cdev_veth_drv-objs += $(SECURE_DIR)memcpy_s.o

ifeq ($(STACK_PROTECT), true)
ccflags-y += -fstack-protector-all
endif

ccflags-y += -Wformat=0 \
                -I$(obj)/../edma_drv -I$(obj)/../include/ -I$(obj)/../../include/
ccflags-y += -I$(obj)/../secure/include/
ccflags-y += -DCDEV_VETH_VERSION=$(DRV_VERSION)
ccflags-y += -Wformat=0
ifeq "$(GCC_VERSION_GREATER_OR_EQUAL_7)" "1"
ccflags-y += -Wimplicit-fallthrough=3
endif

ifeq ($(HAVE_LINUX_STDARG_H_FLAG), true)
ccflags-y += -D HAVE_LINUX_STDARG_H
endif

KBUILD_EXTRA_SYMBOLS+=$(BUILD_PWD)/edma_drv/Module.symvers

all:
	cp ../edma_drv/Module.symvers . && make -C ${KERNEL_DIR} M=${CURDIR} modules
	strip --strip-debug *.ko

clean:
	make -C ${KERNEL_DIR} M=${CURDIR} clean
