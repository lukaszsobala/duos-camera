/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tpu-det: YOLOv8 object detection on the SG2000 TPU, with the Cvitek
 * runtime (cviruntime). For the TDL models from github.com/sophgo/tdl_models
 * (yolov8n_det_*_cv181x.cvimodel): INT8 RGB planar input, and per stride
 * 8/16/32 one box output (64 channels, DFL) and one class output.
 */
#ifndef TPU_DET_H
#define TPU_DET_H

#include <stdint.h>

struct det_box {
	float x1, y1, x2, y2;	/* frame pixels */
	float score;
	int cls;
};

struct det_stats {
	int runs;
	double load_ms, tpu_ms, post_ms;	/* sums over the runs */
};

struct det;

/* Load a model. Prints its tensors to stderr. NULL on error. */
struct det *det_open(const char *model);
void det_close(struct det *d);
int det_classes(const struct det *d);

/*
 * Class names from a comma-separated list, or NULL for the COCO names
 * (80-class models) or the class numbers. The array lives as long as d.
 */
const char *const *det_names(struct det *d, const char *list);

/* Frame size. The frame goes into the model input letterboxed. */
void det_set_frame(struct det *d, int w, int h);

/* Fill the model input from an NV12 frame (stride = width) */
void det_load_nv12(struct det *d, const uint8_t *nv12);

/* Fill the model input from packed 8-bit RGB */
void det_load_rgb(struct det *d, const uint8_t *rgb, int stride);

/*
 * Run the model on the loaded input. Returns the number of boxes put in
 * out, at most max, best score first, in frame pixels.
 */
int det_run(struct det *d, float thresh, float iou, struct det_box *out,
	    int max);

const struct det_stats *det_stats(const struct det *d);

/* Draw boxes with labels. names may be NULL. */
void det_draw_nv12(uint8_t *nv12, int w, int h, const struct det_box *b,
		   int n, const char *const *names);
void det_draw_rgb(uint8_t *rgb, int w, int h, const struct det_box *b, int n,
		  const char *const *names);

#endif
