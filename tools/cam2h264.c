// SPDX-License-Identifier: GPL-2.0
/*
 * cam2h264: GC2083 raw frames -> half-size NV12 -> CODA980 H.264, or
 * -> JPU motion JPEG with -j.
 *
 *   cam2h264 -c /dev/videoN [options]   from the camera (cv1800-vi-raw)
 *   cam2h264 -i cap.raw [options]       from a file of raw frames
 *
 * Options:
 *   -e DEV   encoder (default: the device named coda-video-encoder, or
 *            coda-jpeg-encoder with -j)
 *   -o FILE  output H.264 Annex B stream, or concatenated JPEG frames with
 *            -j (default out.h264, out.mjpeg)
 *   -n N     frames to encode, 0 = until Ctrl-C (default 300)
 *   -q QP    constant QP, 0-51 (default 30)
 *   -g N     GOP size (default 30)
 *   -j       motion JPEG with the JPEG unit, instead of H.264
 *   -Q N     JPEG quality, 5-100 (default 85)
 *   -s F     colour saturation (default 1.5)
 *   -y FILE  write NV12 frames to FILE, without the encoder
 *
 * The input is packed RAW12, RGGB, 1920x1080, 10-bit data in the top bits.
 * Each 2x2 Bayer cell becomes one output pixel: 960x540. The program uses
 * only the top 8 bits of each sample. White balance and exposure are
 * gray-world, from a sample of each frame. There is no colour matrix.
 *
 * Build: cc -O2 -o cam2h264 cam2h264.c -lm
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define RAW_W	1920
#define RAW_H	1080
#define OUT_W	(RAW_W / 2)
#define OUT_H	(RAW_H / 2)
#define BLACK	16		/* 64 in 10 bits, in the top 8 bits */
#define NBUF_CAM 4
#define NBUF_ENC_OUT 2
#define NBUF_ENC_CAP 4

struct buf {
	void *p;
	size_t len;
};

static volatile sig_atomic_t stop;
static uint8_t lut_r[256], lut_g[256], lut_b[256];
static double gain_r = 1.6, gain_b = 1.6, expo = 4.0;
static int awb_done;

static void on_sigint(int sig)
{
	(void)sig;
	stop = 1;
}

static void die(const char *what)
{
	perror(what);
	exit(1);
}

static void xioctl(int fd, unsigned long req, void *arg, const char *name)
{
	while (ioctl(fd, req, arg) < 0) {
		if (errno == EINTR)
			continue;
		die(name);
	}
}

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* Find the video device that the coda driver registered as name */
static int find_encoder(const char *want, char *path, size_t len)
{
	DIR *d = opendir("/sys/class/video4linux");
	struct dirent *e;
	char name[64], fn[300];
	int found = 0;

	if (!d)
		return 0;
	while (!found && (e = readdir(d))) {
		FILE *f;

		if (strncmp(e->d_name, "video", 5))
			continue;
		snprintf(fn, sizeof(fn), "/sys/class/video4linux/%s/name",
			 e->d_name);
		f = fopen(fn, "r");
		if (!f)
			continue;
		if (fgets(name, sizeof(name), f) &&
		    !strncmp(name, want, strlen(want))) {
			snprintf(path, len, "/dev/%s", e->d_name);
			found = 1;
		}
		fclose(f);
	}
	closedir(d);
	return found;
}

static void build_luts(void)
{
	double g[3] = { gain_r * expo, expo, gain_b * expo };
	uint8_t *lut[3] = { lut_r, lut_g, lut_b };
	int c, i;

	for (c = 0; c < 3; c++)
		for (i = 0; i < 256; i++) {
			double v = (i - BLACK) / (255.0 - BLACK) * g[c];

			v = v < 0 ? 0 : v > 1 ? 1 : v;
			lut[c][i] = (uint8_t)(pow(v, 1 / 2.2) * 255 + 0.5);
		}
}

/*
 * Gray world on a sample of the cells: equal channel means, and a mean
 * green of 18% of white. Cells with a clipped sample do not count.
 */
static void update_awb(const uint8_t *raw, int stride)
{
	double sr = 0, sg = 0, sb = 0, n = 0, t;
	int x, y;

	for (y = 0; y < RAW_H; y += 16) {
		const uint8_t *r0 = raw + y * stride, *r1 = r0 + stride;

		for (x = 0; x < OUT_W; x += 8) {
			int r = r0[3 * x], g = (r0[3 * x + 1] + r1[3 * x]) / 2,
			    b = r1[3 * x + 1];

			if (r > 250 || g > 250 || b > 250)
				continue;
			sr += r > BLACK ? r - BLACK : 0;
			sg += g > BLACK ? g - BLACK : 0;
			sb += b > BLACK ? b - BLACK : 0;
			n++;
		}
	}
	if (n < 100 || sr < n || sg < n || sb < n)
		return;
	t = 0.18 * (255 - BLACK) / (sg / n);
	t = t < 1 ? 1 : t > 64 ? 64 : t;
	/* Take the first estimate as it is, then smooth */
	if (!awb_done) {
		gain_r = sg / sr;
		gain_b = sg / sb;
		expo = t;
		awb_done = 1;
	} else {
		gain_r = 0.7 * gain_r + 0.3 * (sg / sr);
		gain_b = 0.7 * gain_b + 0.3 * (sg / sb);
		expo = 0.7 * expo + 0.3 * t;
	}
	build_luts();
}

/* One RGB pixel per Bayer cell, BT.601 limited range, NV12 */
static void raw_to_nv12(const uint8_t *raw, int stride, uint8_t *nv12, int sat)
{
	uint8_t *yp = nv12, *uv = nv12 + OUT_W * OUT_H;
	int x, y;

	for (y = 0; y < OUT_H; y += 2) {
		const uint8_t *a0 = raw + (2 * y) * stride, *a1 = a0 + stride;
		const uint8_t *b0 = a1 + stride, *b1 = b0 + stride;
		uint8_t *y0 = yp + y * OUT_W, *y1 = y0 + OUT_W;
		uint8_t *c = uv + (y / 2) * OUT_W;

		for (x = 0; x < OUT_W; x += 2) {
			const int o = 3 * x;
			int r[4], g[4], b[4], i, sr = 0, sg = 0, sb = 0, u, v;

			r[0] = lut_r[a0[o]];
			g[0] = lut_g[(a0[o + 1] + a1[o]) >> 1];
			b[0] = lut_b[a1[o + 1]];
			r[1] = lut_r[a0[o + 3]];
			g[1] = lut_g[(a0[o + 4] + a1[o + 3]) >> 1];
			b[1] = lut_b[a1[o + 4]];
			r[2] = lut_r[b0[o]];
			g[2] = lut_g[(b0[o + 1] + b1[o]) >> 1];
			b[2] = lut_b[b1[o + 1]];
			r[3] = lut_r[b0[o + 3]];
			g[3] = lut_g[(b0[o + 4] + b1[o + 3]) >> 1];
			b[3] = lut_b[b1[o + 4]];

			for (i = 0; i < 4; i++) {
				int yy = ((66 * r[i] + 129 * g[i] + 25 * b[i] +
					   128) >> 8) + 16;

				(i < 2 ? y0 : y1)[x + (i & 1)] = yy;
				sr += r[i];
				sg += g[i];
				sb += b[i];
			}
			/* Sums of 4 pixels: >> 10 = / 4 / 256. sat is x256. */
			u = (-38 * sr - 74 * sg + 112 * sb) >> 10;
			v = (112 * sr - 94 * sg - 18 * sb) >> 10;
			u = 128 + ((u * sat) >> 8);
			v = 128 + ((v * sat) >> 8);
			c[x] = u < 16 ? 16 : u > 240 ? 240 : u;
			c[x + 1] = v < 16 ? 16 : v > 240 ? 240 : v;
		}
	}
}

/* Copy from uncached DMA memory with wide loads */
static void copy64(void *dst, const void *src, size_t len)
{
	const uint64_t *s = src;
	uint64_t *d = dst;
	size_t i;

	for (i = 0; i < len / 8; i++)
		d[i] = s[i];
}

static void set_ctrl(int fd, uint32_t id, int32_t val, const char *name)
{
	struct v4l2_ext_control c = { .id = id, .value = val };
	struct v4l2_ext_controls cs = {
		.which = V4L2_CTRL_WHICH_CUR_VAL, .count = 1, .controls = &c,
	};

	if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &cs) < 0)
		fprintf(stderr, "warning: %s = %d: %s\n", name, val,
			strerror(errno));
}

static void map_bufs(int fd, enum v4l2_buf_type type, struct buf *b, int n)
{
	struct v4l2_requestbuffers rb = {
		.count = n, .type = type, .memory = V4L2_MEMORY_MMAP,
	};
	int i;

	xioctl(fd, VIDIOC_REQBUFS, &rb, "VIDIOC_REQBUFS");
	if ((int)rb.count < n) {
		fprintf(stderr, "only %u buffers\n", rb.count);
		exit(1);
	}
	for (i = 0; i < n; i++) {
		struct v4l2_buffer vb = {
			.index = i, .type = type, .memory = V4L2_MEMORY_MMAP,
		};

		xioctl(fd, VIDIOC_QUERYBUF, &vb, "VIDIOC_QUERYBUF");
		b[i].len = vb.length;
		b[i].p = mmap(NULL, vb.length, PROT_READ | PROT_WRITE,
			      MAP_SHARED, fd, vb.m.offset);
		if (b[i].p == MAP_FAILED)
			die("mmap");
	}
}

static void qbuf(int fd, enum v4l2_buf_type type, int i, size_t used)
{
	struct v4l2_buffer vb = {
		.index = i, .type = type, .memory = V4L2_MEMORY_MMAP,
		.bytesused = used,
	};

	xioctl(fd, VIDIOC_QBUF, &vb, "VIDIOC_QBUF");
}

static int dqbuf(int fd, enum v4l2_buf_type type, size_t *used)
{
	struct v4l2_buffer vb = { .type = type, .memory = V4L2_MEMORY_MMAP };

	while (ioctl(fd, VIDIOC_DQBUF, &vb) < 0) {
		if (errno == EINTR && !stop)
			continue;
		return -1;
	}
	if (used)
		*used = vb.bytesused;
	return vb.index;
}

static void stream(int fd, enum v4l2_buf_type type, int on)
{
	xioctl(fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type,
	       on ? "VIDIOC_STREAMON" : "VIDIOC_STREAMOFF");
}

int main(int argc, char **argv)
{
	const char *cam_dev = NULL, *in_file = NULL, *out_file = "out.h264";
	const char *yuv_file = NULL;
	char enc_dev[300] = "";
	int frames = 300, qp = 30, gop = 30, sat = 384, jpeg = 0, quality = 85;
	const char *enc_name;
	struct buf cam[NBUF_CAM], eout[NBUF_ENC_OUT], ecap[NBUF_ENC_CAP];
	int stride = (RAW_W * 3 / 2 + 15) / 16 * 16;
	int cfd = -1, efd, ifd = -1, ofd, opt, n, free_out = NBUF_ENC_OUT;
	size_t raw_size, nv12_size = OUT_W * OUT_H * 3 / 2;
	uint8_t *raw, *nv12;
	double t_cap = 0, t_conv = 0, t_enc = 0, t0, t1, t_start;
	long long bytes = 0;
	off_t in_frames = 0;

	while ((opt = getopt(argc, argv, "c:i:e:o:n:q:g:s:y:jQ:")) != -1) {
		switch (opt) {
		case 'c': cam_dev = optarg; break;
		case 'i': in_file = optarg; break;
		case 'e': snprintf(enc_dev, sizeof(enc_dev), "%s", optarg); break;
		case 'o': out_file = optarg; break;
		case 'n': frames = atoi(optarg); break;
		case 'q': qp = atoi(optarg); break;
		case 'g': gop = atoi(optarg); break;
		case 's': sat = (int)(atof(optarg) * 256); break;
		case 'y': yuv_file = optarg; break;
		case 'j': jpeg = 1; break;
		case 'Q': quality = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s -c /dev/videoN | -i cap.raw "
				"[-e enc] [-o out.h264] [-n frames] [-q qp] "
				"[-g gop] [-s saturation] [-y out.nv12] "
				"[-j] [-Q quality]\n",
				argv[0]);
			return 1;
		}
	}
	if (!cam_dev == !in_file) {
		fprintf(stderr, "give -c or -i\n");
		return 1;
	}
	enc_name = jpeg ? "coda-jpeg-encoder" : "coda-video-encoder";
	if (jpeg && !strcmp(out_file, "out.h264"))
		out_file = "out.mjpeg";
	if (!yuv_file && !enc_dev[0] &&
	    !find_encoder(enc_name, enc_dev, sizeof(enc_dev))) {
		fprintf(stderr, "no %s device; is coda loaded?\n", enc_name);
		return 1;
	}

	/* Input */
	if (cam_dev) {
		struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };

		cfd = open(cam_dev, O_RDWR);
		if (cfd < 0)
			die(cam_dev);
		xioctl(cfd, VIDIOC_G_FMT, &f, "camera VIDIOC_G_FMT");
		if (f.fmt.pix.width != RAW_W || f.fmt.pix.height != RAW_H) {
			fprintf(stderr, "camera format %ux%u, need %dx%d: run "
				"j1-stream.sh 0 setup first\n", f.fmt.pix.width,
				f.fmt.pix.height, RAW_W, RAW_H);
			return 1;
		}
		stride = f.fmt.pix.bytesperline;
	} else {
		ifd = open(in_file, O_RDONLY);
		if (ifd < 0)
			die(in_file);
		in_frames = lseek(ifd, 0, SEEK_END) / ((off_t)stride * RAW_H);
		if (in_frames < 1) {
			fprintf(stderr, "%s: no whole frame\n", in_file);
			return 1;
		}
	}
	raw_size = (size_t)stride * RAW_H;
	raw = malloc(raw_size);
	nv12 = malloc(nv12_size);
	if (!raw || !nv12)
		die("malloc");

	/* Encoder: NV12 in, H.264 out */
	efd = yuv_file ? -1 : open(enc_dev, O_RDWR);
	if (!yuv_file && efd < 0)
		die(enc_dev);
	if (efd >= 0) {
		struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
		struct v4l2_streamparm p = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };

		f.fmt.pix.width = OUT_W;
		f.fmt.pix.height = OUT_H;
		f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
		xioctl(efd, VIDIOC_S_FMT, &f, "encoder OUTPUT VIDIOC_S_FMT");
		if (f.fmt.pix.bytesperline != OUT_W ||
		    f.fmt.pix.sizeimage < nv12_size) {
			fprintf(stderr, "encoder input %u bytes per line, %u "
				"bytes: unexpected\n", f.fmt.pix.bytesperline,
				f.fmt.pix.sizeimage);
			return 1;
		}

		memset(&f, 0, sizeof(f));
		f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		f.fmt.pix.width = OUT_W;
		f.fmt.pix.height = OUT_H;
		f.fmt.pix.pixelformat = jpeg ? V4L2_PIX_FMT_JPEG :
					       V4L2_PIX_FMT_H264;
		/*
		 * Without a size, coda allocates 1 bit per pixel (64 KiB). A
		 * larger frame stalls the encoder until the 1 s timeout, and
		 * the driver returns it cut short.
		 */
		f.fmt.pix.sizeimage = nv12_size;
		xioctl(efd, VIDIOC_S_FMT, &f, "encoder CAPTURE VIDIOC_S_FMT");

		/* The JPEG encoder has no frame rate */
		p.parm.output.timeperframe.numerator = 1;
		p.parm.output.timeperframe.denominator = 30;
		if (!jpeg && ioctl(efd, VIDIOC_S_PARM, &p) < 0)
			perror("warning: VIDIOC_S_PARM");
	}
	if (efd >= 0 && jpeg)
		set_ctrl(efd, V4L2_CID_JPEG_COMPRESSION_QUALITY, quality,
			 "JPEG quality");
	if (efd >= 0 && !jpeg) {
		set_ctrl(efd, V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, qp,
			 "I frame QP");
		set_ctrl(efd, V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP, qp,
			 "P frame QP");
		set_ctrl(efd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop, "GOP size");
		set_ctrl(efd, V4L2_CID_MPEG_VIDEO_BITRATE, 0, "bitrate");
	}
	if (efd >= 0) {
		map_bufs(efd, V4L2_BUF_TYPE_VIDEO_OUTPUT, eout, NBUF_ENC_OUT);
		map_bufs(efd, V4L2_BUF_TYPE_VIDEO_CAPTURE, ecap, NBUF_ENC_CAP);
		for (n = 0; n < NBUF_ENC_CAP; n++)
			qbuf(efd, V4L2_BUF_TYPE_VIDEO_CAPTURE, n, 0);
		stream(efd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 1);
		stream(efd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1);
	}

	if (cfd >= 0) {
		map_bufs(cfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, cam, NBUF_CAM);
		for (n = 0; n < NBUF_CAM; n++)
			qbuf(cfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, n, 0);
		stream(cfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1);
	}

	if (yuv_file)
		out_file = yuv_file;
	if (!strcmp(out_file, "-"))
		ofd = STDOUT_FILENO;
	else
		ofd = open(out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (ofd < 0)
		die(out_file);
	signal(SIGINT, on_sigint);
	build_luts();
	fprintf(stderr, "%s -> %dx%d NV12 -> %s -> %s, ",
		cam_dev ? cam_dev : in_file, OUT_W, OUT_H,
		yuv_file ? "no encoder" : enc_dev, out_file);
	if (jpeg)
		fprintf(stderr, "JPEG quality %d\n", quality);
	else
		fprintf(stderr, "QP %d, GOP %d\n", qp, gop);

	t_start = now_ms();
	for (n = 0; !stop && (frames == 0 || n < frames); n++) {
		size_t used;
		int i;

		/* Get a raw frame */
		t0 = now_ms();
		if (cfd >= 0) {
			i = dqbuf(cfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, NULL);
			if (i < 0)
				break;
			copy64(raw, cam[i].p, raw_size);
			qbuf(cfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0);
		} else {
			off_t pos = (off_t)(n % in_frames) * raw_size;

			if (pread(ifd, raw, raw_size, pos) != (ssize_t)raw_size)
				die("pread");
		}
		t1 = now_ms();
		t_cap += t1 - t0;

		/* Convert into a free encoder input buffer */
		if (n % 15 == 0)
			update_awb(raw, stride);
		raw_to_nv12(raw, stride, nv12, sat);
		if (yuv_file) {
			if (write(ofd, nv12, nv12_size) != (ssize_t)nv12_size)
				die("write");
			t_conv += now_ms() - t1;
			bytes += nv12_size;
			continue;
		}
		if (free_out > 0) {
			i = NBUF_ENC_OUT - free_out--;
		} else {
			i = dqbuf(efd, V4L2_BUF_TYPE_VIDEO_OUTPUT, NULL);
			if (i < 0)
				break;
		}
		copy64(eout[i].p, nv12, nv12_size);
		t0 = now_ms();
		t_conv += t0 - t1;

		/* Encode, and write the stream */
		qbuf(efd, V4L2_BUF_TYPE_VIDEO_OUTPUT, i, nv12_size);
		i = dqbuf(efd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &used);
		if (i < 0) {
			perror("encoder VIDIOC_DQBUF");
			break;
		}
		if (write(ofd, ecap[i].p, used) != (ssize_t)used)
			die("write");
		bytes += used;
		qbuf(efd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0);
		t_enc += now_ms() - t0;

		if (n % 30 == 0)
			fprintf(stderr, "frame %d: %zu bytes, gains R %.2f B %.2f, "
				"exposure %.1f\n", n, used, gain_r, gain_b, expo);
	}
	t1 = now_ms();

	if (n > 0)
		fprintf(stderr, "%d frames in %.1f s: %.1f fps, %.0f kbit/s at "
			"30 fps. Per frame: input %.1f ms, convert %.1f ms, "
			"encode %.1f ms\n", n, (t1 - t_start) / 1e3,
			n * 1e3 / (t1 - t_start), bytes * 8.0 * 30 / n / 1e3,
			t_cap / n, t_conv / n, t_enc / n);

	if (cfd >= 0)
		stream(cfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0);
	if (efd >= 0) {
		stream(efd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 0);
		stream(efd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0);
	}
	close(ofd);
	return 0;
}
