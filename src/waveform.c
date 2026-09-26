#include <obs-module.h>
#include <util/platform.h>
#include "plugin-macros.generated.h"
#include <graphics/matrix4.h>
#include "common.h"
#include "util.h"
#include "hdr-scale.h"

#ifdef ENABLE_PROFILE
#define PROFILE_START(x) profile_start(x)
#define PROFILE_END(x) profile_end(x)
static const char *prof_render_name = "wvs_render";
static const char *prof_draw_waveform_name = "draw_waveform";
static const char *prof_draw_name = "draw";
static const char *prof_draw_graticule_name = "graticule";
#else // ENABLE_PROFILE
#define PROFILE_START(x)
#define PROFILE_END(x)
#endif // ! ENABLE_PROFILE

#define WV_SIZE 256

#define DISP_OVERLAY 0
#define DISP_STACK 1
#define DISP_PARADE 2

#define COMP_RGB 0x07
#define COMP_Y 0x20
#define COMP_UV 0x50
#define COMP_YUV (COMP_Y | COMP_UV)

struct wvs_source
{
	struct cm_source cm;

	gs_effect_t *effect;
	gs_texture_t *tex_wv;
	uint32_t tex_wv_width;
	uint32_t tex_wv_levels;
	uint8_t *tex_buf[2];
	uint32_t tex_buf_width[2];
	uint32_t tex_buf_levels[2]; // 256 (8-bit SDR) or 1024 (10-bit HLG)
	bool tex_buf_hlg[2];
	bool tex_buf_full_range[2];
	volatile int w_tex_buf;
	int r_tex_buf;

	gs_vertbuffer_t *graticule_line_vbuf;
	gs_vertbuffer_t *graticule_ref_vbuf; // HLG: black, nominal peak, reference white
	gs_texture_t *label_tex;
	uint32_t label_width;

	int display;
	uint32_t components;
	int intensity;
	int graticule_lines;
	int hdr_scale;
	bool hdr_labels;
	uint32_t graticule_key_prev;
};

static inline uint32_t cur_levels(const struct wvs_source *src)
{
	uint32_t levels = src->tex_buf_levels[src->r_tex_buf];
	return levels ? levels : WV_SIZE;
}

static inline bool cur_hlg(const struct wvs_source *src)
{
	return src->tex_buf_hlg[src->r_tex_buf];
}

static void wvs_update(void *, obs_data_t *);
static void wvs_surface_cb(void *data, struct cm_surface_data *surface_data);

static const char *wvs_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Waveform");
}

static void *wvs_create(obs_data_t *settings, obs_source_t *source)
{
	struct wvs_source *src = bzalloc(sizeof(struct wvs_source));

	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, wvs_surface_cb, src);

	obs_enter_graphics();
	src->effect = create_effect_from_module_file("waveform.effect");
	obs_leave_graphics();

	wvs_update(src, settings);

	return src;
}

static void wvs_destroy(void *data)
{
	struct wvs_source *src = data;

	obs_enter_graphics();
	gs_texture_destroy(src->tex_wv);
	gs_vertexbuffer_destroy(src->graticule_line_vbuf);
	gs_vertexbuffer_destroy(src->graticule_ref_vbuf);
	gs_texture_destroy(src->label_tex);
	obs_leave_graphics();

	cm_destroy(&src->cm);

	bfree(src->tex_buf[0]);
	bfree(src->tex_buf[1]);

	bfree(src);
}

static void wvs_update(void *data, obs_data_t *settings)
{
	struct wvs_source *src = data;
	cm_update(&src->cm, settings);

	src->display = (int)obs_data_get_int(settings, "display");

	src->components = (uint32_t)obs_data_get_int(settings, "components");
	src->cm.flags = (src->components & COMP_RGB ? CM_FLAG_CONVERT_RGB : 0) |
			(src->components & COMP_YUV ? CM_FLAG_CONVERT_YUV : 0);

	src->intensity = (int)obs_data_get_int(settings, "intensity");
	if (src->intensity < 1)
		src->intensity = 1;

	src->graticule_lines = (int)obs_data_get_int(settings, "graticule_lines");
	src->hdr_scale = (int)obs_data_get_int(settings, "hdr_scale");
	src->hdr_labels = obs_data_get_bool(settings, "hdr_labels");
}

static void wvs_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 2);
	obs_data_set_default_int(settings, "intensity", 51);
	obs_data_set_default_int(settings, "components", COMP_RGB);
	obs_data_set_default_int(settings, "graticule_lines", 5);
	obs_data_set_default_int(settings, "hdr_scale", HDR_SCALE_HLG_PERCENT);
	obs_data_set_default_bool(settings, "hdr_labels", true);
}

static bool components_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	uint32_t components = settings ? (uint32_t)obs_data_get_int(settings, "components") : 0;
	obs_property_t *prop = obs_properties_get(props, "colorspace");
	// TODO: temporarily disable colorspace setting if the target is ROI
	bool vis = !!(components & COMP_YUV);
	if (vis && is_roi_source_name(obs_data_get_string(settings, "target_name")))
		vis = false;
	if (prop)
		obs_property_set_visible(prop, vis);
	return true;
}

static obs_properties_t *wvs_get_properties(void *data)
{
	struct wvs_source *src = data;
	obs_properties_t *props;
	obs_property_t *prop;
	props = obs_properties_create();

	cm_get_properties(&src->cm, props);

	prop = obs_properties_add_list(props, "display", obs_module_text("Display"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Overlay"), DISP_OVERLAY);
	obs_property_list_add_int(prop, obs_module_text("Stack"), DISP_STACK);
	obs_property_list_add_int(prop, obs_module_text("Parade"), DISP_PARADE);

	prop = obs_properties_add_list(props, "components", obs_module_text("Components"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_set_modified_callback(prop, components_changed);
	obs_property_list_add_int(prop, obs_module_text("RGB"), COMP_RGB);
	obs_property_list_add_int(prop, obs_module_text("Luma"), COMP_Y);
	obs_property_list_add_int(prop, obs_module_text("Chroma"), COMP_UV);
	obs_property_list_add_int(prop, obs_module_text("YUV"), COMP_YUV);

	// TODO: Disable this property if ROI target is selected.
	properties_add_colorspace(props, "colorspace", obs_module_text("Color space"));

	obs_properties_add_int(props, "intensity", obs_module_text("Intensity"), 1, 255, 1);
	prop = obs_properties_add_list(props, "graticule_lines", obs_module_text("Graticule"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("None"), 0);
	obs_property_list_add_int(prop, obs_module_text("Graticule.Step.100"), 1);
	obs_property_list_add_int(prop, obs_module_text("Graticule.Step.50"), 2);
	obs_property_list_add_int(prop, obs_module_text("Graticule.Step.25"), 4);
	obs_property_list_add_int(prop, obs_module_text("Graticule.Step.20"), 5);
	obs_property_list_add_int(prop, obs_module_text("Graticule.Step.10"), 10);

	properties_add_hdr_scale(props);

	return props;
}

static inline uint32_t n_components(const struct wvs_source *src)
{
	uint32_t c = src->components & (COMP_RGB | COMP_YUV);
	c = c - ((c >> 1) & 0x55);
	c = (c & 0x33) + ((c >> 2) & 0x33);
	c = (c & 0x0F) + ((c >> 4) & 0x0F);
	return c;
}

static uint32_t wvs_get_width(void *data)
{
	struct wvs_source *src = data;
	if (src->cm.bypass)
		return cm_bypass_get_width(&src->cm);
	if (src->display == DISP_PARADE)
		return src->tex_buf_width[src->r_tex_buf] * n_components(src);
	return src->tex_buf_width[src->r_tex_buf];
}

static uint32_t wvs_get_height(void *data)
{
	struct wvs_source *src = data;
	if (src->cm.bypass)
		return cm_bypass_get_height(&src->cm);
	if (src->display == DISP_STACK)
		return cur_levels(src) * n_components(src);
	return cur_levels(src);
}

static inline void inc_uint8(uint8_t *c)
{
	if (*c < 255)
		++*c;
}

static inline void ensure_tex_buf_size(struct wvs_source *src, const uint32_t width, const uint32_t levels, int ix)
{
	if (src->tex_buf[ix] && src->tex_buf_width[ix] == width && src->tex_buf_levels[ix] == levels)
		return;

	if (!width)
		return;

	bfree(src->tex_buf[ix]);
	src->tex_buf[ix] = bzalloc(width * levels * 4);
	src->tex_buf_width[ix] = width;
	src->tex_buf_levels[ix] = levels;
}

static inline void wvs_draw_waveform_10bit(struct wvs_source *src, uint8_t *dbuf,
					   const struct cm_surface_data *surface_data, const uint8_t *video_data)
{
	const uint32_t height = surface_data->height;
	const uint32_t width = surface_data->width;
	const uint32_t top = surface_data->levels - 1;

	const bool calc_b = (src->components & 0x11) ? true : false;
	const bool calc_g = (src->components & 0x22) ? true : false;
	const bool calc_r = (src->components & 0x44) ? true : false;

	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *v = video_data + surface_data->linesize * y;
		for (uint32_t x = 0; x < width; x++, v += 4) {
			uint32_t r, g, b, a;
			cm_unpack_r10g10b10a2(v, &r, &g, &b, &a);
			if (!a)
				continue;
			if (calc_b)
				inc_uint8(dbuf + x * 4 + (top - b) * width * 4 + 0);
			if (calc_g)
				inc_uint8(dbuf + x * 4 + (top - g) * width * 4 + 1);
			if (calc_r)
				inc_uint8(dbuf + x * 4 + (top - r) * width * 4 + 2);
		}
	}
}

static inline void wvs_draw_waveform(struct wvs_source *src, uint8_t *dbuf, const struct cm_surface_data *surface_data)
{
	const uint32_t height = surface_data->height;
	const uint32_t width = surface_data->width;

	for (uint32_t i = 0; i < width * surface_data->levels * 4; i++)
		dbuf[i] = 0;

	const uint8_t *video_data = NULL;
	if (src->components & COMP_RGB)
		video_data = surface_data->rgb_data;
	else if (src->components & COMP_YUV)
		video_data = surface_data->yuv_data;
	if (!video_data)
		return;

	if (surface_data->hlg) {
		wvs_draw_waveform_10bit(src, dbuf, surface_data, video_data);
		return;
	}

	const bool calc_b = (src->components & 0x11) ? true : false;
	const bool calc_g = (src->components & 0x22) ? true : false;
	const bool calc_r = (src->components & 0x44) ? true : false;

	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *v = video_data + surface_data->linesize * y;
		for (uint32_t x = 0; x < width; x++) {
			const uint8_t b = *v++;
			const uint8_t g = *v++;
			const uint8_t r = *v++;
			const uint8_t a = *v++;
			if (!a)
				continue;
			if (calc_b)
				inc_uint8(dbuf + x * 4 + (WV_SIZE - 1 - b) * width * 4 + 0);
			if (calc_g)
				inc_uint8(dbuf + x * 4 + (WV_SIZE - 1 - g) * width * 4 + 1);
			if (calc_r)
				inc_uint8(dbuf + x * 4 + (WV_SIZE - 1 - r) * width * 4 + 2);
		}
	}
}

static void wvs_set_image(struct wvs_source *src, const uint8_t *tex_buf, uint32_t width, uint32_t levels)
{
	if (src->tex_wv && src->tex_wv_width == width && src->tex_wv_levels == levels) {
		gs_texture_set_image(src->tex_wv, tex_buf, width * 4, false);
		return;
	}

	if (src->tex_wv)
		gs_texture_destroy(src->tex_wv);
	src->tex_wv = gs_texture_create(width, levels, GS_BGRX, 1, &tex_buf, GS_DYNAMIC);
	src->tex_wv_width = width;
	src->tex_wv_levels = levels;
}

static void wvs_surface_cb(void *data, struct cm_surface_data *surface_data)
{
	struct wvs_source *src = data;

	if ((src->components & COMP_RGB) && !surface_data->rgb_data)
		return;
	if ((src->components & COMP_YUV) && !surface_data->yuv_data)
		return;
	if (!surface_data->width)
		return;

	const uint32_t levels = surface_data->levels ? surface_data->levels : WV_SIZE;
	ensure_tex_buf_size(src, surface_data->width, levels, src->w_tex_buf);

	PROFILE_START(prof_draw_waveform_name);
	wvs_draw_waveform(src, src->tex_buf[src->w_tex_buf], surface_data);
	PROFILE_END(prof_draw_waveform_name);
	src->tex_buf_hlg[src->w_tex_buf] = surface_data->hlg;
	src->tex_buf_full_range[src->w_tex_buf] = surface_data->full_range;
	src->w_tex_buf ^= 1;
}

static void create_graticule_hlg(struct wvs_source *src)
{
	const uint32_t levels = cur_levels(src);
	struct hdr_scale_mark marks[HDR_SCALE_MAX_MARKS];
	const int n =
		hdr_scale_marks(marks, src->hdr_scale, src->graticule_lines, src->tex_buf_full_range[src->r_tex_buf]);

	for (int pass = 0; pass < 2; pass++) {
		const bool ref = pass == 1;
		int count = 0;
		for (int i = 0; i < n; i++)
			count += marks[i].ref == ref;
		if (!count)
			continue;
		gs_render_start(true);
		for (int i = 0; i < n; i++) {
			if (marks[i].ref != ref)
				continue;
			const float y = (float)(levels - 1) - marks[i].code + 0.5f;
			gs_vertex2f(0.0f, y);
			gs_vertex2f(1.0f, y);
		}
		if (ref)
			src->graticule_ref_vbuf = gs_render_save();
		else
			src->graticule_line_vbuf = gs_render_save();
	}

	if (src->hdr_labels && n > 0) {
		uint32_t w = 0;
		uint8_t *img = hdr_scale_label_image_vertical(marks, n, levels, levels / 256, &w);
		const uint8_t *data = img;
		src->label_tex = gs_texture_create(w, levels, GS_RGBA, 1, &data, 0);
		src->label_width = w;
		bfree(img);
	}
}

static void create_graticule_vbuf(struct wvs_source *src)
{
	obs_enter_graphics();
	gs_vertexbuffer_destroy(src->graticule_line_vbuf);
	src->graticule_line_vbuf = NULL;
	gs_vertexbuffer_destroy(src->graticule_ref_vbuf);
	src->graticule_ref_vbuf = NULL;
	gs_texture_destroy(src->label_tex);
	src->label_tex = NULL;
	if (cur_hlg(src)) {
		create_graticule_hlg(src);
	} else if (src->graticule_lines > 0) {
		gs_render_start(true);
		for (int i = 0; i <= src->graticule_lines; i++) {
			gs_vertex2f(0.0f, 256.0f * i / src->graticule_lines);
			gs_vertex2f(1.0f, 256.0f * i / src->graticule_lines);
		}
		src->graticule_line_vbuf = gs_render_save();
	}
	obs_leave_graphics();
}

static void wvs_render_graticule_hlg(struct wvs_source *src)
{
	const bool stack = src->display == DISP_STACK;
	const bool parade = src->display == DISP_PARADE;
	const int n_stack = stack ? n_components(src) : 1;
	const uint32_t levels = cur_levels(src);
	const float xcoe = (float)(src->tex_buf_width[src->r_tex_buf] * (parade ? n_components(src) : 1));

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	for (int pass = 0; pass < 2; pass++) {
		gs_vertbuffer_t *vbuf = pass ? src->graticule_ref_vbuf : src->graticule_line_vbuf;
		if (!vbuf)
			continue;
		gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"),
				    pass ? 0xC040E0FF /* cyan */ : 0x80FFBF00 /* amber */);
		while (gs_effect_loop(effect, "Solid")) {
			for (int i = 0; i < n_stack; i++) {
				struct matrix4 tr = {
					{.ptr = {xcoe, 0.0f, 0.0f, 0.0f}},
					{.ptr = {0.0f, 1.0f, 0.0f, 0.0f}},
					{.ptr = {0.0f, 0.0f, 1.0f, 0.0f}},
					{.ptr = {0.0f, (float)(levels * i), 0.0f, 1.0f}},
				};
				gs_matrix_push();
				gs_matrix_mul(&tr);
				gs_load_vertexbuffer(vbuf);
				gs_draw(GS_LINES, 0, 0);
				gs_matrix_pop();
			}
		}
	}

	if (src->label_tex) {
		for (int i = 0; i < n_stack; i++)
			draw_texture_blended(src->label_tex, 0.0f, (float)(levels * i));
	}
}

static void wvs_render_graticule(struct wvs_source *src)
{
	if (cur_hlg(src)) {
		wvs_render_graticule_hlg(src);
		return;
	}

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), 0x80FFBF00); // amber
	while (gs_effect_loop(effect, "Solid")) {
		bool stack = src->display == DISP_STACK;
		bool parade = src->display == DISP_PARADE;
		int n_stack = stack ? n_components(src) : 1;
		for (int i = 0; i < n_stack; i++) {
			const float yoff = stack ? cur_levels(src) * i + 0.5f : 0.0f;
			const float xcoe =
				(float)(src->tex_buf_width[src->r_tex_buf] * (parade ? n_components(src) : 1));
			struct matrix4 tr = {
				{.ptr = {xcoe, 0.0f, 0.0f, 0.0f}},
				{.ptr = {0.0f, 1.0f, 0.0f, 0.0f}},
				{.ptr = {0.0f, 0.0f, 1.0f, 0.0f}},
				{.ptr = {0.0f, yoff, 0.0f, 1.0f}},
			};
			gs_matrix_push();
			gs_matrix_mul(&tr);
			gs_load_vertexbuffer(src->graticule_line_vbuf);
			gs_draw(GS_LINES, stack && i ? 2 : 0, 0);
			gs_matrix_pop();
		}
	}
}

static void render_waveform(struct wvs_source *src)
{
	gs_effect_t *effect = src->effect ? src->effect : obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), src->tex_wv);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "intensity"), (float)src->intensity);
	const char *name = "Draw";
	int w = src->tex_wv_width;
	int h = src->tex_wv_levels;
	int n = n_components(src);
	if (src->effect)
		switch (src->display) {
		case DISP_STACK:
			name = n == 3 ? "DrawStack" : n == 2 ? "DrawStackUV" : "DrawOverlay";
			h *= n;
			break;
		case DISP_PARADE:
			name = n == 3 ? "DrawParade" : n == 2 ? "DrawParadeUV" : "DrawOverlay";
			w *= n;
			break;
		default:
			name = "DrawOverlay";
			break;
		}

	while (gs_effect_loop(effect, name))
		gs_draw_sprite(src->tex_wv, 0, w, h);
}

static void wvs_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct wvs_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}
	PROFILE_START(prof_render_name);

	cm_render_target(&src->cm);

	PROFILE_START(prof_draw_name);
	if (src->tex_buf[src->r_tex_buf]) {
		wvs_set_image(src, src->tex_buf[src->r_tex_buf], src->tex_buf_width[src->r_tex_buf], cur_levels(src));
		render_waveform(src);
	}
	PROFILE_END(prof_draw_name);

	PROFILE_START(prof_draw_graticule_name);
	if (src->graticule_lines > 0) {
		// Rebuild when anything affecting the graticule changes.
		uint32_t key = (uint32_t)src->graticule_lines;
		if (cur_hlg(src))
			key |= 0x100u | ((uint32_t)src->hdr_scale << 9) | ((uint32_t)src->hdr_labels << 10) |
			       ((uint32_t)src->tex_buf_full_range[src->r_tex_buf] << 11) | (cur_levels(src) << 12);
		if (key != src->graticule_key_prev) {
			create_graticule_vbuf(src);
			src->graticule_key_prev = key;
		}
		wvs_render_graticule(src);
	}
	PROFILE_END(prof_draw_graticule_name);

	PROFILE_END(prof_render_name);
}

static void wvs_tick(void *data, float second)
{
	struct wvs_source *src = data;
	cm_tick(data, second);

	src->r_tex_buf = src->w_tex_buf ^ 1;
}

const struct obs_source_info colormonitor_waveform = {
	.id = "waveform_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = wvs_get_name,
	.create = wvs_create,
	.destroy = wvs_destroy,
	.update = wvs_update,
	.get_defaults = wvs_get_defaults,
	.get_properties = wvs_get_properties,
	.get_width = wvs_get_width,
	.get_height = wvs_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = wvs_render,
	.video_tick = wvs_tick,
};
