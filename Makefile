# Kernel module. Objects, .cmd, and the .ko stay in ./build.
MODULE_NAME := syscall_monitor
ARCH ?= $(shell uname -m | sed -e 's/x86_64/x86/' -e 's/aarch64/arm64/' -e 's/riscv64/riscv/')
CROSS_COMPILE ?=
KDIR ?= /lib/modules/$(shell uname -r)/build
SRC := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD := $(SRC)/build

.PHONY: module clean
module:
	mkdir -p $(BUILD)
	cp -f $(SRC)/syscall_monitor.c $(SRC)/syscall_monitor_shared.h $(BUILD)/
	printf 'obj-m += syscall_monitor.o\nccflags-y := -I$$(src)\n' > $(BUILD)/Kbuild
	$(MAKE) -C $(KDIR) M=$(BUILD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules
	rm -f $(SRC)/*.o $(SRC)/*.cmd $(SRC)/*.ko $(SRC)/*.mod $(SRC)/*.mod.c \
	      $(SRC)/Module.symvers $(SRC)/modules.order

clean:
	-$(MAKE) -C $(KDIR) M=$(BUILD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean
	rm -rf $(BUILD)/.tmp_versions
	rm -f $(SRC)/*.o $(SRC)/*.cmd $(SRC)/*.ko $(SRC)/*.mod $(SRC)/*.mod.c \
	      $(SRC)/Module.symvers $(SRC)/modules.order
