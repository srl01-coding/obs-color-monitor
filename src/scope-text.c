#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include "plugin-macros.generated.h"
#include "scope-text.h"
#include "scope-font-atlas.h"

bool cm_scope_labels_external = false;

static gs_effect_t *text_effect = NULL;
static gs_texture_t *text_atlas = NULL;
static bool text_failed = false;

static bool ensure_resources(void)
{
	if (text_effect && text_atlas)
		return true;
	if (text_failed)
		return false;

	char *f = obs_module_file("scope-text.effect");
	char *err = NULL;
	text_effect = gs_effect_create_from_file(f, &err);
	if (!text_effect)
		blog(LOG_ERROR, "scope-text: failed to load '%s': %s", f ? f : "(null)", err ? err : "(no message)");
	bfree(err);
	bfree(f);

	const uint8_t *data = scope_font_atlas;
	text_atlas = gs_texture_create(SCOPE_FONT_ATLAS_W, SCOPE_FONT_ATLAS_H, GS_R8, 1, &data, 0);
	if (!text_atlas)
		blog(LOG_ERROR, "scope-text: failed to create the font atlas texture");

	if (!text_effect || !text_atlas) {
		text_failed = true; // do not retry every frame
		return false;
	}
	return true;
}

void scope_text_free(void)
{
	gs_effect_destroy(text_effect);
	text_effect = NULL;
	gs_texture_destroy(text_atlas);
	text_atlas = NULL;
	text_failed = false;
}

static const struct scope_font_glyph *find_glyph(char c)
{
	for (size_t i = 0; i < sizeof(scope_font_glyphs) / sizeof(*scope_font_glyphs); i++)
		if (scope_font_glyphs[i].ch == c)
			return &scope_font_glyphs[i];
	return NULL;
}

#define SPACE_ADVANCE_EM 0.25f

float scope_text_width(const char *text, float size)
{
	const float em = size / SCOPE_FONT_DIGIT_HEIGHT_EM;
	float w = 0.0f;
	for (const char *c = text; c && *c; c++) {
		const struct scope_font_glyph *g = find_glyph(*c);
		w += (g ? g->advance : SPACE_ADVANCE_EM) * em;
	}
	return w;
}

static void emit_quads(const char *text, float pen, float baseline_y, float em)
{
	const float units_per_texel = em / SCOPE_FONT_EM_PX;
	gs_render_start(true);
	for (const char *c = text; *c; c++) {
		const struct scope_font_glyph *g = find_glyph(*c);
		if (!g) {
			pen += SPACE_ADVANCE_EM * em;
			continue;
		}
		const float x0 = pen + g->x * em;
		const float y0 = baseline_y + g->y * em;
		const float x1 = x0 + g->aw * units_per_texel;
		const float y1 = y0 + g->ah * units_per_texel;
		const float u0 = (float)g->ax / SCOPE_FONT_ATLAS_W;
		const float v0 = (float)g->ay / SCOPE_FONT_ATLAS_H;
		const float u1 = (float)(g->ax + g->aw) / SCOPE_FONT_ATLAS_W;
		const float v1 = (float)(g->ay + g->ah) / SCOPE_FONT_ATLAS_H;

		gs_texcoord(u0, v0, 0);
		gs_vertex2f(x0, y0);
		gs_texcoord(u1, v0, 0);
		gs_vertex2f(x1, y0);
		gs_texcoord(u0, v1, 0);
		gs_vertex2f(x0, y1);

		gs_texcoord(u1, v0, 0);
		gs_vertex2f(x1, y0);
		gs_texcoord(u1, v1, 0);
		gs_vertex2f(x1, y1);
		gs_texcoord(u0, v1, 0);
		gs_vertex2f(x0, y1);

		pen += g->advance * em;
	}
	gs_render_stop(GS_TRIS);
}

void scope_text_draw(const char *text, float x, float baseline_y, float size, uint32_t argb, int align)
{
	if (!text || !*text || !(size > 0.0f))
		return;
	if (!ensure_resources())
		return;

	const float em = size / SCOPE_FONT_DIGIT_HEIGHT_EM;
	float pen = x;
	if (align == SCOPE_TEXT_CENTER)
		pen -= 0.5f * scope_text_width(text, size);
	else if (align == SCOPE_TEXT_RIGHT)
		pen -= scope_text_width(text, size);

	struct vec4 color, halo;
	vec4_from_bgra(&color, argb);
	vec4_from_bgra(&halo, 0xD0000000);
	struct vec2 atlas_size;
	vec2_set(&atlas_size, (float)SCOPE_FONT_ATLAS_W, (float)SCOPE_FONT_ATLAS_H);

	gs_effect_set_texture(gs_effect_get_param_by_name(text_effect, "image"), text_atlas);
	gs_effect_set_vec4(gs_effect_get_param_by_name(text_effect, "color"), &color);
	gs_effect_set_vec4(gs_effect_get_param_by_name(text_effect, "halo_color"), &halo);
	gs_effect_set_float(gs_effect_get_param_by_name(text_effect, "halo_px"), 1.5f);
	gs_effect_set_float(gs_effect_get_param_by_name(text_effect, "spread_px"), SCOPE_FONT_SPREAD_PX);
	gs_effect_set_vec2(gs_effect_get_param_by_name(text_effect, "atlas_size"), &atlas_size);

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	// Halos of all glyphs first, so a neighbour's halo never darkens a glyph's fill.
	while (gs_effect_loop(text_effect, "Halo"))
		emit_quads(text, pen, baseline_y, em);
	while (gs_effect_loop(text_effect, "Fill"))
		emit_quads(text, pen, baseline_y, em);
	gs_blend_state_pop();
}
