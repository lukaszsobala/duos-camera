# Out-of-tree build of duos-cam-stub.ko.
#
# On the board, with the matching linux-headers package installed:
#
#   make
#
# On the host, against the tree Armbian already built:
#
#   make ARCH=riscv   CROSS_COMPILE=riscv64-linux-gnu- \
#        KDIR=~/Software/armbian/build/cache/sources/linux-kernel-worktree/7.2__sophgo-sg200x-riscv64__riscv64
#
#   make ARCH=arm64   CROSS_COMPILE=aarch64-linux-gnu- \
#        KDIR=~/Software/armbian/build/cache/sources/linux-kernel-worktree/7.2__sophgo-sg200x-arm64__arm64
#
# This one needs far less of the kernel config than gc2083.ko does: videodev and
# V4L2_ASYNC, both of which come along with any sensor driver. If gc2083.ko
# builds, this builds.

obj-m += duos-cam-stub.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
