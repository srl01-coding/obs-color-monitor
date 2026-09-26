#include <obs-module.h>
#include "plugin-macros.generated.h"
#include "util.h"
#include "hdr-scale.h"

gs_effect_t *create_effect_from_module_file(const char *basename)
{
	char *f = obs_module_file(basename);
	gs_effect_t *effect = gs_effect_create_from_file(f, NULL);
	if (!effect)
		blog(LOG_ERROR, "Cannot load '%s' '%s'", basename, f);
	bfree(f);
	return effect;
}

obs_property_t *properties_add_colorspace(obs_properties_t *props, const char *name, const char *description)
{
	obs_property_t *prop =
		obs_properties_add_list(props, name, description, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Auto"), 0);
	obs_property_list_add_int(prop, obs_module_text("601"), 1);
	obs_property_list_add_int(prop, obs_module_text("709"), 2);
	return prop;
}

int calc_colorspace(int colorspace)
{
	if (1 <= colorspace && colorspace <= 2)
		return colorspace;
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		switch (ovi.colorspace) {
		case VIDEO_CS_601:
			return 1;
		case VIDEO_CS_709:
			return 2;
		default:
			return 2; // TODO: Implement
		}
	}
	return 2; // default
}

void draw_texture_blended(gs_texture_t *tex, float x, float y)
{
	if (!tex)
		return;
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), tex);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(tex, 0, 0, 0);
	gs_matrix_pop();
	gs_blend_state_pop();
}

obs_property_t *properties_add_hdr_scale(obs_properties_t *props)
{
	obs_property_t *prop = obs_properties_add_list(props, "hdr_scale", obs_module_text("HDR.Scale"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("HDR.Scale.HLGPercent"), HDR_SCALE_HLG_PERCENT);
	obs_property_list_add_int(prop, obs_module_text("HDR.Scale.Code10"), HDR_SCALE_CODE_10BIT);
	obs_property_set_long_description(prop, obs_module_text("HDR.Scale.Description"));
	obs_properties_add_bool(props, "hdr_labels", obs_module_text("HDR.Labels"));
	return prop;
}
