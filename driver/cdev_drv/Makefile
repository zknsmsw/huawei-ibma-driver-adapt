
host_cdev_drv-objs := bma_cdev.o
obj-m	:= host_cdev_drv.o

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

host_cdev_drv-objs += $(SECURE_DIR)securecutil.o
host_cdev_drv-objs += $(SECURE_DIR)secureinput_a.o
host_cdev_drv-objs += $(SECURE_DIR)secureprintoutput_a.o
host_cdev_drv-objs += $(SECURE_DIR)memset_s.o
host_cdev_drv-objs += $(SECURE_DIR)memcpy_s.o
host_cdev_drv-objs += $(SECURE_DIR)sprintf_s.o
host_cdev_drv-objs += $(SECURE_DIR)vsprintf_s.o

ifdef DRV_VERSION
ccflags-y += -D DRV_VERSION=$(DRV_VERSION)
endif

ifeq ($(STACK_PROTECT), true)
ccflags-y += -fstack-protector-all
endif

ccflags-y += -I$(obj) -I$(obj)/../../include/ -I$(obj)/../include/ -I$(obj)/../edma_drv

ccflags-y += -I$(obj)/../secure/include/
ccflags-y += -Wformat=0
ifeq "$(GCC_VERSION_GREATER_OR_EQUAL_7)" "1"
ccflags-y += -Wimplicit-fallthrough=3
endif

ifeq ($(HAVE_LINUX_STDARG_H_FLAG), true)
ccflags-y += -D HAVE_LINUX_STDARG_H
endif

PWD       := $(shell pwd)
KBUILD_EXTRA_SYMBOLS+=$(BUILD_PWD)/edma_drv/Module.symvers

all:
	@if [ -f ../edma_drv/Module.symvers ]; \
	then \
		cp ../edma_drv/Module.symvers ./; \
		$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules; \
		strip --strip-debug *.ko; \
	else \
		echo "please make edma_drv first";\
	fi


clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions *.order *.unsigned *.symvers

