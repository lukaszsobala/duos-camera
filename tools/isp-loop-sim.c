// SPDX-License-Identifier: GPL-2.0
/*
 * Host simulation of the ISP-mode auto exposure and white balance loops of
 * cam2h264: a scene of random patches under a warm light, a sensor that
 * clips, the ISP gains, the sRGB curve, BT.601 limited range and NV12.
 * Settings reach the frames DELAY frames after they are set.
 *
 *   cc -O2 -o isp-loop-sim isp-loop-sim.c -lm
 *   ./isp-loop-sim [light, default 0.2] [delay in frames, default 3]
 *                  [lamp area in %, default 0]
 *
 * A lamp area adds patches 20 times brighter than white paper: a backlit
 * scene.
 */
#define ioctl sim_ioctl
#define main cam_main
#include "cam2h264.c"
#undef ioctl
#undef main
#include <stdarg.h>

static int sim_exp = 300, sim_gain = 64, sim_r = 1638, sim_b = 1638;

int sim_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;
	unsigned int i;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	(void)fd;
	if (req == VIDIOC_S_EXT_CTRLS) {
		struct v4l2_ext_controls *cs = arg;

		for (i = 0; i < cs->count; i++) {
			int v = cs->controls[i].value;

			switch (cs->controls[i].id) {
			case V4L2_CID_EXPOSURE: sim_exp = v; break;
			case V4L2_CID_ANALOGUE_GAIN: sim_gain = v; break;
			case V4L2_CID_RED_BALANCE: sim_r = v; break;
			case V4L2_CID_BLUE_BALANCE: sim_b = v; break;
			}
		}
	}
	return 0;
}

#define W 640
#define H 360
static double refl[3][H / 20][W / 20];

static int srgb8(double v)
{
	v = v < 0 ? 0 : v > 1 ? 1 : v;
	v = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
	return (int)(v * 255 + 0.5);
}

/* Sensor + ISP model: frame from exposure/gain and the ISP gains */
static void render(uint8_t *f, double light, const double illum[3], int exp, int gain, int r, int b)
{
	uint8_t *uv = f + W * H;
	double g[3] = { r / 1024.0, 1, b / 1024.0 };
	int x, y, c;
	static int yy[H][W], uu[H][W], vv[H][W];

	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++) {
			int rgb[3];
			for (c = 0; c < 3; c++) {
				double s = refl[c][y / 20][x / 20] * illum[c] * light * exp * gain / 64.0 / 300.0;
				s = s > 1 ? 1 : s;		/* sensor clips */
				rgb[c] = srgb8(s * g[c]);	/* ISP gain, gamma */
			}
			yy[y][x] = ((66 * rgb[0] + 129 * rgb[1] + 25 * rgb[2] + 128) >> 8) + 16;
			uu[y][x] = ((-38 * rgb[0] - 74 * rgb[1] + 112 * rgb[2] + 128) >> 8) + 128;
			vv[y][x] = ((112 * rgb[0] - 94 * rgb[1] - 18 * rgb[2] + 128) >> 8) + 128;
			f[y * W + x] = yy[y][x];
		}
	for (y = 0; y < H; y += 2)
		for (x = 0; x < W; x += 2) {
			uv[(y / 2) * W + x] = (uu[y][x] + uu[y][x + 1] + uu[y + 1][x] + uu[y + 1][x + 1] + 2) / 4;
			uv[(y / 2) * W + x + 1] = (vv[y][x] + vv[y][x + 1] + vv[y + 1][x] + vv[y + 1][x + 1] + 2) / 4;
		}
}

int main(int argc, char **argv)
{
	static uint8_t f[W * H * 3 / 2];
	double illum[3] = { 0.55, 1.0, 0.45 };	/* warm light seen by an RGGB sensor */
	double light = argc > 1 ? atof(argv[1]) : 0.2;
	struct yuv_stats st;
	int n, x, y, c;
	unsigned int seed = 1;
	static int hist[8][4];
	int DELAY = argc > 2 ? atoi(argv[2]) : 3;
	double lamp = argc > 3 ? atof(argv[3]) / 100 : 0;

	for (y = 0; y < H / 20; y++)
		for (x = 0; x < W / 20; x++)
			for (c = 0; c < 3; c++) {
				seed = seed * 1103515245 + 12345;
				refl[c][y][x] = 0.05 + 0.6 * ((seed >> 16) & 0x7fff) / 32768.0;
			}
	/* The lamp: whole rows of patches from the top, then a part row */
	for (n = 0; n < (int)(lamp * (H / 20) * (W / 20) + 0.5); n++)
		for (c = 0; c < 3; c++)
			refl[c][n / (W / 20)][n % (W / 20)] = 20;
	ae.fd = 3; ae.exp_min = 1; ae.exp_max = 1120; ae.gain_min = 64;
	ae.gain_max = 64 * 32; ae.exp = sim_exp; ae.gain = sim_gain; ae.flicker = 0;
	expo = 1;
	build_srgb_lin();
	for (n = 0; n < 80; n++) {
		/* Settings reach the frame DELAY frames after they were set */
		hist[n % 8][0] = sim_exp; hist[n % 8][1] = sim_gain;
		hist[n % 8][2] = sim_r; hist[n % 8][3] = sim_b;
		{
			int k = n >= DELAY ? (n - DELAY) % 8 : 0;
			render(f, light, illum, hist[k][0], hist[k][1], hist[k][2], hist[k][3]);
		}
		if (n % 4 == 0) {
			isp_3a(3, f, W, H, W, &st);
			if (n % 4 == 0)
				printf("frame %2d: lum %.3f clip %4.1f%% hi %3d  mean R %.3f G %.3f B %.3f  exp %4d gain %5.2fx  WB R %.2f B %.2f\n",
				       n, st.lum, st.clipped * 100, st.hi, st.r, st.g, st.b, sim_exp, sim_gain / 64.0, isp_r, isp_b);
		}
	}
	printf("ideal WB for this scene: R %.2f B %.2f (from the illuminant alone)\n", 1 / illum[0], 1 / illum[2]);
	return 0;
}
