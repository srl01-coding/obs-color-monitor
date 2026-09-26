#pragma once

#include <util/threading.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline bool is_program_name(const char *name)
{
	return name && name[0] == 0;
}

static inline bool is_mainview_name(const char *name)
{
	return name && name[0] == 0x01 && name[1] == 0;
}

static inline bool is_preview_name(const char *name)
{
	return name && name[0] == 0x10 && name[1] == 0;
}

struct cm_surface_data
{
	uint8_t *rgb_data, *yuv_data;
	uint32_t linesize, width, height;
	int colorspace;
	gs_texture_t *tex; // for bypass mode

	// HDR (HLG) path: pixels are packed GS_R10G10B10A2 (R bits 0-9, G 10-19,
	// B 20-29, A 30-31) instead of 8-bit BGRA. Channel roles match the 8-bit
	// path: R=R or V(Cr), G=G or Y, B=B or U(Cb).
	bool hlg;
	bool full_range;
	uint32_t levels; // 256 for 8-bit SDR, 1024 for 10-bit HLG
};

static inline void cm_unpack_r10g10b10a2(const uint8_t *p, uint32_t *r, uint32_t *g, uint32_t *b, uint32_t *a)
{
	const uint32_t w = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
	*r = w & 0x3FF;
	*g = (w >> 10) & 0x3FF;
	*b = (w >> 20) & 0x3FF;
	*a = w >> 30;
}

typedef void (*cm_surface_cb_t)(void *data, struct cm_surface_data *surface_data);

struct cm_surface_queue_item
{
	gs_texrender_t *texrender;
	gs_stagesurf_t *stagesurface;
	uint32_t width, height, sheight;
	uint32_t flags; // RGB or YUV
	int colorspace;
	bool hlg;
	bool full_range;
	enum gs_color_format texrender_format;
	enum gs_color_format stagesurface_format;

	cm_surface_cb_t cb;
	void *cb_data;
};

#define CM_SURFACE_QUEUE_SIZE 3

struct cm_source
{
	obs_source_t *self;

	// graphics
	struct cm_surface_queue_item queue[CM_SURFACE_QUEUE_SIZE];
	volatile int i_write_queue, i_staging_queue;
	volatile int i_read_queue;
	int i_bypass_queue;
	gs_texrender_t *texrender;
	enum gs_color_format texrender_format;
	uint32_t texrender_width, texrender_height;
	gs_effect_t *effect;
	bool hlg;        // canvas is Rec.2100 HLG; updated every render
	bool full_range; // canvas uses full range
	bool rendered;
	int x0, x1, y0, y1; // for ROI

	// threading
	pthread_t pipeline_thread;
	pthread_mutex_t pipeline_mutex;
	pthread_cond_t pipeline_cond;
	volatile bool pipeline_thread_running;
	volatile bool request_exit;

	// upper layer
	cm_surface_cb_t callback;
	void *callback_data;

	bool enumerating; // not thread safe but I have no other idea.

	// target
	pthread_mutex_t target_update_mutex;
	obs_weak_source_t *weak_target;
	obs_source_t *roi_src;
	struct roi_source *roi;
	char *target_name;

	// properties
	int target_scale;
	int colorspace; // get from ovi if auto
	uint32_t flags;
	bool bypass;
};

#define CM_FLAG_CONVERT_RGB 1
#define CM_FLAG_CONVERT_YUV 2
#define CM_FLAG_RAW_TEXTURE 4
#define CM_FLAG_ROI 8

void cm_create(struct cm_source *src, obs_data_t *settings, obs_source_t *source);
void cm_destroy(struct cm_source *src);
void cm_update(struct cm_source *src, obs_data_t *settings);
void cm_enum_sources(void *data, obs_source_enum_proc_t enum_callback, void *param);
void cm_get_properties(struct cm_source *src, obs_properties_t *props);
void cm_render_target(struct cm_source *src);
bool cm_stagesurface_map(struct cm_source *src, uint8_t **video_data, uint32_t *video_linesize);
void cm_stagesurface_unmap(struct cm_source *src);
void cm_bypass_render(struct cm_source *src);
void cm_tick(void *data, float unused);

void cm_request(struct cm_source *src, cm_surface_cb_t callback, void *data);

uint32_t cm_bypass_get_width(struct cm_source *src);
uint32_t cm_bypass_get_height(struct cm_source *src);
gs_texture_t *cm_bypass_get_texture(struct cm_source *src);
static inline bool cm_is_roi(const struct cm_source *src)
{
	return src->roi_src && src->roi;
}

bool is_roi_source_name(const char *name);

#ifdef __cplusplus
}
#endif
