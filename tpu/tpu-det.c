// SPDX-License-Identifier: GPL-2.0
/*
 * tpu-det: YOLOv8 object detection on the SG2000 TPU. See tpu-det.h.
 *
 * The decoding follows the vendor TDL SDK
 * (tdl_sdk/modules/core/object_detection/yolov8/yolov8.cpp): class logits
 * through a sigmoid, box sides as the expected value of a 16-bin softmax
 * (DFL), then NMS per class.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cviruntime.h"
#include "font8x8.h"
#include "tpu-det.h"

#define NSTRIDE		3
#define REG_MAX		16
#define BOX_CH		(4 * REG_MAX)
#define MAX_CAND	1000

static const char *const coco80[80] = {
	"person", "bicycle", "car", "motorbike", "aeroplane", "bus", "train",
	"truck", "boat", "traffic light", "fire hydrant", "stop sign",
	"parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
	"elephant", "bear", "zebra", "giraffe", "backpack", "umbrella",
	"handbag", "tie", "suitcase", "frisbee", "skis", "snowboard",
	"sports ball", "kite", "baseball bat", "baseball glove", "skateboard",
	"surfboard", "tennis racket", "bottle", "wine glass", "cup", "fork",
	"knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
	"broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
	"sofa", "pottedplant", "bed", "diningtable", "toilet", "tvmonitor",
	"laptop", "mouse", "remote", "keyboard", "cell phone", "microwave",
	"oven", "toaster", "sink", "refrigerator", "book", "clock", "vase",
	"scissors", "teddy bear", "hair drier", "toothbrush",
};

struct level {
	int stride, fw, fh;
	CVI_TENSOR *box, *cls;
};

struct det {
	CVI_MODEL_HANDLE model;
	CVI_TENSOR *in, *out;
	int nin, nout;
	int iw, ih, ncls;
	struct level lv[NSTRIDE];
	int8_t q[1024];		/* 8-bit value + 384 -> input code */
	/* Letterbox: frame (fw x fh) scaled to (nw x nh) at (px, py) */
	int fw, fh, nw, nh, px, py;
	float scale;
	int *sx;		/* input column -> frame column */
	int8_t *amax;		/* per anchor: highest class logit */
	uint8_t *aidx;		/* per anchor: its class */
	struct det_box *cand;
	char *names_buf;
	const char **names;
	struct det_stats st;
};

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static const char *fmt_name(CVI_FMT f)
{
	static const char *const n[] = {
		"FP32", "INT32", "UINT32", "BF16", "INT16", "UINT16", "INT8",
		"UINT8",
	};

	return (unsigned)f < 8 ? n[f] : "?";
}

static void print_tensor(const char *kind, CVI_TENSOR *t)
{
	CVI_SHAPE s = CVI_NN_TensorShape(t);
	size_t i;

	fprintf(stderr, "  %s %-16s %-5s ", kind, CVI_NN_TensorName(t),
		fmt_name(t->fmt));
	for (i = 0; i < s.dim_size; i++)
		fprintf(stderr, "%s%d", i ? "x" : "", s.dim[i]);
	fprintf(stderr, " qscale %g\n", CVI_NN_TensorQuantScale(t));
}

struct det *det_open(const char *path)
{
	struct det *d = calloc(1, sizeof(*d));
	CVI_SHAPE s;
	int i, j, na = 0;

	if (!d)
		return NULL;
	if (CVI_NN_RegisterModel(path, &d->model) != CVI_RC_SUCCESS) {
		fprintf(stderr, "%s: CVI_NN_RegisterModel failed\n", path);
		free(d);
		return NULL;
	}
	CVI_NN_GetInputOutputTensors(d->model, &d->in, &d->nin, &d->out,
				     &d->nout);
	fprintf(stderr, "%s: %d input, %d outputs\n", path, d->nin, d->nout);
	for (i = 0; i < d->nin; i++)
		print_tensor("in ", &d->in[i]);
	for (i = 0; i < d->nout; i++)
		print_tensor("out", &d->out[i]);

	s = CVI_NN_TensorShape(&d->in[0]);
	if (d->nin != 1 || s.dim_size != 4 || s.dim[0] != 1 || s.dim[1] != 3 ||
	    d->in[0].fmt != CVI_FMT_INT8 || d->in[0].aligned) {
		fprintf(stderr, "need one INT8 1x3xHxW input, not aligned\n");
		goto fail;
	}
	d->ih = s.dim[2];
	d->iw = s.dim[3];

	/* Sort the outputs by stride, and into box and class maps */
	for (i = 0; i < NSTRIDE; i++)
		d->lv[i].stride = 8 << i;
	for (i = 0; i < d->nout; i++) {
		CVI_TENSOR *t = &d->out[i];
		int c, st;

		s = CVI_NN_TensorShape(t);
		if (s.dim_size != 4 || s.dim[0] != 1 || t->fmt != CVI_FMT_INT8) {
			fprintf(stderr, "output %d: need INT8 1xCxHxW\n", i);
			goto fail;
		}
		c = s.dim[1];
		st = d->ih / s.dim[2];
		for (j = 0; j < NSTRIDE; j++)
			if (d->lv[j].stride == st)
				break;
		if (j == NSTRIDE || d->iw / s.dim[3] != st) {
			fprintf(stderr, "output %d: no stride 8/16/32\n", i);
			goto fail;
		}
		d->lv[j].fw = s.dim[3];
		d->lv[j].fh = s.dim[2];
		if (c == BOX_CH && !d->lv[j].box) {
			d->lv[j].box = t;
		} else if (!d->lv[j].cls && (!d->ncls || c == d->ncls)) {
			d->lv[j].cls = t;
			d->ncls = c;
		} else {
			fprintf(stderr, "output %d: unexpected %d channels\n",
				i, c);
			goto fail;
		}
	}
	for (j = 0; j < NSTRIDE; j++) {
		if (!d->lv[j].box || !d->lv[j].cls) {
			fprintf(stderr, "stride %d: no box or class output\n",
				d->lv[j].stride);
			goto fail;
		}
		na += d->lv[j].fw * d->lv[j].fh;
	}
	if (d->ncls > 256) {
		fprintf(stderr, "%d classes: at most 256\n", d->ncls);
		goto fail;
	}

	/* Input code: round(v / 255 * qscale), v clamped to 0-255 */
	for (i = 0; i < 1024; i++) {
		int v = i - 384;
		float q;

		v = v < 0 ? 0 : v > 255 ? 255 : v;
		q = roundf(v / 255.0f * CVI_NN_TensorQuantScale(&d->in[0]));
		d->q[i] = q > 127 ? 127 : (int8_t)q;
	}

	d->sx = malloc(d->iw * sizeof(*d->sx));
	d->amax = malloc(na);
	d->aidx = malloc(na);
	d->cand = malloc(MAX_CAND * sizeof(*d->cand));
	if (!d->sx || !d->amax || !d->aidx || !d->cand)
		goto fail;
	fprintf(stderr, "detector: %dx%d input, %d classes, %d anchors\n",
		d->iw, d->ih, d->ncls, na);
	return d;
fail:
	det_close(d);
	return NULL;
}

void det_close(struct det *d)
{
	if (!d)
		return;
	if (d->model)
		CVI_NN_CleanupModel(d->model);
	free(d->sx);
	free(d->amax);
	free(d->aidx);
	free(d->cand);
	free(d->names_buf);
	free(d->names);
	free(d);
}

int det_classes(const struct det *d)
{
	return d->ncls;
}

const struct det_stats *det_stats(const struct det *d)
{
	return &d->st;
}

const char *const *det_names(struct det *d, const char *list)
{
	char *p;
	int i;

	if (!list && d->ncls == 80)
		return coco80;
	free(d->names);
	free(d->names_buf);
	d->names = calloc(d->ncls, sizeof(*d->names));
	d->names_buf = malloc(list ? strlen(list) + 1 : (size_t)d->ncls * 4);
	if (!d->names || !d->names_buf)
		return NULL;
	if (list) {
		strcpy(d->names_buf, list);
		p = d->names_buf;
		for (i = 0; i < d->ncls && p; i++) {
			d->names[i] = p;
			p = strchr(p, ',');
			if (p)
				*p++ = 0;
		}
		for (; i < d->ncls; i++)
			d->names[i] = "?";
	} else {
		p = d->names_buf;
		for (i = 0; i < d->ncls; i++) {
			d->names[i] = p;
			p += sprintf(p, "%d", i) + 1;
		}
	}
	return d->names;
}

void det_set_frame(struct det *d, int w, int h)
{
	int x;

	d->fw = w;
	d->fh = h;
	d->scale = (float)d->iw / w < (float)d->ih / h ?
		   (float)d->iw / w : (float)d->ih / h;
	d->nw = (int)(w * d->scale + 0.5f);
	d->nh = (int)(h * d->scale + 0.5f);
	d->nw = d->nw > d->iw ? d->iw : d->nw;
	d->nh = d->nh > d->ih ? d->ih : d->nh;
	d->px = (d->iw - d->nw) / 2;
	d->py = (d->ih - d->nh) / 2;
	for (x = 0; x < d->nw; x++)
		d->sx[d->px + x] = (int)((x + 0.5f) * w / d->nw);
}

/* Pad rows and columns outside the picture with code 0 (black) */
static void clear_border(struct det *d, int8_t *pl)
{
	int y;

	memset(pl, 0, (size_t)d->py * d->iw);
	memset(pl + (size_t)(d->py + d->nh) * d->iw, 0,
	       (size_t)(d->ih - d->py - d->nh) * d->iw);
	for (y = d->py; y < d->py + d->nh; y++) {
		memset(pl + (size_t)y * d->iw, 0, d->px);
		memset(pl + (size_t)y * d->iw + d->px + d->nw, 0,
		       d->iw - d->px - d->nw);
	}
}

/* Nearest-pixel scaling, BT.601 limited range to RGB */
void det_load_nv12(struct det *d, const uint8_t *nv12)
{
	int8_t *r = CVI_NN_TensorPtr(&d->in[0]);
	size_t plane = (size_t)d->iw * d->ih;
	int8_t *g = r + plane, *b = g + plane;
	const uint8_t *uv = nv12 + (size_t)d->fw * d->fh;
	const int8_t *q = d->q + 384;
	double t0 = now_ms();
	int x, y, c;

	for (c = 0; c < 3; c++)
		clear_border(d, r + c * plane);
	for (y = 0; y < d->nh; y++) {
		int sy = (int)((y + 0.5f) * d->fh / d->nh);
		const uint8_t *yl = nv12 + (size_t)sy * d->fw;
		const uint8_t *cl = uv + (size_t)(sy / 2) * d->fw;
		size_t o = (size_t)(d->py + y) * d->iw;

		for (x = d->px; x < d->px + d->nw; x++) {
			int sx = d->sx[x];
			int yy = 298 * (yl[sx] - 16) + 128;
			int u = cl[sx & ~1] - 128, v = cl[sx | 1] - 128;

			r[o + x] = q[(yy + 409 * v) >> 8];
			g[o + x] = q[(yy - 100 * u - 208 * v) >> 8];
			b[o + x] = q[(yy + 516 * u) >> 8];
		}
	}
	d->st.load_ms += now_ms() - t0;
}

void det_load_rgb(struct det *d, const uint8_t *rgb, int stride)
{
	int8_t *r = CVI_NN_TensorPtr(&d->in[0]);
	size_t plane = (size_t)d->iw * d->ih;
	int8_t *g = r + plane, *b = g + plane;
	const int8_t *q = d->q + 384;
	double t0 = now_ms();
	int x, y, c;

	for (c = 0; c < 3; c++)
		clear_border(d, r + c * plane);
	for (y = 0; y < d->nh; y++) {
		int sy = (int)((y + 0.5f) * d->fh / d->nh);
		const uint8_t *l = rgb + (size_t)sy * stride;
		size_t o = (size_t)(d->py + y) * d->iw;

		for (x = d->px; x < d->px + d->nw; x++) {
			const uint8_t *p = l + 3 * d->sx[x];

			r[o + x] = q[p[0]];
			g[o + x] = q[p[1]];
			b[o + x] = q[p[2]];
		}
	}
	d->st.load_ms += now_ms() - t0;
}

/* Expected value of a softmax over REG_MAX bins */
static float dfl(const int8_t *p, int step, float qs)
{
	float e[REG_MAX], m = -1e9f, sum = 0, val = 0;
	int i;

	for (i = 0; i < REG_MAX; i++) {
		e[i] = p[i * step] * qs;
		m = e[i] > m ? e[i] : m;
	}
	for (i = 0; i < REG_MAX; i++) {
		float x = expf(e[i] - m);

		sum += x;
		val += x * i;
	}
	return val / sum;
}

static float iou(const struct det_box *a, const struct det_box *b)
{
	float w = fminf(a->x2, b->x2) - fmaxf(a->x1, b->x1);
	float h = fminf(a->y2, b->y2) - fmaxf(a->y1, b->y1);
	float i, u;

	if (w <= 0 || h <= 0)
		return 0;
	i = w * h;
	u = (a->x2 - a->x1) * (a->y2 - a->y1) +
	    (b->x2 - b->x1) * (b->y2 - b->y1) - i;
	return u > 0 ? i / u : 0;
}

static int by_score(const void *a, const void *b)
{
	float d = ((const struct det_box *)b)->score -
		  ((const struct det_box *)a)->score;

	return d > 0 ? 1 : d < 0 ? -1 : 0;
}

int det_run(struct det *d, float thresh, float iou_max, struct det_box *out,
	    int max)
{
	/* sigmoid(x) >= thresh  <=>  x >= log(thresh / (1 - thresh)) */
	float lt = logf(thresh / (1 - thresh));
	double t0 = now_ms(), t1;
	int nc = 0, n = 0, i, j, k;

	if (CVI_NN_Forward(d->model, d->in, d->nin, d->out, d->nout) !=
	    CVI_RC_SUCCESS) {
		fprintf(stderr, "CVI_NN_Forward failed\n");
		return -1;
	}
	t1 = now_ms();
	d->st.tpu_ms += t1 - t0;

	for (k = 0; k < NSTRIDE; k++) {
		struct level *l = &d->lv[k];
		int na = l->fw * l->fh;
		const int8_t *cls = CVI_NN_TensorPtr(l->cls);
		const int8_t *box = CVI_NN_TensorPtr(l->box);
		float cqs = CVI_NN_TensorQuantScale(l->cls);
		float bqs = CVI_NN_TensorQuantScale(l->box);
		/* The smallest code at or above the threshold */
		int qt = (int)ceilf(lt / cqs);
		int8_t *am = d->amax;
		uint8_t *ai = d->aidx;

		if (qt > 127)
			continue;
		qt = qt < -128 ? -128 : qt;

		/* Highest class per anchor, class by class: contiguous reads */
		memcpy(am, cls, na);
		memset(ai, 0, na);
		for (i = 1; i < d->ncls; i++) {
			const int8_t *c = cls + (size_t)i * na;

			for (j = 0; j < na; j++)
				if (c[j] > am[j]) {
					am[j] = c[j];
					ai[j] = i;
				}
		}

		for (j = 0; j < na && nc < MAX_CAND; j++) {
			struct det_box *b = &d->cand[nc];
			float gx = j % l->fw + 0.5f, gy = j / l->fw + 0.5f;

			if (am[j] < qt)
				continue;
			b->score = 1 / (1 + expf(-am[j] * cqs));
			b->cls = ai[j];
			b->x1 = (gx - dfl(box + j, na, bqs)) * l->stride;
			b->y1 = (gy - dfl(box + (size_t)REG_MAX * na + j, na,
					  bqs)) * l->stride;
			b->x2 = (gx + dfl(box + (size_t)2 * REG_MAX * na + j,
					  na, bqs)) * l->stride;
			b->y2 = (gy + dfl(box + (size_t)3 * REG_MAX * na + j,
					  na, bqs)) * l->stride;
			/* Model input -> frame pixels */
			b->x1 = (b->x1 - d->px) / d->scale;
			b->x2 = (b->x2 - d->px) / d->scale;
			b->y1 = (b->y1 - d->py) / d->scale;
			b->y2 = (b->y2 - d->py) / d->scale;
			b->x1 = fmaxf(b->x1, 0);
			b->y1 = fmaxf(b->y1, 0);
			b->x2 = fminf(b->x2, d->fw - 1);
			b->y2 = fminf(b->y2, d->fh - 1);
			if (b->x2 - b->x1 > 1 && b->y2 - b->y1 > 1)
				nc++;
		}
	}

	/* NMS per class */
	qsort(d->cand, nc, sizeof(*d->cand), by_score);
	for (i = 0; i < nc && n < max; i++) {
		for (j = 0; j < n; j++)
			if (out[j].cls == d->cand[i].cls &&
			    iou(&out[j], &d->cand[i]) > iou_max)
				break;
		if (j == n)
			out[n++] = d->cand[i];
	}
	d->st.post_ms += now_ms() - t1;
	d->st.runs++;
	return n;
}

/* Drawing */

static const uint8_t palette[8][3] = {
	{ 255, 56, 56 }, { 72, 249, 10 }, { 0, 194, 255 }, { 255, 157, 151 },
	{ 207, 210, 49 }, { 146, 204, 23 }, { 255, 112, 31 }, { 132, 56, 255 },
};

#define THICK	2
#define TEXT_SCALE 2

struct canvas {
	uint8_t *p;
	int w, h, nv12;
};

static void color_yuv(const uint8_t *c, int *y, int *u, int *v)
{
	*y = ((66 * c[0] + 129 * c[1] + 25 * c[2] + 128) >> 8) + 16;
	*u = ((-38 * c[0] - 74 * c[1] + 112 * c[2] + 128) >> 8) + 128;
	*v = ((112 * c[0] - 94 * c[1] - 18 * c[2] + 128) >> 8) + 128;
}

/* Fill a rectangle, clipped. NV12: chroma at even coordinates. */
static void fill(struct canvas *cv, int x1, int y1, int x2, int y2,
		 const uint8_t *c)
{
	int x, y, yy, u, v;

	x1 = x1 < 0 ? 0 : x1;
	y1 = y1 < 0 ? 0 : y1;
	x2 = x2 > cv->w ? cv->w : x2;
	y2 = y2 > cv->h ? cv->h : y2;
	if (x1 >= x2 || y1 >= y2)
		return;
	if (!cv->nv12) {
		for (y = y1; y < y2; y++)
			for (x = x1; x < x2; x++)
				memcpy(cv->p + ((size_t)y * cv->w + x) * 3, c, 3);
		return;
	}
	color_yuv(c, &yy, &u, &v);
	for (y = y1; y < y2; y++)
		memset(cv->p + (size_t)y * cv->w + x1, yy, x2 - x1);
	for (y = y1 & ~1; y < y2; y += 2) {
		uint8_t *l = cv->p + (size_t)cv->w * cv->h +
			     (size_t)(y / 2) * cv->w;

		for (x = x1 & ~1; x < x2; x += 2) {
			l[x] = u;
			l[x + 1] = v;
		}
	}
}

static void text(struct canvas *cv, int x0, int y0, const char *s,
		 const uint8_t *fg)
{
	for (; *s; s++, x0 += 8 * TEXT_SCALE) {
		const unsigned char *g;
		int r, b;

		if (*s < 0x20 || *s > 0x7e)
			continue;
		g = font8x8[*s - 0x20];
		for (r = 0; r < 8; r++)
			for (b = 0; b < 8; b++)
				if (g[r] >> b & 1)
					fill(cv, x0 + b * TEXT_SCALE,
					     y0 + r * TEXT_SCALE,
					     x0 + (b + 1) * TEXT_SCALE,
					     y0 + (r + 1) * TEXT_SCALE, fg);
	}
}

static void draw(struct canvas *cv, const struct det_box *bx, int n,
		 const char *const *names)
{
	static const uint8_t white[3] = { 255, 255, 255 };
	static const uint8_t black[3] = { 0, 0, 0 };
	int i;

	for (i = 0; i < n; i++) {
		const uint8_t *c = palette[bx[i].cls % 8];
		int x1 = (int)bx[i].x1, y1 = (int)bx[i].y1;
		int x2 = (int)bx[i].x2, y2 = (int)bx[i].y2;
		int light = 66 * c[0] + 129 * c[1] + 25 * c[2] > 128 * 220;
		char label[48];
		int tw, th = 8 * TEXT_SCALE + 2, ty;

		fill(cv, x1, y1, x2 + 1, y1 + THICK, c);
		fill(cv, x1, y2 + 1 - THICK, x2 + 1, y2 + 1, c);
		fill(cv, x1, y1, x1 + THICK, y2 + 1, c);
		fill(cv, x2 + 1 - THICK, y1, x2 + 1, y2 + 1, c);

		if (names)
			snprintf(label, sizeof(label), "%s %.0f",
				 names[bx[i].cls], bx[i].score * 100);
		else
			snprintf(label, sizeof(label), "%d %.0f", bx[i].cls,
				 bx[i].score * 100);
		tw = (int)strlen(label) * 8 * TEXT_SCALE + 2;
		/* Above the box, or inside it at the top of the picture */
		ty = y1 >= th ? y1 - th : y1;
		fill(cv, x1, ty, x1 + tw, ty + th, c);
		text(cv, x1 + 1, ty + 1, label, light ? black : white);
	}
}

void det_draw_nv12(uint8_t *nv12, int w, int h, const struct det_box *b,
		   int n, const char *const *names)
{
	struct canvas cv = { nv12, w, h, 1 };

	draw(&cv, b, n, names);
}

void det_draw_rgb(uint8_t *rgb, int w, int h, const struct det_box *b, int n,
		  const char *const *names)
{
	struct canvas cv = { rgb, w, h, 0 };

	draw(&cv, b, n, names);
}
