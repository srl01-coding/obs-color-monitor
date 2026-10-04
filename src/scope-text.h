#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCOPE_TEXT_LEFT 0
#define SCOPE_TEXT_CENTER 1
#define SCOPE_TEXT_RIGHT 2

// Anti-aliased label text (SDF atlas, data/scope-text.effect). Coordinates are in
// the current drawing units with y growing downward; `size` is the height of a digit
// in the same units. Colours are 0xAARRGGBB, as gs_effect_set_color.
float scope_text_width(const char *text, float size);
void scope_text_draw(const char *text, float x, float baseline_y, float size, uint32_t argb, int align);

// Release the effect and atlas (graphics thread / inside obs_enter_graphics).
void scope_text_free(void);

// Set by the scope dock while it renders a waveform / histogram source: the dock
// draws the labels itself at a fixed on-screen size, so the source skips its own.
extern bool cm_scope_labels_external;

// Called by the scope dock right after rendering the source with
// gs_ortho(0, w_src, -1, h_src) in a viewport of w x h pixels; draws the HLG scale
// labels in pixel space. `ui_scale` is the device pixel ratio.
void wvs_draw_overlay_labels(void *source, int w, int h, float ui_scale);
void his_draw_overlay_labels(void *source, int w, int h, float ui_scale);

#ifdef __cplusplus
}
#endif
