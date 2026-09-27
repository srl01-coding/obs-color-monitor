/*
 * HDR Lift filter: gain / highlight expansion in OBS's linear HDR space.
 *
 * Stock OBS filters that could lift an SDR source above SDR white on an HLG
 * canvas either skip HDR sources (Color Correction, Color Grade) or cap at
 * 480 nits (Compose SDR on HDR). This filter always works in GS_CS_709_EXTENDED
 * with a 16-bit float intermediate, so nothing is clamped at SDR white.
 *
 * Linear values: 1.0 = OBS SDR white level (Settings > Advanced), for SDR
 * sources (OBS linearises them) and HDR sources alike.
 */

#include <obs-module.h>
#include <math.h>
#include "plugin-macros.generated.h"
#include "util.h"

#define MODE_GAIN 0
#define MODE_KNEE 1

struct hdr_lift
{
	obs_source_t *context;
	gs_effect_t *effect;

	int mode;
	float white_nits; // SDR white (linear 1.0) is mapped to this many nits
	float knee_pct;   // knee mode: below this HLG level nothing changes
	float trim_ev;
};

static const char *hdr_lift_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("HDRLift");
}

static void hdr_lift_update(void *data, obs_data_t *settings)
{
	struct hdr_lift *f = data;
	f->mode = (int)obs_data_get_int(settings, "mode");
	f->white_nits = (float)obs_data_get_double(settings, "white_nits");
	f->knee_pct = (float)obs_data_get_double(settings, "knee_pct");
	f->trim_ev = (float)obs_data_get_double(settings, "trim_ev");
}

static void *hdr_lift_create(obs_data_t *settings, obs_source_t *context)
{
	struct hdr_lift *f = bzalloc(sizeof(struct hdr_lift));
	f->context = context;

	obs_enter_graphics();
	f->effect = create_effect_from_module_file("hdr-lift.effect");
	obs_leave_graphics();

	hdr_lift_update(f, settings);
	return f;
}

static void hdr_lift_destroy(void *data)
{
	struct hdr_lift *f = data;
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	obs_leave_graphics();
	bfree(f);
}

static void hdr_lift_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "mode", MODE_KNEE);
	obs_data_set_default_double(settings, "white_nits", 1000.0);
	obs_data_set_default_double(settings, "knee_pct", 60.0);
	obs_data_set_default_double(settings, "trim_ev", 0.0);
}

static bool mode_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	obs_property_set_visible(obs_properties_get(props, "knee_pct"),
				 obs_data_get_int(settings, "mode") == MODE_KNEE);
	return true;
}

static obs_properties_t *hdr_lift_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	obs_properties_add_text(props, "info", obs_module_text("HDRLift.Info"), OBS_TEXT_INFO);

	p = obs_properties_add_list(props, "mode", obs_module_text("HDRLift.Mode"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("HDRLift.Mode.Knee"), MODE_KNEE);
	obs_property_list_add_int(p, obs_module_text("HDRLift.Mode.Gain"), MODE_GAIN);
	obs_property_set_modified_callback(p, mode_modified);

	p = obs_properties_add_float_slider(props, "white_nits", obs_module_text("HDRLift.WhiteNits"), 50.0, 4000.0,
					    1.0);
	obs_property_float_set_suffix(p, " nits");

	p = obs_properties_add_float_slider(props, "knee_pct", obs_module_text("HDRLift.Knee"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(p, " %");

	p = obs_properties_add_float_slider(props, "trim_ev", obs_module_text("HDRLift.Trim"), -3.0, 3.0, 0.05);
	obs_property_float_set_suffix(p, " EV");

	return props;
}

// HLG signal level (0..1) -> display nits, grey, consistent with OBS's HLG
// output encoding for a 1000-nit reference display (OOTF gamma 1.2).
static float hlg_to_nits(float e)
{
	float s;
	if (e <= 0.5f)
		s = e * e / 3.0f;
	else
		s = (expf((e - 0.55991073f) / 0.17883277f) + 0.28466892f) / 12.0f;
	return 1000.0f * powf(s, 1.2f);
}

static void hdr_lift_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct hdr_lift *f = data;

	struct obs_video_info ovi;
	const bool hdr_canvas = obs_get_video_info(&ovi) &&
				(ovi.colorspace == VIDEO_CS_2100_HLG || ovi.colorspace == VIDEO_CS_2100_PQ);
	if (!f->effect || !hdr_canvas) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	if (!obs_source_process_filter_begin_with_color_space(f->context, GS_RGBA16F, GS_CS_709_EXTENDED,
							      OBS_NO_DIRECT_RENDERING))
		return;

	const float sdr_white = obs_get_video_sdr_white_level();
	const float trim = exp2f(f->trim_ev);
	const float out_white = f->white_nits / sdr_white; // linear value SDR white is mapped to
	float knee = hlg_to_nits(f->knee_pct / 100.0f) / sdr_white;
	if (knee > 0.99f)
		knee = 0.99f; // knee must be below input white (1.0)
	// Quadratic knee: out(t) = knee + (1 - knee) * (t + a t^2), t = (x - knee) / (1 - knee)
	// slope 1 at the knee (C1-continuous with the untouched range), out(1) = out_white.
	float a = (out_white - knee) / (1.0f - knee) - 1.0f;
	if (a < -0.5f)
		a = -0.5f; // keep the curve monotonic when lowering

	gs_effect_set_float(gs_effect_get_param_by_name(f->effect, "gain"), out_white * trim);
	gs_effect_set_float(gs_effect_get_param_by_name(f->effect, "knee"), knee);
	gs_effect_set_float(gs_effect_get_param_by_name(f->effect, "knee_a"), a);
	gs_effect_set_float(gs_effect_get_param_by_name(f->effect, "trim"), trim);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_tech_end(f->context, f->effect, 0, 0, f->mode == MODE_GAIN ? "Gain" : "Knee");
	gs_blend_state_pop();
}

static enum gs_color_space hdr_lift_get_color_space(void *data, size_t count,
						    const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);
	struct hdr_lift *f = data;
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi) && (ovi.colorspace == VIDEO_CS_2100_HLG || ovi.colorspace == VIDEO_CS_2100_PQ))
		return GS_CS_709_EXTENDED;

	// SDR canvas: filter is bypassed, report the target's space
	const enum gs_color_space potential[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED};
	return obs_source_get_color_space(obs_filter_get_target(f->context), OBS_COUNTOF(potential), potential);
}

const struct obs_source_info colormonitor_hdr_lift_filter = {
	.id = "colormonitor_hdr_lift_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = hdr_lift_get_name,
	.create = hdr_lift_create,
	.destroy = hdr_lift_destroy,
	.update = hdr_lift_update,
	.get_defaults = hdr_lift_defaults,
	.get_properties = hdr_lift_properties,
	.video_render = hdr_lift_render,
	.video_get_color_space = hdr_lift_get_color_space,
};
