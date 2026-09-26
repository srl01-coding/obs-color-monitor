#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <util/bmem.h>
#include "hdr-scale.h"

#define GLYPH_W 5
#define GLYPH_H 7
#define GLYPH_ADVANCE (GLYPH_W + 1)
#define LABEL_MARGIN 3

// 5x7 bitmap font, bit 4 = leftmost column
static const uint8_t font_digits[10][GLYPH_H] = {
	{0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, // 0
	{0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 1
	{0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, // 2
	{0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, // 3
	{0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // 4
	{0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 5
	{0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, // 6
	{0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
	{0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, // 8
	{0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, // 9
};
static const uint8_t font_percent[GLYPH_H] = {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03};

static const uint8_t *glyph(char c)
{
	if (c >= '0' && c <= '9')
		return font_digits[c - '0'];
	if (c == '%')
		return font_percent;
	return NULL;
}

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

static void put_text(uint8_t *img, uint32_t img_w, uint32_t img_h, int x0, int y0, const char *text, uint32_t s,
		     const uint8_t rgba[4])
{
	for (const char *c = text; *c; c++, x0 += GLYPH_ADVANCE * (int)s) {
		const uint8_t *g = glyph(*c);
		if (!g)
			continue;
		for (int gy = 0; gy < GLYPH_H; gy++) {
			for (int gx = 0; gx < GLYPH_W; gx++) {
				if (!(g[gy] & (0x10 >> gx)))
					continue;
				for (uint32_t dy = 0; dy < s; dy++) {
					const int y = y0 + gy * (int)s + (int)dy;
					if (y < 0 || y >= (int)img_h)
						continue;
					for (uint32_t dx = 0; dx < s; dx++) {
						const int x = x0 + gx * (int)s + (int)dx;
						if (x < 0 || x >= (int)img_w)
							continue;
						memcpy(img + ((size_t)y * img_w + x) * 4, rgba, 4);
					}
				}
			}
		}
	}
}

static const uint8_t color_normal[4] = {0xFF, 0xBF, 0x00, 0xE0}; // amber, as the graticule
static const uint8_t color_ref[4] = {0x40, 0xE0, 0xFF, 0xF0};    // cyan

static int text_width(const char *text, uint32_t s)
{
	return (int)(strlen(text) * GLYPH_ADVANCE * s) - (int)s;
}

uint8_t *hdr_scale_label_image_vertical(const struct hdr_scale_mark *marks, int n, uint32_t levels,
					uint32_t glyph_scale, uint32_t *width)
{
	int max_w = 0;
	for (int i = 0; i < n; i++) {
		int w = text_width(marks[i].label, glyph_scale);
		if (w > max_w)
			max_w = w;
	}
	const uint32_t w = (uint32_t)max_w + LABEL_MARGIN * 2;
	uint8_t *img = bzalloc((size_t)w * levels * 4);

	const int th = GLYPH_H * (int)glyph_scale;
	for (int i = 0; i < n; i++) {
		const int line_y = (int)levels - 1 - (int)(marks[i].code + 0.5f);
		int y0 = line_y - LABEL_MARGIN - th; // above the line
		if (y0 < 0)
			y0 = line_y + LABEL_MARGIN + 1; // no room: below the line
		put_text(img, w, levels, LABEL_MARGIN, y0, marks[i].label, glyph_scale,
			 marks[i].ref ? color_ref : color_normal);
	}

	*width = w;
	return img;
}

uint8_t *hdr_scale_label_image_horizontal(const struct hdr_scale_mark *marks, int n, uint32_t levels,
					  uint32_t glyph_scale, uint32_t *height)
{
	const uint32_t h = GLYPH_H * glyph_scale + LABEL_MARGIN * 2;
	uint8_t *img = bzalloc((size_t)levels * h * 4);

	for (int i = 0; i < n; i++) {
		const int tw = text_width(marks[i].label, glyph_scale);
		int x0 = (int)(marks[i].code + 0.5f) - tw / 2;
		if (x0 < LABEL_MARGIN)
			x0 = LABEL_MARGIN;
		if (x0 + tw > (int)levels - LABEL_MARGIN)
			x0 = (int)levels - LABEL_MARGIN - tw;
		put_text(img, levels, h, x0, LABEL_MARGIN, marks[i].label, glyph_scale,
			 marks[i].ref ? color_ref : color_normal);
	}

	*height = h;
	return img;
}
