// SPDX-License-Identifier: GPL-2.0
/*
 * det-image: run a YOLOv8 TDL model on the TPU on one picture.
 *
 *   det-image [options] model.cvimodel in.ppm [out.ppm]
 *
 * Options:
 *   -t T     score threshold, 0-1 (default 0.5)
 *   -L LIST  class names, comma-separated (default: COCO for 80 classes)
 *   -n N     run the model N times, for the timing (default 1)
 *
 * in.ppm is a binary PPM (P6, 8-bit RGB). Convert on the PC, for example
 * with Pillow: Image.open("dog.jpg").convert("RGB").save("dog.ppm").
 * out.ppm is the picture with the boxes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tpu-det.h"

#define MAX_BOX 100

static uint8_t *read_ppm(const char *path, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	uint8_t *p = NULL;
	int maxv;

	if (!f) {
		perror(path);
		return NULL;
	}
	if (fscanf(f, "P6 %d %d %d", w, h, &maxv) != 3 || maxv != 255 ||
	    fgetc(f) == EOF || *w <= 0 || *h <= 0) {
		fprintf(stderr, "%s: need a binary 8-bit PPM (P6)\n", path);
		fclose(f);
		return NULL;
	}
	p = malloc((size_t)*w * *h * 3);
	if (p && fread(p, 3, (size_t)*w * *h, f) != (size_t)*w * *h) {
		fprintf(stderr, "%s: short file\n", path);
		free(p);
		p = NULL;
	}
	fclose(f);
	return p;
}

int main(int argc, char **argv)
{
	struct det_box box[MAX_BOX];
	const char *names_list = NULL;
	const char *const *names;
	const struct det_stats *st;
	float thresh = 0.5f;
	int opt, w, h, n = 0, i, runs = 1;
	struct det *d;
	uint8_t *rgb;

	while ((opt = getopt(argc, argv, "t:L:n:")) != -1) {
		switch (opt) {
		case 't': thresh = atof(optarg); break;
		case 'L': names_list = optarg; break;
		case 'n': runs = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s [-t thresh] [-L names] "
				"[-n runs] model.cvimodel in.ppm [out.ppm]\n",
				argv[0]);
			return 1;
		}
	}
	if (argc - optind < 2) {
		fprintf(stderr, "give a model and a picture\n");
		return 1;
	}
	rgb = read_ppm(argv[optind + 1], &w, &h);
	if (!rgb)
		return 1;
	d = det_open(argv[optind]);
	if (!d)
		return 1;
	names = det_names(d, names_list);
	det_set_frame(d, w, h);

	for (i = 0; i < runs; i++) {
		det_load_rgb(d, rgb, w * 3);
		n = det_run(d, thresh, 0.5f, box, MAX_BOX);
		if (n < 0)
			return 1;
	}

	printf("%dx%d, %d objects\n", w, h, n);
	for (i = 0; i < n; i++)
		printf("  %-14s %.3f  %4.0f %4.0f %4.0f %4.0f\n",
		       names ? names[box[i].cls] : "?", box[i].score,
		       box[i].x1, box[i].y1, box[i].x2, box[i].y2);
	st = det_stats(d);
	printf("%d runs. Per run: input %.1f ms, TPU %.1f ms, decode %.1f ms\n",
	       st->runs, st->load_ms / st->runs, st->tpu_ms / st->runs,
	       st->post_ms / st->runs);

	if (argc - optind > 2) {
		FILE *f = fopen(argv[optind + 2], "wb");

		det_draw_rgb(rgb, w, h, box, n, names);
		if (!f || fprintf(f, "P6\n%d %d\n255\n", w, h) < 0 ||
		    fwrite(rgb, 3, (size_t)w * h, f) != (size_t)w * h) {
			perror(argv[optind + 2]);
			return 1;
		}
		fclose(f);
	}
	det_close(d);
	free(rgb);
	return 0;
}
