# Out-of-tree build of duos-csi.ko.
#
# On the board, with the matching linux-headers package installed:
#
#   make
#
# On the host, against the tree Armbian already built:
#
#   make ARCH=riscv   CROSS_COMPILE=riscv64-linux-gnu- \
#        KDIR=<armbian-build>/cache/sources/linux-kernel-worktree/7.2__sophgo-sg200x-riscv64__riscv64
#
#   make ARCH=arm64   CROSS_COMPILE=aarch64-linux-gnu- \
#        KDIR=<armbian-build>/cache/sources/linux-kernel-worktree/7.2__sophgo-sg200x-arm64__arm64
#
# Same config requirements as duos-cam-stub.ko - videodev and V4L2_ASYNC, both of
# which come along with any sensor driver - plus nothing: the clock, reset and
# io accessors it adds are always built in.

obj-m += duos-csi.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
