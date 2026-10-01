ifeq ($(shell uname -m), x86_64)
	host_kbox_drv-objs := kbox_main.o kbox_ram_drive.o kbox_ram_image.o kbox_ram_op.o kbox_printk.o kbox_dump.o kbox_hook.o kbox_mce.o kbox_panic.o
else
	host_kbox_drv-objs := kbox_main.o kbox_ram_drive.o kbox_ram_image.o kbox_ram_op.o kbox_printk.o kbox_dump.o kbox_hook.o kbox_panic.o
endif

obj-m	:= host_kbox_drv.o

KERNEL_VERSION ?= $(shell uname -r)
KERNEL_DIR ?= /lib/modules/${KERNEL_VERSION}/build
SECURE_DIR ?= ../secure/src/
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

# Kernel API feature probe: x86 MSR safe-read helper was renamed (rdmsrl_safe -> rdmsrq_safe).
HAVE_MSR_Q_SAFE := $(shell grep -qsw rdmsrq_safe $(KERNEL_DIR)/arch/x86/include/asm/msr.h 2>/dev/null && echo 1)
ifneq ($(HAVE_MSR_Q_SAFE),)
ccflags-y += -DHAVE_MSR_Q_SAFE
endif

GCC_VERSION_GREATER_OR_EQUAL_7 ?= $(shell expr `gcc -dumpversion | cut -f1 -d.` \>= 7)

host_kbox_drv-objs += $(SECURE_DIR)securecutil.o
host_kbox_drv-objs += $(SECURE_DIR)secureinput_a.o
host_kbox_drv-objs += $(SECURE_DIR)secureprintoutput_a.o
host_kbox_drv-objs += $(SECURE_DIR)memset_s.o
host_kbox_drv-objs += $(SECURE_DIR)memcpy_s.o
host_kbox_drv-objs += $(SECURE_DIR)sprintf_s.o
host_kbox_drv-objs += $(SECURE_DIR)vsnprintf_s.o
host_kbox_drv-objs += $(SECURE_DIR)vsprintf_s.o


ifdef DRV_VERSION
ccflags-y += -D DRV_VERSION=$(DRV_VERSION)
endif

ifeq ($(HAVE_LINUX_STDARG_H_FLAG), true)
ccflags-y += -D HAVE_LINUX_STDARG_H
endif

ifeq ($(STACK_PROTECT), true)
ccflags-y += -fstack-protector-all
endif

ccflags-y += -I$(obj) -I$(obj)/../edma_drv -I$(obj)/../../include/
ccflags-y += -I$(obj)/../secure/include/
ccflags-y += -I$(obj)/../include/
ccflags-y += -Wformat=0
ccflags-y += -I$(obj) -I$(obj)/../edma_drv
ccflags-y += -Wformat=0
ifeq "$(GCC_VERSION_GREATER_OR_EQUAL_7)" "1"
ccflags-y += -Wimplicit-fallthrough=3
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

