# Kernel module. Objects, .cmd files and the .ko go to ./build/kmod, apart
# from the CMake tree in ./build, so "make clean" (kbuild clean deletes
# every *.o under M=) can never touch the userspace objects.
#
#   make                         build against the running kernel
#   make KDIR=/path/to/linux     build against another configured tree
#   make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KDIR=...   cross build
MODULE_NAME := syscall_monitor
KDIR ?= /lib/modules/$(shell uname -r)/build
SRC := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD := $(SRC)/build/kmod

# A native build passes no ARCH and kbuild uses the configured kernel's.
# A given ARCH may be a uname -m name; map it to the kernel's arch name.
norm_arch = $(patsubst i%86,x86,$(patsubst x86_64,x86,$(patsubst aarch64,arm64,$(patsubst armv%,arm,$(patsubst riscv64,riscv,$(1))))))
KBUILD_ARGS := $(if $(ARCH),ARCH=$(call norm_arch,$(ARCH))) $(if $(CROSS_COMPILE),CROSS_COMPILE=$(CROSS_COMPILE))

.PHONY: module clean
module:
	mkdir -p $(BUILD)
	cp -f $(SRC)/syscall_monitor.c $(SRC)/syscall_monitor_shared.h $(BUILD)/
	printf 'obj-m += $(MODULE_NAME).o\nccflags-y := -I$$(src)\n' > $(BUILD)/Kbuild
	$(MAKE) -C $(KDIR) M=$(BUILD) $(KBUILD_ARGS) modules
	rm -f $(SRC)/*.o $(SRC)/*.cmd $(SRC)/*.ko $(SRC)/*.mod $(SRC)/*.mod.c \
	      $(SRC)/Module.symvers $(SRC)/modules.order

clean:
	-[ -d $(BUILD) ] && $(MAKE) -C $(KDIR) M=$(BUILD) $(KBUILD_ARGS) clean
	rm -rf $(BUILD)
	rm -f $(SRC)/*.o $(SRC)/*.cmd $(SRC)/*.ko $(SRC)/*.mod $(SRC)/*.mod.c \
	      $(SRC)/Module.symvers $(SRC)/modules.order
