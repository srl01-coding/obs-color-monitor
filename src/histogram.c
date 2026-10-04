#include <obs-module.h>
#include <util/platform.h>
#include "plugin-macros.generated.h"
#include <graphics/matrix4.h>
#include "common.h"
#include "util.h"
#include "hdr-scale.h"
#include "scope-text.h"
#include <string.h>

#define MAX(x, y) ((x) > (y) ? (x) : (y))

#ifdef ENABLE_PROFILE
#define PROFILE_START(x) profile_start(x)
#define PROFILE_END(x) profile_end(x)
static const char *prof_render_name = "his_render";
static const char *prof_draw_histogram_name = "draw_histogram";
static const char *prof_draw_name = "draw";
#else // ENABLE_PROFILE
#define PROFILE_START(x)
#define PROFILE_END(x)
#endif // ! ENABLE_PROFILE

#define HI_SIZE 256
#define HI_SIZE_MAX 1024 // 10-bit HLG

#define DISP_OVERLAY 0
#define DISP_STACK 1
#define DISP_PARADE 2

#define COMP_RGB 0x07
#define COMP_Y 0x20
#define COMP_UV 0x50
#define COMP_YUV (COMP_Y | COMP_UV)

#define LEVEL_MODE_NONE 0
#define LEVEL_MODE_PIXEL 1
#define LEVEL_MODE_RATIO 2

#define GRATICULE_H_MAX 64

struct his_source
{
	struct cm_source cm;

	gs_effect_t *effect;
	gs_texture_t *tex_hi;
	struct vec3 vec_hi_max;
	uint32_t tex_hi_levels;
	uint8_t *tex_buf[2];
	uint32_t hi_max[2][3];
	uint32_t tex_buf_levels[2]; // display columns: 256 (SDR), hdr_cols (HLG)
	bool tex_buf_hlg[2];
	bool tex_buf_full_range[2];
	volatile int w_tex_buf;

	gs_vertbuffer_t *graticule_line_vbuf;
	gs_vertbuffer_t *graticule_ref_vbuf;              // HLG: black, nominal peak, reference white
	struct hdr_scale_mark marks[HDR_SCALE_MAX_MARKS]; // HLG scale, for the labels
	int n_marks;
	int hdr_scale;
	bool hdr_labels;
	uint32_t hdr_cols; // HLG: 256, 512 or 1024 bins; 10-bit codes are binned 4, 2 or 1 per bin
	uint32_t graticule_key_prev;

	int display;
	uint32_t components;
	int level_height;
	int level_fixed_value;
	int level_ratio_value;
	bool logscale;
	int graticule_vertical_lines;
	float graticule_horizontal_step;
	bool graticule_need_update;
};

static void his_update(void *, obs_data_t *);

static inline uint32_t cur_levels(const struct his_source *src)
{
	uint32_t levels = src->tex_buf_levels[src->w_tex_buf ^ 1];
	return levels ? levels : HI_SIZE;
}

static inline bool cur_hlg(const struct his_source *src)
{
	return src->tex_buf_hlg[src->w_tex_buf ^ 1];
}
static void his_surface_cb(void *data, struct cm_surface_data *surface_data);

static const char *his_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Histogram");
}

static void *his_create(obs_data_t *settings, obs_source_t *source)
{
	struct his_source *src = bzalloc(sizeof(struct his_source));

	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, his_surface_cb, src);
	obs_enter_graphics();
	src->effect = create_effect_from_module_file("histogram.effect");
	obs_leave_graphics();

	his_update(src, settings);

	return src;
}

static void his_destroy(void *data)
{
	struct his_source *src = data;

	obs_enter_graphics();
	gs_texture_destroy(src->tex_hi);
	gs_vertexbuffer_destroy(src->graticule_line_vbuf);
	gs_vertexbuffer_destroy(src->graticule_ref_vbuf);
	obs_leave_graphics();

	cm_destroy(&src->cm);

	bfree(src->tex_buf[0]);
	bfree(src->tex_buf[1]);

	bfree(src);
}

static void his_update(void *data, obs_data_t *settings)
{
#define UPDATE_PROP(type, variable, value, update) \
	do {                                       \
		type x = (value);                  \
		if (x != (variable)) {             \
			(variable) = x;            \
			(update) = true;           \
		}                                  \
	} while (0)

	struct his_source *src = data;
	cm_update(&src->cm, settings);

	src->display = (int)obs_data_get_int(settings, "display");

	src->components = (uint32_t)obs_data_get_int(settings, "components");
	src->cm.flags = (src->components & COMP_RGB ? CM_FLAG_CONVERT_RGB : 0) |
			(src->components & COMP_YUV ? CM_FLAG_CONVERT_YUV : 0);

	src->level_height = (int)obs_data_get_int(settings, "level_height");

	bool logscale = obs_data_get_bool(settings, "logscale");
	if (logscale != src->logscale) {
		src->logscale = logscale;
		src->graticule_need_update = true;
	}

	int level_mode = (int)obs_data_get_int(settings, "level_mode");
	switch (level_mode) {
	case LEVEL_MODE_NONE:
		UPDATE_PROP(int, src->level_ratio_value, 0, src->graticule_need_update);
		UPDATE_PROP(int, src->level_fixed_value, 0, src->graticule_need_update);
		break;
	case LEVEL_MODE_PIXEL:
		UPDATE_PROP(int, src->level_fixed_value, (int)obs_data_get_int(settings, "level_fixed_value"),
			    src->graticule_need_update);
		UPDATE_PROP(float, src->graticule_horizontal_step,
			    (float)obs_data_get_double(settings, "graticule_horizontal_step_fixed"),
			    src->graticule_need_update);
		src->level_ratio_value = 0;
		break;
	case LEVEL_MODE_RATIO:
		UPDATE_PROP(int, src->level_ratio_value,
			    (int)(obs_data_get_double(settings, "level_ratio_value") * 10.0 + 0.5),
			    src->graticule_need_update);
		UPDATE_PROP(float, src->graticule_horizontal_step,
			    (float)obs_data_get_double(settings, "graticule_horizontal_step_ratio"),
			    src->graticule_need_update);
		src->level_fixed_value = 0;
		break;
	default:
		blog(LOG_ERROR, "histogram '%s': Invalid level_mode %d", obs_source_get_name(src->cm.self), level_mode);
	}

	UPDATE_PROP(int, src->graticule_vertical_lines, (int)obs_data_get_int(settings, "graticule_vertical_lines"),
		    src->graticule_need_update);
	UPDATE_PROP(int, src->hdr_scale, (int)obs_data_get_int(settings, "hdr_scale"), src->graticule_need_update);
	UPDATE_PROP(bool, src->hdr_labels, obs_data_get_bool(settings, "hdr_labels"), src->graticule_need_update);
	src->hdr_cols = hdr_rows_sanitize((int)obs_data_get_int(settings, "hdr_cols"));

#undef UPDATE_PROP
}

static void his_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 2);
	obs_data_set_default_int(settings, "components", COMP_RGB);
	obs_data_set_default_int(settings, "level_height", 200);
	obs_data_set_default_int(settings, "graticule_vertical_lines", 5);
	obs_data_set_default_int(settings, "level_fixed_value", 1000);
	obs_data_set_default_double(settings, "level_ratio_value", 10.0);
	obs_data_set_default_int(settings, "hdr_scale", HDR_SCALE_HLG_PERCENT);
	obs_data_set_default_bool(settings, "hdr_labels", true);
	obs_data_set_default_int(settings, "hdr_cols", 256);
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

static void graticule_horizontal_combo_init(obs_property_t *prop, float val_min, float val_max, const char *suffix)
{
	float div = 1.0f;
	while (val_min * div < 1.0f)
		div *= 10;

	obs_property_list_add_float(prop, obs_module_text("None"), -1.0f);

	for (float ten = 1.0f; ten / div <= val_max; ten *= 10.0f) {
		const float ff[] = {1.0f, 2.0f, 5.0f};
		for (size_t i = 0; i < sizeof(ff) / sizeof(*ff); i++) {
			float v = ff[i] * ten / div;
			if (v < val_min)
				continue;
			if (v > val_max)
				break;
			char name[64];
			snprintf(name, sizeof(name) - 1, "%g%s", v, suffix);
			name[sizeof(name) - 1] = 0;
			obs_property_list_add_float(prop, name, v);
		}
	}
}

static bool level_mode_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	obs_property_t *prop;
	int level_mode = (int)obs_data_get_int(settings, "level_mode");

	prop = obs_properties_get(props, "level_fixed_value");
	obs_property_set_visible(prop, level_mode == LEVEL_MODE_PIXEL);

	prop = obs_properties_get(props, "level_ratio_value");
	obs_property_set_visible(prop, level_mode == LEVEL_MODE_RATIO);

	prop = obs_properties_get(props, "graticule_horizontal_step_fixed");
	obs_property_set_visible(prop, level_mode == LEVEL_MODE_PIXEL);

	prop = obs_properties_get(props, "graticule_horizontal_step_ratio");
	obs_property_set_visible(prop, level_mode == LEVEL_MODE_RATIO);

	return true;
}

static obs_properties_t *his_get_properties(void *data)
{
	struct his_source *src = data;
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

	obs_properties_add_int(props, "level_height", obs_module_text("Height"), 50, 2048, 1);
	obs_properties_add_bool(props, "logscale", obs_module_text("Log scale"));

	prop = obs_properties_add_list(props, "level_mode", obs_module_text("Level mode"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Auto"), LEVEL_MODE_NONE);
	obs_property_list_add_int(prop, obs_module_text("Pixels"), LEVEL_MODE_PIXEL);
	obs_property_list_add_int(prop, obs_module_text("Ratio"), LEVEL_MODE_RATIO);
	obs_property_set_modified_callback(prop, level_mode_modified);

	prop = obs_properties_add_int(props, "level_fixed_value", obs_module_text("Top level"), 50, 65535, 1);
	obs_property_int_set_suffix(prop, " px");
	prop = obs_properties_add_float(props, "level_ratio_value", obs_module_text("Top level"), 1.0, 100.0, 0.1);
	obs_property_float_set_suffix(prop, "%");

	prop = obs_properties_add_list(props, "graticule_vertical_lines", obs_module_text("Histogram.Graticule.V"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("None"), 0);
	obs_property_list_add_int(prop, obs_module_text("Graticule.Step.100"), 1);
	obs_property_list_add_int(prop, obs_module_text("Graticule.Step.50"), 2);
	obs_property_list_add_int(prop, obs_module_text("Graticule.Step.25"), 4);
	obs_property_list_add_int(prop, obs_module_text("Graticule.Step.20"), 5);
	obs_property_list_add_int(prop, obs_module_text("Graticule.Step.10"), 10);

	prop = obs_properties_add_list(props, "graticule_horizontal_step_fixed",
				       obs_module_text("Histogram.Graticule.H"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_FLOAT);
	graticule_horizontal_combo_init(prop, 50.0f / GRATICULE_H_MAX, 32768.f, " px");
	prop = obs_properties_add_list(props, "graticule_horizontal_step_ratio",
				       obs_module_text("Histogram.Graticule.H"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_FLOAT);
	graticule_horizontal_combo_init(prop, 1.0f / GRATICULE_H_MAX, 50.0f, "%");

	properties_add_hdr_scale(props);
	properties_add_hdr_resolution(props, "hdr_cols", obs_module_text("HDR.Resolution.Histogram"));

	return props;
}

static inline uint32_t n_components(const struct his_source *src)
{
	uint32_t c = src->components & (COMP_RGB | COMP_YUV);
	c = c - ((c >> 1) & 0x55);
	c = (c & 0x33) + ((c >> 2) & 0x33);
	c = (c & 0x0F) + ((c >> 4) & 0x0F);
	return c;
}

static uint32_t his_get_width(void *data)
{
	struct his_source *src = data;
	if (src->cm.bypass)
		return cm_bypass_get_width(&src->cm);
	if (src->display == DISP_PARADE)
		return cur_levels(src) * n_components(src);
	return cur_levels(src);
}

static uint32_t his_get_height(void *data)
{
	struct his_source *src = data;
	if (src->cm.bypass)
		return cm_bypass_get_height(&src->cm);
	if (src->display == DISP_STACK)
		return src->level_height * n_components(src);
	return src->level_height;
}

static inline void inc_uint16(uint16_t *c)
{
	if (*c < 65535)
		++*c;
}

static inline void his_calculate_max(struct his_source *src, uint32_t *hi_max, const uint32_t *dbuf,
				     const uint32_t levels)
{
	const bool calc_b = (src->components & 0x11) ? true : false;
	const bool calc_g = (src->components & 0x22) ? true : false;
	const bool calc_r = (src->components & 0x44) ? true : false;

	hi_max[0] = 1;
	hi_max[1] = 1;
	hi_max[2] = 1;
	for (uint32_t i = 0; i < levels; i++) {
		if (calc_r && dbuf[i * 4 + 0] > hi_max[0])
			hi_max[0] = dbuf[i * 4 + 0];
		if (calc_g && dbuf[i * 4 + 1] > hi_max[1])
			hi_max[1] = dbuf[i * 4 + 1];
		if (calc_b && dbuf[i * 4 + 2] > hi_max[2])
			hi_max[2] = dbuf[i * 4 + 2];
	}
}

static inline void his_fix_max_level(uint32_t *hi_max, uint32_t x)
{
	uint32_t v = x == 0 ? 1 : x;
	hi_max[0] = v;
	hi_max[1] = v;
	hi_max[2] = v;
}

static inline void his_draw_histogram(struct his_source *src, uint8_t *tex_buf, uint32_t *hi_max,
				      const struct cm_surface_data *surface_data)
{
	const uint32_t height = surface_data->height;
	const uint32_t width = surface_data->width;
	const uint32_t levels = surface_data->hlg ? src->hdr_cols : HI_SIZE;
	const uint32_t shift = hdr_rows_shift(levels); // 10-bit code -> bin

	uint32_t *dbuf = (uint32_t *)tex_buf;
	for (uint32_t i = 0; i < levels * 4; i++)
		dbuf[i] = 0;

	const uint8_t *video_data = NULL;
	if (src->components & COMP_RGB)
		video_data = surface_data->rgb_data;
	else if (src->components & COMP_YUV)
		video_data = surface_data->yuv_data;
	if (!video_data)
		return;

	const bool calc_b = (src->components & 0x11) ? true : false;
	const bool calc_g = (src->components & 0x22) ? true : false;
	const bool calc_r = (src->components & 0x44) ? true : false;

	for (uint32_t y = 0; y < height && surface_data->hlg; y++) {
		const uint8_t *v = video_data + surface_data->linesize * y;
		for (uint32_t x = 0; x < width; x++, v += 4) {
			uint32_t r, g, b, a;
			cm_unpack_r10g10b10a2(v, &r, &g, &b, &a);
			if (!a)
				continue;
			if (calc_r)
				dbuf[(r >> shift) * 4 + 0]++;
			if (calc_g)
				dbuf[(g >> shift) * 4 + 1]++;
			if (calc_b)
				dbuf[(b >> shift) * 4 + 2]++;
		}
	}

	for (uint32_t y = 0; y < height && !surface_data->hlg; y++) {
		const uint8_t *v = video_data + surface_data->linesize * y;
		for (uint32_t x = 0; x < width; x++) {
			const uint8_t b = *v++;
			const uint8_t g = *v++;
			const uint8_t r = *v++;
			const uint8_t a = *v++;
			if (!a)
				continue;
			if (calc_r)
				dbuf[r * 4 + 0]++;
			if (calc_g)
				dbuf[g * 4 + 1]++;
			if (calc_b)
				dbuf[b * 4 + 2]++;
		}
	}

	if (src->level_fixed_value > 0)
		his_fix_max_level(hi_max, src->level_fixed_value);
	else if (src->level_ratio_value > 0)
		his_fix_max_level(hi_max, (uint64_t)width * height * src->level_ratio_value / 1000);
	else
		his_calculate_max(src, hi_max, dbuf, levels);

	float *flt = (float *)tex_buf;
	if (src->logscale) {
		for (int j = 0, mask = 0x44; j < 3; j++, mask >>= 1) {
			if (!(src->components & mask))
				continue;
			const float s = 1.0f / logf((float)(hi_max[j] + 1));
			for (uint32_t i = 0; i < levels; i++)
				flt[i * 4 + j] = dbuf[i * 4 + j] ? logf((float)(dbuf[i * 4 + j] + 1)) * s : 0;
			hi_max[j] = 1;
		}
	} else {
		for (uint32_t i = 0; i < levels * 4; i++)
			flt[i] = (float)dbuf[i];
	}
}

static void his_set_image(struct his_source *src, const uint8_t *tex_buf, uint32_t *hi_max, uint32_t levels)
{
	if (src->tex_hi && src->tex_hi_levels != levels) {
		gs_texture_destroy(src->tex_hi);
		src->tex_hi = NULL;
	}

	if (!src->tex_hi) {
		src->tex_hi = gs_texture_create(levels, 1, GS_RGBA32F, 1, &tex_buf, GS_DYNAMIC);
		src->tex_hi_levels = levels;
	} else {
		gs_texture_set_image(src->tex_hi, tex_buf, sizeof(float) * levels * 4, false);
	}

	for (int i = 0; i < 3; i++)
		src->vec_hi_max.ptr[i] = (float)hi_max[i];
}

static void his_surface_cb(void *data, struct cm_surface_data *surface_data)
{
	struct his_source *src = data;

	if ((src->components & COMP_RGB) && !surface_data->rgb_data)
		return;
	if ((src->components & COMP_YUV) && !surface_data->yuv_data)
		return;
	if (!surface_data->width)
		return;

	if (!surface_data->levels || surface_data->levels > HI_SIZE_MAX)
		return;

	if (!src->tex_buf[src->w_tex_buf])
		src->tex_buf[src->w_tex_buf] = bzalloc(MAX(sizeof(uint32_t), sizeof(float)) * HI_SIZE_MAX * 4);

	PROFILE_START(prof_draw_histogram_name);
	his_draw_histogram(src, src->tex_buf[src->w_tex_buf], src->hi_max[src->w_tex_buf], surface_data);
	PROFILE_END(prof_draw_histogram_name);
	src->tex_buf_levels[src->w_tex_buf] = surface_data->hlg ? src->hdr_cols : HI_SIZE;
	src->tex_buf_hlg[src->w_tex_buf] = surface_data->hlg;
	src->tex_buf_full_range[src->w_tex_buf] = surface_data->full_range;
	src->w_tex_buf ^= 1;
}

static void create_graticule_vbuf(struct his_source *src)
{
	float y_max = 0;
	if (src->logscale)
		y_max = 0;
	else if (src->level_fixed_value)
		y_max = (float)src->level_fixed_value;
	else if (src->level_ratio_value)
		y_max = src->level_ratio_value / 10.f;
	float y_step = y_max > 0 ? src->graticule_horizontal_step / y_max : 0.0f;

	bool has_graticule_vertical = src->graticule_vertical_lines > 0;
	bool has_graticule_horizontal = y_step > 1.0f / GRATICULE_H_MAX;

	gs_vertexbuffer_destroy(src->graticule_line_vbuf);
	src->graticule_line_vbuf = NULL;
	gs_vertexbuffer_destroy(src->graticule_ref_vbuf);
	src->graticule_ref_vbuf = NULL;
	src->n_marks = 0;

	const bool hlg = cur_hlg(src);
	const uint32_t levels = cur_levels(src);
	struct hdr_scale_mark marks[HDR_SCALE_MAX_MARKS];
	int n_marks = 0;
	if (hlg) {
		n_marks = hdr_scale_marks(marks, src->hdr_scale, src->graticule_vertical_lines,
					  src->tex_buf_full_range[src->w_tex_buf ^ 1]);
		has_graticule_vertical = false;
		for (int i = 0; i < n_marks; i++)
			has_graticule_vertical |= !marks[i].ref;

		bool has_ref = false;
		for (int i = 0; i < n_marks; i++)
			has_ref |= marks[i].ref;
		if (has_ref) {
			gs_render_start(true);
			for (int i = 0; i < n_marks; i++) {
				if (!marks[i].ref)
					continue;
				gs_vertex2f(hdr_scale_code_to_px(marks[i].code, levels), 0.0f);
				gs_vertex2f(hdr_scale_code_to_px(marks[i].code, levels), 1.0f);
			}
			src->graticule_ref_vbuf = gs_render_save();
		}

		memcpy(src->marks, marks, sizeof(marks));
		src->n_marks = n_marks;
	}

	if (!has_graticule_vertical && !has_graticule_horizontal)
		return;

	gs_render_start(true);

	if (hlg) {
		for (int i = 0; i < n_marks; i++) {
			if (marks[i].ref)
				continue;
			gs_vertex2f(hdr_scale_code_to_px(marks[i].code, levels), 0.0f);
			gs_vertex2f(hdr_scale_code_to_px(marks[i].code, levels), 1.0f);
		}
	} else if (has_graticule_vertical) {
		const int n = src->graticule_vertical_lines;
		for (int i = 0; i <= n; i++) {
			gs_vertex2f(256.0f * i / n, 0.0f);
			gs_vertex2f(256.0f * i / n, 1.0f);
		}
	}

	if (has_graticule_horizontal) {
		for (float y = 1.0f; y >= 0.0f; y -= y_step) {
			gs_vertex2f(0.0f, y);
			gs_vertex2f((float)levels, y);
		}
	}

	src->graticule_line_vbuf = gs_render_save();
}

static void his_render_graticule_vbuf(struct his_source *src, gs_vertbuffer_t *vbuf, uint32_t color, bool hlg)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), color);
	const uint32_t levels = cur_levels(src);
	while (gs_effect_loop(effect, "Solid")) {
		bool stack = src->display == DISP_STACK;
		bool parade = src->display == DISP_PARADE;
		int n_parade = parade ? n_components(src) : 1;
		int n_stack = stack ? n_components(src) : 1;
		for (int j = 0; j < n_stack; j++) {
			for (int i = 0; i < n_parade; i++) {
				const float ycoe = (float)(src->level_height * 1);
				const float yoff = (float)(src->level_height * j);
				const float xoff = parade ? (float)(levels * i) : hlg ? 0.0f : 1.0f;
				struct matrix4 tr = {
					{.ptr = {1.0f, 0.0f, 0.0f, 0.0f}},
					{.ptr = {0.0f, ycoe, 0.0f, 0.0f}},
					{.ptr = {0.0f, 0.0f, 1.0f, 0.0f}},
					{.ptr = {xoff, yoff, 0.0f, 1.0f}},
				};
				gs_matrix_push();
				gs_matrix_mul(&tr);
				gs_load_vertexbuffer(vbuf);
				gs_draw(GS_LINES, parade && i && !hlg ? 2 : 0, 0);
				gs_matrix_pop();
			}
		}
	}
}

#define LABEL_COLOR 0xF0FFBF00     // amber, as the graticule
#define LABEL_REF_COLOR 0xFF40E0FF // cyan, as the reference lines

// Draw the HLG scale labels at the bottom of each panel. Drawing units:
// x_draw = x_source * sx, y_draw = (y_source + oy) * sy.
static void his_draw_labels(struct his_source *src, float sx, float sy, float oy, float size, float margin)
{
	if (!src->hdr_labels || src->n_marks <= 0)
		return;
	const bool stack = src->display == DISP_STACK;
	const bool parade = src->display == DISP_PARADE;
	const int n_parade = parade ? (int)n_components(src) : 1;
	const int n_stack = stack ? (int)n_components(src) : 1;
	const uint32_t levels = cur_levels(src);
	if ((float)src->level_height * sy < 3.0f * size)
		return; // panel too short for labels
	float line_x[HDR_SCALE_MAX_MARKS];
	struct hdr_scale_label labels[HDR_SCALE_MAX_MARKS];
	for (int j = 0; j < n_stack; j++) {
		const float baseline = ((float)(src->level_height * (j + 1)) + oy) * sy - margin;
		for (int i = 0; i < n_parade; i++) {
			const float x0 = (float)(levels * i);
			for (int m = 0; m < src->n_marks; m++)
				line_x[m] = (x0 + hdr_scale_code_to_px(src->marks[m].code, levels)) * sx;
			const int n = hdr_scale_layout_horizontal(src->marks, line_x, src->n_marks, baseline, size,
								  x0 * sx + margin, (x0 + (float)levels) * sx - margin,
								  scope_text_width, labels);
			for (int l = 0; l < n; l++)
				scope_text_draw(labels[l].text, labels[l].x, labels[l].y, size,
						labels[l].ref ? LABEL_REF_COLOR : LABEL_COLOR, SCOPE_TEXT_LEFT);
		}
	}
}

static void his_render_graticule(struct his_source *src)
{
	const bool hlg = cur_hlg(src);
	if (src->graticule_line_vbuf)
		his_render_graticule_vbuf(src, src->graticule_line_vbuf, 0x80FFBF00 /* amber */, hlg);
	if (src->graticule_ref_vbuf)
		his_render_graticule_vbuf(src, src->graticule_ref_vbuf, 0xC040E0FF /* cyan */, hlg);

	// Labels in source units, sized for a scope shown near its native size. The
	// scope dock draws them itself at a fixed on-screen size instead.
	if (hlg && !cm_scope_labels_external) {
		const float k = (float)cur_levels(src) / 256.0f;
		his_draw_labels(src, 1.0f, 1.0f, 0.0f, 8.0f * k, 3.0f * k);
	}
}

void his_draw_overlay_labels(void *source, int w, int h, float ui_scale)
{
	obs_source_t *s = source;
	const char *id = s ? obs_source_get_unversioned_id(s) : NULL;
	if (!id || strcmp(id, "histogram_source") != 0 || w <= 0 || h <= 0)
		return;
	struct his_source *src = obs_obj_get_data(s);
	if (!src || src->cm.bypass || !cur_hlg(src))
		return;
	const uint32_t w_src = his_get_width(src), h_src = his_get_height(src);
	if (!w_src || !h_src)
		return;
	const float k = ui_scale > 0.0f ? ui_scale : 1.0f;
	// the dock maps source x in [0, w_src] onto [0, w] and y in [-1, h_src] onto [0, h]
	his_draw_labels(src, (float)w / (float)w_src, (float)h / (float)(h_src + 1), 1.0f, 10.0f * k, 5.0f * k);
}

static inline void render_histogram(struct his_source *src)
{
	gs_effect_t *effect = src->effect ? src->effect : obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), src->tex_hi);
	gs_effect_set_vec3(gs_effect_get_param_by_name(effect, "hi_max"), &src->vec_hi_max);
	struct vec2 image_size;
	vec2_set(&image_size, (float)src->tex_hi_levels, 1.0f);
	gs_eparam_t *p_size = gs_effect_get_param_by_name(effect, "image_size");
	if (p_size)
		gs_effect_set_vec2(p_size, &image_size);
	const char *name = "Draw";
	int w = src->tex_hi_levels;
	int h = src->level_height;
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
	while (gs_effect_loop(effect, name)) {
		gs_draw_sprite(src->tex_hi, 0, w, h);
	}
}

static void his_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct his_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}
	PROFILE_START(prof_render_name);

	cm_render_target(&src->cm);

	PROFILE_START(prof_draw_name);
	int r_tex_buf = src->w_tex_buf ^ 1;
	if (src->tex_buf[r_tex_buf]) {
		his_set_image(src, src->tex_buf[r_tex_buf], src->hi_max[r_tex_buf],
			      src->tex_buf_levels[r_tex_buf] ? src->tex_buf_levels[r_tex_buf] : HI_SIZE);
		render_histogram(src);
	}
	PROFILE_END(prof_draw_name);

	// HLG state and levels come from the analysed frame; rebuild when they change.
	const uint32_t key =
		cur_hlg(src) ? (1u | ((uint32_t)src->tex_buf_full_range[r_tex_buf] << 1) | (cur_levels(src) << 2)) : 0u;
	if (key != src->graticule_key_prev) {
		src->graticule_need_update = true;
		src->graticule_key_prev = key;
	}

	if (src->graticule_need_update) {
		create_graticule_vbuf(src);
		src->graticule_need_update = false;
	}
	if (src->graticule_line_vbuf || src->graticule_ref_vbuf || src->n_marks > 0)
		his_render_graticule(src);

	PROFILE_END(prof_render_name);
}

const struct obs_source_info colormonitor_histogram = {
	.id = "histogram_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = his_get_name,
	.create = his_create,
	.destroy = his_destroy,
	.update = his_update,
	.get_defaults = his_get_defaults,
	.get_properties = his_get_properties,
	.get_width = his_get_width,
	.get_height = his_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = his_render,
	.video_tick = cm_tick,
};
