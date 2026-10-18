# Out-of-tree build against the kernel tree Armbian already built.
#
#   make ARCH=arm64   CROSS_COMPILE=aarch64-linux-gnu- \
#        KDIR=<armbian-build>/cache/sources/linux-kernel-worktree/7.2__sophgo-sg200x-arm64__arm64
#
#   make ARCH=riscv   CROSS_COMPILE=riscv64-linux-gnu- \
#        KDIR=<armbian-build>/cache/sources/linux-kernel-worktree/7.2__sophgo-sg200x-riscv64__riscv64
#
# The worktree is owned by root (Armbian builds under sudo), so either run this
# with sudo -E or copy the tree first. On the board, `make -C /lib/modules/$(uname -r)/build`
# works instead if the linux-headers package is installed.

obj-m += duos-mclk.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
