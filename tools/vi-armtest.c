// SPDX-License-Identifier: GPL-2.0
/*
 * vi-armtest: does cv1800-vi return frames that its DMA did not fill?
 *
 *   vi-armtest -c /dev/videoN [-n frames] [-s ms] [-S subdev]
 *
 * Run j1-stream.sh 0 setup first. Each buffer is zeroed before it goes back
 * to the driver. A real row is never all zero (the black level is 16 in the
 * top 8 bits), so zero rows in a returned frame were not written.
 *
 *   -s MS      hold each buffer MS milliseconds, like a slow consumer: the
 *              driver runs out of buffers and arms again mid-frame
 *              (default 200; 0 = queue back at once)
 *   -S SUBDEV  switch the sensor analogue gain between 1x and 2x every
 *              frame, to see if the sensor sends bad frames on a change
 *
 * Build: gcc -O2 -o vi-armtest vi-armtest.c
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

#define NBUF	4

static void xioctl(int fd, unsigned long req, void *arg, const char *name)
{
	while (ioctl(fd, req, arg) < 0) {
		if (errno == EINTR)
			continue;
		perror(name);
		exit(1);
	}
}

int main(int argc, char **argv)
{
	const char *dev = NULL, *subdev = NULL;
	int frames = 50, hold_ms = 200, opt, fd, sfd = -1, i, n, bad = 0;
	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct v4l2_requestbuffers rb = {
		.count = NBUF, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	uint8_t *p[NBUF];
	size_t len[NBUF];
	unsigned int w, h, stride;

	while ((opt = getopt(argc, argv, "c:n:s:S:")) != -1) {
		switch (opt) {
		case 'c': dev = optarg; break;
		case 'n': frames = atoi(optarg); break;
		case 's': hold_ms = atoi(optarg); break;
		case 'S': subdev = optarg; break;
		default:
			fprintf(stderr, "usage: %s -c /dev/videoN [-n frames] "
				"[-s ms] [-S subdev]\n", argv[0]);
			return 1;
		}
	}
	if (!dev) {
		fprintf(stderr, "give -c\n");
		return 1;
	}
	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror(dev);
		return 1;
	}
	if (subdev) {
		sfd = open(subdev, O_RDWR);
		if (sfd < 0) {
			perror(subdev);
			return 1;
		}
	}
	xioctl(fd, VIDIOC_G_FMT, &f, "VIDIOC_G_FMT");
	w = f.fmt.pix.width;
	h = f.fmt.pix.height;
	stride = f.fmt.pix.bytesperline;

	xioctl(fd, VIDIOC_REQBUFS, &rb, "VIDIOC_REQBUFS");
	for (i = 0; i < NBUF; i++) {
		struct v4l2_buffer vb = {
			.index = i, .type = type, .memory = V4L2_MEMORY_MMAP,
		};

		xioctl(fd, VIDIOC_QUERYBUF, &vb, "VIDIOC_QUERYBUF");
		len[i] = vb.length;
		p[i] = mmap(NULL, vb.length, PROT_READ | PROT_WRITE, MAP_SHARED,
			    fd, vb.m.offset);
		if (p[i] == MAP_FAILED) {
			perror("mmap");
			return 1;
		}
		memset(p[i], 0, len[i]);
		xioctl(fd, VIDIOC_QBUF, &vb, "VIDIOC_QBUF");
	}
	xioctl(fd, VIDIOC_STREAMON, &type, "VIDIOC_STREAMON");
	printf("%ux%u, stride %u, hold %d ms%s\n", w, h, stride, hold_ms,
	       sfd >= 0 ? ", gain switching" : "");

	for (n = 0; n < frames; n++) {
		struct v4l2_buffer vb = { .type = type, .memory = V4L2_MEMORY_MMAP };
		unsigned int y, x, rows = 0, zero = 0, first = h, last = 0;
		double sum = 0;
		long cnt = 0;

		xioctl(fd, VIDIOC_DQBUF, &vb, "VIDIOC_DQBUF");
		/* Every 8th row, 32 samples spread over the row */
		for (y = 0; y < h; y += 8) {
			const uint8_t *r = p[vb.index] + (size_t)y * stride;
			int nz = 0;

			for (x = 0; x < 32; x++) {
				uint8_t v = r[(w * 3 / 2 / 32 * x) / 3 * 3];

				nz += v != 0;
				sum += v;
				cnt++;
			}
			rows++;
			if (!nz) {
				zero++;
				if (y < first)
					first = y;
				last = y;
			}
		}
		if (zero)
			bad++;
		printf("seq %4u buf %u: mean %5.1f, unwritten rows %3u of %u",
		       vb.sequence, vb.index, sum / cnt, zero, rows);
		if (zero)
			printf(" (lines %u-%u)", first, last);
		printf("%s\n", vb.flags & V4L2_BUF_FLAG_ERROR ? " ERROR" : "");

		if (sfd >= 0) {
			struct v4l2_control c = {
				.id = V4L2_CID_ANALOGUE_GAIN,
				.value = n & 1 ? 64 : 128,
			};

			if (ioctl(sfd, VIDIOC_S_CTRL, &c) < 0)
				perror("gain");
		}
		if (hold_ms)
			usleep(hold_ms * 1000);
		memset(p[vb.index], 0, len[vb.index]);
		xioctl(fd, VIDIOC_QBUF, &vb, "VIDIOC_QBUF");
	}
	xioctl(fd, VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF");
	printf("%d of %d frames had unwritten rows\n", bad, frames);
	return 0;
}
