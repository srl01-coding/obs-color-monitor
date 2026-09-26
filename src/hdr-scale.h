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

// RGBA label images (row 0 = top) for overlaying on scopes. Caller frees with bfree.
// `levels` is the axis length in pixels; 10-bit codes are mapped onto it (1024 codes -> levels px).
// Labels that would collide are dropped, reference labels have priority.
// Vertical: height `levels`, text placed just above each mark (code increases upward).
uint8_t *hdr_scale_label_image_vertical(const struct hdr_scale_mark *marks, int n, uint32_t levels,
					uint32_t glyph_scale, uint32_t *width);
// Horizontal: width `levels`, text centred on each mark (code increases rightward).
uint8_t *hdr_scale_label_image_horizontal(const struct hdr_scale_mark *marks, int n, uint32_t levels,
					  uint32_t glyph_scale, uint32_t *height);

#ifdef __cplusplus
}
#endif
