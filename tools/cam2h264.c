// SPDX-License-Identifier: GPL-2.0
/*
 * cam2h264: GC2083 raw frames -> half-size NV12 -> CODA980 H.264, or
 * -> WAVE420L H.265 with -x, or -> JPU motion JPEG with -j. From the ISP
 * node, full-size NV12 frames go to the encoder as they are.
 *
 *   cam2h264 -c /dev/videoN [options]   from the camera (cv1800-vi-raw,
 *                                       or cv1800-vi-yuv: ISP mode)
 *   cam2h264 -i cap.raw [options]       from a file of raw frames
 *
 * Options:
 *   -e DEV   encoder (default: the device named coda-video-encoder,
 *            wave420l-encoder with -x, coda-jpeg-encoder with -j)
 *   -o FILE  output H.264 or H.265 Annex B stream, or concatenated JPEG
 *            frames with -j (default out.h264, out.h265, out.mjpeg)
 *   -n N     frames to encode, 0 = until Ctrl-C (default 300)
 *   -q QP    constant QP, 0-51 (default 30)
 *   -g N     GOP size (default 30)
 *   -x       H.265 with the WAVE420L, instead of H.264
 *   -j       motion JPEG with the JPEG unit, instead of H.264
 *   -Q N     JPEG quality, 5-100 (default 85)
 *   -s F     colour saturation (default 1.5). ISP mode: through the ISP's
 *            colour matrix (default 2)
 *   -y FILE  write NV12 frames to FILE, without the encoder
 *   -S DEV   sensor subdev for auto exposure (default: the subdev named
 *            gc2083...)
 *   -M       no auto exposure: keep the sensor exposure and gain
 *   -F HZ    mains frequency, 50 or 60: exposure times in steps of the
 *            light flicker period, against banding. 0 = off (default 50)
 *   -G X     highest analogue gain for auto exposure (default 32x)
 *   -d FILE  object detection on the TPU with this YOLOv8 cvimodel: boxes
 *            and labels in the picture (build with -DWITH_TPU)
 *   -t T     detection score threshold, 0-1 (default 0.5)
 *   -L LIST  detection class names, comma-separated (default: COCO for
 *            80-class models)
 *   -B N     ISP mode: black level in 12-bit units (default 252, the
 *            GC2083's)
 *   -K       ISP mode: no cache hints on the camera buffers
 *   -Y N     ISP mode: luma noise reduction strength at 20x gain, scaled
 *            with the square root of the gain, off below 2x. 0 = off
 *            (default 10)
 *
 * The input is packed RAW12, RGGB, 1920x1080, 10-bit data in the top bits.
 * Each 2x2 Bayer cell becomes one output pixel: 960x540. The program uses
 * only the top 8 bits of each sample. White balance is gray world, from a
 * sample of each frame. There is no colour matrix.
 *
 * Exposure: with -c, auto exposure sets the sensor exposure time first,
 * then its analogue gain, for a mean green of 18% of white. Software gain
 * is added only when the sensor is at its limits. With -i or -M, the
 * software gain alone sets the exposure.
 *
 * ISP mode: the camera node gives NV12 (or NV21: the program asks for NV12)
 * from the hardware ISP, at the sensor size: BT.601 limited range for
 * H.264, full range for JPEG. The encoder reads the camera
 * buffers through DMABUF, without a copy. Capture, encoding and output
 * overlap: eight camera buffers give room for frames in the encoder and
 * for stalls of the output. The summary counts the frames the ISP dropped.
 * Every 4th camera buffer (every 4th frame) gives the statistics, from
 * every 16th pixel: linear RGB through the inverse BT.601 matrix and the
 * sRGB curve. They are computed while the encoder reads the frame. The
 * other buffers skip the kernel's cache invalidate, and no buffer needs a
 * cache clean: the CPU never writes them. Auto exposure sets the sensor for
 * a mean luminance of 18%, with no software gain. White balance is gray
 * world: it sets the ISP's red and blue gains for equal linear means. No
 * detection in ISP mode. -K keeps the kernel's full cache syncs, for
 * comparison.
 *
 * Detection: a second thread takes the newest frame whenever the TPU is
 * free. Each frame gets the latest boxes. With -i, each frame waits for
 * its own boxes.
 *
 * Build: cc -O2 -o cam2h264 cam2h264.c -lm
 * With detection, from this directory, with the runtime in /opt/cviruntime:
 *   cc -O2 -DWITH_TPU -I../tpu -I/opt/cviruntime/include -o cam2h264 \
 *      cam2h264.c ../tpu/tpu-det.c -L/opt/cviruntime/lib \
 *      -Wl,-rpath,/opt/cviruntime/lib -lcviruntime -lpthread -lm
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
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <linux/videodev2.h>
#ifdef WITH_TPU
#include <pthread.h>
#include "tpu-det.h"
#endif

#define RAW_W	1920
#define RAW_H	1080
#define OUT_W	(RAW_W / 2)
#define OUT_H	(RAW_H / 2)
#define BLACK	16		/* 64 in 10 bits, in the top 8 bits */
#define NBUF_CAM 4
#define NBUF_ISP 8		/* ISP mode: room for frames in the encoder */
#define NBUF_ENC_OUT 2
#define NBUF_ENC_CAP 4

struct buf {
	void *p;
	size_t len;
};

static volatile sig_atomic_t stop;
static uint8_t lut_r[256], lut_g[256], lut_b[256];
/* White balance gains, and the software (digital) exposure gain */
static double gain_r = 1.6, gain_b = 1.6, expo = 4.0;
static int awb_done;

/* Mean green of 18% of white, in 8-bit samples above black */
#define AE_TARGET	(0.18 * (255 - BLACK))
/* The highest software gain when the sensor is at its limits */
#define AE_DIGITAL_MAX	8.0

/* ISP mode */
#define ISP_AE_TARGET	0.18		/* mean linear luminance */
#define ISP_CID_BLACK_LEVEL	(V4L2_CID_USER_BASE | 0x1000)
#define ISP_CID_CCM		(V4L2_CID_USER_BASE | 0x1001)
#define ISP_CID_NR_LUMA		(V4L2_CID_USER_BASE | 0x1003)
#define ISP_STEP	16		/* statistics from every 16th pixel */
#define ISP_SAT		2.0		/* default saturation of the colour matrix */
#define ISP_AE_HI	240		/* the brightest 2% stay below this */
static float srgb_lin[256];		/* sRGB-encoded 8-bit value, linear */
static double isp_r = 1.6, isp_b = 1.6;	/* ISP white balance gains */
static int isp_full;			/* full-range YUV, else limited */
static double isp_sat = 1;		/* saturation of the ISP colour matrix */
static int isp_nr20 = 10;		/* luma noise reduction at 20x gain */
static int isp_nr = -1;			/* the strength in use, -1: not set */
/* Linear luminance weights: the colour matrix keeps this sum */
static const double lum_w[3] = { 0.2126, 0.7152, 0.0722 };

struct yuv_stats {
	double r, g, b;		/* linear means of the white balance samples */
	double lum;		/* mean linear luminance of all samples */
	double clipped;		/* share of samples with a clipped channel */
	int nwb;		/* white balance samples */
	int hi;			/* brightest channel of the brightest 2%, 0-255 */
};

/* Auto exposure through the sensor subdev controls */
static struct {
	int fd;				/* < 0: no auto exposure */
	int exp, exp_min, exp_max;	/* exposure, in lines */
	int gain, gain_min, gain_max;	/* analogue gain; gain_min = 1x */
	int flicker;			/* lines per flicker period, 0: off */
} ae = { .fd = -1 };

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

/*
 * Find the device node (videoN or v4l-subdevN, by the node prefix) whose
 * name starts with want
 */
static int find_node(const char *node, const char *want, char *path,
		     size_t len)
{
	DIR *d = opendir("/sys/class/video4linux");
	struct dirent *e;
	char name[64], fn[300];
	int found = 0;

	if (!d)
		return 0;
	while (!found && (e = readdir(d))) {
		FILE *f;

		if (strncmp(e->d_name, node, strlen(node)))
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
 * Gray world on a sample of the cells: equal channel means, and without
 * auto exposure a mean green of 18% of white. Cells with a clipped sample
 * do not count.
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
	t = AE_TARGET / (sg / n);
	t = t < 1 ? 1 : t > 64 ? 64 : t;
	/* Take the first estimate as it is, then smooth */
	if (!awb_done) {
		gain_r = sg / sr;
		gain_b = sg / sb;
		if (ae.fd < 0)
			expo = t;
		awb_done = 1;
	} else {
		gain_r = 0.7 * gain_r + 0.3 * (sg / sr);
		gain_b = 0.7 * gain_b + 0.3 * (sg / sb);
		if (ae.fd < 0)
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

/* The current value of a control, 32 or 64 bit. -1 on error. */
static int64_t get_ctrl(int fd, uint32_t id)
{
	struct v4l2_query_ext_ctrl q = { .id = id };
	struct v4l2_ext_control c = { .id = id };
	struct v4l2_ext_controls cs = {
		.which = V4L2_CTRL_WHICH_CUR_VAL, .count = 1, .controls = &c,
	};

	if (ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &q) < 0 ||
	    ioctl(fd, VIDIOC_G_EXT_CTRLS, &cs) < 0)
		return -1;
	return q.type == V4L2_CTRL_TYPE_INTEGER64 ? c.value64 : c.value;
}

static int ctrl_range(int fd, uint32_t id, int *min, int *max)
{
	struct v4l2_query_ext_ctrl q = { .id = id };

	if (ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &q) < 0)
		return -1;
	*min = q.minimum;
	*max = q.maximum;
	return 0;
}

/*
 * Open the sensor subdev for auto exposure. max_gain is the highest
 * analogue gain, in x. hz is the mains frequency, 0 for no flicker steps.
 */
static void ae_open(const char *dev, int max_gain, int hz)
{
	int64_t rate, hblank;

	ae.fd = open(dev, O_RDWR);
	if (ae.fd < 0) {
		perror(dev);
		fprintf(stderr, "no auto exposure\n");
		return;
	}
	if (ctrl_range(ae.fd, V4L2_CID_EXPOSURE, &ae.exp_min, &ae.exp_max) ||
	    ctrl_range(ae.fd, V4L2_CID_ANALOGUE_GAIN, &ae.gain_min,
		       &ae.gain_max) ||
	    (ae.exp = get_ctrl(ae.fd, V4L2_CID_EXPOSURE)) < 0 ||
	    (ae.gain = get_ctrl(ae.fd, V4L2_CID_ANALOGUE_GAIN)) < 0) {
		fprintf(stderr, "%s: no exposure or gain control: no auto "
			"exposure\n", dev);
		close(ae.fd);
		ae.fd = -1;
		return;
	}
	if (max_gain > 0 && (int64_t)ae.gain_min * max_gain < ae.gain_max)
		ae.gain_max = ae.gain_min * max_gain;

	/* Flicker period in lines: a line is (width + hblank) pixels */
	rate = get_ctrl(ae.fd, V4L2_CID_PIXEL_RATE);
	hblank = get_ctrl(ae.fd, V4L2_CID_HBLANK);
	if (hz > 0 && rate > 0 && hblank >= 0)
		ae.flicker = (int)(rate / (2.0 * hz) / (RAW_W + hblank) + 0.5);
	if (ae.flicker > ae.exp_max)
		ae.flicker = 0;

	expo = 1;
	fprintf(stderr, "auto exposure: %s, exposure %d-%d lines, gain up to "
		"%.0fx, flicker step %d lines\n", dev, ae.exp_min, ae.exp_max,
		(double)ae.gain_max / ae.gain_min, ae.flicker);
}

/*
 * Mean green of a sample of cells, above black. Clipped cells count, and
 * clipped is set to their share.
 */
static double ae_measure(const uint8_t *raw, int stride, double *clipped)
{
	double sg = 0, n = 0, nc = 0;
	int x, y;

	for (y = 0; y < RAW_H; y += 16) {
		const uint8_t *r0 = raw + y * stride, *r1 = r0 + stride;

		for (x = 0; x < OUT_W; x += 8) {
			int r = r0[3 * x], g = (r0[3 * x + 1] + r1[3 * x]) / 2,
			    b = r1[3 * x + 1];

			if (r > 250 || g > 250 || b > 250)
				nc++;
			sg += g > BLACK ? g - BLACK : 0;
			n++;
		}
	}
	*clipped = nc / n;
	return sg / n;
}

/*
 * One step of auto exposure: ratio is the change of brightness to reach the
 * target. digital: software gain may make up for a sensor at its limits.
 */
static void ae_step(double ratio, double clipped, int digital)
{
	double want, e, g, d;
	struct v4l2_ext_control c[2] = {
		{ .id = V4L2_CID_EXPOSURE }, { .id = V4L2_CID_ANALOGUE_GAIN },
	};
	struct v4l2_ext_controls cs = {
		.which = V4L2_CTRL_WHICH_CUR_VAL, .count = 2, .controls = c,
	};

	/* Clipped highlights: darker, whatever the mean says */
	if (clipped > 0.02 && ratio > 0.8)
		ratio = 0.8;
	if (ratio > 0.92 && ratio < 1.08)
		return;
	/* Most of the way, at most 4x per step: no overshoot */
	ratio = pow(ratio, 0.7);
	ratio = ratio < 0.25 ? 0.25 : ratio > 4 ? 4 : ratio;
	want = (double)ae.exp * ae.gain * expo * ratio;

	/* Exposure time first, in whole flicker periods when it is long */
	e = want / ae.gain_min;
	if (ae.flicker && e >= ae.flicker)
		e = (int)(e / ae.flicker) * ae.flicker;
	if (e > ae.exp_max)
		e = ae.flicker ? (ae.exp_max / ae.flicker) * ae.flicker :
				 ae.exp_max;
	if (e < ae.exp_min)
		e = ae.exp_min;
	/* Then analogue gain, then software gain */
	g = want / e;
	g = g < ae.gain_min ? ae.gain_min : g > ae.gain_max ? ae.gain_max : g;
	d = want / (e * g);
	d = d < 1 ? 1 : d > AE_DIGITAL_MAX ? AE_DIGITAL_MAX : d;

	if ((int)e != ae.exp || (int)g != ae.gain) {
		c[0].value = (int)e;
		c[1].value = (int)g;
		if (ioctl(ae.fd, VIDIOC_S_EXT_CTRLS, &cs) < 0) {
			perror("auto exposure: VIDIOC_S_EXT_CTRLS");
			return;
		}
		ae.exp = (int)e;
		ae.gain = (int)g;
	}
	if (digital && d != expo) {
		expo = d;
		build_luts();
	}
}

/*
 * One step of auto exposure from a raw frame. The sensor applies a new
 * exposure about two frames later: call this every third frame.
 */
static void ae_update(const uint8_t *raw, int stride)
{
	double clipped, mean = ae_measure(raw, stride, &clipped);

	ae_step(AE_TARGET / ((mean > 0.5 ? mean : 0.5) * expo), clipped, 1);
}

static void build_srgb_lin(void)
{
	int i;

	for (i = 0; i < 256; i++) {
		double v = i / 255.0;

		srgb_lin[i] = v <= 0.04045 ? v / 12.92 :
			      pow((v + 0.055) / 1.055, 2.4);
	}
}

static int clamp255(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : v;
}

/*
 * Statistics of an NV12 frame from every step-th pixel in each direction,
 * BT.601 limited range, or full range if isp_full.
 * Samples with a clipped channel count as white for the luminance and do
 * not count for white balance; nor do dark ones, which are mostly noise.
 */
static void yuv_measure(const uint8_t *f, int w, int h, int bpl, int step,
			struct yuv_stats *st)
{
	const uint8_t *uvp = f + (size_t)bpl * h;
	/* Y offset and gain, then V to R, U and V to G, U to B, 256 = 1.0 */
	const int k[6] = { isp_full ? 0 : 16, isp_full ? 256 : 298,
			   isp_full ? 359 : 409, isp_full ? 88 : 100,
			   isp_full ? 183 : 208, isp_full ? 454 : 516 };
	double sr = 0, sg = 0, sb = 0, sl = 0;
	int x, y, n = 0, nc = 0, nwb = 0, hist[256] = { 0 };

	for (y = 0; y < h; y += step)
		for (x = 0; x < w; x += step) {
			const uint8_t *c = uvp + (size_t)(y / 2) * bpl + (x & ~1);
			int yy = k[1] * (f[(size_t)y * bpl + x] - k[0]);
			int u = c[0] - 128, v = c[1] - 128;
			int r = clamp255((yy + k[2] * v + 128) >> 8);
			int g = clamp255((yy - k[3] * u - k[4] * v + 128) >> 8);
			int b = clamp255((yy + k[5] * u + 128) >> 8);
			int m = r > g ? (r > b ? r : b) : (g > b ? g : b);
			double lr, lg, lb;

			n++;
			hist[m]++;
			if (m >= 250) {
				nc++;
				sl += 1;
				continue;
			}
			lr = srgb_lin[r];
			lg = srgb_lin[g];
			lb = srgb_lin[b];
			sl += 0.2126 * lr + 0.7152 * lg + 0.0722 * lb;
			if (lg < 0.005)
				continue;
			sr += lr;
			sg += lg;
			sb += lb;
			nwb++;
		}
	st->lum = n ? sl / n : 0;
	st->clipped = n ? (double)nc / n : 0;
	st->nwb = nwb;
	st->r = nwb ? sr / nwb : 0;
	st->g = nwb ? sg / nwb : 0;
	st->b = nwb ? sb / nwb : 0;
	for (x = 255, y = 0; x > 0 && (y += hist[x]) < n / 50; x--)
		;
	st->hi = x;
}

/*
 * Auto exposure in ISP mode: the mean luminance goes to the target, but
 * the brightest 2% of the samples must not clip. A lamp or a window in the
 * picture may pull the mean down to half the target, no further. The
 * highlight limit is continuous: an on/off limit at 2% clipped samples
 * made the loop cycle between two exposures.
 */
static void isp_ae(const struct yuv_stats *st)
{
	double lum = st->lum > 1e-3 ? st->lum : 1e-3;
	double ratio = ISP_AE_TARGET / lum, hi;

	if (st->hi >= 250)
		hi = 0.85;	/* clipped: how far over is not known */
	else
		hi = srgb_lin[ISP_AE_HI] / srgb_lin[st->hi > 8 ? st->hi : 8];
	if (ratio > hi)
		ratio = hi;
	if (ratio < 0.5 * ISP_AE_TARGET / lum)
		ratio = 0.5 * ISP_AE_TARGET / lum;
	ae_step(ratio, 0, 0);
}

/* Both gains in one call: the ISP takes them for the same frame */
static void isp_set_wb(int fd)
{
	struct v4l2_ext_control c[2] = {
		{ .id = V4L2_CID_RED_BALANCE, .value = (int)(isp_r * 1024 + 0.5) },
		{ .id = V4L2_CID_BLUE_BALANCE, .value = (int)(isp_b * 1024 + 0.5) },
	};
	struct v4l2_ext_controls cs = {
		.which = V4L2_CTRL_WHICH_CUR_VAL, .count = 2, .controls = c,
	};

	if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &cs) < 0)
		fprintf(stderr, "warning: ISP white balance R %d B %d: %s\n",
			c[0].value, c[1].value, strerror(errno));
}

static double clampd(double v, double lo, double hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/*
 * The saturation matrix, rows R G B: s on the diagonal, minus what keeps
 * the luminance and white. Its inverse is the matrix for 1 / s.
 */
static void sat_matrix(double s, double m[9])
{
	int r, c;

	for (r = 0; r < 3; r++)
		for (c = 0; c < 3; c++)
			m[3 * r + c] = (r == c ? s : 0) + (1 - s) * lum_w[c];
}

/* The ISP colour matrix for saturation s. Returns 0 if the driver took it. */
static int isp_set_ccm(int fd, double s)
{
	double m[9];
	int32_t v[9];
	struct v4l2_ext_control c = {
		.id = ISP_CID_CCM, .size = sizeof(v), .p_s32 = v,
	};
	struct v4l2_ext_controls cs = {
		.which = V4L2_CTRL_WHICH_CUR_VAL, .count = 1, .controls = &c,
	};
	int i;

	sat_matrix(s, m);
	for (i = 0; i < 9; i++)
		v[i] = (int32_t)lround(m[i] * 1024);
	if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &cs) < 0) {
		fprintf(stderr, "warning: no ISP colour matrix: %s\n",
			strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * Gray world under the gains in use: the corrections that make the linear
 * means equal. The first step takes them as they are, later steps half of
 * them (in log terms). The means come after the colour matrix: its inverse
 * gives them as the white balance gains see them.
 */
static void isp_awb(int fd, const struct yuv_stats *st)
{
	double k = awb_done ? 0.5 : 1.0, cr, cb, m[9], r, g, b;

	sat_matrix(1 / isp_sat, m);
	r = m[0] * st->r + m[1] * st->g + m[2] * st->b;
	g = m[3] * st->r + m[4] * st->g + m[5] * st->b;
	b = m[6] * st->r + m[7] * st->g + m[8] * st->b;
	if (st->nwb < 100 || r < 1e-4 || g < 1e-4 || b < 1e-4)
		return;
	cr = clampd(pow(g / r, k), 0.5, 2);
	cb = clampd(pow(g / b, k), 0.5, 2);
	awb_done = 1;
	if (fabs(cr - 1) < 0.01 && fabs(cb - 1) < 0.01)
		return;
	isp_r = clampd(isp_r * cr, 0.25, 15.9);
	isp_b = clampd(isp_b * cb, 0.25, 15.9);
	isp_set_wb(fd);
}

#ifndef V4L2_MEMORY_FLAG_NON_COHERENT
#define V4L2_MEMORY_FLAG_NON_COHERENT	(1 << 0)
#endif

/*
 * flags = V4L2_MEMORY_FLAG_NON_COHERENT asks for cached buffers. Return
 * the flags that the driver accepted.
 */
static unsigned int map_bufs(int fd, enum v4l2_buf_type type, struct buf *b,
			     int n, unsigned int flags)
{
	struct v4l2_requestbuffers rb = {
		.count = n, .type = type, .memory = V4L2_MEMORY_MMAP,
		.flags = flags,
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
	return rb.flags;
}

static void qbuf(int fd, enum v4l2_buf_type type, int i, size_t used)
{
	struct v4l2_buffer vb = {
		.index = i, .type = type, .memory = V4L2_MEMORY_MMAP,
		.bytesused = used,
	};

	xioctl(fd, VIDIOC_QBUF, &vb, "VIDIOC_QBUF");
}

/* The flags, sequence and timestamp of the last dequeued buffer */
static uint32_t dq_flags, dq_seq;
static struct timeval dq_tv;
static double dq_ts;		/* dq_tv in ms */

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
	dq_flags = vb.flags;
	dq_seq = vb.sequence;
	dq_tv = vb.timestamp;
	dq_ts = vb.timestamp.tv_sec * 1e3 + vb.timestamp.tv_usec / 1e3;
	return vb.index;
}

/*
 * Give a camera buffer back in ISP mode. The CPU never writes the buffers,
 * so they need no cache clean. inval = 0: the CPU does not read this
 * buffer, so it needs no cache invalidate either. The kernel keeps these
 * hints for the buffer, also on later QBUFs without them. The first QBUF
 * of each buffer must have no hints: it cleans the zeroed memory.
 */
static void isp_requeue(int fd, int i, int inval)
{
	struct v4l2_buffer vb = {
		.index = i, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
		.flags = V4L2_BUF_FLAG_NO_CACHE_CLEAN |
			 (inval ? 0 : V4L2_BUF_FLAG_NO_CACHE_INVALIDATE),
	};

	xioctl(fd, VIDIOC_QBUF, &vb, "VIDIOC_QBUF");
}

static void stream(int fd, enum v4l2_buf_type type, int on)
{
	xioctl(fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type,
	       on ? "VIDIOC_STREAMON" : "VIDIOC_STREAMOFF");
}

/*
 * Encoder input from a camera buffer, through its DMABUF. The encoder
 * copies ts, the camera timestamp, to the encoded frame.
 */
static void qbuf_dmabuf(int fd, int i, int dmafd, size_t len,
			struct timeval ts)
{
	struct v4l2_buffer vb = {
		.index = i, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.memory = V4L2_MEMORY_DMABUF, .bytesused = len, .length = len,
		.timestamp = ts,
	};

	vb.m.fd = dmafd;
	xioctl(fd, VIDIOC_QBUF, &vb, "encoder VIDIOC_QBUF");
}

static int dqbuf_dmabuf(int fd)
{
	struct v4l2_buffer vb = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_DMABUF,
	};

	while (ioctl(fd, VIDIOC_DQBUF, &vb) < 0) {
		if (errno == EINTR && !stop)
			continue;
		return -1;
	}
	return vb.index;
}

static double isp_t_3a;	/* time in isp_3a(), ms */
static int isp_n_3a;		/* calls of isp_3a() */

/* Statistics of one frame, then one step of auto exposure and white balance */
/*
 * Luma noise reduction for the sensor gain in use. At a fixed output
 * brightness, the noise grows about with the square root of the gain.
 * Below 2x, the picture stays sharp. On the board, 10 at 20x removed the
 * grain; above 20 the picture went soft.
 */
static void isp_nr_update(int fd)
{
	struct v4l2_ext_control c = { .id = ISP_CID_NR_LUMA };
	struct v4l2_ext_controls cs = {
		.which = V4L2_CTRL_WHICH_CUR_VAL, .count = 1, .controls = &c,
	};
	double g = (double)ae.gain / ae.gain_min;
	long v = g < 2 ? 0 : lround(isp_nr20 * sqrt(g / 20));

	c.value = v > 255 ? 255 : v;
	if (c.value == isp_nr)
		return;
	if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &cs) < 0) {
		fprintf(stderr, "warning: no ISP noise reduction: %s\n",
			strerror(errno));
		isp_nr20 = 0;		/* do not try again */
		return;
	}
	isp_nr = c.value;
}

static void isp_3a(int fd, const uint8_t *f, int w, int h, int bpl,
		   struct yuv_stats *st)
{
	double t0 = now_ms();

	yuv_measure(f, w, h, bpl, ISP_STEP, st);
	if (ae.fd >= 0)
		isp_ae(st);
	if (ae.fd >= 0 && isp_nr20 > 0)
		isp_nr_update(fd);
	isp_awb(fd, st);
	isp_t_3a += now_ms() - t0;
	isp_n_3a++;
}

/* ISP mode: statistics from every 4th camera buffer, so every 4th frame */
#define ISP_MEAS(i)	((i) % 4 == 0)

/*
 * ISP mode, with the encoder: camera buffer i goes to the encoder as its
 * input buffer i, and back to the camera when the encoder is done with it.
 * Each step waits for whichever comes first: a camera frame, an encoded
 * frame, or an input buffer the encoder is done with. The encoder can hold
 * several frames, so capture, encoding and output overlap. hints: cache
 * hints on the camera buffers. Returns the number of encoded frames.
 */
static int isp_loop(int cfd, int efd, int ofd, const struct buf *cam,
		    const int *dmafd, const struct buf *ecap, int w, int h,
		    int bpl, int frames, int jpeg, int hevc, int to_stdout,
		    int hints, long long *bytes, double *t_lat, int *lost)
{
	size_t size = (size_t)bpl * h * 3 / 2, used = 0;
	struct yuv_stats st = { 0 };
	struct pollfd pfd[2] = {
		{ .fd = cfd, .events = POLLIN },
		{ .fd = efd, .events = POLLIN | POLLOUT },
	};
	int n = 0, done = 0, i, k;
	uint32_t seq = 0;

	while (!stop && (frames == 0 || done < frames)) {
		/* poll() skips a negative fd: no more frames are needed */
		pfd[0].fd = frames == 0 || n < frames ? cfd : -1;
		if (poll(pfd, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			die("poll");
		}
		if ((pfd[0].revents | pfd[1].revents) & (POLLERR | POLLNVAL)) {
			fprintf(stderr, "poll: error on the %s\n",
				pfd[0].revents & (POLLERR | POLLNVAL) ?
				"camera" : "encoder");
			break;
		}

		/* A camera frame: to the encoder at once */
		if (pfd[0].revents & POLLIN) {
			i = dqbuf(cfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, NULL);
			if (i < 0)
				break;
			/* A gap in the sequence: frames the ISP dropped */
			if (n > 0)
				*lost += dq_seq - seq;
			seq = dq_seq + 1;
			qbuf_dmabuf(efd, i, dmafd[i], size, dq_tv);
			n++;
			/*
			 * Both only read the frame: the CPU works while the
			 * encoder does. The camera and the ISP apply new
			 * settings a few frames later.
			 */
			if (ISP_MEAS(i) && !(dq_flags & V4L2_BUF_FLAG_ERROR))
				isp_3a(cfd, cam[i].p, w, h, bpl, &st);
		}

		/* An input frame the encoder is done with: back to the camera */
		if (pfd[1].revents & POLLOUT) {
			k = dqbuf_dmabuf(efd);
			if (k < 0) {
				perror("encoder input VIDIOC_DQBUF");
				break;
			}
			if (hints)
				isp_requeue(cfd, k, ISP_MEAS(k));
			else
				qbuf(cfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, k, 0);
		}

		/* An encoded frame: write it */
		if (pfd[1].revents & POLLIN) {
			k = dqbuf(efd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &used);
			if (k < 0) {
				perror("encoder VIDIOC_DQBUF");
				break;
			}
			/* The encoder copies the camera timestamp */
			if (dq_ts > 0)
				*t_lat += now_ms() - dq_ts;
			if (write(ofd, ecap[k].p, used) != (ssize_t)used)
				die("write");
			if (!jpeg && to_stdout &&
			    (hevc ? write(ofd, "\0\0\0\1\x46\x01\x50", 7) != 7 :
				    write(ofd, "\0\0\0\1\x09\xf0", 6) != 6))
				die("write");
			*bytes += used;
			qbuf(efd, V4L2_BUF_TYPE_VIDEO_CAPTURE, k, 0);

			if (done % 30 == 0)
				fprintf(stderr, "frame %d: %zu bytes, lost %d, "
					"luminance %.3f, clipped %.1f%%, top 2%% %d, "
					"ISP WB R %.2f B %.2f, exposure %d lines, "
					"gain %.2fx, NR %d\n", done, used, *lost,
					st.lum, st.clipped * 100, st.hi, isp_r, isp_b,
					ae.exp, ae.fd >= 0 ?
					(double)ae.gain / ae.gain_min : 0,
					isp_nr > 0 ? isp_nr : 0);
			done++;
		}
	}
	return done;
}

/* ISP mode without the encoder: the NV12 frames go to ofd */
static int isp_loop_file(int cfd, int ofd, const struct buf *cam, int w,
			 int h, int bpl, int frames, int hints, long long *bytes)
{
	size_t size = (size_t)bpl * h * 3 / 2;
	struct yuv_stats st = { 0 };
	int n, i;

	for (n = 0; !stop && (frames == 0 || n < frames); n++) {
		i = dqbuf(cfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, NULL);
		if (i < 0)
			break;
		if (ISP_MEAS(i) && !(dq_flags & V4L2_BUF_FLAG_ERROR))
			isp_3a(cfd, cam[i].p, w, h, bpl, &st);
		if (write(ofd, cam[i].p, size) != (ssize_t)size)
			die("write");
		/* The CPU reads every buffer here */
		if (hints)
			isp_requeue(cfd, i, 1);
		else
			qbuf(cfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0);
		*bytes += size;
	}
	return n;
}

#ifdef WITH_TPU
#define DET_MAX	32

/* The detector thread and what it shares with the frame loop */
static struct {
	struct det *d;
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	uint8_t *nv12;		/* the frame for the detector */
	int busy;		/* nv12 is waiting or in work */
	int quit;
	float thresh;
	struct det_box box[DET_MAX];	/* the latest result */
	int nbox;
} tpu = {
	.lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER,
};

static void *tpu_main(void *arg)
{
	struct det_box box[DET_MAX];
	int n;

	(void)arg;
	pthread_mutex_lock(&tpu.lock);
	for (;;) {
		while (!tpu.busy && !tpu.quit)
			pthread_cond_wait(&tpu.cond, &tpu.lock);
		if (tpu.quit)
			break;
		pthread_mutex_unlock(&tpu.lock);
		det_load_nv12(tpu.d, tpu.nv12);
		n = det_run(tpu.d, tpu.thresh, 0.5f, box, DET_MAX);
		pthread_mutex_lock(&tpu.lock);
		if (n >= 0) {
			memcpy(tpu.box, box, n * sizeof(box[0]));
			tpu.nbox = n;
		}
		tpu.busy = 0;
		pthread_cond_broadcast(&tpu.cond);
	}
	pthread_mutex_unlock(&tpu.lock);
	return NULL;
}

static void tpu_start(const char *model, const char *names_list,
		      float thresh, const char *const **names)
{
	tpu.d = det_open(model);
	if (!tpu.d)
		exit(1);
	*names = det_names(tpu.d, names_list);
	det_set_frame(tpu.d, OUT_W, OUT_H);
	tpu.thresh = thresh;
	tpu.nv12 = malloc(OUT_W * OUT_H * 3 / 2);
	if (!tpu.nv12)
		die("malloc");
	if (pthread_create(&tpu.thread, NULL, tpu_main, NULL))
		die("pthread_create");
}

/*
 * Give the frame to the detector if it is free, then draw the latest
 * boxes. wait: wait for this frame's own boxes.
 */
static int tpu_frame(uint8_t *nv12, int wait, const char *const *names,
		     struct det_box *box)
{
	int n;

	pthread_mutex_lock(&tpu.lock);
	if (!tpu.busy) {
		memcpy(tpu.nv12, nv12, OUT_W * OUT_H * 3 / 2);
		tpu.busy = 1;
		pthread_cond_broadcast(&tpu.cond);
	}
	while (wait && tpu.busy)
		pthread_cond_wait(&tpu.cond, &tpu.lock);
	n = tpu.nbox;
	memcpy(box, tpu.box, n * sizeof(box[0]));
	pthread_mutex_unlock(&tpu.lock);
	det_draw_nv12(nv12, OUT_W, OUT_H, box, n, names);
	return n;
}

static void tpu_stop(void)
{
	const struct det_stats *st;

	pthread_mutex_lock(&tpu.lock);
	tpu.quit = 1;
	pthread_cond_broadcast(&tpu.cond);
	pthread_mutex_unlock(&tpu.lock);
	pthread_join(tpu.thread, NULL);
	st = det_stats(tpu.d);
	if (st->runs)
		fprintf(stderr, "detector: %d runs. Per run: input %.1f ms, "
			"TPU %.1f ms, decode %.1f ms\n", st->runs,
			st->load_ms / st->runs, st->tpu_ms / st->runs,
			st->post_ms / st->runs);
	det_close(tpu.d);
}

static void print_boxes(const struct det_box *b, int n,
			const char *const *names)
{
	int i;

	fprintf(stderr, "  %d objects:", n);
	for (i = 0; i < n; i++)
		fprintf(stderr, " %s %.0f%% (%.0f,%.0f %.0fx%.0f)",
			names ? names[b[i].cls] : "?", b[i].score * 100,
			b[i].x1, b[i].y1, b[i].x2 - b[i].x1, b[i].y2 - b[i].y1);
	fprintf(stderr, "\n");
}
#endif

int main(int argc, char **argv)
{
	const char *cam_dev = NULL, *in_file = NULL, *out_file = "out.h264";
	const char *yuv_file = NULL, *sensor_dev = NULL;
	const char *model = NULL, *names_list = NULL;
	float thresh = 0.5f;
#ifdef WITH_TPU
	const char *const *names = NULL;
	struct det_box box[DET_MAX];
	int nbox = 0;
#endif
	char enc_dev[300] = "", sensor_path[300];
	int manual = 0, mains_hz = 50, max_gain = 32, black = 252;
	int isp = 0, out_w = OUT_W, out_h = OUT_H, out_bpl = OUT_W;
	int dmafd[NBUF_ISP], ncam = NBUF_CAM, lost = 0;
	int frames = 300, qp = 30, gop = 30, sat = -1, jpeg = 0, quality = 85;
	int hevc = 0;
	size_t cap_size = 0;
	const char *enc_name;
	struct buf cam[NBUF_ISP], eout[NBUF_ENC_OUT], ecap[NBUF_ENC_CAP];
	int stride = (RAW_W * 3 / 2 + 15) / 16 * 16;
	int cfd = -1, efd, ifd = -1, ofd, opt, n, free_out = NBUF_ENC_OUT;
	int cam_cached = 0, to_stdout = 0, no_hints = 0;
	size_t raw_size, nv12_size = OUT_W * OUT_H * 3 / 2;
	uint8_t *raw, *nv12;
	double t_cap = 0, t_conv = 0, t_enc = 0, t0, t1, t_start;
	struct rusage ru0, ru1;
	long long bytes = 0;
	off_t in_frames = 0;

	while ((opt = getopt(argc, argv, "c:i:e:o:n:q:g:s:y:jxQ:b:S:MF:G:d:t:L:B:KY:")) != -1) {
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
		case 'x': hevc = 1; break;
		case 'Q': quality = atoi(optarg); break;
		case 'b': cap_size = strtoul(optarg, NULL, 0); break;
		case 'S': sensor_dev = optarg; break;
		case 'M': manual = 1; break;
		case 'F': mains_hz = atoi(optarg); break;
		case 'G': max_gain = atoi(optarg); break;
		case 'd': model = optarg; break;
		case 't': thresh = atof(optarg); break;
		case 'L': names_list = optarg; break;
		case 'B': black = atoi(optarg); break;
		case 'K': no_hints = 1; break;
		case 'Y': isp_nr20 = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s -c /dev/videoN | -i cap.raw "
				"[-e enc] [-o out.h264] [-n frames] [-q qp] "
				"[-g gop] [-s saturation] [-y out.nv12] "
				"[-x] [-j] [-Q quality] [-b bytes] "
				"[-S subdev] [-M] [-F hz] [-G gain] "
				"[-d model] [-t thresh] [-L names] [-B black] [-K] [-Y nr]\n",
				argv[0]);
			return 1;
		}
	}
	if (!cam_dev == !in_file) {
		fprintf(stderr, "give -c or -i\n");
		return 1;
	}
	if (jpeg && hevc) {
		fprintf(stderr, "give -j or -x, not both\n");
		return 1;
	}
#ifndef WITH_TPU
	if (model) {
		fprintf(stderr, "-d: built without TPU support (-DWITH_TPU)\n");
		return 1;
	}
	(void)names_list;
	(void)thresh;
#endif
	enc_name = jpeg ? "coda-jpeg-encoder" :
		   hevc ? "wave420l-encoder" : "coda-video-encoder";
	if (!strcmp(out_file, "out.h264") && (jpeg || hevc))
		out_file = jpeg ? "out.mjpeg" : "out.h265";
	if (!yuv_file && !enc_dev[0] &&
	    !find_node("video", enc_name, enc_dev, sizeof(enc_dev))) {
		fprintf(stderr, "no %s device; is the driver loaded?\n", enc_name);
		return 1;
	}

	/* Input */
	if (cam_dev) {
		struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };

		cfd = open(cam_dev, O_RDWR);
		if (cfd < 0)
			die(cam_dev);
		xioctl(cfd, VIDIOC_G_FMT, &f, "camera VIDIOC_G_FMT");
		if (f.fmt.pix.pixelformat == V4L2_PIX_FMT_NV12 ||
		    f.fmt.pix.pixelformat == V4L2_PIX_FMT_NV21) {
			/* The ISP node: full size, NV12 for the encoder */
			isp = 1;
			ncam = NBUF_ISP;
			f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
			/* JPEG is full range; H.264 without VUI is limited */
			f.fmt.pix.priv = V4L2_PIX_FMT_PRIV_MAGIC;
			f.fmt.pix.flags = V4L2_PIX_FMT_FLAG_SET_CSC;
			f.fmt.pix.colorspace = V4L2_COLORSPACE_DEFAULT;
			f.fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
			f.fmt.pix.xfer_func = V4L2_XFER_FUNC_DEFAULT;
			f.fmt.pix.quantization = jpeg ?
				V4L2_QUANTIZATION_FULL_RANGE :
				V4L2_QUANTIZATION_LIM_RANGE;
			xioctl(cfd, VIDIOC_S_FMT, &f, "camera VIDIOC_S_FMT");
			isp_full = f.fmt.pix.quantization ==
				   V4L2_QUANTIZATION_FULL_RANGE;
			if (jpeg && !isp_full)
				fprintf(stderr, "warning: the ISP gives limited "
					"range: the JPEG frames have low contrast\n");
			out_w = f.fmt.pix.width;
			out_h = f.fmt.pix.height;
			out_bpl = f.fmt.pix.bytesperline;
			nv12_size = (size_t)out_bpl * out_h * 3 / 2;
			if (model) {
				fprintf(stderr, "-d: not in ISP mode\n");
				return 1;
			}
		} else if (f.fmt.pix.width != RAW_W || f.fmt.pix.height != RAW_H) {
			fprintf(stderr, "camera format %ux%u, need %dx%d: run "
				"j1-stream.sh 0 setup first\n", f.fmt.pix.width,
				f.fmt.pix.height, RAW_W, RAW_H);
			return 1;
		}
		stride = f.fmt.pix.bytesperline;

		if (!manual && !sensor_dev &&
		    find_node("v4l-subdev", "gc2083", sensor_path,
			      sizeof(sensor_path)))
			sensor_dev = sensor_path;
		if (!manual && sensor_dev)
			ae_open(sensor_dev, max_gain, mains_hz);
		else if (!manual)
			fprintf(stderr, "no gc2083 subdev: no auto exposure\n");
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

		f.fmt.pix.width = out_w;
		f.fmt.pix.height = out_h;
		f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
		xioctl(efd, VIDIOC_S_FMT, &f, "encoder OUTPUT VIDIOC_S_FMT");
		if (f.fmt.pix.width != (unsigned int)out_w ||
		    f.fmt.pix.height != (unsigned int)out_h ||
		    f.fmt.pix.bytesperline != (unsigned int)out_bpl ||
		    f.fmt.pix.sizeimage < nv12_size) {
			fprintf(stderr, "encoder input %u bytes per line, %u "
				"bytes: unexpected\n", f.fmt.pix.bytesperline,
				f.fmt.pix.sizeimage);
			return 1;
		}

		memset(&f, 0, sizeof(f));
		f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		f.fmt.pix.width = out_w;
		f.fmt.pix.height = out_h;
		f.fmt.pix.pixelformat = jpeg ? V4L2_PIX_FMT_JPEG :
				       hevc ? V4L2_PIX_FMT_HEVC :
					      V4L2_PIX_FMT_H264;
		/*
		 * Without a size, coda allocates 1 bit per pixel (64 KiB). A
		 * larger frame stalls the encoder until the 1 s timeout, and
		 * the driver returns it cut short.
		 */
		f.fmt.pix.sizeimage = cap_size ? cap_size : nv12_size;
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
	if (efd >= 0 && hevc) {
		set_ctrl(efd, V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP, qp,
			 "I frame QP");
		set_ctrl(efd, V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP, qp,
			 "P frame QP");
		set_ctrl(efd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop, "GOP size");
	} else if (efd >= 0 && !jpeg) {
		set_ctrl(efd, V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, qp,
			 "I frame QP");
		set_ctrl(efd, V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP, qp,
			 "P frame QP");
		set_ctrl(efd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop, "GOP size");
		set_ctrl(efd, V4L2_CID_MPEG_VIDEO_BITRATE, 0, "bitrate");
	}
	if (efd >= 0) {
		if (isp) {
			/* The camera buffers are the encoder input */
			struct v4l2_requestbuffers rb = {
				.count = ncam, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
				.memory = V4L2_MEMORY_DMABUF,
			};

			xioctl(efd, VIDIOC_REQBUFS, &rb, "encoder VIDIOC_REQBUFS");
			if ((int)rb.count < ncam) {
				fprintf(stderr, "encoder: only %u input buffers\n",
					rb.count);
				return 1;
			}
		} else {
			map_bufs(efd, V4L2_BUF_TYPE_VIDEO_OUTPUT, eout,
				 NBUF_ENC_OUT, 0);
		}
		map_bufs(efd, V4L2_BUF_TYPE_VIDEO_CAPTURE, ecap, NBUF_ENC_CAP, 0);
		for (n = 0; n < NBUF_ENC_CAP; n++)
			qbuf(efd, V4L2_BUF_TYPE_VIDEO_CAPTURE, n, 0);
		stream(efd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 1);
		stream(efd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1);
	}

	if (yuv_file)
		out_file = yuv_file;
	if (!strcmp(out_file, "-")) {
		/*
		 * The stream gets its own descriptor, and stdout goes to
		 * stderr: library messages (the TPU runtime prints some) must
		 * not get into the stream.
		 */
		to_stdout = 1;
		ofd = dup(STDOUT_FILENO);
		if (ofd >= 0 && dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
			die("dup2");
	} else {
		ofd = open(out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	}
	if (ofd < 0)
		die(out_file);

#ifdef WITH_TPU
	/* Load the model before the camera starts */
	if (model)
		tpu_start(model, names_list, thresh, &names);
#endif

	if (cfd >= 0) {
		/*
		 * Cached buffers: the kernel invalidates the cache at DQBUF,
		 * and the conversion reads the frame in place. Uncached, a
		 * copy with wide loads is faster than reading in place.
		 */
		cam_cached = map_bufs(cfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, cam,
				      ncam, V4L2_MEMORY_FLAG_NON_COHERENT) &
			     V4L2_MEMORY_FLAG_NON_COHERENT;
		fprintf(stderr, "camera buffers: %s\n",
			!cam_cached ? (isp ? "uncached" : "uncached, copied") :
			!isp ? "cached" : no_hints ? "cached, full cache syncs" :
			"cached, cache hints");
		for (n = 0; isp && n < ncam; n++) {
			struct v4l2_exportbuffer eb = {
				.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .index = n,
				.flags = O_RDWR | O_CLOEXEC,
			};

			xioctl(cfd, VIDIOC_EXPBUF, &eb, "camera VIDIOC_EXPBUF");
			dmafd[n] = eb.fd;
		}
		if (isp) {
			double s = sat >= 0 ? sat / 256.0 : ISP_SAT;

			set_ctrl(cfd, ISP_CID_BLACK_LEVEL, black, "ISP black level");
			set_ctrl(cfd, V4L2_CID_GAMMA, 1, "ISP gamma");
			if (!isp_set_ccm(cfd, s))
				isp_sat = s;
			isp_set_wb(cfd);
			build_srgb_lin();
		}
		for (n = 0; n < ncam; n++)
			qbuf(cfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, n, 0);
		stream(cfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1);
	}

	signal(SIGINT, on_sigint);
	build_luts();
	fprintf(stderr, "%s -> %s%dx%d NV12 -> %s -> %s, ",
		cam_dev ? cam_dev : in_file, isp ? "ISP -> " : "", out_w, out_h,
		yuv_file ? "no encoder" : enc_dev, out_file);
	if (jpeg)
		fprintf(stderr, "JPEG quality %d\n", quality);
	else
		fprintf(stderr, "QP %d, GOP %d\n", qp, gop);

	if (sat < 0)
		sat = 384;		/* the raw path: 1.5 */
	t_start = now_ms();
	getrusage(RUSAGE_SELF, &ru0);
	n = 0;
	if (isp && efd >= 0)
		n = isp_loop(cfd, efd, ofd, cam, dmafd, ecap, out_w, out_h,
			     out_bpl, frames, jpeg, hevc, to_stdout,
			     cam_cached && !no_hints, &bytes, &t_enc, &lost);
	else if (isp)
		n = isp_loop_file(cfd, ofd, cam, out_w, out_h, out_bpl, frames,
				  cam_cached && !no_hints, &bytes);
	for (; !isp && !stop && (frames == 0 || n < frames); n++) {
		const uint8_t *src = raw;
		size_t used;
		int i, ci = -1;

		/* Get a raw frame */
		t0 = now_ms();
		if (cfd >= 0) {
			i = dqbuf(cfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, NULL);
			if (i < 0)
				break;
			if (cam_cached) {
				src = cam[i].p;
				ci = i;		/* queue it after the conversion */
			} else {
				copy64(raw, cam[i].p, raw_size);
				qbuf(cfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0);
			}
		} else {
			off_t pos = (off_t)(n % in_frames) * raw_size;

			if (pread(ifd, raw, raw_size, pos) != (ssize_t)raw_size)
				die("pread");
		}
		t1 = now_ms();
		t_cap += t1 - t0;

		/* Convert into a free encoder input buffer */
		if (n % 15 == 0)
			update_awb(src, stride);
		if (ae.fd >= 0 && n % 3 == 0)
			ae_update(src, stride);
		raw_to_nv12(src, stride, nv12, sat);
		if (ci >= 0)
			qbuf(cfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, ci, 0);
#ifdef WITH_TPU
		if (tpu.d)
			nbox = tpu_frame(nv12, ifd >= 0, names, box);
		if (tpu.d && (n % 30 == 0 || ifd >= 0))
			print_boxes(box, nbox, names);
#endif
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
		/*
		 * An access unit delimiter ends the picture: a stream reader
		 * can then send it at once, not at the next picture.
		 */
		if (!jpeg && to_stdout &&
		    (hevc ? write(ofd, "\0\0\0\1\x46\x01\x50", 7) != 7 :
			    write(ofd, "\0\0\0\1\x09\xf0", 6) != 6))
			die("write");
		bytes += used;
		qbuf(efd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0);
		t_enc += now_ms() - t0;

		if (n % 30 == 0 && ae.fd >= 0)
			fprintf(stderr, "frame %d: %zu bytes, WB R %.2f B %.2f, "
				"exposure %d lines, gain %.2fx, digital %.2fx\n",
				n, used, gain_r, gain_b, ae.exp,
				(double)ae.gain / ae.gain_min, expo);
		else if (n % 30 == 0)
			fprintf(stderr, "frame %d: %zu bytes, WB R %.2f B %.2f, "
				"digital gain %.2fx\n", n, used, gain_r, gain_b,
				expo);
	}
	t1 = now_ms();
	getrusage(RUSAGE_SELF, &ru1);
#ifdef WITH_TPU
	if (tpu.d)
		tpu_stop();
#endif

	if (n > 0 && isp)
		fprintf(stderr, "%d frames in %.1f s: %.1f fps, %.0f kbit/s at "
			"30 fps, %d frames lost. Latency from frame start to "
			"stream %.1f ms. Statistics: %d times, %.2f ms each\n",
			n, (t1 - t_start) / 1e3, n * 1e3 / (t1 - t_start),
			bytes * 8.0 * 30 / n / 1e3, lost, t_enc / n, isp_n_3a,
			isp_n_3a ? isp_t_3a / isp_n_3a : 0);
	else if (n > 0)
		fprintf(stderr, "%d frames in %.1f s: %.1f fps, %.0f kbit/s at "
			"30 fps. Per frame: input %.1f ms, convert %.1f ms, "
			"encode %.1f ms\n", n, (t1 - t_start) / 1e3,
			n * 1e3 / (t1 - t_start), bytes * 8.0 * 30 / n / 1e3,
			t_cap / n, t_conv / n, t_enc / n);
	/* System time includes the kernel's cache syncs of the camera buffers */
	if (n > 0)
		fprintf(stderr, "CPU per frame: user %.2f ms, system %.2f ms\n",
			((ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) * 1e3 +
			 (ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) / 1e3) / n,
			((ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) * 1e3 +
			 (ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) / 1e3) / n);

	if (cfd >= 0)
		stream(cfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0);
	if (efd >= 0) {
		stream(efd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 0);
		stream(efd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0);
	}
	close(ofd);
	return 0;
}
