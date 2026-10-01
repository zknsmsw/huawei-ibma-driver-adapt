
host_edma_drv-objs := bma_pci.o bma_devintf.o edma_host.o edma_queue.o
obj-m	:= host_edma_drv.o

KERNEL_VERSION ?= $(shell uname -r)
KERNEL_DIR ?= /lib/modules/${KERNEL_VERSION}/build

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

SECURE_DIR ?= ../secure/src/

# Kernel API feature probes: test the real headers of the kernel we build against,
# so the driver also works with distro backports (e.g. PVE 6.14 has timer_delete_sync).
HAVE_TIMER_DELETE_SYNC := $(shell grep -qsw timer_delete_sync $(KERNEL_DIR)/include/linux/timer.h 2>/dev/null && echo 1)
HAVE_HRTIMER_SETUP := $(shell grep -qsw hrtimer_setup $(KERNEL_DIR)/include/linux/hrtimer.h 2>/dev/null && echo 1)
ifneq ($(HAVE_TIMER_DELETE_SYNC),)
ccflags-y += -DHAVE_TIMER_DELETE_SYNC
endif
ifneq ($(HAVE_HRTIMER_SETUP),)
ccflags-y += -DHAVE_HRTIMER_SETUP
endif

GCC_VERSION_GREATER_OR_EQUAL_7 ?= $(shell expr `gcc -dumpversion | cut -f1 -d.` \>= 7)

host_edma_drv-objs += $(SECURE_DIR)securecutil.o
host_edma_drv-objs += $(SECURE_DIR)secureinput_a.o
host_edma_drv-objs += $(SECURE_DIR)secureprintoutput_a.o
host_edma_drv-objs += $(SECURE_DIR)memset_s.o
host_edma_drv-objs += $(SECURE_DIR)memcpy_s.o
host_edma_drv-objs += $(SECURE_DIR)sprintf_s.o
host_edma_drv-objs += $(SECURE_DIR)vsprintf_s.o

ifdef DRV_VERSION
ccflags-y += -D DRV_VERSION=$(DRV_VERSION)
endif

ifeq ($(STACK_PROTECT), true)
ccflags-y += -fstack-protector-all
endif

ccflags-y += -I$(obj) -I$(obj)/../../include/ -I$(obj)/../include/
ccflags-y += -I$(obj)/../secure/include/
ccflags-y += -Wformat=0
ifeq "$(GCC_VERSION_GREATER_OR_EQUAL_7)" "1"
ccflags-y += -Wimplicit-fallthrough=3
endif

ifeq ($(HAVE_LINUX_STDARG_H_FLAG), true)
ccflags-y += -D HAVE_LINUX_STDARG_H
endif

PWD       := $(shell pwd)

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD)
	strip --strip-debug *.ko;

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions *.order *.unsigned *.symvers

