# Modified: added exporting DRV_VERSION
VERSION_VAR=0.4.0
export DRV_VERSION=${VERSION_VAR}

dirs=edma_drv cdev_drv kbox_drv veth_drv cdev_veth_drv

KERNEL_VERSION ?= $(shell uname -r)
SUPPORT_SAFE_FLAG = $(shell grep -woEi 'kylin|uos|bclinux' /etc/os-release | wc -l)
ifneq ($(SUPPORT_SAFE_FLAG),0)
STACK_PROTECT ?= false
endif
STACK_PROTECT ?= true

all:
	@echo nowbuild
	@for dir in $(dirs) ; do \
		( cd $$dir && make KERNEL_VERSION=${KERNEL_VERSION} STACK_PROTECT=${STACK_PROTECT} && mv *.ko ..) \
	done;

clean:
	@for dir in $(dirs) ; do \
		( cd $$dir && make clean) \
	done;
	-rm *.ko
