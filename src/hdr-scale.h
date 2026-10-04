#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Graticule scale for 10-bit HLG scopes
#define HDR_SCALE_HLG_PERCENT 0 // HLG signal level E' in %, 0% = black, 100% = nominal peak
#define HDR_SCALE_CODE_10BIT 1  // 10-bit code value (narrow range: 64 = black, 940 = nominal peak)

#define HDR_SCALE_MAX_MARKS 24
#define HDR_SCALE_REF_WHITE_PERCENT 75 // ITU-R BT.2408 HDR reference white (graphics white)

struct hdr_scale_mark
{
	float code; // position in 10-bit code values (0..1023)
	bool ref;   // reference line (black, nominal peak, reference white): drawn highlighted
	char label[8];
};

// divisions: graticule setting of the source (1, 2, 4, 5, 10); 0 = no graticule
int hdr_scale_marks(struct hdr_scale_mark *marks, int mode, int divisions, bool full_range);

// Convert HLG signal level (0..1) to 10-bit code value
static inline float hdr_scale_signal_to_code(float e, bool full_range)
{
	return full_range ? e * 1023.0f : 64.0f + 876.0f * e;
}

// Position of the centre of a 10-bit code's bin on an axis of `levels` pixels (bins of 1024/levels codes)
static inline float hdr_scale_code_to_px(float code, uint32_t levels)
{
	return ((float)((uint32_t)code * levels / 1024) + 0.5f);
}

// Display resolution of HLG scopes: 256 (4 codes per row, same size as SDR), 512 or 1024
static inline uint32_t hdr_rows_sanitize(int rows)
{
	return rows >= 1024 ? 1024 : rows >= 512 ? 512 : 256;
}

static inline uint32_t hdr_rows_shift(uint32_t rows)
{
	return rows >= 1024 ? 0 : rows >= 512 ? 1 : 2;
}

// Label placement for drawing with scope_text (any drawing units, y grows downward).
struct hdr_scale_label
{
	float x, y; // anchor: x as per `align`, y = text baseline
	int align;  // SCOPE_TEXT_LEFT / CENTER / RIGHT
	bool ref;
	char text[8];
};

// Vertical axis (waveform): line_y[i] is the drawn position of marks[i]. Text is
// left-aligned at x with its baseline `gap` above the line, or below the line when
// there is no room above `top`. Labels that would collide are dropped, reference
// labels first. Returns the number of labels written to out (<= n).
int hdr_scale_layout_vertical(const struct hdr_scale_mark *marks, const float *line_y, int n, float x, float size,
			      float gap, float top, struct hdr_scale_label *out);

// Horizontal axis (histogram): text centred on line_x[i] with its baseline at y,
// kept within [left, right]; `measure` returns the width of a text at `size`.
int hdr_scale_layout_horizontal(const struct hdr_scale_mark *marks, const float *line_x, int n, float y, float size,
				float left, float right, float (*measure)(const char *, float),
				struct hdr_scale_label *out);

#ifdef __cplusplus
}
#endif
