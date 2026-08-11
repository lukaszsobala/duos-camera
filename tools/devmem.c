// SPDX-License-Identifier: GPL-2.0
/*
 * A 32-bit /dev/mem peek/poke, for boards whose busybox was built without the
 * devmem applet. Needs no kernel headers:
 *
 *	gcc -O2 -Wall -o devmem devmem.c
 *
 *	./devmem 0x03001b14		read
 *	./devmem 0x03001b14 0x44	write, then read back
 *
 * The Armbian sophgo-sg200x kernels have CONFIG_DEVMEM=y and STRICT_DEVMEM
 * unset, so the pinctrl registers at 0x03001000 are reachable even though the
 * pinctrl driver has claimed them.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	unsigned long addr, page;
	volatile uint32_t *reg;
	int writing = (argc == 3);
	long pagesize;
	void *map;
	int fd;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: %s <phys-addr> [value]\n", argv[0]);
		return 2;
	}

	addr = strtoul(argv[1], NULL, 0);
	if (addr & 3) {
		fprintf(stderr, "address must be 4-byte aligned\n");
		return 2;
	}

	fd = open("/dev/mem", (writing ? O_RDWR : O_RDONLY) | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}

	pagesize = sysconf(_SC_PAGESIZE);
	page = addr & ~(unsigned long)(pagesize - 1);

	map = mmap(NULL, pagesize, writing ? PROT_READ | PROT_WRITE : PROT_READ,
		   MAP_SHARED, fd, page);
	if (map == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return 1;
	}

	reg = (volatile uint32_t *)((char *)map + (addr - page));

	if (writing) {
		*reg = (uint32_t)strtoul(argv[2], NULL, 0);
		__sync_synchronize();
	}
	printf("0x%08lx = 0x%08x\n", addr, *reg);

	munmap(map, pagesize);
	close(fd);
	return 0;
}
