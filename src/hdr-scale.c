#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <util/bmem.h>
#include "hdr-scale.h"

static void add_mark(struct hdr_scale_mark *marks, int *n, float code, bool ref, const char *label)
{
	for (int i = 0; i < *n; i++) {
		if (marks[i].code > code - 0.5f && marks[i].code < code + 0.5f) {
			marks[i].ref |= ref;
			return;
		}
	}
	if (*n >= HDR_SCALE_MAX_MARKS)
		return;
	marks[*n].code = code;
	marks[*n].ref = ref;
	snprintf(marks[*n].label, sizeof(marks[*n].label), "%s", label);
	++*n;
}

int hdr_scale_marks(struct hdr_scale_mark *marks, int mode, int divisions, bool full_range)
{
	int n = 0;
	char label[16];

	if (divisions <= 0)
		return 0;

	if (mode == HDR_SCALE_CODE_10BIT) {
		if (full_range) {
			add_mark(marks, &n, 0.0f, true, "0");
			add_mark(marks, &n, 1023.0f, true, "1023");
		} else {
			add_mark(marks, &n, 64.0f, true, "64");
			add_mark(marks, &n, 940.0f, true, "940");
		}
		const int step = divisions >= 10 ? 64 : divisions >= 4 ? 128 : 256;
		for (int c = step; c < 1024; c += step) {
			// keep labels of the reference lines readable
			if (!full_range && (abs(c - 64) < 40 || abs(c - 940) < 40))
				continue;
			snprintf(label, sizeof(label), "%d", c);
			add_mark(marks, &n, (float)c, false, label);
		}
		return n;
	}

	// HLG %
	for (int k = 0; k <= divisions; k++) {
		const int pct = 100 * k / divisions;
		snprintf(label, sizeof(label), "%d%%", pct);
		add_mark(marks, &n, hdr_scale_signal_to_code(pct / 100.0f, full_range), pct == 0 || pct == 100, label);
	}
	snprintf(label, sizeof(label), "%d%%", HDR_SCALE_REF_WHITE_PERCENT);
	add_mark(marks, &n, hdr_scale_signal_to_code(HDR_SCALE_REF_WHITE_PERCENT / 100.0f, full_range), true, label);
	return n;
}

struct placed
{
	float lo, hi;
};

static bool overlaps(const struct placed *p, int n, float lo, float hi, float pad)
{
	for (int i = 0; i < n; i++) {
		if (lo <= p[i].hi + pad && hi >= p[i].lo - pad)
			return true;
	}
	return false;
}

int hdr_scale_layout_vertical(const struct hdr_scale_mark *marks, const float *line_y, int n, float x, float size,
			      float gap, float top, struct hdr_scale_label *out)
{
	struct placed placed[HDR_SCALE_MAX_MARKS];
	int n_out = 0;
	const float pad = 0.25f * size;

	// Reference labels first; other labels are dropped if they would collide.
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < n && n_out < HDR_SCALE_MAX_MARKS; i++) {
			if (marks[i].ref != (pass == 0))
				continue;
			float baseline = line_y[i] - gap; // above the line
			if (baseline - size < top)
				baseline = line_y[i] + gap + size; // no room: below the line
			if (overlaps(placed, n_out, baseline - size, baseline, pad))
				continue;
			placed[n_out].lo = baseline - size;
			placed[n_out].hi = baseline;
			struct hdr_scale_label *l = &out[n_out++];
			l->x = x;
			l->y = baseline;
			l->align = 0; // left
			l->ref = marks[i].ref;
			snprintf(l->text, sizeof(l->text), "%s", marks[i].label);
		}
	}
	return n_out;
}

int hdr_scale_layout_horizontal(const struct hdr_scale_mark *marks, const float *line_x, int n, float y, float size,
				float left, float right, float (*measure)(const char *, float),
				struct hdr_scale_label *out)
{
	struct placed placed[HDR_SCALE_MAX_MARKS];
	int n_out = 0;
	const float pad = 0.5f * size;

	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < n && n_out < HDR_SCALE_MAX_MARKS; i++) {
			if (marks[i].ref != (pass == 0))
				continue;
			const float tw = measure(marks[i].label, size);
			float x0 = line_x[i] - 0.5f * tw;
			if (x0 < left)
				x0 = left;
			if (x0 + tw > right)
				x0 = right - tw;
			if (x0 < left)
				continue; // does not fit at all
			if (overlaps(placed, n_out, x0, x0 + tw, pad))
				continue;
			placed[n_out].lo = x0;
			placed[n_out].hi = x0 + tw;
			struct hdr_scale_label *l = &out[n_out++];
			l->x = x0;
			l->y = y;
			l->align = 0; // left edge already resolved
			l->ref = marks[i].ref;
			snprintf(l->text, sizeof(l->text), "%s", marks[i].label);
		}
	}
	return n_out;
}
