# Out-of-tree build of gc2083.ko.
#
# A kernel module is not built with `gcc gc2083.c -o gc2083.ko`. That invocation
# compiles against glibc's /usr/include, which is why it fails on the very first
# kernel header (`linux/array_size.h: No such file or directory`) - that header
# lives in the kernel tree, not in libc. Kbuild has to drive the compiler.
#
# On the board, with linux-headers-<branch>-sophgo-sg200x-<arch> installed:
#
#   make
#
# On the host, against the tree Armbian already built:
#
#   make ARCH=arm64   CROSS_COMPILE=aarch64-linux-gnu- \
#        KDIR=~/Software/armbian/build/cache/sources/linux-kernel-worktree/7.2__sophgo-sg200x-arm64__arm64
#
#   make ARCH=riscv   CROSS_COMPILE=riscv64-linux-gnu- \
#        KDIR=~/Software/armbian/build/cache/sources/linux-kernel-worktree/7.2__sophgo-sg200x-riscv64__riscv64
#
# Either way this only links against a kernel whose config has V4L2_CCI_I2C and
# VIDEO_V4L2_SUBDEV_API. Kernels with the PR #10936 camera patches have both.
# Without them the build stops at `v4l2_subdev_get_fmt undeclared` or at
# modpost `cci_read undefined`.
# See "Protocol: gc2083.ko on the board" in README.md.

obj-m += gc2083.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
