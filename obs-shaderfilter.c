// Version 2.0 by Exeldro https://github.com/exeldro/obs-shaderfilter
// Version 1.21 by Charles Fettinger https://github.com/Oncorporation/obs-shaderfilter
// original version by nleseul https://github.com/nleseul/obs-shaderfilter
#include "version.h"
#include <float.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <graphics/math-extra.h>
#include <limits.h>
#include <math.h>
#include <obs-module.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <util/base.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include "shader-gamepad.h"
#include "shader-audio-energy.h"
#include "shader-spectrum.h"
#include "shader-spectrum-effect.h"
#include "shader-tools.h"
#include "shader-helpers.generated.h"

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "2.0"
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/time.h>
#endif

#define AUDIO_BAND_COUNTER_WRAP 166320.0f
#define AUDIO_TIMESERIES_BUFFER_WIDTH 512
#define AUDIO_TIMESERIES_BUFFER_HEIGHT 1
#define AUDIO_BAND_COUNT 4
#define AUDIO_CROSSOVER_COUNT 5

typedef float (*move_get_transition_filter_t)(obs_source_t *source, obs_source_t **filter);
static move_get_transition_filter_t move_get_transition_filter = NULL;
/* OpenGL controller upload shared by all filters; graphics thread only. */
static gs_texture_t *gamepad_texture;
static uint64_t gamepad_texture_frame = UINT64_MAX;

struct image_file {
	gs_texture_t *texture;
	enum gs_color_format format;
	uint32_t cx;
	uint32_t cy;
	bool is_animated_gif;
	bool frame_updated;
	bool loaded;
};

void *(*gs_image_file_create_func)(void) = NULL;
void (*gs_image_file_free_func)(struct image_file *image) = NULL;
void (*gs_image_file_init_func)(struct image_file *image, const char *file, enum gs_image_alpha_mode alpha_mode) = NULL;
void (*gs_image_file_init_texture_func)(struct image_file *image) = NULL;
bool (*gs_image_file_tick_func)(struct image_file *image, uint64_t elapsed_time_ns);
void (*gs_image_file_update_texture_func)(struct image_file *image) = NULL;

void *gs_image_file4_create(void)
{
	return bzalloc(41336); //sizeof(gs_image_file4_t)
}

void *gs_image_file_ex_create(void)
{
	return bzalloc(72); //sizeof(gs_image_file_ex_t));
}

#define nullptr ((void *)0)

static const char *effect_template_begin = "\n\
uniform float4x4 ViewProj;\n\
uniform texture2d image;\n\
\n\
uniform float2 uv_offset;\n\
uniform float2 uv_scale;\n\
uniform float2 uv_pixel_interval;\n\
uniform float2 uv_size;\n\
uniform float rand_f;\n\
uniform float rand_instance_f;\n\
uniform float rand_activation_f;\n\
uniform float elapsed_time;\n\
uniform float elapsed_time_start;\n\
uniform float elapsed_time_show;\n\
uniform float elapsed_time_active;\n\
uniform float elapsed_time_enable;\n\
uniform int loops;\n\
uniform float loop_second;\n\
uniform float local_time;\n\
uniform float audio_peak;\n\
uniform float audio_magnitude;\n\
uniform float energy;\n\
uniform float e_bass;\n\
uniform float e_lows;\n\
uniform float e_mids;\n\
uniform float e_treb;\n\
uniform float bass;\n\
uniform float lows;\n\
uniform float mids;\n\
uniform float treb;\n\
uniform float bass_c;\n\
uniform float lows_c;\n\
uniform float mids_c;\n\
uniform float treb_c;\n\
uniform texture2d t_bass;\n\
uniform texture2d t_lows;\n\
uniform texture2d t_mids;\n\
uniform texture2d t_treb;\n\
\n\
uniform float ef_bass_fast;\n\
uniform float ef_bass_medium;\n\
uniform float ef_bass_slow;\n\
uniform float ef_bass_slug;\n\
uniform float ef_lows_fast;\n\
uniform float ef_lows_medium;\n\
uniform float ef_lows_slow;\n\
uniform float ef_lows_slug;\n\
uniform float ef_mids_fast;\n\
uniform float ef_mids_medium;\n\
uniform float ef_mids_slow;\n\
uniform float ef_mids_slug;\n\
uniform float ef_treb_fast;\n\
uniform float ef_treb_medium;\n\
uniform float ef_treb_slow;\n\
uniform float ef_treb_slug;\n\
\n\
uniform float lf_bass_fast;\n\
uniform float lf_bass_medium;\n\
uniform float lf_bass_slow;\n\
uniform float lf_bass_slug;\n\
uniform float lf_lows_fast;\n\
uniform float lf_lows_medium;\n\
uniform float lf_lows_slow;\n\
uniform float lf_lows_slug;\n\
uniform float lf_mids_fast;\n\
uniform float lf_mids_medium;\n\
uniform float lf_mids_slow;\n\
uniform float lf_mids_slug;\n\
uniform float lf_treb_fast;\n\
uniform float lf_treb_medium;\n\
uniform float lf_treb_slow;\n\
uniform float lf_treb_slug;\n\
\n\
sampler_state textureSampler{\n\
	Filter = Linear;\n\
	AddressU = Border;\n\
	AddressV = Border;\n\
	BorderColor = 00000000;\n\
};\n\
\n\
struct VertData {\n\
	float4 pos : POSITION;\n\
	float2 uv : TEXCOORD0;\n\
};\n\
\n\
VertData mainTransform(VertData v_in)\n\
{\n\
	VertData vert_out;\n\
	vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);\n\
	vert_out.uv = v_in.uv * uv_scale + uv_offset;\n\
	return vert_out;\n\
}\n\
\n\
float4 acc(float rate) {\n\
	float divisor = pow(166320.0, saturate(1.0 - rate));\n\
	return frac(float4(bass_c, lows_c, mids_c, treb_c) / divisor);\n\
}\n\
\n\
float4 ts(float x) {\n\
	float2 uv = float2(x, 0.5);\n\
	float bass_val = t_bass.Sample(textureSampler, uv).x;\n\
	float lows_val = t_lows.Sample(textureSampler, uv).x;\n\
	float mids_val = t_mids.Sample(textureSampler, uv).x;\n\
	float treb_val = t_treb.Sample(textureSampler, uv).x;\n\
	return float4(bass_val, lows_val, mids_val, treb_val);\n\
}\n\
\n\
float srgb_nonlinear_to_linear_channel(float u)\n\
{\n\
	return (u <= 0.04045) ? (u / 12.92) : pow((u + 0.055) / 1.055, 2.4);\n\
}\n\
\n\
float3 srgb_nonlinear_to_linear(float3 v)\n\
{\n\
	return float3(srgb_nonlinear_to_linear_channel(v.r),\n\
		      srgb_nonlinear_to_linear_channel(v.g),\n\
		      srgb_nonlinear_to_linear_channel(v.b));\n\
}\n\
\n\
float interp_falloff5(float t, float p0, float p1, float p2, float p3, float p4) {\n\
	t = saturate(t);\n\
	float pos = t * 4.0;\n\
	int idx = int(floor(pos));\n\
	float f = frac(pos);\n\
	if (idx == 0) return lerp(p0, p1, f);\n\
	if (idx == 1) return lerp(p1, p2, f);\n\
	if (idx == 2) return lerp(p2, p3, f);\n\
	if (idx == 3) return lerp(p3, p4, f);\n\
	return p4;\n\
}\n\
\n\
float4 sf_expfoff(float4 tailLengths) {\n\
	return float4(\n\
		interp_falloff5(tailLengths.x, bass, ef_bass_fast, ef_bass_medium, ef_bass_slow, ef_bass_slug),\n\
		interp_falloff5(tailLengths.y, lows, ef_lows_fast, ef_lows_medium, ef_lows_slow, ef_lows_slug),\n\
		interp_falloff5(tailLengths.z, mids, ef_mids_fast, ef_mids_medium, ef_mids_slow, ef_mids_slug),\n\
		interp_falloff5(tailLengths.w, treb, ef_treb_fast, ef_treb_medium, ef_treb_slow, ef_treb_slug));\n\
}\n\
\n\
#define expfoff(tailLengths) sf_expfoff(float4(0.0, 0.0, 0.0, 0.0) + (tailLengths))\n\
\n\
float4 sf_linfoff(float4 tailLengths) {\n\
	return float4(\n\
		interp_falloff5(tailLengths.x, bass, lf_bass_fast, lf_bass_medium, lf_bass_slow, lf_bass_slug),\n\
		interp_falloff5(tailLengths.y, lows, lf_lows_fast, lf_lows_medium, lf_lows_slow, lf_lows_slug),\n\
		interp_falloff5(tailLengths.z, mids, lf_mids_fast, lf_mids_medium, lf_mids_slow, lf_mids_slug),\n\
		interp_falloff5(tailLengths.w, treb, lf_treb_fast, lf_treb_medium, lf_treb_slow, lf_treb_slug));\n\
}\n\
\n\
#define linfoff(tailLengths) sf_linfoff(float4(0.0, 0.0, 0.0, 0.0) + (tailLengths))\n\
\n\
float3 hsv2rgb(float3 hsv) {\n\
	float3 rgb = float3(hsv.z, hsv.z, hsv.z);\n\
	if (hsv.y > 0.0f) {\n\
		float h = frac(hsv.x) * 6.0f;\n\
		int i = int(floor(h));\n\
		float f = h - i;\n\
		float p = hsv.z * (1.0f - hsv.y);\n\
		float q = hsv.z * (1.0f - hsv.y * f);\n\
		float t = hsv.z * (1.0f - hsv.y * (1.0f - f));\n\
		if (i == 0) rgb = float3(hsv.z, t, p);\n\
		else if (i == 1) rgb = float3(q, hsv.z, p);\n\
		else if (i == 2) rgb = float3(p, hsv.z, t);\n\
		else if (i == 3) rgb = float3(p, q, hsv.z);\n\
		else if (i == 4) rgb = float3(t, p, hsv.z);\n\
		else rgb = float3(hsv.z, p, q);\n\
	}\n\
	return rgb;\n\
}\n\
\n\
float3 rgb2hsv(float3 rgb) {\n\
	float maxc = max(rgb.r, max(rgb.g, rgb.b));\n\
	float minc = min(rgb.r, min(rgb.g, rgb.b));\n\
	float delta = maxc - minc;\n\
	float3 hsv;\n\
	hsv.z = maxc;\n\
	if (delta > 0.00001f) {\n\
		hsv.y = delta / maxc;\n\
		if (rgb.r >= maxc) hsv.x = (rgb.g - rgb.b) / delta;\n\
		else if (rgb.g >= maxc) hsv.x = 2.0f + (rgb.b - rgb.r) / delta;\n\
		else hsv.x = 4.0f + (rgb.r - rgb.g) / delta;\n\
		hsv.x /= 6.0f;\n\
		if (hsv.x < 0.0f) hsv.x += 1.0f;\n\
	} else {\n\
		hsv.y = 0.0f;\n\
		hsv.x = 0.0f;\n\
	}\n\
	return hsv;\n\
}\n\
\n\
float cu_bass(float speed) { return acc(speed).x; }\n\
float cu_lows(float speed) { return acc(speed).y; }\n\
float cu_mids(float speed) { return acc(speed).z; }\n\
float cu_treb(float speed) { return acc(speed).w; }\n\
";


static const char *effect_template_default_image_shader = "\n\
float4 mainImage(VertData v_in) : TARGET\n\
{\n\
	return image.Sample(textureSampler, v_in.uv);\n\
}\n\
";

static const char *effect_template_default_transition_image_shader = "\n\
uniform texture2d image_a;\n\
uniform texture2d image_b;\n\
uniform float transition_time = 0.5;\n\
uniform bool convert_linear = true;\n\
\n\
float4 mainImage(VertData v_in) : TARGET\n\
{\n\
	float4 a_val = image_a.Sample(textureSampler, v_in.uv);\n\
	float4 b_val = image_b.Sample(textureSampler, v_in.uv);\n\
	float4 rgba = lerp(a_val, b_val, transition_time);\n\
	if (convert_linear)\n\
		rgba.rgb = srgb_nonlinear_to_linear(rgba.rgb);\n\
	return rgba;\n\
}\n\
";

static const char *effect_template_end = "\n\
technique Draw\n\
{\n\
	pass\n\
	{\n\
		vertex_shader = mainTransform(v_in);\n\
		pixel_shader = mainImage(v_in);\n\
	}\n\
}\n";

struct effect_param_data {
	struct dstr name;
	struct dstr display_name;
	struct dstr widget_type;
	struct dstr group;
	struct dstr path;
	struct dstr tooltip;
	DARRAY(int) option_values;
	DARRAY(struct dstr) option_labels;

	enum gs_shader_param_type type;
	gs_eparam_t *param;

	struct image_file *image;
	gs_texrender_t *render;
	obs_weak_source_t *source;

	union {
		long long i;
		double f;
		char *string;
		struct vec2 vec2;
		struct vec3 vec3;
		struct vec4 vec4;
	} value;
	union {
		long long i;
		double f;
		char *string;
		struct vec2 vec2;
		struct vec3 vec3;
		struct vec4 vec4;
	} default_value;
	bool has_default;
	char *label;
	union {
		long long i;
		double f;
	} minimum;
	union {
		long long i;
		double f;
	} maximum;
	union {
		long long i;
		double f;
	} step;
};

enum audio_band {
	AUDIO_BAND_BASS,
	AUDIO_BAND_LOWS,
	AUDIO_BAND_MIDS,
	AUDIO_BAND_TREB,
};

enum audio_falloff_speed {
	AUDIO_FALLOFF_FAST,
	AUDIO_FALLOFF_MEDIUM,
	AUDIO_FALLOFF_SLOW,
	AUDIO_FALLOFF_SLUG,
};

struct audio_source_analysis {
	struct audio_source_analysis *next;
	obs_weak_source_t *source;
	size_t users;
	pthread_mutex_t mutex;
	uint32_t sample_rate;
	float lowpass_alpha[AUDIO_CROSSOVER_COUNT];
	float lowpass_state[MAX_AUDIO_CHANNELS][AUDIO_CROSSOVER_COUNT];
	float band_weights[AUDIO_BAND_COUNT];
	uint64_t capture_ns;
	float capture_seconds;
	struct sf_audio_energy energy_window;
	struct sf_spectrum *spectrum;
	size_t spectrum_users;
	float audio_peak;
	float audio_magnitude;
	float bands[AUDIO_BAND_COUNT];
	float cumulative[AUDIO_BAND_COUNT];
	float exp_falloff[AUDIO_BAND_COUNT][AUDIO_FALLOFF_SLUG + 1];
	float lin_falloff[AUDIO_BAND_COUNT][AUDIO_FALLOFF_SLUG + 1];
	float history[AUDIO_BAND_COUNT][AUDIO_TIMESERIES_BUFFER_WIDTH];
	float history_smooth[AUDIO_BAND_COUNT];
	size_t history_write_pos;
};

/* The registry lock protects membership/reference counts; each entry lock
 * protects results shared between the audio and video threads. */
static pthread_mutex_t audio_analyses_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct audio_source_analysis *audio_analyses;

struct shader_filter_data {
	obs_source_t *context;
	gs_effect_t *effect;
	gs_effect_t *output_effect;
	gs_vertbuffer_t *sprite_buffer;

	gs_texrender_t *input_texrender;
	gs_texrender_t *previous_input_texrender;
	gs_texrender_t *output_texrender;
	gs_texrender_t *previous_output_texrender;
	gs_eparam_t *param_output_image;

	bool reload_effect;
	struct dstr last_path;
	bool last_from_file;
	bool transition;
	bool transitioning;
	bool prev_transitioning;

	bool use_pm_alpha;
	bool output_rendered;
	bool input_rendered;

	float shader_start_time;
	float shader_show_time;
	float shader_active_time;
	float shader_enable_time;
	bool enabled;
	bool use_template;

	gs_eparam_t *param_uv_offset;
	gs_eparam_t *param_uv_scale;
	gs_eparam_t *param_uv_pixel_interval;
	gs_eparam_t *param_uv_size;
	gs_eparam_t *param_current_time_ms;
	gs_eparam_t *param_current_time_sec;
	gs_eparam_t *param_current_time_min;
	gs_eparam_t *param_current_time_hour;
	gs_eparam_t *param_current_time_day_of_week;
	gs_eparam_t *param_current_time_day_of_month;
	gs_eparam_t *param_current_time_month;
	gs_eparam_t *param_current_time_day_of_year;
	gs_eparam_t *param_current_time_year;
	gs_eparam_t *param_elapsed_time;
	gs_eparam_t *param_elapsed_time_start;
	gs_eparam_t *param_elapsed_time_show;
	gs_eparam_t *param_elapsed_time_active;
	gs_eparam_t *param_elapsed_time_enable;
	gs_eparam_t *param_loops;
	gs_eparam_t *param_loop_second;
	gs_eparam_t *param_local_time;
	gs_eparam_t *param_rand_f;
	gs_eparam_t *param_rand_instance_f;
	gs_eparam_t *param_rand_activation_f;
	gs_eparam_t *param_image;
	gs_eparam_t *param_previous_image;
	gs_eparam_t *param_image_a;
	gs_eparam_t *param_image_b;
	gs_eparam_t *param_transition_time;
	gs_eparam_t *param_convert_linear;
	gs_eparam_t *param_previous_output;
	gs_eparam_t *param_audio_peak;
	gs_eparam_t *param_gamepad_available;
	gs_eparam_t *param_gamepad_info;
	gs_eparam_t *param_gamepad_buttons;
	gs_eparam_t *param_gamepad_presses;
	gs_eparam_t *param_gamepad_axes;
	gs_eparam_t *param_gamepad_axes_c;
	gs_eparam_t *param_gamepad_axes_clamped;
	gs_eparam_t *param_gamepad_data;
	gs_eparam_t *param_audio_magnitude;
	gs_eparam_t *param_energy;
	gs_eparam_t *param_e_bass;
	gs_eparam_t *param_e_lows;
	gs_eparam_t *param_e_mids;
	gs_eparam_t *param_e_treb;
	gs_eparam_t *param_bass;
	gs_eparam_t *param_lows;
	gs_eparam_t *param_mids;
	gs_eparam_t *param_treb;
	gs_eparam_t *param_bass_c;
	gs_eparam_t *param_lows_c;
	gs_eparam_t *param_mids_c;
	gs_eparam_t *param_treb_c;

	// Audio timeseries textures
	gs_eparam_t *param_t_bass;
	gs_eparam_t *param_spec;
	gs_texture_t *spec_texture;
	float spec_values[SF_SPECTRUM_VALUES];
	uint64_t spec_revision;
	uint64_t spec_uploaded_revision;
	bool spectrum_registered;
	bool spectrum_declared;
	gs_eparam_t *param_t_lows;
	gs_eparam_t *param_t_mids;
	gs_eparam_t *param_t_treb;
	gs_texrender_t *t_bass_render;
	gs_texrender_t *t_lows_render;
	gs_texrender_t *t_mids_render;
	gs_texrender_t *t_treb_render;
	float *t_bass_buffer;
	float *t_lows_buffer;
	float *t_mids_buffer;
	float *t_treb_buffer;

	// Exponential falloff presets (4 per channel)
	gs_eparam_t *param_ef_bass_fast;
	gs_eparam_t *param_ef_bass_medium;
	gs_eparam_t *param_ef_bass_slow;
	gs_eparam_t *param_ef_bass_slug;
	gs_eparam_t *param_ef_lows_fast;
	gs_eparam_t *param_ef_lows_medium;
	gs_eparam_t *param_ef_lows_slow;
	gs_eparam_t *param_ef_lows_slug;
	gs_eparam_t *param_ef_mids_fast;
	gs_eparam_t *param_ef_mids_medium;
	gs_eparam_t *param_ef_mids_slow;
	gs_eparam_t *param_ef_mids_slug;
	gs_eparam_t *param_ef_treb_fast;
	gs_eparam_t *param_ef_treb_medium;
	gs_eparam_t *param_ef_treb_slow;
	gs_eparam_t *param_ef_treb_slug;

	float ef_bass_fast;
	float ef_bass_medium;
	float ef_bass_slow;
	float ef_bass_slug;
	float ef_lows_fast;
	float ef_lows_medium;
	float ef_lows_slow;
	float ef_lows_slug;
	float ef_mids_fast;
	float ef_mids_medium;
	float ef_mids_slow;
	float ef_mids_slug;
	float ef_treb_fast;
	float ef_treb_medium;
	float ef_treb_slow;
	float ef_treb_slug;

	// Linear falloff presets (4 per channel)
	gs_eparam_t *param_lf_bass_fast;
	gs_eparam_t *param_lf_bass_medium;
	gs_eparam_t *param_lf_bass_slow;
	gs_eparam_t *param_lf_bass_slug;
	gs_eparam_t *param_lf_lows_fast;
	gs_eparam_t *param_lf_lows_medium;
	gs_eparam_t *param_lf_lows_slow;
	gs_eparam_t *param_lf_lows_slug;
	gs_eparam_t *param_lf_mids_fast;
	gs_eparam_t *param_lf_mids_medium;
	gs_eparam_t *param_lf_mids_slow;
	gs_eparam_t *param_lf_mids_slug;
	gs_eparam_t *param_lf_treb_fast;
	gs_eparam_t *param_lf_treb_medium;
	gs_eparam_t *param_lf_treb_slow;
	gs_eparam_t *param_lf_treb_slug;

	float lf_bass_fast;
	float lf_bass_medium;
	float lf_bass_slow;
	float lf_bass_slug;
	float lf_lows_fast;
	float lf_lows_medium;
	float lf_lows_slow;
	float lf_lows_slug;
	float lf_mids_fast;
	float lf_mids_medium;
	float lf_mids_slow;
	float lf_mids_slug;
	float lf_treb_fast;
	float lf_treb_medium;
	float lf_treb_slow;
	float lf_treb_slug;

	int expand_left;
	int expand_right;
	int expand_top;
	int expand_bottom;

	int total_width;
	int total_height;
	bool no_repeat;
	bool rendering;

	struct vec2 uv_offset;
	struct vec2 uv_scale;
	struct vec2 uv_pixel_interval;
	struct vec2 uv_size;
	float elapsed_time;
	float elapsed_time_loop;
	int loops;
	float local_time;
	float rand_f;
	float rand_instance_f;
	float rand_activation_f;
	float audio_peak;
	float audio_magnitude;
	float energy;
	float e_bass;
	float e_lows;
	float e_mids;
	float e_treb;
	float bass;
	float lows;
	float mids;
	float treb;
	float bass_c;
	float lows_c;
	float mids_c;
	float treb_c;

	char *audio_source_name;
	struct audio_source_analysis *audio_analysis;

	DARRAY(struct effect_param_data) stored_param_list;
	uint64_t last_frame_time;
};

static void shader_filter_release_audio_analysis(struct shader_filter_data *filter);
static void shader_filter_release_spectrum(struct shader_filter_data *filter);

static unsigned int rand_interval(unsigned int min, unsigned int max)
{
	unsigned int r;
	const unsigned int range = 1 + max - min;
	const unsigned int buckets = RAND_MAX / range;
	const unsigned int limit = buckets * range;

	/* Create equal size buckets all in a row, then fire randomly towards
	 * the buckets until you land in one of them. All buckets are equally
	 * likely. If you land off the end of the line of buckets, try again. */
	do {
		r = rand();
	} while (r >= limit);

	return min + (r / buckets);
}

static char *load_shader_from_file(const char *file_name) // add input of visited files
{
	char *file_ptr = os_quick_read_utf8_file(file_name);
	if (file_ptr == NULL) {
		blog(LOG_WARNING, "[obs-shaderfilter] failed to read file: %s", file_name);
		return NULL;
	}
	char **lines = strlist_split(file_ptr, '\n', true);
	struct dstr shader_file;
	dstr_init(&shader_file);

	size_t line_i = 0;
	while (lines[line_i] != NULL) {
		char *line = lines[line_i];
		line_i++;
		if (strncmp(line, "#include", 8) == 0) {
			// Open the included file, place contents here.
			char *pos = strrchr(file_name, '/');
			const size_t length = pos - file_name + 1;
			struct dstr include_path = {0};
			dstr_ncopy(&include_path, file_name, length);
			char *start = strchr(line, '"') + 1;
			char *end = strrchr(line, '"');

			dstr_ncat(&include_path, start, end - start);
			char *abs_include_path = os_get_abs_path_ptr(include_path.array);
			char *file_contents = load_shader_from_file(abs_include_path);
			dstr_cat(&shader_file, file_contents);
			dstr_cat(&shader_file, "\n");
			bfree(abs_include_path);
			bfree(file_contents);
			dstr_free(&include_path);
		} else {
			// else place current line here.
			dstr_cat(&shader_file, line);
			dstr_cat(&shader_file, "\n");
		}
	}

	// Add file_name to visited files
	// Do stuff with the file, and populate shader_file
	/*

		for line in file:
		   if line starts with #include
		       get path
		       if path is not in visited files
			   include_file_contents = load_shader_from_file(path)
	                   concat include_file_contents onto shader_file
	           else
		       concat line onto shader_file
	*/
	bfree(file_ptr);
	strlist_free(lines);
	return shader_file.array;
}

static void shader_filter_clear_params(struct shader_filter_data *filter)
{
	shader_filter_release_spectrum(filter);
	filter->param_spec = NULL;
	filter->spectrum_declared = false;
	if (filter->spec_texture) {
		obs_enter_graphics();
		gs_texture_destroy(filter->spec_texture);
		filter->spec_texture = NULL;
		obs_leave_graphics();
	}
	filter->param_current_time_ms = NULL;
	filter->param_current_time_sec = NULL;
	filter->param_current_time_min = NULL;
	filter->param_current_time_hour = NULL;
	filter->param_current_time_day_of_week = NULL;
	filter->param_current_time_day_of_month = NULL;
	filter->param_current_time_month = NULL;
	filter->param_current_time_day_of_year = NULL;
	filter->param_current_time_year = NULL;
	filter->param_elapsed_time = NULL;
	filter->param_elapsed_time_start = NULL;
	filter->param_elapsed_time_show = NULL;
	filter->param_elapsed_time_active = NULL;
	filter->param_elapsed_time_enable = NULL;
	filter->param_uv_offset = NULL;
	filter->param_uv_pixel_interval = NULL;
	filter->param_uv_scale = NULL;
	filter->param_uv_size = NULL;
	filter->param_rand_f = NULL;
	filter->param_rand_activation_f = NULL;
	filter->param_rand_instance_f = NULL;
	filter->param_loops = NULL;
	filter->param_loop_second = NULL;
	filter->param_local_time = NULL;
	filter->param_audio_peak = NULL;
	filter->param_gamepad_available = NULL;
	filter->param_gamepad_info = NULL;
	filter->param_gamepad_buttons = NULL;
	filter->param_gamepad_presses = NULL;
	filter->param_gamepad_axes = NULL;
	filter->param_gamepad_axes_c = NULL;
	filter->param_gamepad_axes_clamped = NULL;
	filter->param_gamepad_data = NULL;
	filter->param_audio_magnitude = NULL;
	filter->param_energy = NULL;
	filter->param_e_bass = NULL;
	filter->param_e_lows = NULL;
	filter->param_e_mids = NULL;
	filter->param_e_treb = NULL;
	filter->param_bass = NULL;
	filter->param_lows = NULL;
	filter->param_mids = NULL;
	filter->param_treb = NULL;
	filter->param_bass_c = NULL;
	filter->param_lows_c = NULL;
	filter->param_mids_c = NULL;
	filter->param_treb_c = NULL;
	filter->param_t_bass = NULL;
	filter->param_t_lows = NULL;
	filter->param_t_mids = NULL;
	filter->param_t_treb = NULL;
	filter->param_ef_bass_fast = NULL;
	filter->param_ef_bass_medium = NULL;
	filter->param_ef_bass_slow = NULL;
	filter->param_ef_bass_slug = NULL;
	filter->param_ef_lows_fast = NULL;
	filter->param_ef_lows_medium = NULL;
	filter->param_ef_lows_slow = NULL;
	filter->param_ef_lows_slug = NULL;
	filter->param_ef_mids_fast = NULL;
	filter->param_ef_mids_medium = NULL;
	filter->param_ef_mids_slow = NULL;
	filter->param_ef_mids_slug = NULL;
	filter->param_ef_treb_fast = NULL;
	filter->param_ef_treb_medium = NULL;
	filter->param_ef_treb_slow = NULL;
	filter->param_ef_treb_slug = NULL;
	filter->param_lf_bass_fast = NULL;
	filter->param_lf_bass_medium = NULL;
	filter->param_lf_bass_slow = NULL;
	filter->param_lf_bass_slug = NULL;
	filter->param_lf_lows_fast = NULL;
	filter->param_lf_lows_medium = NULL;
	filter->param_lf_lows_slow = NULL;
	filter->param_lf_lows_slug = NULL;
	filter->param_lf_mids_fast = NULL;
	filter->param_lf_mids_medium = NULL;
	filter->param_lf_mids_slow = NULL;
	filter->param_lf_mids_slug = NULL;
	filter->param_lf_treb_fast = NULL;
	filter->param_lf_treb_medium = NULL;
	filter->param_lf_treb_slow = NULL;
	filter->param_lf_treb_slug = NULL;
	filter->param_image = NULL;
	filter->param_previous_image = NULL;
	filter->param_image_a = NULL;
	filter->param_image_b = NULL;
	filter->param_transition_time = NULL;
	filter->param_convert_linear = NULL;
	filter->param_previous_output = NULL;

	size_t param_count = filter->stored_param_list.num;
	for (size_t param_index = 0; param_index < param_count; param_index++) {
		struct effect_param_data *param = (filter->stored_param_list.array + param_index);
		if (param->image) {
			obs_enter_graphics();
			gs_image_file_free_func(param->image);
			obs_leave_graphics();

			bfree(param->image);
			param->image = NULL;
		}
		if (param->source) {
			obs_source_t *source = obs_weak_source_get_source(param->source);
			if (source) {
				if ((!filter->transition || filter->prev_transitioning) && obs_source_active(filter->context))
					obs_source_dec_active(source);
				if ((!filter->transition || filter->prev_transitioning) && obs_source_showing(filter->context))
					obs_source_dec_showing(source);
				obs_source_release(source);
			}
			obs_weak_source_release(param->source);
			param->source = NULL;
		}
		if (param->render) {
			obs_enter_graphics();
			gs_texrender_destroy(param->render);
			obs_leave_graphics();
			param->render = NULL;
		}
		dstr_free(&param->name);
		dstr_free(&param->display_name);
		dstr_free(&param->widget_type);
		dstr_free(&param->group);
		dstr_free(&param->path);
		dstr_free(&param->tooltip);
		da_free(param->option_values);
		for (size_t i = 0; i < param->option_labels.num; i++) {
			dstr_free(&param->option_labels.array[i]);
		}
		da_free(param->option_labels);
	}

	da_free(filter->stored_param_list);
}

static void load_output_effect(struct shader_filter_data *filter)
{
	if (filter->output_effect != NULL) {
		obs_enter_graphics();
		gs_effect_destroy(filter->output_effect);
		filter->output_effect = NULL;
		obs_leave_graphics();
	}

	char *shader_text = NULL;
	struct dstr filename = {0};
	dstr_cat(&filename, obs_get_module_data_path(obs_current_module()));
	dstr_cat(&filename, "/internal/render_output.effect");
	char *abs_path = os_get_abs_path_ptr(filename.array);
	if (abs_path) {
		shader_text = load_shader_from_file(abs_path);
		bfree(abs_path);
	}
	if (!shader_text)
		shader_text = load_shader_from_file(filename.array);

	char *errors = NULL;
	dstr_free(&filename);

	obs_enter_graphics();
	filter->output_effect = gs_effect_create(shader_text, NULL, &errors);
	obs_leave_graphics();

	bfree(shader_text);
	if (filter->output_effect == NULL) {
		blog(LOG_WARNING, "[obs-shaderfilter] Unable to load render_output.effect file.  Errors:\n%s",
		     (errors == NULL || strlen(errors) == 0 ? "(None)" : errors));
		bfree(errors);
	} else {
		size_t effect_count = gs_effect_get_num_params(filter->output_effect);
		for (size_t effect_index = 0; effect_index < effect_count; effect_index++) {
			gs_eparam_t *param = gs_effect_get_param_by_idx(filter->output_effect, effect_index);
			struct gs_effect_param_info info;
			gs_effect_get_param_info(param, &info);
			if (strcmp(info.name, "output_image") == 0) {
				filter->param_output_image = param;
			}
		}
	}
}

static void load_sprite_buffer(struct shader_filter_data *filter)
{
	if (filter->sprite_buffer)
		return;
	struct gs_vb_data *vbd = gs_vbdata_create();
	vbd->num = 4;
	vbd->points = bmalloc(sizeof(struct vec3) * 4);
	vbd->num_tex = 1;
	vbd->tvarray = bmalloc(sizeof(struct gs_tvertarray));
	vbd->tvarray[0].width = 2;
	vbd->tvarray[0].array = bmalloc(sizeof(struct vec2) * 4);
	memset(vbd->points, 0, sizeof(struct vec3) * 4);
	memset(vbd->tvarray[0].array, 0, sizeof(struct vec2) * 4);
	filter->sprite_buffer = gs_vertexbuffer_create(vbd, GS_DYNAMIC);
}

static void shader_filter_reload_effect(struct shader_filter_data *filter)
{
	obs_data_t *settings = obs_source_get_settings(filter->context);

	// First, clean up the old effect and all references to it.
	filter->shader_start_time = 0.0f;
	shader_filter_clear_params(filter);

	if (filter->effect != NULL) {
		obs_enter_graphics();
		gs_effect_destroy(filter->effect);
		filter->effect = NULL;
		obs_leave_graphics();
	}

	// Load text and build the effect from the template, if necessary.
	char *shader_text = NULL;
	bool use_template = !obs_data_get_bool(settings, "override_entire_effect");

	if (obs_data_get_bool(settings, "from_file")) {
		const char *file_name = obs_data_get_string(settings, "shader_file_name");
		if (!strlen(file_name)) {
			obs_data_unset_user_value(settings, "last_error");
			goto end;
		}
		shader_text = load_shader_from_file(file_name);
		if (!shader_text) {
			obs_data_set_string(settings, "last_error", obs_module_text("ShaderFilter.FileLoadFailed"));
			goto end;
		}
	} else {
		shader_text = bstrdup(obs_data_get_string(settings, "shader_text"));
		use_template = true;
	}
	filter->use_template = use_template;

	struct dstr effect_text = {0};

	if (use_template) {
		dstr_cat(&effect_text, effect_template_begin);
		dstr_cat(&effect_text, effect_template_helpers);
		if (shader_text && strstr(shader_text, "gamepad_"))
			dstr_cat(&effect_text, effect_template_gamepad);
	}

	if (shader_text) {
		dstr_cat(&effect_text, shader_text);
		bfree(shader_text);
	}

	if (use_template) {
		dstr_cat(&effect_text, effect_template_end);
	}

	// Create the effect.
	char *errors = NULL;

	obs_enter_graphics();
	int device_type = gs_get_device_type();
	if (device_type == GS_DEVICE_OPENGL) {
		dstr_replace(&effect_text, "[loop]", "");
		dstr_insert(&effect_text, 0, "#define OPENGL 1\n");
	}

	if (effect_text.len && dstr_find(&effect_text, "#define USE_PM_ALPHA 1")) {
		filter->use_pm_alpha = true;
	} else {
		filter->use_pm_alpha = false;
	}

	if (filter->effect)
		gs_effect_destroy(filter->effect);
	filter->spectrum_declared = sf_shader_append_spectrum(&effect_text);
	filter->effect = gs_effect_create(effect_text.array, NULL, &errors);
	obs_leave_graphics();

	if (filter->effect == NULL) {
		blog(LOG_WARNING, "[obs-shaderfilter] Unable to create effect. Errors returned from parser:\n%s",
		     (errors == NULL || strlen(errors) == 0 ? "(None)" : errors));
		if (errors && strlen(errors)) {
			obs_data_set_string(settings, "last_error", errors);
		} else {
			obs_data_set_string(settings, "last_error", obs_module_text("ShaderFilter.Unknown"));
		}
		dstr_free(&effect_text);
		bfree(errors);
		goto end;
	} else {
		dstr_free(&effect_text);
		obs_data_unset_user_value(settings, "last_error");
	}

	// Store references to the new effect's parameters.
	da_free(filter->stored_param_list);

	size_t effect_count = gs_effect_get_num_params(filter->effect);
	for (size_t effect_index = 0; effect_index < effect_count; effect_index++) {
		gs_eparam_t *param = gs_effect_get_param_by_idx(filter->effect, effect_index);
		if (!param)
			continue;
		struct gs_effect_param_info info;
		gs_effect_get_param_info(param, &info);

		if (strcmp(info.name, "uv_offset") == 0) {
			filter->param_uv_offset = param;
		} else if (strcmp(info.name, "uv_scale") == 0) {
			filter->param_uv_scale = param;
		} else if (strcmp(info.name, "uv_pixel_interval") == 0) {
			filter->param_uv_pixel_interval = param;
		} else if (strcmp(info.name, "uv_size") == 0) {
			filter->param_uv_size = param;
		} else if (strcmp(info.name, "current_time_ms") == 0) {
			filter->param_current_time_ms = param;
		} else if (strcmp(info.name, "current_time_sec") == 0) {
			filter->param_current_time_sec = param;
		} else if (strcmp(info.name, "current_time_min") == 0) {
			filter->param_current_time_min = param;
		} else if (strcmp(info.name, "current_time_hour") == 0) {
			filter->param_current_time_hour = param;
		} else if (strcmp(info.name, "current_time_day_of_week") == 0) {
			filter->param_current_time_day_of_week = param;
		} else if (strcmp(info.name, "current_time_day_of_month") == 0) {
			filter->param_current_time_day_of_month = param;
		} else if (strcmp(info.name, "current_time_month") == 0) {
			filter->param_current_time_month = param;
		} else if (strcmp(info.name, "current_time_day_of_year") == 0) {
			filter->param_current_time_day_of_year = param;
		} else if (strcmp(info.name, "current_time_year") == 0) {
			filter->param_current_time_year = param;
		} else if (strcmp(info.name, "elapsed_time") == 0) {
			filter->param_elapsed_time = param;
		} else if (strcmp(info.name, "elapsed_time_start") == 0) {
			filter->param_elapsed_time_start = param;
		} else if (strcmp(info.name, "elapsed_time_show") == 0) {
			filter->param_elapsed_time_show = param;
		} else if (strcmp(info.name, "elapsed_time_active") == 0) {
			filter->param_elapsed_time_active = param;
		} else if (strcmp(info.name, "elapsed_time_enable") == 0) {
			filter->param_elapsed_time_enable = param;
		} else if (strcmp(info.name, "rand_f") == 0) {
			filter->param_rand_f = param;
		} else if (strcmp(info.name, "rand_activation_f") == 0) {
			filter->param_rand_activation_f = param;
		} else if (strcmp(info.name, "rand_instance_f") == 0) {
			filter->param_rand_instance_f = param;
		} else if (strcmp(info.name, "loops") == 0) {
			filter->param_loops = param;
		} else if (strcmp(info.name, "loop_second") == 0) {
			filter->param_loop_second = param;
		} else if (strcmp(info.name, "local_time") == 0) {
			filter->param_local_time = param;
		} else if (strcmp(info.name, "audio_peak") == 0) {
			filter->param_audio_peak = param;
		} else if (strcmp(info.name, "gamepad_available") == 0) {
			filter->param_gamepad_available = param;
		} else if (strcmp(info.name, "gamepad_info") == 0) {
			filter->param_gamepad_info = param;
		} else if (strcmp(info.name, "gamepad_buttons") == 0) {
			filter->param_gamepad_buttons = param;
		} else if (strcmp(info.name, "gamepad_presses") == 0) {
			filter->param_gamepad_presses = param;
		} else if (strcmp(info.name, "gamepad_axes") == 0) {
			filter->param_gamepad_axes = param;
		} else if (strcmp(info.name, "gamepad_axes_c") == 0) {
			filter->param_gamepad_axes_c = param;
		} else if (strcmp(info.name, "gamepad_axes_clamped") == 0) {
			filter->param_gamepad_axes_clamped = param;
		} else if (strcmp(info.name, "gamepad_data") == 0) {
			filter->param_gamepad_data = param;
		} else if (strcmp(info.name, "audio_magnitude") == 0) {
			filter->param_audio_magnitude = param;
		} else if (strcmp(info.name, "energy") == 0) {
			filter->param_energy = param;
		} else if (strcmp(info.name, "e_bass") == 0) {
			filter->param_e_bass = param;
		} else if (strcmp(info.name, "e_lows") == 0) {
			filter->param_e_lows = param;
		} else if (strcmp(info.name, "e_mids") == 0) {
			filter->param_e_mids = param;
		} else if (strcmp(info.name, "e_treb") == 0) {
			filter->param_e_treb = param;
		} else if (strcmp(info.name, "bass") == 0) {
			filter->param_bass = param;
		} else if (strcmp(info.name, "lows") == 0) {
			filter->param_lows = param;
		} else if (strcmp(info.name, "mids") == 0) {
			filter->param_mids = param;
		} else if (strcmp(info.name, "treb") == 0) {
			filter->param_treb = param;
		} else if (strcmp(info.name, "bass_c") == 0) {
			filter->param_bass_c = param;
		} else if (strcmp(info.name, "lows_c") == 0) {
			filter->param_lows_c = param;
		} else if (strcmp(info.name, "mids_c") == 0) {
			filter->param_mids_c = param;
		} else if (strcmp(info.name, "treb_c") == 0) {
			filter->param_treb_c = param;
		} else if (strcmp(info.name, "t_bass") == 0) {
			filter->param_t_bass = param;
		} else if (strcmp(info.name, "spec") == 0 && info.type == GS_SHADER_PARAM_TEXTURE && filter->spectrum_declared) {
			filter->param_spec = param;
		} else if (strcmp(info.name, "t_lows") == 0) {
			filter->param_t_lows = param;
		} else if (strcmp(info.name, "t_mids") == 0) {
			filter->param_t_mids = param;
		} else if (strcmp(info.name, "t_treb") == 0) {
			filter->param_t_treb = param;
		} else if (strcmp(info.name, "ef_bass_fast") == 0) {
			filter->param_ef_bass_fast = param;
		} else if (strcmp(info.name, "ef_bass_medium") == 0) {
			filter->param_ef_bass_medium = param;
		} else if (strcmp(info.name, "ef_bass_slow") == 0) {
			filter->param_ef_bass_slow = param;
		} else if (strcmp(info.name, "ef_bass_slug") == 0) {
			filter->param_ef_bass_slug = param;
		} else if (strcmp(info.name, "ef_lows_fast") == 0) {
			filter->param_ef_lows_fast = param;
		} else if (strcmp(info.name, "ef_lows_medium") == 0) {
			filter->param_ef_lows_medium = param;
		} else if (strcmp(info.name, "ef_lows_slow") == 0) {
			filter->param_ef_lows_slow = param;
		} else if (strcmp(info.name, "ef_lows_slug") == 0) {
			filter->param_ef_lows_slug = param;
		} else if (strcmp(info.name, "ef_mids_fast") == 0) {
			filter->param_ef_mids_fast = param;
		} else if (strcmp(info.name, "ef_mids_medium") == 0) {
			filter->param_ef_mids_medium = param;
		} else if (strcmp(info.name, "ef_mids_slow") == 0) {
			filter->param_ef_mids_slow = param;
		} else if (strcmp(info.name, "ef_mids_slug") == 0) {
			filter->param_ef_mids_slug = param;
		} else if (strcmp(info.name, "ef_treb_fast") == 0) {
			filter->param_ef_treb_fast = param;
		} else if (strcmp(info.name, "ef_treb_medium") == 0) {
			filter->param_ef_treb_medium = param;
		} else if (strcmp(info.name, "ef_treb_slow") == 0) {
			filter->param_ef_treb_slow = param;
		} else if (strcmp(info.name, "ef_treb_slug") == 0) {
			filter->param_ef_treb_slug = param;
		} else if (strcmp(info.name, "lf_bass_fast") == 0) {
			filter->param_lf_bass_fast = param;
		} else if (strcmp(info.name, "lf_bass_medium") == 0) {
			filter->param_lf_bass_medium = param;
		} else if (strcmp(info.name, "lf_bass_slow") == 0) {
			filter->param_lf_bass_slow = param;
		} else if (strcmp(info.name, "lf_bass_slug") == 0) {
			filter->param_lf_bass_slug = param;
		} else if (strcmp(info.name, "lf_lows_fast") == 0) {
			filter->param_lf_lows_fast = param;
		} else if (strcmp(info.name, "lf_lows_medium") == 0) {
			filter->param_lf_lows_medium = param;
		} else if (strcmp(info.name, "lf_lows_slow") == 0) {
			filter->param_lf_lows_slow = param;
		} else if (strcmp(info.name, "lf_lows_slug") == 0) {
			filter->param_lf_lows_slug = param;
		} else if (strcmp(info.name, "lf_mids_fast") == 0) {
			filter->param_lf_mids_fast = param;
		} else if (strcmp(info.name, "lf_mids_medium") == 0) {
			filter->param_lf_mids_medium = param;
		} else if (strcmp(info.name, "lf_mids_slow") == 0) {
			filter->param_lf_mids_slow = param;
		} else if (strcmp(info.name, "lf_mids_slug") == 0) {
			filter->param_lf_mids_slug = param;
		} else if (strcmp(info.name, "lf_treb_fast") == 0) {
			filter->param_lf_treb_fast = param;
		} else if (strcmp(info.name, "lf_treb_medium") == 0) {
			filter->param_lf_treb_medium = param;
		} else if (strcmp(info.name, "lf_treb_slow") == 0) {
			filter->param_lf_treb_slow = param;
		} else if (strcmp(info.name, "lf_treb_slug") == 0) {
			filter->param_lf_treb_slug = param;
		} else if (strcmp(info.name, "ViewProj") == 0) {
			// Nothing.
		} else if (strcmp(info.name, "image") == 0) {
			filter->param_image = param;
		} else if (strcmp(info.name, "previous_image") == 0) {
			filter->param_previous_image = param;
		} else if (strcmp(info.name, "previous_output") == 0) {
			filter->param_previous_output = param;
		} else if (filter->transition && strcmp(info.name, "image_a") == 0) {
			filter->param_image_a = param;
		} else if (filter->transition && strcmp(info.name, "image_b") == 0) {
			filter->param_image_b = param;
		} else if (filter->transition && strcmp(info.name, "transition_time") == 0) {
			filter->param_transition_time = param;
		} else if (filter->transition && strcmp(info.name, "convert_linear") == 0) {
			filter->param_convert_linear = param;
		} else {
			struct effect_param_data *cached_data = da_push_back_new(filter->stored_param_list);
			dstr_copy(&cached_data->name, info.name);
			cached_data->type = info.type;
			cached_data->param = param;
			da_init(cached_data->option_values);
			da_init(cached_data->option_labels);
			const size_t annotation_count = gs_param_get_num_annotations(param);
			for (size_t annotation_index = 0; annotation_index < annotation_count; annotation_index++) {
				gs_eparam_t *annotation = gs_param_get_annotation_by_idx(param, annotation_index);
				void *annotation_default = gs_effect_get_default_val(annotation);
				gs_effect_get_param_info(annotation, &info);
				if (strcmp(info.name, "name") == 0 && info.type == GS_SHADER_PARAM_STRING) {
					dstr_copy(&cached_data->display_name, (const char *)annotation_default);
				} else if (strcmp(info.name, "label") == 0 && info.type == GS_SHADER_PARAM_STRING) {
					dstr_copy(&cached_data->display_name, (const char *)annotation_default);
				} else if (strcmp(info.name, "widget_type") == 0 && info.type == GS_SHADER_PARAM_STRING) {
					dstr_copy(&cached_data->widget_type, (const char *)annotation_default);
				} else if (strcmp(info.name, "group") == 0 && info.type == GS_SHADER_PARAM_STRING) {
					dstr_copy(&cached_data->group, (const char *)annotation_default);
				} else if (strcmp(info.name, "tooltip") == 0 && info.type == GS_SHADER_PARAM_STRING) {
					dstr_copy(&cached_data->tooltip, (const char *)annotation_default);
				} else if (strcmp(info.name, "minimum") == 0) {
					if (info.type == GS_SHADER_PARAM_FLOAT || info.type == GS_SHADER_PARAM_VEC2 ||
					    info.type == GS_SHADER_PARAM_VEC3 || info.type == GS_SHADER_PARAM_VEC4) {
						cached_data->minimum.f = *(float *)annotation_default;
					} else if (info.type == GS_SHADER_PARAM_INT) {
						cached_data->minimum.i = *(int *)annotation_default;
					}
				} else if (strcmp(info.name, "maximum") == 0) {
					if (info.type == GS_SHADER_PARAM_FLOAT || info.type == GS_SHADER_PARAM_VEC2 ||
					    info.type == GS_SHADER_PARAM_VEC3 || info.type == GS_SHADER_PARAM_VEC4) {
						cached_data->maximum.f = *(float *)annotation_default;
					} else if (info.type == GS_SHADER_PARAM_INT) {
						cached_data->maximum.i = *(int *)annotation_default;
					}
				} else if (strcmp(info.name, "step") == 0) {
					if (info.type == GS_SHADER_PARAM_FLOAT || info.type == GS_SHADER_PARAM_VEC2 ||
					    info.type == GS_SHADER_PARAM_VEC3 || info.type == GS_SHADER_PARAM_VEC4) {
						cached_data->step.f = *(float *)annotation_default;
					} else if (info.type == GS_SHADER_PARAM_INT) {
						cached_data->step.i = *(int *)annotation_default;
					}
				} else if (strncmp(info.name, "option_", 7) == 0) {
					int id = atoi(info.name + 7);
					if (info.type == GS_SHADER_PARAM_INT) {
						int val = *(int *)annotation_default;
						int *cd = da_insert_new(cached_data->option_values, id);
						*cd = val;

					} else if (info.type == GS_SHADER_PARAM_STRING) {
						struct dstr val = {0};
						dstr_copy(&val, (const char *)annotation_default);
						struct dstr *cs = da_insert_new(cached_data->option_labels, id);
						*cs = val;
					}
				}
				bfree(annotation_default);
			}
		}
	}

end:
	obs_data_release(settings);
}

static const char *shader_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ShaderFilter");
}

static void *shader_filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct shader_filter_data *filter = bzalloc(sizeof(struct shader_filter_data));
	filter->context = source;
	filter->reload_effect = true;

	dstr_init(&filter->last_path);
	dstr_copy(&filter->last_path, obs_data_get_string(settings, "shader_file_name"));
	filter->last_from_file = obs_data_get_bool(settings, "from_file");
	filter->rand_instance_f = (float)((double)rand_interval(0, 10000) / (double)10000);
	filter->rand_activation_f = (float)((double)rand_interval(0, 10000) / (double)10000);

	da_init(filter->stored_param_list);

	// Allocate circular buffers for audio timeseries
	filter->t_bass_buffer = bzalloc(AUDIO_TIMESERIES_BUFFER_WIDTH * sizeof(float));
	filter->t_lows_buffer = bzalloc(AUDIO_TIMESERIES_BUFFER_WIDTH * sizeof(float));
	filter->t_mids_buffer = bzalloc(AUDIO_TIMESERIES_BUFFER_WIDTH * sizeof(float));
	filter->t_treb_buffer = bzalloc(AUDIO_TIMESERIES_BUFFER_WIDTH * sizeof(float));

	// Initialize falloff presets to zero
	filter->ef_bass_fast = filter->ef_bass_medium = filter->ef_bass_slow = filter->ef_bass_slug = 0.0f;
	filter->ef_lows_fast = filter->ef_lows_medium = filter->ef_lows_slow = filter->ef_lows_slug = 0.0f;
	filter->ef_mids_fast = filter->ef_mids_medium = filter->ef_mids_slow = filter->ef_mids_slug = 0.0f;
	filter->ef_treb_fast = filter->ef_treb_medium = filter->ef_treb_slow = filter->ef_treb_slug = 0.0f;

	filter->lf_bass_fast = filter->lf_bass_medium = filter->lf_bass_slow = filter->lf_bass_slug = 0.0f;
	filter->lf_lows_fast = filter->lf_lows_medium = filter->lf_lows_slow = filter->lf_lows_slug = 0.0f;
	filter->lf_mids_fast = filter->lf_mids_medium = filter->lf_mids_slow = filter->lf_mids_slug = 0.0f;
	filter->lf_treb_fast = filter->lf_treb_medium = filter->lf_treb_slow = filter->lf_treb_slug = 0.0f;

	load_output_effect(filter);
	obs_source_update(source, settings);

	return filter;
}

static void shader_filter_destroy(void *data)
{
	struct shader_filter_data *filter = data;
	shader_filter_clear_params(filter);

	obs_enter_graphics();
	if (filter->effect)
		gs_effect_destroy(filter->effect);
	if (filter->output_effect)
		gs_effect_destroy(filter->output_effect);
	if (filter->input_texrender)
		gs_texrender_destroy(filter->input_texrender);
	if (filter->output_texrender)
		gs_texrender_destroy(filter->output_texrender);
	if (filter->previous_input_texrender)
		gs_texrender_destroy(filter->previous_input_texrender);
	if (filter->previous_output_texrender)
		gs_texrender_destroy(filter->previous_output_texrender);
	if (filter->sprite_buffer)
		gs_vertexbuffer_destroy(filter->sprite_buffer);
	if (filter->t_bass_render)
		gs_texrender_destroy(filter->t_bass_render);
	if (filter->t_lows_render)
		gs_texrender_destroy(filter->t_lows_render);
	if (filter->t_mids_render)
		gs_texrender_destroy(filter->t_mids_render);
	if (filter->t_treb_render)
		gs_texrender_destroy(filter->t_treb_render);
    // ef_/lf_ texrenders removed; falloff presets now uploaded as floats
	obs_leave_graphics();

	dstr_free(&filter->last_path);
	da_free(filter->stored_param_list);

	if (filter->t_bass_buffer)
		bfree(filter->t_bass_buffer);
	if (filter->t_lows_buffer)
		bfree(filter->t_lows_buffer);
	if (filter->t_mids_buffer)
		bfree(filter->t_mids_buffer);
	if (filter->t_treb_buffer)
		bfree(filter->t_treb_buffer);

	shader_filter_release_audio_analysis(filter);
	if (filter->audio_source_name)
		bfree(filter->audio_source_name);

	bfree(filter);
}

static bool shader_filter_from_file_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	struct shader_filter_data *filter = obs_properties_get_param(props);

	bool from_file = obs_data_get_bool(settings, "from_file");

	obs_property_set_visible(obs_properties_get(props, "shader_text"), !from_file);
	obs_property_set_visible(obs_properties_get(props, "shader_file_name"), from_file);

	if (from_file != filter->last_from_file) {
		filter->reload_effect = true;
	}
	filter->last_from_file = from_file;

	return true;
}

static bool shader_filter_text_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	struct shader_filter_data *filter = obs_properties_get_param(props);
	if (!filter)
		return false;

	const char *shader_text = obs_data_get_string(settings, "shader_text");
	bool can_convert = strstr(shader_text, "void mainImage( out vec4") || strstr(shader_text, "void mainImage(out vec4") ||
			   strstr(shader_text, "void main()") || strstr(shader_text, "vec4 effect(vec4");
	obs_property_t *shader_convert = obs_properties_get(props, "shader_convert");
	bool visible = obs_property_visible(obs_properties_get(props, "shader_text"));
	if (obs_property_visible(shader_convert) != (can_convert && visible)) {
		obs_property_set_visible(shader_convert, can_convert && visible);
		return true;
	}
	return false;
}

static bool shader_filter_file_name_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	struct shader_filter_data *filter = obs_properties_get_param(props);
	const char *new_file_name = obs_data_get_string(settings, obs_property_name(p));

	if ((dstr_is_empty(&filter->last_path) && strlen(new_file_name)) ||
	    (filter->last_path.array && dstr_cmp(&filter->last_path, new_file_name) != 0)) {
		filter->reload_effect = true;
		dstr_copy(&filter->last_path, new_file_name);
		size_t l = strlen(new_file_name);
		if (l > 7 && strncmp(new_file_name + l - 7, ".effect", 7) == 0) {
			obs_data_set_bool(settings, "override_entire_effect", true);
		} else if (l > 7 && strncmp(new_file_name + l - 7, ".shader", 7) == 0) {
			obs_data_set_bool(settings, "override_entire_effect", false);
		}
	}

	return false;
}

static bool shader_filter_reload_effect_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct shader_filter_data *filter = data;

	filter->reload_effect = true;

	obs_source_update(filter->context, NULL);

	return false;
}

static bool add_source_to_list(void *data, obs_source_t *source)
{
	obs_property_t *p = data;
	const char *name = obs_source_get_name(source);
	size_t count = obs_property_list_item_count(p);
	size_t idx = 0;
	while (idx < count && strcmp(name, obs_property_list_item_string(p, idx)) > 0)
		idx++;
	obs_property_list_insert_string(p, idx, name, name);
	return true;
}

static bool is_var_char(char ch)
{
	return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

static void convert_if_defined(struct dstr *effect_text)
{
	char *pos = strstr(effect_text->array, "#if defined(");
	while (pos) {
		size_t diff = pos - effect_text->array;
		char *ch = strstr(effect_text->array + diff + 12, ")\n");
		pos = strstr(effect_text->array + diff + 12, "#if defined(");
		if (ch && (!pos || ch < pos)) {
			*ch = ' ';
			dstr_remove(effect_text, diff, 12);
			dstr_insert(effect_text, diff, "#ifdef ");
			pos = strstr(effect_text->array + diff + 5, "#if defined(");
		}
	}
}

static void convert_atan(struct dstr *effect_text)
{
	char *pos = strstr(effect_text->array, "atan(");
	while (pos) {
		if (is_var_char(*(pos - 1))) {
			pos = strstr(pos + 5, "atan(");
			continue;
		}
		size_t diff = pos - effect_text->array;
		char *comma = strstr(pos + 5, ",");
		char *divide = strstr(pos + 5, "/");
		if (!comma && !divide)
			return;

		int depth = 0;
		char *open = strstr(pos + 5, "(");
		char *close = strstr(pos + 5, ")");
		if (!close)
			return;
		do {
			while (open && open < close) {
				depth++;
				open = strstr(open + 1, "(");
			}
			while (depth > 0 && close && (!open || close < open)) {
				depth--;
				if (depth == 0 && (!comma || comma < close))
					comma = strstr(close + 1, ",");
				if (depth == 0 && (!divide || divide < close))
					divide = strstr(close + 1, "/");
				if (comma && open && comma > open && comma < close)
					comma = NULL;
				if (divide && open && divide > open && divide < close)
					divide = NULL;

				open = strstr(close + 1, "(");
				close = strstr(close + 1, ")");
			}
		} while (depth > 0 && close);

		if (close && comma && comma < close && (!open || comma < open)) {
			//*comma = '/';
			dstr_insert(effect_text, diff + 4, "2");
		}
		if (close && divide && divide < close && (!open || divide < open)) {
			*divide = ',';
			dstr_insert(effect_text, diff + 4, "2");
		}

		pos = strstr(effect_text->array + diff + 5, "atan(");
	}
}

static void insert_begin_of_block(struct dstr *effect_text, size_t diff, char *insert)
{
	int depth = 0;
	char *ch = effect_text->array + diff;
	while (ch > effect_text->array && (*ch == ' ' || *ch == '\t'))
		ch--;
	while (ch > effect_text->array) {
		while (*ch != '=' && *ch != '(' && *ch != ')' && *ch != '+' && *ch != '-' && *ch != '/' && *ch != '*' &&
		       *ch != ' ' && *ch != '\t' && ch > effect_text->array)
			ch--;
		if (*ch == '(') {
			if (depth == 0) {
				dstr_insert(effect_text, ch - effect_text->array + 1, insert);
				return;
			}
			ch--;
			depth--;
		} else if (*ch == ')') {
			ch--;
			depth++;
		} else if (*ch == '=') {
			dstr_insert(effect_text, ch - effect_text->array + 1, insert);
			return;
		} else if (*ch == '+' || *ch == '-' || *ch == '/' || *ch == '*' || *ch == ' ' || *ch == '\t') {
			if (depth == 0) {
				dstr_insert(effect_text, ch - effect_text->array + 1, insert);
				return;
			}
			ch--;
		}
	}
}

static void insert_end_of_block(struct dstr *effect_text, size_t diff, char *insert)
{
	int depth = 0;
	char *ch = effect_text->array + diff;
	while (*ch == ' ' || *ch == '\t')
		ch++;
	while (*ch != 0) {
		while (*ch != ';' && *ch != '(' && *ch != ')' && *ch != '+' && *ch != '-' && *ch != '/' && *ch != '*' &&
		       *ch != ' ' && *ch != '\t' && *ch != ',' && *ch != 0)
			ch++;
		if (*ch == '(') {
			ch++;
			depth++;
		} else if (*ch == ')') {
			if (depth == 0) {
				dstr_insert(effect_text, ch - effect_text->array, insert);
				return;
			}
			ch++;
			depth--;
		} else if (*ch == ';') {
			dstr_insert(effect_text, ch - effect_text->array, insert);
			return;
		} else if (*ch == '+' || *ch == '-' || *ch == '/' || *ch == '*' || *ch == ' ' || *ch == '\t' || *ch == ',') {
			if (depth == 0) {
				dstr_insert(effect_text, ch - effect_text->array, insert);
				return;
			}
			ch++;
		}
	}
}

static void convert_mat_mul_var(struct dstr *effect_text, struct dstr *var_function_name)
{
	char *pos = strstr(effect_text->array, var_function_name->array);
	while (pos) {
		if (is_var_char(*(pos - 1)) || is_var_char(*(pos + var_function_name->len))) {
			pos = strstr(pos + var_function_name->len, var_function_name->array);
			continue;
		}
		size_t diff = pos - effect_text->array;
		char *ch = pos + var_function_name->len;
		while (*ch == ' ' || *ch == '\t')
			ch++;
		if (*ch == '*' && *(ch + 1) != '=') {
			size_t diff3 = ch - effect_text->array + 1;
			*ch = ',';
			insert_end_of_block(effect_text, diff3, ")");
			dstr_insert(effect_text, diff, "mul(");
			pos = strstr(effect_text->array + diff + 4 + var_function_name->len, var_function_name->array);
			continue;
		}
		ch = pos - 1;
		while ((*ch == ' ' || *ch == '\t') && ch > effect_text->array)
			ch--;
		if (*ch == '=' && *(ch - 1) == '*') {
			char *end = ch - 2;
			while ((*end == ' ' || *end == '\t') && end > effect_text->array)
				end--;
			char *start = end;
			while ((is_var_char(*start) || *start == '.') && start > effect_text->array)
				start--;
			start++;
			end++;

			diff = ch - 1 - effect_text->array;

			struct dstr insert = {0};
			dstr_init_copy(&insert, "= mul(");
			dstr_ncat(&insert, start, end - start);
			dstr_cat(&insert, ",");
			dstr_remove(effect_text, diff, 2);
			dstr_insert(effect_text, diff, insert.array);

			char *line_end = effect_text->array + diff;
			while (*line_end != ';' && *line_end != 0)
				line_end++;

			dstr_insert(effect_text, line_end - effect_text->array, ")");

			pos = strstr(effect_text->array + diff + insert.len + 1 + var_function_name->len, var_function_name->array);
			dstr_free(&insert);

			continue;
		} else if (*ch == '*') {
			size_t diff2 = ch - effect_text->array - 1;
			*ch = ',';
			insert_end_of_block(effect_text, diff + var_function_name->len, ")");
			insert_begin_of_block(effect_text, diff2, "mul(");

			pos = strstr(effect_text->array + diff + 4 + var_function_name->len, var_function_name->array);
			continue;
		}

		pos = strstr(effect_text->array + diff + var_function_name->len, var_function_name->array);
	}
}

static void convert_mat_mul(struct dstr *effect_text, char *var_type)
{
	size_t len = strlen(var_type);
	char *pos = strstr(effect_text->array, var_type);
	while (pos) {
		if (is_var_char(*(pos - 1))) {
			pos = strstr(pos + len, var_type);
			continue;
		}
		size_t diff = pos - effect_text->array;
		char *begin = pos + len;
		if (*begin == '(') {
			char *ch = pos - 1;
			while ((*ch == ' ' || *ch == '\t') && ch > effect_text->array)
				ch--;
			if (*ch == '=' && *(ch - 1) == '*') {
				// mat constructor with *= in front of it
				char *end = ch - 2;
				while ((*end == ' ' || *end == '\t') && end > effect_text->array)
					end--;
				char *start = end;
				while ((is_var_char(*start) || *start == '.') && start > effect_text->array)
					start--;
				start++;
				end++;

				size_t diff2 = ch - effect_text->array - 1;

				struct dstr insert = {0};
				dstr_init_copy(&insert, "= mul(");
				dstr_ncat(&insert, start, end - start);
				dstr_cat(&insert, ",");
				dstr_remove(effect_text, diff2, 2);
				dstr_insert(effect_text, diff2, insert.array);

				char *line_end = effect_text->array + diff2;
				while (*line_end != ';' && *line_end != 0)
					line_end++;

				dstr_insert(effect_text, line_end - effect_text->array, ")");

				pos = strstr(effect_text->array + diff + insert.len + len + 1, var_type);
				dstr_free(&insert);

				continue;
			} else if (*ch == '*') {
				// mat constructor with * in front of it
				size_t diff2 = ch - effect_text->array - 1;
				*ch = ',';
				insert_end_of_block(effect_text, diff + len, ")");
				insert_begin_of_block(effect_text, diff2, "mul(");

				pos = strstr(effect_text->array + diff + len + 4, var_type);
				continue;
			}

			int depth = 1;
			ch = begin + 1;
			while (*ch != 0) {
				while (*ch != ';' && *ch != '(' && *ch != ')' && *ch != '+' && *ch != '-' && *ch != '/' &&
				       *ch != '*' && *ch != 0)
					ch++;
				if (*ch == '(') {
					ch++;
					depth++;
				} else if (*ch == ')') {
					if (depth == 0) {
						break;
					}
					ch++;
					depth--;
				} else if (*ch == '*') {
					if (depth == 0) {
						//mat constructor follow by *
						*ch = ',';
						insert_end_of_block(effect_text, ch - effect_text->array + 1, ")");
						dstr_insert(effect_text, diff, "mul(");
						break;
					}
					ch++;
				} else if (*ch == ';') {
					break;
				} else if (depth == 0) {
					break;
				} else if (*ch != 0) {
					ch++;
				}
			}
		}
		if (*begin != ' ' && *begin != '\t') {
			pos = strstr(pos + len, var_type);
			continue;
		}
		while (*begin == ' ' || *begin == '\t')
			begin++;
		if (!is_var_char(*begin)) {
			pos = strstr(pos + len, var_type);
			continue;
		}
		char *end = begin;
		while (is_var_char(*end))
			end++;
		struct dstr var_function_name = {0};
		dstr_ncat(&var_function_name, begin, end - begin);

		convert_mat_mul_var(effect_text, &var_function_name);
		dstr_free(&var_function_name);

		pos = strstr(effect_text->array + diff + len, var_type);
	}
}

static bool is_in_function(struct dstr *effect_text, size_t diff)
{
	char *pos = effect_text->array + diff;
	int depth = 0;
	while (depth >= 0) {
		char *end = strstr(pos, "}");
		char *begin = strstr(pos, "{");
		if (end && begin && begin < end) {
			pos = begin + 1;
			depth++;
		} else if (end && begin && end < begin) {
			pos = end + 1;
			depth--;
		} else if (end) {
			pos = end + 1;
			depth--;
		} else if (begin && depth < 0) {
			break;
		} else if (begin) {
			pos = begin + 1;
			depth++;
		} else {
			break;
		}
	}
	return depth != 0;
}

static void convert_init(struct dstr *effect_text, char *name)
{
	const size_t len = strlen(name);
	char extra = 0;
	char *pos = strstr(effect_text->array, name);
	while (pos) {
		size_t diff = pos - effect_text->array;
		if (pos > effect_text->array && is_var_char(*(pos - 1))) {
			pos = strstr(effect_text->array + diff + len, name);
			continue;
		}
		char *ch = pos + len;
		if (*ch >= '2' && *ch <= '9') {
			extra = *ch;
			ch++;
		} else {
			extra = 0;
		}
		if (*ch != ' ' && *ch != '\t') {
			pos = strstr(effect_text->array + diff + len, name);
			continue;
		}
		char *begin = pos - 1;
		while (begin > effect_text->array && (*begin == ' ' || *begin == '\t'))
			begin--;
		if (*begin == '(' || *begin == ',' ||
		    (is_var_char(*begin) && memcmp("uniform", begin - 6, 7) != 0 && memcmp("const", begin - 4, 5) != 0)) {
			pos = strstr(effect_text->array + diff + len, name);
			continue;
		}
		if (!is_in_function(effect_text, diff)) {
			while (true) {
				while (*ch != 0 && (*ch == ' ' || *ch == '\t'))
					ch++;
				while (is_var_char(*ch))
					ch++;
				while (*ch != 0 && (*ch == ' ' || *ch == '\t'))
					ch++;
				if (*ch != ',')
					break;
				*ch = ';';
				diff = ch - effect_text->array;
				dstr_insert(effect_text, diff + 1, " ");
				if (extra) {
					dstr_insert_ch(effect_text, diff + 1, extra);
				}
				dstr_insert(effect_text, diff + 1, name);
				if (memcmp("const", begin - 4, 5) == 0) {
					dstr_insert(effect_text, diff + 1, "\nconst ");
					diff += 9;
				} else {
					dstr_insert(effect_text, diff + 1, "\nuniform ");
					diff += 11;
				}
				if (extra)
					diff++;
				ch = effect_text->array + diff + len;
			}

			if ((*ch == '=' && *(ch + 1) != '=') || *ch == ';') {
				if (memcmp("uniform", begin - 6, 7) != 0 && memcmp("const", begin - 4, 5) != 0) {
					dstr_insert(effect_text, begin - effect_text->array + 1, "uniform ");
					diff += 8;
				}
			}
		}
		pos = strstr(effect_text->array + diff + len, name);
	}
}

static void convert_vector_init(struct dstr *effect_text, char *name, int count)
{
	const size_t len = strlen(name);

	char *pos = strstr(effect_text->array, name);
	while (pos) {
		size_t diff = pos - effect_text->array;
		char *ch = pos + len;
		int depth = 0;
		int function_depth = -1;
		bool only_one = true;
		bool only_numbers = true;
		bool only_float = true;
		while (*ch != 0 && (only_numbers || only_float)) {
			if (*ch == '(') {
				depth++;
			} else if (*ch == ')') {
				if (depth == 0) {
					break;
				}
				depth--;
				if (depth == function_depth) {
					function_depth = -1;
				}
			} else if (*ch == ',') {
				if (depth == 0) {
					only_one = false;
				}
			} else if (*ch == ';') {
				only_one = false;
				break;
			} else if (is_var_char(*ch) && (*ch < '0' || *ch > '9')) {
				only_numbers = false;
				char *begin = ch;
				while (is_var_char(*ch))
					ch++;
				if (*ch == '.') {
					size_t c = 1;
					while (is_var_char(*(ch + c)))
						c++;
					if (c != 2 && function_depth < 0)
						only_float = false;
					ch += c - 1;
				} else if (function_depth >= 0) {
					ch--;
				} else if (*ch == '(' &&
					   (strncmp(begin, "length(", 7) == 0 || strncmp(begin, "float(", 6) == 0 ||
					    strncmp(begin, "uint(", 5) == 0 || strncmp(begin, "int(", 4) == 0 ||
					    strncmp(begin, "asfloat(", 8) == 0 || strncmp(begin, "asdouble(", 9) == 0 ||
					    strncmp(begin, "asint(", 6) == 0 || strncmp(begin, "asuint(", 7) == 0 ||
					    strncmp(begin, "determinant(", 12) == 0 || strncmp(begin, "distance(", 9) == 0 ||
					    strncmp(begin, "dot(", 4) == 0 || strncmp(begin, "countbits(", 10) == 0 ||
					    strncmp(begin, "firstbithigh(", 13) == 0 || strncmp(begin, "firstbitlow(", 12) == 0 ||
					    strncmp(begin, "reversebits(", 12) == 0)) {
					function_depth = depth;
					depth++;
				} else if (*ch == '(' &&
					   (strncmp(begin, "abs(", 4) == 0 || strncmp(begin, "acos(", 5) == 0 ||
					    strncmp(begin, "asin(", 5) == 0 || strncmp(begin, "atan(", 5) == 0 ||
					    strncmp(begin, "atan2(", 6) == 0 || strncmp(begin, "ceil(", 5) == 0 ||
					    strncmp(begin, "clamp(", 6) == 0 || strncmp(begin, "cos(", 4) == 0 ||
					    strncmp(begin, "cosh(", 5) == 0 || strncmp(begin, "ddx(", 4) == 0 ||
					    strncmp(begin, "ddy(", 4) == 0 || strncmp(begin, "degrees(", 8) == 0 ||
					    strncmp(begin, "exp(", 4) == 0 || strncmp(begin, "exp2(", 5) == 0 ||
					    strncmp(begin, "floor(", 6) == 0 || strncmp(begin, "fma(", 4) == 0 ||
					    strncmp(begin, "fmod(", 5) == 0 || strncmp(begin, "frac(", 5) == 0 ||
					    strncmp(begin, "frexp(", 6) == 0 || strncmp(begin, "fwidth(", 7) == 0 ||
					    strncmp(begin, "ldexp(", 6) == 0 || strncmp(begin, "lerp(", 5) == 0 ||
					    strncmp(begin, "log(", 4) == 0 || strncmp(begin, "log10(", 6) == 0 ||
					    strncmp(begin, "log2(", 5) == 0 || strncmp(begin, "mad(", 4) == 0 ||
					    strncmp(begin, "max(", 4) == 0 || strncmp(begin, "min(", 4) == 0 ||
					    strncmp(begin, "modf(", 5) == 0 || strncmp(begin, "mod(", 4) == 0 ||
					    strncmp(begin, "mul(", 4) == 0 || strncmp(begin, "normalize(", 10) == 0 ||
					    strncmp(begin, "pow(", 4) == 0 || strncmp(begin, "radians(", 8) == 0 ||
					    strncmp(begin, "rcp(", 4) == 0 || strncmp(begin, "reflect(", 8) == 0 ||
					    strncmp(begin, "refract(", 8) == 0 || strncmp(begin, "round(", 6) == 0 ||
					    strncmp(begin, "rsqrt(", 6) == 0 || strncmp(begin, "saturate(", 9) == 0 ||
					    strncmp(begin, "sign(", 5) == 0 || strncmp(begin, "sin(", 4) == 0 ||
					    strncmp(begin, "sincos(", 7) == 0 || strncmp(begin, "sinh(", 5) == 0 ||
					    strncmp(begin, "smoothstep(", 11) == 0 || strncmp(begin, "sqrt(", 5) == 0 ||
					    strncmp(begin, "step(", 5) == 0 || strncmp(begin, "tan(", 4) == 0 ||
					    strncmp(begin, "tanh(", 5) == 0 || strncmp(begin, "transpose(", 10) == 0 ||
					    strncmp(begin, "trunc(", 6) == 0)) {
					depth++;
				} else {
					struct dstr find = {0};
					bool found = false;
					dstr_copy(&find, "float ");
					dstr_ncat(&find, begin, ch - begin + (*ch == '(' ? 1 : 0));
					char *t = strstr(effect_text->array, find.array);
					while (t != NULL) {
						t += find.len;
						if (*ch == '(') {
							found = true;
							break;
						} else if (!is_var_char(*t)) {
							found = true;
							break;
						}
						t = strstr(t, find.array);
					}
					if (!found) {
						dstr_copy(&find, "int ");
						dstr_ncat(&find, begin, ch - begin + (*ch == '(' ? 1 : 0));
						t = strstr(effect_text->array, find.array);
						while (t != NULL) {
							t += find.len;
							if (*ch == '(') {
								found = true;
								break;
							} else if (!is_var_char(*t)) {
								found = true;
								break;
							}
							t = strstr(t, find.array);
						}
					}
					if (!found && *ch != '(') {
						dstr_copy(&find, "#define ");
						dstr_ncat(&find, begin, ch - begin);
						char *t = strstr(effect_text->array, find.array);
						while (t != NULL) {
							t += find.len;
							if (!is_var_char(*t)) {
								while (*t == ' ' || *t == '\t')
									t++;
								if (*t >= '0' && *t <= '9') {
									found = true;
									break;
								}
							}
							t = strstr(t, find.array);
						}
					}
					if (!found) {
						only_float = false;
					} else if (*ch == '(') {
						function_depth = depth;
					}
					dstr_free(&find);
					ch--;
				}
			} else if ((*ch < '0' || *ch > '9') && *ch != '.' && *ch != ' ' && *ch != '\t' && *ch != '+' &&
				   *ch != '-' && *ch != '*' && *ch != '/') {
				only_numbers = false;
			}
			ch++;
		}
		size_t end_diff = ch - effect_text->array;
		if (count > 1 && only_one && (only_numbers || only_float)) {
			//only 1 simple arg in the float4
			struct dstr found = {0};
			dstr_init(&found);
			dstr_ncat(&found, pos, ch - pos + 1);

			struct dstr replacement = {0};
			dstr_init_copy(&replacement, name);
			dstr_ncat(&replacement, pos + len, ch - (pos + len));
			for (int i = 1; i < count; i++) {
				dstr_cat(&replacement, ",");
				dstr_ncat(&replacement, pos + len, ch - (pos + len));
			}
			dstr_cat(&replacement, ")");

			dstr_replace(effect_text, found.array, replacement.array);

			end_diff -= found.len;
			end_diff += replacement.len;
			dstr_free(&replacement);
			dstr_free(&found);
		}

		if (!is_in_function(effect_text, diff)) {
			char *begin = effect_text->array + diff - 1;
			while (begin > effect_text->array && (*begin == ' ' || *begin == '\t'))
				begin--;
			if (*begin == '=') {
				begin--;
				while (begin > effect_text->array && (*begin == ' ' || *begin == '\t'))
					begin--;
				while (is_var_char(*begin))
					begin--;
				while (begin > effect_text->array && (*begin == ' ' || *begin == '\t'))
					begin--;
				if (memcmp(name, begin - len + 2, len - 1) == 0) {

					begin -= len - 1;
					while (begin > effect_text->array && (*begin == ' ' || *begin == '\t'))
						begin--;
					if (memcmp("uniform", begin - 6, 7) != 0 && memcmp("const", begin - 4, 5) != 0) {
						dstr_insert(effect_text, begin - effect_text->array + 1, "uniform ");
						diff += 8;
						end_diff += 8;
					}
					if (effect_text->array[end_diff] == ')') {
						if (count > 1) {
							effect_text->array[end_diff] = '}';
							dstr_remove(effect_text, diff, len);
							dstr_insert(effect_text, diff, "{");
						} else {
							dstr_remove(effect_text, end_diff, 1);
							dstr_remove(effect_text, diff, len);
						}
					}
				}
			}
		}

		pos = strstr(effect_text->array + diff + len, name);
	}
}

static void convert_if0(struct dstr *effect_text)
{
	char *begin = strstr(effect_text->array, "#if 0");
	while (begin) {
		size_t diff = begin - effect_text->array;
		char *end = strstr(begin, "#endif");
		if (!end)
			return;
		char *el = strstr(begin, "#else");
		char *eli = strstr(begin, "#elif");
		if (eli && eli < end && (!el || eli < el)) {
			//replace #elif with #if
			dstr_remove(effect_text, diff, el - begin + 5);
			dstr_insert(effect_text, diff, "#if");
			begin = strstr(effect_text->array + diff + 3, "#if 0");
		} else if (el && el < end) {
			dstr_remove(effect_text, end - effect_text->array,
				    6); // #endif
			dstr_remove(effect_text, diff, el - begin + 5);
			begin = strstr(effect_text->array + diff, "#if 0");
		} else if (!el || el > end) {
			dstr_remove(effect_text, diff, end - begin + 6);
			begin = strstr(effect_text->array + diff, "#if 0");
		} else {
			begin = strstr(effect_text->array + diff + 5, "#if 0");
		}
	}
}

static void convert_if1(struct dstr *effect_text)
{
	char *begin = strstr(effect_text->array, "#if 1");
	while (begin) {
		size_t diff = begin - effect_text->array;
		char *end = strstr(begin, "#endif");
		if (!end)
			return;
		char *el = strstr(begin, "#el");
		if (el && el < end) {
			dstr_remove(effect_text, el - effect_text->array,
				    end - el + 6); // #endif
			end = strstr(effect_text->array + diff, "\n");
			if (end)
				dstr_remove(effect_text, diff, end - (effect_text->array + diff));
			begin = strstr(effect_text->array + diff, "#if 1");
		} else if (!el || el > end) {
			dstr_remove(effect_text, end - effect_text->array, 6);
			end = strstr(effect_text->array + diff, "\n");
			if (end)
				dstr_remove(effect_text, diff, end - (effect_text->array + diff));
			begin = strstr(effect_text->array + diff, "#if 1");
		} else {
			begin = strstr(effect_text->array + diff + 5, "#if 1");
		}
	}
}

static void convert_define(struct dstr *effect_text)
{
	char *pos = strstr(effect_text->array, "#define ");
	while (pos) {
		size_t diff = pos - effect_text->array;
		char *start = pos + 8;
		while (*start == ' ' || *start == '\t')
			start++;
		char *end = start;
		while (*end != ' ' && *end != '\t' && *end != '\n' && *end != 0)
			end++;
		char *t = strstr(start, "(");
		if (t && t < end) {
			// don't replace macro
			pos = strstr(effect_text->array + diff + 8, "#define ");
			continue;
		}

		struct dstr def_name = {0};
		dstr_ncat(&def_name, start, end - start);

		start = end;
		while (*start == ' ' || *start == '\t')
			start++;

		end = start;
		while (*end != '\n' && *end != 0 && (*end != '/' || *(end + 1) != '/'))
			end++;

		t = strstr(start, "(");
		if (*start == '(' || (t && t < end)) {
			struct dstr replacement = {0};
			dstr_ncat(&replacement, start, end - start);

			dstr_remove(effect_text, diff, end - (effect_text->array + diff));

			dstr_replace(effect_text, def_name.array, replacement.array);

			dstr_free(&replacement);
			pos = strstr(effect_text->array + diff, "#define ");
		} else {
			pos = strstr(effect_text->array + diff + 8, "#define ");
		}
		dstr_free(&def_name);
	}
}

static void convert_return(struct dstr *effect_text, struct dstr *var_name, size_t main_diff)
{
	size_t count = 0;
	char *pos = strstr(effect_text->array + main_diff, var_name->array);
	while (pos) {
		if (is_var_char(*(pos - 1)) || (*(pos - 1) == '/' && *(pos - 2) == '/')) {
			pos = strstr(pos + var_name->len, var_name->array);
			continue;
		}
		size_t diff = pos - effect_text->array;
		char *ch = pos + var_name->len;
		if (*ch == '.') {
			ch++;
			while (is_var_char(*ch))
				ch++;
		}
		while (*ch == ' ' || *ch == '\t')
			ch++;

		if (*ch == '=' || (*(ch + 1) == '=' && (*ch == '*' || *ch == '/' || *ch == '+' || *ch == '-'))) {
			char *bpos = pos - 1;
			while (*bpos != '\n' && (*bpos != '/' || *(bpos + 1) != '/') && bpos > effect_text->array)
				bpos--;
			if (*bpos == '/' && *(bpos + 1) == '/') {
				//comment line
			} else {
				count++;
			}
		}

		pos = strstr(effect_text->array + diff + var_name->len, var_name->array);
	}
	if (count == 0)
		return;
	if (count == 1) {
		pos = strstr(effect_text->array + main_diff, var_name->array);
		while (pos) {
			if (is_var_char(*(pos - 1))) {
				pos = strstr(pos + var_name->len, var_name->array);
				continue;
			}
			char *bpos = pos - 1;
			while (*bpos != '\n' && (*bpos != '/' || *(bpos + 1) != '/') && bpos > effect_text->array)
				bpos--;
			if (*bpos == '/' && *(bpos + 1) == '/') {
				pos = strstr(pos + var_name->len, var_name->array);
				continue;
			}

			size_t diff = pos - effect_text->array;
			char *ch = pos + var_name->len;
			bool contains_alpha = true;
			if (*ch == '.') {
				contains_alpha = false;
				ch++;
				while (is_var_char(*ch)) {
					if (*ch == 'a' || *ch == 'A' || *ch == 'w' || *ch == 'W')
						contains_alpha = true;
					ch++;
				}
			}

			while (*ch == ' ' || *ch == '\t')
				ch++;

			if (*ch == '=') {
				dstr_remove(effect_text, diff, ch - pos + 1);
				if (!contains_alpha) {
					dstr_insert(effect_text, diff, "return float4(");
					while (diff < effect_text->len && effect_text->array[diff] != ';')
						diff++;
					dstr_insert(effect_text, diff, ",1.0)");
				} else {
					dstr_insert(effect_text, diff, "return ");
				}
				return;
			} else if (*(ch + 1) == '=' && (*ch == '*' || *ch == '/' || *ch == '+' || *ch == '-')) {
				dstr_remove(effect_text, diff, ch - pos + 2);
				if (!contains_alpha) {
					dstr_insert(effect_text, diff, "return float4(");
					while (diff < effect_text->len && effect_text->array[diff] != ';')
						diff++;
					dstr_insert(effect_text, diff, ",1.0)");
				} else {
					dstr_insert(effect_text, diff, "return ");
				}
				return;
			}

			pos = strstr(effect_text->array + diff + var_name->len, var_name->array);
		}
		return;
	}

	size_t replaced = 0;
	size_t start_diff = 0;
	bool declared = false;
	pos = strstr(effect_text->array + main_diff, "{");
	if (pos) {
		size_t insert_diff = pos - effect_text->array + 1;
		dstr_insert(effect_text, insert_diff, " = float4(0.0,0.0,0.0,1.0);\n");
		dstr_insert(effect_text, insert_diff, var_name->array);
		dstr_insert(effect_text, insert_diff, "\n\tfloat4 ");
		declared = true;
		start_diff = insert_diff - main_diff + 37 + var_name->len;
	}

	pos = strstr(effect_text->array + main_diff + start_diff, var_name->array);
	while (pos) {
		size_t diff = pos - effect_text->array;
		char *ch = pos + var_name->len;
		bool part = false;
		if (*ch == '.') {
			part = true;
			ch++;
			while (is_var_char(*ch))
				ch++;
		}
		while (*ch == ' ' || *ch == '\t')
			ch++;

		if (*ch == '=') {
			replaced++;
			if (replaced == 1 && !declared) {
				if (part) {
					dstr_insert(effect_text, diff, " = float4(0.0,0.0,0.0,1.0);\n");
					dstr_insert(effect_text, diff, var_name->array);
					dstr_insert(effect_text, diff, "float4 ");
					diff += 35 + var_name->len;
				} else {
					dstr_insert(effect_text, diff, "float4 ");
					diff += 7;
				}
			} else if (replaced == count) {
				if (part) {
					while (*ch != ';' && *ch != 0)
						ch++;
					diff = ch - effect_text->array + 1;
					dstr_insert(effect_text, diff, ";");
					dstr_insert(effect_text, diff, var_name->array);
					dstr_insert(effect_text, diff, "\n\treturn ");
				} else {
					dstr_remove(effect_text, diff, ch - pos + 1);
					dstr_insert(effect_text, diff, "return ");
				}
				return;
			}
		} else if (*(ch + 1) == '=' && (*ch == '*' || *ch == '/' || *ch == '+' || *ch == '-')) {
			replaced++;
			if (replaced == 1 && !declared) {
				dstr_insert(effect_text, diff, " = float4(0.0,0.0,0.0,1.0);\n");
				dstr_insert(effect_text, diff, var_name->array);
				dstr_insert(effect_text, diff, "float4 ");
				diff += 35 + var_name->len;
			} else if (replaced == count) {
				while (*ch != ';' && *ch != 0)
					ch++;
				diff = ch - effect_text->array + 1;
				dstr_insert(effect_text, diff, ";");
				dstr_insert(effect_text, diff, var_name->array);
				dstr_insert(effect_text, diff, "\n\treturn ");
				return;
			}
		}

		pos = strstr(effect_text->array + diff + var_name->len, var_name->array);
	}
}

static bool shader_filter_convert(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	if (!data)
		return false;
	struct shader_filter_data *filter = data;
	obs_data_t *settings = obs_source_get_settings(filter->context);
	if (!settings)
		return false;
	struct dstr effect_text = {0};
	dstr_init_copy(&effect_text, obs_data_get_string(settings, "shader_text"));

	//convert_define(&effect_text);

	size_t start_diff = 24;
	bool main_no_args = false;
	int uv = 0;
	char *main_pos = strstr(effect_text.array, "void mainImage(out vec4");
	if (!main_pos) {
		main_pos = strstr(effect_text.array, "void mainImage( out vec4");
		start_diff++;
	}
	if (!main_pos) {
		main_pos = strstr(effect_text.array, "void main()");
		if (main_pos)
			main_no_args = true;
	}
	if (!main_pos) {
		main_pos = strstr(effect_text.array, "vec4 effect(vec4");
		start_diff = 17;
		uv = 1;
	}

	if (!main_pos) {
		dstr_free(&effect_text);
		obs_data_release(settings);
		return false;
	}
	size_t main_diff = main_pos - effect_text.array;
	struct dstr return_color_name = {0};
	struct dstr coord_name = {0};
	if (main_no_args) {

		dstr_replace(&effect_text, "void main()", "float4 mainImage(VertData v_in) : TARGET");
		if (strstr(effect_text.array, "varying vec2 position;")) {
			uv = 1;
			dstr_init_copy(&coord_name, "position");
		} else if (strstr(effect_text.array, "varying vec2 pos;")) {
			uv = 1;
			dstr_init_copy(&coord_name, "pos");
		} else if (strstr(effect_text.array, "fNormal")) {
			uv = 2;
			dstr_init_copy(&coord_name, "fNormal");
		} else {
			uv = 0;
			dstr_init_copy(&coord_name, "gl_FragCoord");
		}

		char *out_start = strstr(effect_text.array, "out vec4");
		if (out_start) {
			char *start = out_start + 9;
			while (*start == ' ' || *start == '\t')
				start++;
			char *end = start;
			while (*end != ' ' && *end != '\t' && *end != '\n' && *end != ',' && *end != '(' && *end != ')' &&
			       *end != ';' && *end != 0)
				end++;
			dstr_ncat(&return_color_name, start, end - start);
			while (*end == ' ' || *end == '\t')
				end++;
			if (*end == ';')
				dstr_remove(&effect_text, out_start - effect_text.array, (end + 1) - out_start);
		} else {
			dstr_init_copy(&return_color_name, "gl_FragColor");
		}
	} else {

		char *start = main_pos + start_diff;
		while (*start == ' ' || *start == '\t')
			start++;
		char *end = start;
		while (*end != ' ' && *end != '\t' && *end != '\n' && *end != ',' && *end != ')' && *end != 0)
			end++;

		dstr_ncat(&return_color_name, start, end - start);

		start = strstr(end, ",");
		if (!start) {
			dstr_free(&effect_text);
			dstr_free(&return_color_name);
			obs_data_release(settings);
			return false;
		}
		start++;
		while (*start == ' ' || *start == '\t')
			start++;
		char *v2i = strstr(start, "in vec2 ");
		char *v2 = strstr(start, "vec2 ");
		char *close = strstr(start, ")");
		if (v2i && close && v2i < close) {
			start = v2i + 8;
		} else if (v2 && close && v2 < close) {
			start = v2 + 5;
		} else {
			if (*start == 'i' && *(start + 1) == 'n' && (*(start + 2) == ' ' || *(start + 2) == '\t'))
				start += 3;
			while (*start == ' ' || *start == '\t')
				start++;
			if (*start == 'v' && *(start + 1) == 'e' && *(start + 2) == 'c' && *(start + 3) == '2' &&
			    (*(start + 4) == ' ' || *(start + 4) == '\t'))
				start += 5;
		}
		while (*start == ' ' || *start == '\t')
			start++;

		end = start;
		while (*end != ' ' && *end != '\t' && *end != '\n' && *end != ',' && *end != ')' && *end != 0)
			end++;

		dstr_ncat(&coord_name, start, end - start);

		while (*end != ')' && *end != 0)
			end++;
		size_t idx = main_pos - effect_text.array;
		dstr_remove(&effect_text, idx, end - main_pos + 1);
		dstr_insert(&effect_text, idx, "float4 mainImage(VertData v_in) : TARGET");
	}

	convert_return(&effect_text, &return_color_name, main_diff);

	dstr_free(&return_color_name);

	if (dstr_cmp(&coord_name, "fragCoord") != 0) {
		if (dstr_find(&coord_name, "fragCoord")) {
			dstr_replace(&effect_text, coord_name.array, "fragCoord");
		} else {
			char *pos = strstr(effect_text.array, coord_name.array);
			while (pos) {
				size_t diff = pos - effect_text.array;
				if (((main_no_args || diff < main_diff) || diff > main_diff + 24) && !is_var_char(*(pos - 1)) &&
				    !is_var_char(*(pos + coord_name.len))) {
					dstr_remove(&effect_text, diff, coord_name.len);
					dstr_insert(&effect_text, diff, "fragCoord");
					pos = strstr(effect_text.array + diff + 9, coord_name.array);

				} else {
					pos = strstr(effect_text.array + diff + coord_name.len, coord_name.array);
				}
			}
		}
	}
	dstr_free(&coord_name);

	convert_if0(&effect_text);
	convert_if1(&effect_text);

	dstr_replace(&effect_text, "varying vec3", "//varying vec3");
	dstr_replace(&effect_text, "precision highp float;", "//precision highp float;");
	if (uv == 1) {
		dstr_replace(&effect_text, "fragCoord", "v_in.uv");
	} else if (uv == 2) {
		dstr_replace(&effect_text, "fragCoord.xy", "v_in.uv");
		dstr_replace(&effect_text, "fragCoord", "float3(v_in.uv,0.0)");
	} else {
		dstr_replace(&effect_text, "fragCoord.xy/iResolution.xy", "v_in.uv");
		dstr_replace(&effect_text, "fragCoord.xy / iResolution.xy", "v_in.uv");
		dstr_replace(&effect_text, "fragCoord/iResolution.xy", "v_in.uv");
		dstr_replace(&effect_text, "fragCoord / iResolution.xy", "v_in.uv");
		dstr_replace(&effect_text, "fragCoord", "(v_in.uv * uv_size)");
		dstr_replace(&effect_text, "gl_FragCoord.xy/iResolution.xy", "v_in.uv");
		dstr_replace(&effect_text, "gl_FragCoord.xy / iResolution.xy", "v_in.uv");
		dstr_replace(&effect_text, "gl_FragCoord/iResolution.xy", "v_in.uv");
		dstr_replace(&effect_text, "gl_FragCoord / iResolution.xy", "v_in.uv");
		dstr_replace(&effect_text, "gl_FragCoord", "(v_in.uv * uv_size)");
	}
	dstr_replace(&effect_text, "love_ScreenSize", "uv_size");
	dstr_replace(&effect_text, "u_resolution", "uv_size");
	dstr_replace(&effect_text, "uResolution", "uv_size");
	dstr_replace(&effect_text, "iResolution.xyz", "float3(uv_size,0.0)");
	dstr_replace(&effect_text, "iResolution.xy", "uv_size");
	dstr_replace(&effect_text, "iResolution.x", "uv_size.x");
	dstr_replace(&effect_text, "iResolution.y", "uv_size.y");
	dstr_replace(&effect_text, "iResolution", "float4(uv_size,uv_pixel_interval)");

	dstr_replace(&effect_text, "uniform vec2 uv_size;", "");

	if (strstr(effect_text.array, "iTime"))
		dstr_replace(&effect_text, "iTime", "elapsed_time");
	else if (strstr(effect_text.array, "uTime"))
		dstr_replace(&effect_text, "uTime", "elapsed_time");
	else if (strstr(effect_text.array, "u_time"))
		dstr_replace(&effect_text, "u_time", "elapsed_time");
	else
		dstr_replace(&effect_text, "time", "elapsed_time");

	dstr_replace(&effect_text, "uniform float elapsed_time;", "");

	dstr_replace(&effect_text, "iDate.w", "local_time");

	dstr_replace(&effect_text, "bvec2", "bool2");
	dstr_replace(&effect_text, "bvec3", "bool3");
	dstr_replace(&effect_text, "bvec4", "bool4");

	dstr_replace(&effect_text, "ivec4", "int4");
	dstr_replace(&effect_text, "ivec3", "int3");
	dstr_replace(&effect_text, "ivec2", "int2");

	dstr_replace(&effect_text, "uvec4", "uint4");
	dstr_replace(&effect_text, "uvec3", "uint3");
	dstr_replace(&effect_text, "uvec2", "uint2");

	dstr_replace(&effect_text, "vec4", "float4");
	dstr_replace(&effect_text, "vec3", "float3");
	dstr_replace(&effect_text, "vec2", "float2");

	dstr_replace(&effect_text, " number ", " float ");

	//dstr_replace(&effect_text, "mat4", "float4x4");
	//dstr_replace(&effect_text, "mat3", "float3x3");
	//dstr_replace(&effect_text, "mat2", "float2x2");

	dstr_replace(&effect_text, "dFdx(", "ddx(");
	dstr_replace(&effect_text, "dFdy(", "ddy(");
	dstr_replace(&effect_text, "mix(", "lerp(");
	dstr_replace(&effect_text, "fract(", "frac(");
	dstr_replace(&effect_text, "inversesqrt(", "rsqrt(");

	dstr_replace(&effect_text, "extern bool", "uniform bool");
	dstr_replace(&effect_text, "extern uint", "uniform uint");
	dstr_replace(&effect_text, "extern int", "uniform int");
	dstr_replace(&effect_text, "extern float", "uniform float");

	convert_init(&effect_text, "bool");
	convert_init(&effect_text, "uint");
	convert_init(&effect_text, "int");
	convert_init(&effect_text, "float");

	convert_vector_init(&effect_text, "bool(", 1);
	convert_vector_init(&effect_text, "bool2(", 2);
	convert_vector_init(&effect_text, "bool3(", 3);
	convert_vector_init(&effect_text, "bool4(", 4);
	convert_vector_init(&effect_text, "uint(", 1);
	convert_vector_init(&effect_text, "uint2(", 2);
	convert_vector_init(&effect_text, "uint3(", 3);
	convert_vector_init(&effect_text, "uint4(", 4);
	convert_vector_init(&effect_text, "int(", 1);
	convert_vector_init(&effect_text, "int2(", 2);
	convert_vector_init(&effect_text, "int3(", 3);
	convert_vector_init(&effect_text, "int4(", 4);
	convert_vector_init(&effect_text, "float(", 1);
	convert_vector_init(&effect_text, "float2(", 2);
	convert_vector_init(&effect_text, "float3(", 3);
	convert_vector_init(&effect_text, "float4(", 4);
	convert_vector_init(&effect_text, "mat2(", 4);
	convert_vector_init(&effect_text, "mat3(", 9);
	convert_vector_init(&effect_text, "mat4(", 16);

	convert_atan(&effect_text);
	convert_mat_mul(&effect_text, "mat2");
	convert_mat_mul(&effect_text, "mat3");
	convert_mat_mul(&effect_text, "mat4");

	dstr_replace(&effect_text, "point(", "point2(");
	dstr_replace(&effect_text, "line(", "line2(");

	dstr_replace(&effect_text, "#version ", "//#version ");

	convert_if_defined(&effect_text);

	dstr_replace(&effect_text, "acos(-1.0)", "3.14159265359");
	dstr_replace(&effect_text, "acos(-1.)", "3.14159265359");
	dstr_replace(&effect_text, "acos(-1)", "3.14159265359");

	struct dstr insert_text = {0};
	dstr_init_copy(&insert_text, "#ifndef OPENGL\n");

	if (dstr_find(&effect_text, "mat2"))
		dstr_cat(&insert_text, "#define mat2 float2x2\n");
	if (dstr_find(&effect_text, "mat3"))
		dstr_cat(&insert_text, "#define mat3 float3x3\n");
	if (dstr_find(&effect_text, "mat4"))
		dstr_cat(&insert_text, "#define mat4 float4x4\n");

	if (dstr_find(&effect_text, "mod("))
		dstr_cat(&insert_text, "#define mod(x,y) ((x) - (y) * floor((x) / (y)))\n");
	if (dstr_find(&effect_text, "lessThan("))
		dstr_cat(&insert_text, "#define lessThan(a,b) ((a) < (b))\n");
	if (dstr_find(&effect_text, "greaterThan("))
		dstr_cat(&insert_text, "#define greaterThan(a,b) ((a) > (b))\n");
	dstr_cat(&insert_text, "#endif\n");

	if (dstr_find(&effect_text, "iMouse") && !dstr_find(&effect_text, "float2 iMouse"))
		dstr_cat(
			&insert_text,
			"uniform float4 iMouse<\nstring widget_type = \"slider\";\nfloat minimum=0.0;\nfloat maximum=1000.0;\nfloat step=1.0;\n>;\n");

	if (dstr_find(&effect_text, "iFrame") && !dstr_find(&effect_text, "float iFrame"))
		dstr_cat(&insert_text, "uniform float iFrame;\n");

	if (dstr_find(&effect_text, "iSampleRate") && !dstr_find(&effect_text, "float iSampleRate"))
		dstr_cat(&insert_text, "uniform float iSampleRate;\n");

	if (dstr_find(&effect_text, "iTimeDelta") && !dstr_find(&effect_text, "float iTimeDelta"))
		dstr_cat(&insert_text, "uniform float iTimeDelta;\n");

	int num_textures = 0;
	struct dstr texture_name = {0};
	struct dstr replacing = {0};
	struct dstr replacement = {0};

	char *texture_find[] = {"texture(", "texture2D(", "texelFetch(", "Texel(", "textureLod("};
	char *texture = NULL;
	size_t texture_diff = 0;
	for (size_t i = 0; i < sizeof(texture_find) / sizeof(char *); i++) {
		char *t = strstr(effect_text.array, texture_find[i]);
		if (t && (!texture || t < texture)) {
			texture = t;
			texture_diff = strlen(texture_find[i]);
		}
	}
	while (texture) {
		const size_t diff = texture - effect_text.array;
		if (is_var_char(*(texture - 1))) {
			texture = NULL;
			size_t prev_diff = texture_diff;
			for (size_t i = 0; i < 3; i++) {
				char *t = strstr(effect_text.array + diff + prev_diff, texture_find[i]);
				if (t && (!texture || t < texture)) {
					texture = t;
					texture_diff = strlen(texture_find[i]);
				}
			}
			continue;
		}
		char *start = texture + texture_diff;
		while (*start == ' ' || *start == '\t')
			start++;
		char *end = start;
		while (*end != ' ' && *end != '\t' && *end != ',' && *end != ')' && *end != '\n' && *end != 0)
			end++;
		dstr_copy(&texture_name, "");
		dstr_ncat(&texture_name, start, end - start);

		dstr_copy(&replacing, "");
		dstr_ncat(&replacing, texture, end - texture);

		dstr_copy(&replacement, "");

		if (num_textures) {
			dstr_cat_dstr(&replacement, &texture_name);
			dstr_cat(&replacement, ".Sample(textureSampler");

			dstr_cat(&insert_text, "uniform texture2d ");
			dstr_cat(&insert_text, texture_name.array);
			dstr_cat(&insert_text, ";\n");
		} else {
			dstr_cat(&replacement, "image.Sample(textureSampler");
		}
		dstr_replace(&effect_text, replacing.array, replacement.array);

		dstr_copy(&replacing, "textureSize(");
		dstr_cat(&replacing, texture_name.array);
		dstr_cat(&replacing, ", 0)");

		dstr_replace(&effect_text, replacing.array, "uv_size");
		dstr_replace(&replacing, ", 0)", ",0)");
		dstr_replace(&effect_text, replacing.array, "uv_size");

		num_textures++;
		size_t prev_diff = texture_diff;
		texture = NULL;
		for (size_t i = 0; i < sizeof(texture_find) / sizeof(char *); i++) {
			char *t = strstr(effect_text.array + diff + prev_diff, texture_find[i]);
			if (t && (!texture || t < texture)) {
				texture = t;
				texture_diff = strlen(texture_find[i]);
			}
		}
	}
	dstr_free(&replacing);
	dstr_free(&replacement);
	dstr_free(&texture_name);

	if (insert_text.len > 24) {
		dstr_insert_dstr(&effect_text, 0, &insert_text);
	}
	dstr_free(&insert_text);

	obs_data_set_string(settings, "shader_text", effect_text.array);

	dstr_free(&effect_text);

	obs_data_release(settings);
	obs_property_set_visible(property, false);

	filter->reload_effect = true;

	obs_source_update(filter->context, NULL);
	return true;
}

static const char *shader_filter_texture_file_filter = "Textures (*.bmp *.tga *.png *.jpeg *.jpg *.gif);;";

#define MIN_AUDIO_THRESHOLD -60.0f

static bool shader_text_uses_audio(const char *shader_text)
{
	if (!shader_text)
		return false;

	// Audio identifiers to search for - these are the actual variable/function names
	const char *audio_identifiers[] = {
		"bass", "lows", "mids", "treb",
		"bass_c", "lows_c", "mids_c", "treb_c",
		"audio_peak", "audio_magnitude",
		"energy", "e_bass", "e_lows", "e_mids", "e_treb",
		"acc(", "ts(", "cu_bass(", "cu_lows(", "cu_mids(", "cu_treb(",
		"t_bass", "t_lows", "t_mids", "t_treb",
		"expfoff(", "linfoff(",
		"ef_bass_fast", "ef_bass_medium", "ef_bass_slow", "ef_bass_slug",
		"ef_lows_fast", "ef_lows_medium", "ef_lows_slow", "ef_lows_slug",
		"ef_mids_fast", "ef_mids_medium", "ef_mids_slow", "ef_mids_slug",
		"ef_treb_fast", "ef_treb_medium", "ef_treb_slow", "ef_treb_slug",
		"lf_bass_fast", "lf_bass_medium", "lf_bass_slow", "lf_bass_slug",
		"lf_lows_fast", "lf_lows_medium", "lf_lows_slow", "lf_lows_slug",
		"lf_mids_fast", "lf_mids_medium", "lf_mids_slow", "lf_mids_slug",
		"lf_treb_fast", "lf_treb_medium", "lf_treb_slow", "lf_treb_slug"
	};

	for (size_t i = 0; i < sizeof(audio_identifiers) / sizeof(audio_identifiers[0]); i++) {
		if (strstr(shader_text, audio_identifiers[i]) != NULL)
			return true;
	}

	return false;
}

static bool uses_audio_bands(struct shader_filter_data *filter)
{
	if (!filter)
		return false;

	// Get shader text from settings
	obs_data_t *settings = obs_source_get_settings(filter->context);
	const char *shader_text = obs_data_get_string(settings, "shader_text");
	char *loaded_text = NULL;

	if (!shader_text || !strlen(shader_text)) {
		// If no text in settings, try loading from file
		const char *file_name = obs_data_get_string(settings, "shader_file_name");
		if (file_name && strlen(file_name)) {
			loaded_text = load_shader_from_file(file_name);
			shader_text = loaded_text;
		}
	}

	const bool result = shader_text_uses_audio(shader_text);

	if (loaded_text)
		bfree(loaded_text);
	obs_data_release(settings);

	return result;
}

static inline float wrap_audio_band_counter(float value)
{
	return value >= AUDIO_BAND_COUNTER_WRAP ? fmodf(value, AUDIO_BAND_COUNTER_WRAP) : value;
}

static inline float shader_filter_update_exp_falloff(float previous, float current, float tail_seconds, float delta_time)
{
	// Reach 1% of the starting level after tail_seconds, independently of FPS.
	return fmaxf(previous * expf(-4.605170186f * delta_time / tail_seconds), current);
}

static inline float shader_filter_update_lin_falloff(float previous, float current, float tail_seconds, float delta_time)
{
	// Constant slope in the shader's normalized level: 1 -> 0 in tail_seconds.
	return fmaxf(previous - delta_time / tail_seconds, current);
}

/* Adjacent outputs from these one-pole low-pass filters are subtracted to
 * produce the four inexpensive time-domain band-pass signals. */
static const float audio_crossover_frequencies[AUDIO_CROSSOVER_COUNT] = {30.0f, 120.0f, 500.0f, 3000.0f, 13000.0f};

/* Tail zero is the unsmoothed base band. The longest preset has the same
 * approximate duration as the 131072-sample spectrum window at 48 kHz. */
static const float audio_falloff_seconds[AUDIO_FALLOFF_SLUG + 1] = {0.12f, 0.4f, 1.0f, 131072.0f / 48000.0f};

static uint32_t shader_filter_audio_sample_rate(void)
{
	audio_t *audio = obs_get_audio();
	const uint32_t sample_rate = audio ? audio_output_get_sample_rate(audio) : 0;
	return sample_rate ? sample_rate : 48000;
}

static size_t shader_filter_audio_channel_count(void)
{
	audio_t *audio = obs_get_audio();
	const size_t channels = audio ? audio_output_get_channels(audio) : 0;
	return channels ? (channels < MAX_AUDIO_CHANNELS ? channels : MAX_AUDIO_CHANNELS) : MAX_AUDIO_CHANNELS;
}

static void shader_filter_configure_audio_crossover(struct audio_source_analysis *analysis, uint32_t sample_rate)
{
	if (analysis->sample_rate == sample_rate)
		return;

	const float nyquist_guard = (float)sample_rate * 0.45f;
	for (size_t i = 0; i < AUDIO_CROSSOVER_COUNT; i++) {
		const float cutoff = fminf(audio_crossover_frequencies[i], nyquist_guard);
		analysis->lowpass_alpha[i] = 1.0f - expf(-2.0f * 3.14159265358979323846f * cutoff / (float)sample_rate);
	}
	// Approximate hearing sensitivity with the same A curve as the spectrum,
	// evaluated at each band's geometric center. No arbitrary bass/treble boost.
	for (size_t band = 0; band < AUDIO_BAND_COUNT; ++band) {
		const float low = fminf(audio_crossover_frequencies[band], nyquist_guard);
		const float high = fminf(audio_crossover_frequencies[band + 1], nyquist_guard);
		analysis->band_weights[band] = low < high ? (float)sf_spectrum_a_weight(sqrt((double)low * high)) : 0.0f;
	}

	analysis->sample_rate = sample_rate;
	analysis->capture_ns = 0;
	memset(&analysis->energy_window, 0, sizeof(analysis->energy_window));
	memset(analysis->lowpass_state, 0, sizeof(analysis->lowpass_state));
	memset(analysis->bands, 0, sizeof(analysis->bands));
	memset(analysis->exp_falloff, 0, sizeof(analysis->exp_falloff));
	memset(analysis->lin_falloff, 0, sizeof(analysis->lin_falloff));
}

static float shader_filter_audio_level(float amplitude)
{
	if (amplitude <= 0.001f)
		return 0.0f;

	const float db = 20.0f * log10f(amplitude);
	return fmaxf(0.0f, fminf(1.0f, (db - MIN_AUDIO_THRESHOLD) / -MIN_AUDIO_THRESHOLD));
}

static void shader_filter_update_shared_audio_derivatives(struct audio_source_analysis *analysis, float delta_time,
							float falloff_delta_time)
{
	for (size_t band = 0; band < AUDIO_BAND_COUNT; band++) {
		analysis->cumulative[band] =
			wrap_audio_band_counter(analysis->cumulative[band] + analysis->bands[band] * delta_time * 100.0f);

		for (size_t speed = 0; speed <= AUDIO_FALLOFF_SLUG; speed++) {
			analysis->exp_falloff[band][speed] =
				shader_filter_update_exp_falloff(analysis->exp_falloff[band][speed], analysis->bands[band],
								 audio_falloff_seconds[speed], falloff_delta_time);
			analysis->lin_falloff[band][speed] =
				shader_filter_update_lin_falloff(analysis->lin_falloff[band][speed], analysis->bands[band],
								 audio_falloff_seconds[speed], falloff_delta_time);
		}

		const float history_decay = expf(-delta_time / 0.15f);
		analysis->history_smooth[band] =
			analysis->history_smooth[band] * history_decay + analysis->bands[band] * (1.0f - history_decay);
		analysis->history[band][analysis->history_write_pos] = analysis->history_smooth[band];
	}

	analysis->history_write_pos = (analysis->history_write_pos + 1) % AUDIO_TIMESERIES_BUFFER_WIDTH;
}

static void shader_filter_audio_analysis_callback(void *data, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	UNUSED_PARAMETER(source);
	struct audio_source_analysis *analysis = data;
	if (!analysis || !audio || !audio->frames)
		return;

	pthread_mutex_lock(&analysis->mutex);
	const uint32_t sample_rate = shader_filter_audio_sample_rate();
	shader_filter_configure_audio_crossover(analysis, sample_rate);
	const float delta_time = (float)audio->frames / (float)sample_rate;
	const double capture_seconds = (double)audio->frames / (double)sample_rate;
	const uint64_t capture_ns = os_gettime_ns();
	float falloff_delta_time = delta_time;
	if (analysis->capture_ns && capture_ns > analysis->capture_ns) {
		const float gap = (float)((double)(capture_ns - analysis->capture_ns) * 1e-9);
		if (gap > fmaxf(0.05f, 2.0f * analysis->capture_seconds)) {
			falloff_delta_time = fmaxf(delta_time, gap);
			memset(analysis->lowpass_state, 0, sizeof(analysis->lowpass_state));
		}
	}
	analysis->capture_ns = capture_ns;
	analysis->capture_seconds = delta_time;
	double power[SF_ENERGY_CHANNELS] = {0.0};
	if (analysis->spectrum) {
		const float *samples[MAX_AUDIO_CHANNELS] = {0};
		const size_t channels = shader_filter_audio_channel_count();
		for (size_t channel = 0; channel < channels; ++channel)
			samples[channel] = (const float *)audio->data[channel];
		sf_spectrum_push(analysis->spectrum, samples, channels, audio->frames, sample_rate,
				 muted || !audio->data[0], audio->timestamp, capture_ns);
	}

	if (muted || !audio->data[0]) {
		analysis->audio_peak = 0.0f;
		analysis->audio_magnitude = 0.0f;
		memset(analysis->bands, 0, sizeof(analysis->bands));
		memset(analysis->lowpass_state, 0, sizeof(analysis->lowpass_state));
		sf_audio_energy_capture(&analysis->energy_window, power, capture_seconds, audio->timestamp, capture_ns);
		shader_filter_update_shared_audio_derivatives(analysis, delta_time, falloff_delta_time);
		pthread_mutex_unlock(&analysis->mutex);
		return;
	}

	size_t channels[MAX_AUDIO_CHANNELS];
	size_t channel_count = 0;
	const size_t available_channels = shader_filter_audio_channel_count();
	for (size_t channel = 0; channel < available_channels; channel++) {
		if (audio->data[channel])
			channels[channel_count++] = channel;
	}

	double full_energy = 0.0;
	double band_energy[AUDIO_BAND_COUNT] = {0.0};
	float peak = 0.0f;
	for (size_t frame = 0; frame < audio->frames; frame++) {
		for (size_t channel_index = 0; channel_index < channel_count; channel_index++) {
			const size_t channel = channels[channel_index];
			const float input = ((const float *)audio->data[channel])[frame];
			const float sample = isfinite(input) ? input : 0.0f;
			peak = fmaxf(peak, fabsf(sample));
			full_energy += (double)sample * (double)sample;

			float lowpass[AUDIO_CROSSOVER_COUNT];
			for (size_t crossover = 0; crossover < AUDIO_CROSSOVER_COUNT; crossover++) {
				float *state = &analysis->lowpass_state[channel][crossover];
				*state += analysis->lowpass_alpha[crossover] * (sample - *state);
				lowpass[crossover] = *state;
			}

			const float band_samples[AUDIO_BAND_COUNT] = {
				lowpass[1] - lowpass[0],
				lowpass[2] - lowpass[1],
				lowpass[3] - lowpass[2],
				lowpass[4] - lowpass[3],
			};
			for (size_t band = 0; band < AUDIO_BAND_COUNT; band++)
				band_energy[band] += (double)band_samples[band] * (double)band_samples[band];
		}
	}

	const size_t sample_count = audio->frames * channel_count;
	if (sample_count) {
		power[0] = full_energy / (double)sample_count;
		for (size_t band = 0; band < AUDIO_BAND_COUNT; ++band)
			power[band + 1] = band_energy[band] / (double)sample_count;
		analysis->audio_peak = shader_filter_audio_level(peak);
		analysis->audio_magnitude = shader_filter_audio_level(sqrtf((float)(full_energy / (double)sample_count)));

		for (size_t band = 0; band < AUDIO_BAND_COUNT; band++) {
			const float rms = sqrtf((float)(band_energy[band] / (double)sample_count));
			analysis->bands[band] = shader_filter_audio_level(rms * analysis->band_weights[band]);
		}
	} else {
		analysis->audio_peak = 0.0f;
		analysis->audio_magnitude = 0.0f;
		memset(analysis->bands, 0, sizeof(analysis->bands));
	}

	sf_audio_energy_capture(&analysis->energy_window, power, capture_seconds, audio->timestamp, capture_ns);
	shader_filter_update_shared_audio_derivatives(analysis, delta_time, falloff_delta_time);
	pthread_mutex_unlock(&analysis->mutex);
}

static struct audio_source_analysis *shader_filter_acquire_audio_analysis(obs_source_t *source)
{
	if (!source)
		return NULL;

	pthread_mutex_lock(&audio_analyses_mutex);
	for (struct audio_source_analysis *analysis = audio_analyses; analysis; analysis = analysis->next) {
		obs_source_t *analysis_source = obs_weak_source_get_source(analysis->source);
		const bool matches = analysis_source == source;
		if (analysis_source)
			obs_source_release(analysis_source);
		if (matches) {
			analysis->users++;
			pthread_mutex_unlock(&audio_analyses_mutex);
			return analysis;
		}
	}

	struct audio_source_analysis *analysis = bzalloc(sizeof(*analysis));
	if (pthread_mutex_init(&analysis->mutex, NULL) != 0) {
		bfree(analysis);
		pthread_mutex_unlock(&audio_analyses_mutex);
		return NULL;
	}

	analysis->source = obs_source_get_weak_source(source);
	analysis->users = 1;
	analysis->next = audio_analyses;
	audio_analyses = analysis;
	obs_source_add_audio_capture_callback(source, shader_filter_audio_analysis_callback, analysis);
	pthread_mutex_unlock(&audio_analyses_mutex);
	return analysis;
}

static bool shader_filter_audio_analysis_matches(struct shader_filter_data *filter, obs_source_t *source)
{
	if (!filter->audio_analysis)
		return false;

	obs_source_t *analysis_source = obs_weak_source_get_source(filter->audio_analysis->source);
	const bool matches = analysis_source == source;
	if (analysis_source)
		obs_source_release(analysis_source);
	return matches;
}

static void shader_filter_release_spectrum(struct shader_filter_data *filter)
{
	if (filter->spectrum_registered && filter->audio_analysis) {
		struct audio_source_analysis *analysis = filter->audio_analysis;
		pthread_mutex_lock(&analysis->mutex);
		if (--analysis->spectrum_users == 0) {
			sf_spectrum_destroy(analysis->spectrum);
			analysis->spectrum = NULL;
		}
		pthread_mutex_unlock(&analysis->mutex);
	}
	filter->spectrum_registered = false;
	memset(filter->spec_values, 0, sizeof(filter->spec_values));
	filter->spec_revision = 0;
	filter->spec_uploaded_revision = UINT64_MAX;
}

static void shader_filter_sync_spectrum(struct shader_filter_data *filter)
{
	if (!filter->param_spec || !filter->audio_analysis) {
		shader_filter_release_spectrum(filter);
		return;
	}
	if (filter->spectrum_registered)
		return;
	struct audio_source_analysis *analysis = filter->audio_analysis;
	pthread_mutex_lock(&analysis->mutex);
	if (!analysis->spectrum)
		analysis->spectrum = sf_spectrum_create(shader_filter_audio_sample_rate(), shader_filter_audio_channel_count());
	if (analysis->spectrum) {
		++analysis->spectrum_users;
		filter->spectrum_registered = true;
	} else {
		blog(LOG_WARNING, "[obs-shaderfilter] Unable to initialize spectrum FFT worker");
	}
	pthread_mutex_unlock(&analysis->mutex);
}

static void shader_filter_release_audio_analysis(struct shader_filter_data *filter)
{
	if (!filter || !filter->audio_analysis)
		return;

	shader_filter_release_spectrum(filter);
	struct audio_source_analysis *analysis = filter->audio_analysis;
	filter->audio_analysis = NULL;

	pthread_mutex_lock(&audio_analyses_mutex);
	if (--analysis->users) {
		pthread_mutex_unlock(&audio_analyses_mutex);
		return;
	}

	struct audio_source_analysis **link = &audio_analyses;
	while (*link && *link != analysis)
		link = &(*link)->next;
	if (*link)
		*link = analysis->next;

	obs_source_t *source = obs_weak_source_get_source(analysis->source);
	if (source) {
		obs_source_remove_audio_capture_callback(source, shader_filter_audio_analysis_callback, analysis);
		obs_source_release(source);
	}
	pthread_mutex_unlock(&audio_analyses_mutex);

	obs_weak_source_release(analysis->source);
	pthread_mutex_destroy(&analysis->mutex);
	bfree(analysis);
}

static void shader_filter_attach_audio_analysis(struct shader_filter_data *filter, obs_source_t *source)
{
	if ((!uses_audio_bands(filter) && !filter->param_spec) || !source) {
		shader_filter_release_audio_analysis(filter);
		return;
	}

	if (shader_filter_audio_analysis_matches(filter, source))
		return;

	shader_filter_release_audio_analysis(filter);
	filter->audio_analysis = shader_filter_acquire_audio_analysis(source);
}

static void shader_filter_copy_audio_analysis(struct shader_filter_data *filter)
{
	struct audio_source_analysis *analysis = filter->audio_analysis;
	if (!analysis)
		return;

	float *history_buffers[AUDIO_BAND_COUNT] = {
		filter->t_bass_buffer,
		filter->t_lows_buffer,
		filter->t_mids_buffer,
		filter->t_treb_buffer,
	};
	float *exp_falloff[AUDIO_BAND_COUNT][AUDIO_FALLOFF_SLUG + 1] = {
		{&filter->ef_bass_fast, &filter->ef_bass_medium, &filter->ef_bass_slow, &filter->ef_bass_slug},
		{&filter->ef_lows_fast, &filter->ef_lows_medium, &filter->ef_lows_slow, &filter->ef_lows_slug},
		{&filter->ef_mids_fast, &filter->ef_mids_medium, &filter->ef_mids_slow, &filter->ef_mids_slug},
		{&filter->ef_treb_fast, &filter->ef_treb_medium, &filter->ef_treb_slow, &filter->ef_treb_slug},
	};
	float *lin_falloff[AUDIO_BAND_COUNT][AUDIO_FALLOFF_SLUG + 1] = {
		{&filter->lf_bass_fast, &filter->lf_bass_medium, &filter->lf_bass_slow, &filter->lf_bass_slug},
		{&filter->lf_lows_fast, &filter->lf_lows_medium, &filter->lf_lows_slow, &filter->lf_lows_slug},
		{&filter->lf_mids_fast, &filter->lf_mids_medium, &filter->lf_mids_slow, &filter->lf_mids_slug},
		{&filter->lf_treb_fast, &filter->lf_treb_medium, &filter->lf_treb_slow, &filter->lf_treb_slug},
	};

	pthread_mutex_lock(&analysis->mutex);
	const uint64_t now = os_gettime_ns();
	const float age = analysis->capture_ns && now > analysis->capture_ns
				  ? (float)((double)(now - analysis->capture_ns) * 1e-9)
				  : 0.0f;
	const bool stale = age > fmaxf(0.05f, 2.0f * analysis->capture_seconds);
	filter->audio_peak = stale ? 0.0f : analysis->audio_peak;
	filter->audio_magnitude = stale ? 0.0f : analysis->audio_magnitude;
	double power[SF_ENERGY_CHANNELS];
	sf_audio_energy_read(&analysis->energy_window, now, power);
	filter->energy = shader_filter_audio_level((float)sqrt(power[0]));
	float *band_energies[AUDIO_BAND_COUNT] = {&filter->e_bass, &filter->e_lows, &filter->e_mids, &filter->e_treb};
	for (size_t band = 0; band < AUDIO_BAND_COUNT; ++band)
		*band_energies[band] = shader_filter_audio_level((float)sqrt(power[band + 1]) * analysis->band_weights[band]);
	filter->bass = stale ? 0.0f : analysis->bands[AUDIO_BAND_BASS];
	filter->lows = stale ? 0.0f : analysis->bands[AUDIO_BAND_LOWS];
	filter->mids = stale ? 0.0f : analysis->bands[AUDIO_BAND_MIDS];
	filter->treb = stale ? 0.0f : analysis->bands[AUDIO_BAND_TREB];
	filter->bass_c = analysis->cumulative[AUDIO_BAND_BASS];
	filter->lows_c = analysis->cumulative[AUDIO_BAND_LOWS];
	filter->mids_c = analysis->cumulative[AUDIO_BAND_MIDS];
	filter->treb_c = analysis->cumulative[AUDIO_BAND_TREB];

	for (size_t band = 0; band < AUDIO_BAND_COUNT; band++) {
		for (size_t speed = 0; speed <= AUDIO_FALLOFF_SLUG; speed++) {
			// Project from the last capture without modifying shared state, so
			// stopped sources decay and multiple video consumers cannot double-step.
			*exp_falloff[band][speed] =
				stale ? shader_filter_update_exp_falloff(analysis->exp_falloff[band][speed], 0.0f,
									audio_falloff_seconds[speed], age)
				      : analysis->exp_falloff[band][speed];
			*lin_falloff[band][speed] =
				stale ? shader_filter_update_lin_falloff(analysis->lin_falloff[band][speed], 0.0f,
									audio_falloff_seconds[speed], age)
				      : analysis->lin_falloff[band][speed];
		}
		for (size_t i = 0; i < AUDIO_TIMESERIES_BUFFER_WIDTH; i++) {
			const size_t source_index = (analysis->history_write_pos + i) % AUDIO_TIMESERIES_BUFFER_WIDTH;
			history_buffers[band][i] = analysis->history[band][source_index];
		}
	}
	pthread_mutex_unlock(&analysis->mutex);
}

static void shader_filter_clear_audio_snapshot(struct shader_filter_data *filter)
{
	filter->audio_peak = 0.0f;
	filter->audio_magnitude = 0.0f;
	memset(filter->spec_values, 0, sizeof(filter->spec_values));
	filter->spec_revision = 0;
	filter->energy = filter->e_bass = filter->e_lows = filter->e_mids = filter->e_treb = 0.0f;
	filter->bass = 0.0f;
	filter->lows = 0.0f;
	filter->mids = 0.0f;
	filter->treb = 0.0f;
	filter->bass_c = 0.0f;
	filter->lows_c = 0.0f;
	filter->mids_c = 0.0f;
	filter->treb_c = 0.0f;

	filter->ef_bass_fast = filter->ef_bass_medium = filter->ef_bass_slow = filter->ef_bass_slug = 0.0f;
	filter->ef_lows_fast = filter->ef_lows_medium = filter->ef_lows_slow = filter->ef_lows_slug = 0.0f;
	filter->ef_mids_fast = filter->ef_mids_medium = filter->ef_mids_slow = filter->ef_mids_slug = 0.0f;
	filter->ef_treb_fast = filter->ef_treb_medium = filter->ef_treb_slow = filter->ef_treb_slug = 0.0f;
	filter->lf_bass_fast = filter->lf_bass_medium = filter->lf_bass_slow = filter->lf_bass_slug = 0.0f;
	filter->lf_lows_fast = filter->lf_lows_medium = filter->lf_lows_slow = filter->lf_lows_slug = 0.0f;
	filter->lf_mids_fast = filter->lf_mids_medium = filter->lf_mids_slow = filter->lf_mids_slug = 0.0f;
	filter->lf_treb_fast = filter->lf_treb_medium = filter->lf_treb_slow = filter->lf_treb_slug = 0.0f;

	memset(filter->t_bass_buffer, 0, AUDIO_TIMESERIES_BUFFER_WIDTH * sizeof(float));
	memset(filter->t_lows_buffer, 0, AUDIO_TIMESERIES_BUFFER_WIDTH * sizeof(float));
	memset(filter->t_mids_buffer, 0, AUDIO_TIMESERIES_BUFFER_WIDTH * sizeof(float));
	memset(filter->t_treb_buffer, 0, AUDIO_TIMESERIES_BUFFER_WIDTH * sizeof(float));
}

static void shader_filter_render_audio_timeseries_textures(struct shader_filter_data *filter)
{
	if (!filter || !filter->t_bass_buffer)
		return;

	// Create textures if needed
	if (!filter->t_bass_render)
		filter->t_bass_render = gs_texrender_create(GS_R32F, GS_ZS_NONE);
	if (!filter->t_lows_render)
		filter->t_lows_render = gs_texrender_create(GS_R32F, GS_ZS_NONE);
	if (!filter->t_mids_render)
		filter->t_mids_render = gs_texrender_create(GS_R32F, GS_ZS_NONE);
	if (!filter->t_treb_render)
		filter->t_treb_render = gs_texrender_create(GS_R32F, GS_ZS_NONE);

	// Ensure textures are created with the right dimensions
	gs_texture_t *tex;

	tex = gs_texrender_get_texture(filter->t_bass_render);
	if (!tex || gs_texture_get_width(tex) != AUDIO_TIMESERIES_BUFFER_WIDTH ||
		gs_texture_get_height(tex) != AUDIO_TIMESERIES_BUFFER_HEIGHT) {
		gs_texrender_destroy(filter->t_bass_render);
		filter->t_bass_render = gs_texrender_create(GS_R32F, GS_ZS_NONE);
		if (gs_texrender_begin(filter->t_bass_render, AUDIO_TIMESERIES_BUFFER_WIDTH, AUDIO_TIMESERIES_BUFFER_HEIGHT)) {
			gs_texrender_end(filter->t_bass_render);
		}
	}

	tex = gs_texrender_get_texture(filter->t_lows_render);
	if (!tex || gs_texture_get_width(tex) != AUDIO_TIMESERIES_BUFFER_WIDTH ||
		gs_texture_get_height(tex) != AUDIO_TIMESERIES_BUFFER_HEIGHT) {
		gs_texrender_destroy(filter->t_lows_render);
		filter->t_lows_render = gs_texrender_create(GS_R32F, GS_ZS_NONE);
		if (gs_texrender_begin(filter->t_lows_render, AUDIO_TIMESERIES_BUFFER_WIDTH, AUDIO_TIMESERIES_BUFFER_HEIGHT)) {
			gs_texrender_end(filter->t_lows_render);
		}
	}

	tex = gs_texrender_get_texture(filter->t_mids_render);
	if (!tex || gs_texture_get_width(tex) != AUDIO_TIMESERIES_BUFFER_WIDTH ||
		gs_texture_get_height(tex) != AUDIO_TIMESERIES_BUFFER_HEIGHT) {
		gs_texrender_destroy(filter->t_mids_render);
		filter->t_mids_render = gs_texrender_create(GS_R32F, GS_ZS_NONE);
		if (gs_texrender_begin(filter->t_mids_render, AUDIO_TIMESERIES_BUFFER_WIDTH, AUDIO_TIMESERIES_BUFFER_HEIGHT)) {
			gs_texrender_end(filter->t_mids_render);
		}
	}

	tex = gs_texrender_get_texture(filter->t_treb_render);
	if (!tex || gs_texture_get_width(tex) != AUDIO_TIMESERIES_BUFFER_WIDTH ||
		gs_texture_get_height(tex) != AUDIO_TIMESERIES_BUFFER_HEIGHT) {
		gs_texrender_destroy(filter->t_treb_render);
		filter->t_treb_render = gs_texrender_create(GS_R32F, GS_ZS_NONE);
		if (gs_texrender_begin(filter->t_treb_render, AUDIO_TIMESERIES_BUFFER_WIDTH, AUDIO_TIMESERIES_BUFFER_HEIGHT)) {
			gs_texrender_end(filter->t_treb_render);
		}
	}

	// Update texture data by uploading from buffers
	tex = gs_texrender_get_texture(filter->t_bass_render);
	if (tex)
		gs_texture_set_image(tex, (const uint8_t *)filter->t_bass_buffer,
							AUDIO_TIMESERIES_BUFFER_WIDTH * sizeof(float), false);

	tex = gs_texrender_get_texture(filter->t_lows_render);
	if (tex)
		gs_texture_set_image(tex, (const uint8_t *)filter->t_lows_buffer,
							AUDIO_TIMESERIES_BUFFER_WIDTH * sizeof(float), false);

	tex = gs_texrender_get_texture(filter->t_mids_render);
	if (tex)
		gs_texture_set_image(tex, (const uint8_t *)filter->t_mids_buffer,
							AUDIO_TIMESERIES_BUFFER_WIDTH * sizeof(float), false);

	tex = gs_texrender_get_texture(filter->t_treb_render);
	if (tex)
		gs_texture_set_image(tex, (const uint8_t *)filter->t_treb_buffer,
							AUDIO_TIMESERIES_BUFFER_WIDTH * sizeof(float), false);
}

static bool shader_filter_enum_audio_sources(void *data, obs_source_t *source)
{
	obs_property_t *prop = (obs_property_t *)data;
	uint32_t flags = obs_source_get_output_flags(source);

	if ((flags & OBS_SOURCE_AUDIO) != 0) {
		const char *name = obs_source_get_name(source);
		obs_property_list_add_string(prop, name, name);
	}

	return true;
}

static bool uses_audio(struct shader_filter_data *filter)
{
	return filter->param_spec || filter->param_audio_magnitude || filter->param_audio_peak || uses_audio_bands(filter);
}

static inline void shader_filter_set_tooltip(obs_property_t *p, const struct dstr *tooltip)
{
	if (p && tooltip && tooltip->len)
		obs_property_set_long_description(p, tooltip->array);
}

static obs_properties_t *shader_filter_properties(void *data)
{
	struct shader_filter_data *filter = data;

	struct dstr examples_path = {0};
	dstr_init(&examples_path);
	dstr_cat(&examples_path, obs_get_module_data_path(obs_current_module()));
	dstr_cat(&examples_path, "/examples");

	obs_properties_t *props = obs_properties_create();
	obs_properties_set_param(props, filter, NULL);

	if (!filter || !filter->transition) {
		obs_properties_add_int(props, "expand_left", obs_module_text("ShaderFilter.ExpandLeft"), 0, 9999, 1);
		obs_properties_add_int(props, "expand_right", obs_module_text("ShaderFilter.ExpandRight"), 0, 9999, 1);
		obs_properties_add_int(props, "expand_top", obs_module_text("ShaderFilter.ExpandTop"), 0, 9999, 1);
		obs_properties_add_int(props, "expand_bottom", obs_module_text("ShaderFilter.ExpandBottom"), 0, 9999, 1);
	}

	obs_properties_add_bool(props, "override_entire_effect", obs_module_text("ShaderFilter.OverrideEntireEffect"));

	obs_property_t *from_file = obs_properties_add_bool(props, "from_file", obs_module_text("ShaderFilter.LoadFromFile"));
	obs_property_set_modified_callback(from_file, shader_filter_from_file_changed);

	obs_property_t *shader_text =
		obs_properties_add_text(props, "shader_text", obs_module_text("ShaderFilter.ShaderText"), OBS_TEXT_MULTILINE);
	obs_property_set_modified_callback(shader_text, shader_filter_text_changed);

	obs_properties_add_button2(props, "shader_convert", obs_module_text("ShaderFilter.Convert"), shader_filter_convert, data);

	char *abs_path = os_get_abs_path_ptr(examples_path.array);
	obs_property_t *file_name = obs_properties_add_path(props, "shader_file_name",
							    obs_module_text("ShaderFilter.ShaderFileName"), OBS_PATH_FILE, NULL,
							    abs_path ? abs_path : examples_path.array);
	if (abs_path)
		bfree(abs_path);
	dstr_free(&examples_path);
	obs_property_set_modified_callback(file_name, shader_filter_file_name_changed);

	if (filter) {
		obs_data_t *settings = obs_source_get_settings(filter->context);
		const char *last_error = obs_data_get_string(settings, "last_error");
		if (last_error && strlen(last_error)) {
			obs_property_t *error =
				obs_properties_add_text(props, "last_error", obs_module_text("ShaderFilter.Error"), OBS_TEXT_INFO);
			obs_property_text_set_info_type(error, OBS_TEXT_INFO_ERROR);
		}
		obs_data_release(settings);
	}

	obs_properties_add_button2(props, "reload_effect", obs_module_text("ShaderFilter.ReloadEffect"),
				   shader_filter_reload_effect_clicked, data);

	if (filter && (uses_audio(filter))) {
		obs_property_t *audio_source = obs_properties_add_list(props, "audio_source", "Audio source", OBS_COMBO_TYPE_LIST,
								       OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(audio_source, "None", "");

		obs_enum_sources(shader_filter_enum_audio_sources, audio_source);
	}

	DARRAY(obs_property_t *) groups;
	da_init(groups);

	size_t param_count = filter->stored_param_list.num;
	for (size_t param_index = 0; param_index < param_count; param_index++) {
		struct effect_param_data *param = (filter->stored_param_list.array + param_index);
		//gs_eparam_t *annot = gs_param_get_annotation_by_idx(param->param, param_index);
		const char *param_name = param->name.array;
		const char *label = param->display_name.array;
		const char *widget_type = param->widget_type.array;
		const char *group_name = param->group.array;
		const int *options = param->option_values.array;
		const struct dstr *option_labels = param->option_labels.array;

		struct dstr display_name = {0};
		struct dstr sources_name = {0};

		if (label == NULL) {
			dstr_ncat(&display_name, param_name, param->name.len);
			dstr_replace(&display_name, "_", " ");
		} else {
			dstr_ncat(&display_name, label, param->display_name.len);
		}
		obs_properties_t *group = NULL;
		if (group_name && strlen(group_name)) {
			for (size_t i = 0; i < groups.num; i++) {
				const char *n = obs_property_name(groups.array[i]);
				if (strcmp(n, group_name) == 0) {
					group = obs_property_group_content(groups.array[i]);
				}
			}
			if (!group) {
				group = obs_properties_create();
				obs_property_t *p =
					obs_properties_add_group(props, group_name, group_name, OBS_GROUP_NORMAL, group);
				da_push_back(groups, &p);
			}
		}
		if (!group)
			group = props;
		obs_property_t *p = NULL;
		switch (param->type) {
		case GS_SHADER_PARAM_BOOL:
			p = obs_properties_add_bool(group, param_name, display_name.array);
			break;
		case GS_SHADER_PARAM_FLOAT: {
			double range_min = param->minimum.f;
			double range_max = param->maximum.f;
			double step = param->step.f;
			if (range_min == range_max) {
				range_min = -1000.0;
				range_max = 1000.0;
				step = 0.0001;
			}
			obs_properties_remove_by_name(props, param_name);
			if (widget_type != NULL && strcmp(widget_type, "slider") == 0) {
				p = obs_properties_add_float_slider(group, param_name, display_name.array, range_min, range_max,
								    step);
			} else {
				p = obs_properties_add_float(group, param_name, display_name.array, range_min, range_max, step);
			}
			break;
		}
		case GS_SHADER_PARAM_INT: {
			int range_min = (int)param->minimum.i;
			int range_max = (int)param->maximum.i;
			int step = (int)param->step.i;
			if (range_min == range_max) {
				range_min = -1000;
				range_max = 1000;
				step = 1;
			}
			obs_properties_remove_by_name(props, param_name);

			if (widget_type != NULL && strcmp(widget_type, "slider") == 0) {
				p = obs_properties_add_int_slider(group, param_name, display_name.array, range_min, range_max,
								  step);
			} else if (widget_type != NULL && strcmp(widget_type, "select") == 0) {
				p = obs_properties_add_list(group, param_name, display_name.array, OBS_COMBO_TYPE_LIST,
							    OBS_COMBO_FORMAT_INT);
				for (size_t i = 0; i < param->option_values.num; i++) {
					obs_property_list_add_int(p, option_labels[i].array, options[i]);
				}
			} else {
				p = obs_properties_add_int(group, param_name, display_name.array, range_min, range_max, step);
			}
			break;
		}
		case GS_SHADER_PARAM_INT3:

			break;
		case GS_SHADER_PARAM_VEC2: {
			double range_min = param->minimum.f;
			double range_max = param->maximum.f;
			double step = param->step.f;
			if (range_min == range_max) {
				range_min = -1000.0;
				range_max = 1000.0;
				step = 0.0001;
			}

			bool slider = (widget_type != NULL && strcmp(widget_type, "slider") == 0);

			for (size_t i = 0; i < 2; i++) {
				dstr_printf(&sources_name, "%s_%zu", param_name, i);
				obs_property_t *sub = NULL;
				if (i < param->option_labels.num) {
					if (slider) {
						sub = obs_properties_add_float_slider(group, sources_name.array,
										      param->option_labels.array[i].array,
										      range_min, range_max, step);
					} else {
						sub = obs_properties_add_float(group, sources_name.array,
									       param->option_labels.array[i].array, range_min,
									       range_max, step);
					}
				} else if (slider) {

					sub = obs_properties_add_float_slider(group, sources_name.array, display_name.array,
									      range_min, range_max, step);
				} else {
					sub = obs_properties_add_float(group, sources_name.array, display_name.array, range_min,
								       range_max, step);
				}
				shader_filter_set_tooltip(sub, &param->tooltip);
				if (i == 0)
					p = sub;
			}
			dstr_free(&sources_name);

			break;
		}
		case GS_SHADER_PARAM_VEC3:
			if (widget_type != NULL && strcmp(widget_type, "slider") == 0) {
				double range_min = param->minimum.f;
				double range_max = param->maximum.f;
				double step = param->step.f;
				if (range_min == range_max) {
					range_min = -1000.0;
					range_max = 1000.0;
					step = 0.0001;
				}
				for (size_t i = 0; i < 3; i++) {
					dstr_printf(&sources_name, "%s_%zu", param_name, i);
					obs_property_t *sub = NULL;
					if (i < param->option_labels.num) {
						sub = obs_properties_add_float_slider(group, sources_name.array,
										      param->option_labels.array[i].array,
										      range_min, range_max, step);
					} else {
						sub = obs_properties_add_float_slider(group, sources_name.array, display_name.array,
										      range_min, range_max, step);
					}
					shader_filter_set_tooltip(sub, &param->tooltip);
					if (i == 0)
						p = sub;
				}
				dstr_free(&sources_name);
			} else {
				p = obs_properties_add_color(group, param_name, display_name.array);
			}
			break;
		case GS_SHADER_PARAM_VEC4:
			if (widget_type != NULL && strcmp(widget_type, "slider") == 0) {
				double range_min = param->minimum.f;
				double range_max = param->maximum.f;
				double step = param->step.f;
				if (range_min == range_max) {
					range_min = -1000.0;
					range_max = 1000.0;
					step = 0.0001;
				}
				for (size_t i = 0; i < 4; i++) {
					dstr_printf(&sources_name, "%s_%zu", param_name, i);
					obs_property_t *sub = NULL;
					if (i < param->option_labels.num) {
						sub = obs_properties_add_float_slider(group, sources_name.array,
										      param->option_labels.array[i].array,
										      range_min, range_max, step);
					} else {
						sub = obs_properties_add_float_slider(group, sources_name.array, display_name.array,
										      range_min, range_max, step);
					}
					shader_filter_set_tooltip(sub, &param->tooltip);
					if (i == 0)
						p = sub;
				}
				dstr_free(&sources_name);
			} else {
				p = obs_properties_add_color_alpha(group, param_name, display_name.array);
			}
			break;
		case GS_SHADER_PARAM_TEXTURE:
			if (widget_type != NULL && strcmp(widget_type, "source") == 0) {
				dstr_init_copy_dstr(&sources_name, &param->name);
				dstr_cat(&sources_name, "_source");
				p = obs_properties_add_list(group, sources_name.array, display_name.array, OBS_COMBO_TYPE_EDITABLE,
							    OBS_COMBO_FORMAT_STRING);
				dstr_free(&sources_name);
				obs_enum_sources(add_source_to_list, p);
				obs_enum_scenes(add_source_to_list, p);
				obs_property_list_insert_string(p, 0, "", "");

			} else if (widget_type != NULL && strcmp(widget_type, "file") == 0) {
				p = obs_properties_add_path(group, param_name, display_name.array, OBS_PATH_FILE,
							    shader_filter_texture_file_filter, NULL);
			} else {
				dstr_init_copy_dstr(&sources_name, &param->name);
				dstr_cat(&sources_name, "_source");
				obs_property_t *src = obs_properties_add_list(group, sources_name.array, display_name.array,
									      OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
				dstr_free(&sources_name);
				obs_property_list_add_string(src, "", "");
				obs_enum_sources(add_source_to_list, src);
				obs_enum_scenes(add_source_to_list, src);
				shader_filter_set_tooltip(src, &param->tooltip);
				p = obs_properties_add_path(group, param_name, display_name.array, OBS_PATH_FILE,
							    shader_filter_texture_file_filter, NULL);
			}
			break;
		case GS_SHADER_PARAM_STRING:
			if (widget_type != NULL && strcmp(widget_type, "info") == 0) {
				p = obs_properties_add_text(group, param_name, display_name.array, OBS_TEXT_INFO);
			} else {
				p = obs_properties_add_text(group, param_name, display_name.array, OBS_TEXT_MULTILINE);
			}
			break;
		default:;
		}
		shader_filter_set_tooltip(p, &param->tooltip);
		dstr_free(&display_name);
	}
	da_free(groups);

	obs_properties_add_text(
		props, "plugin_info",
		"<a href=\"https://obsproject.com/forum/resources/obs-shaderfilter.1736/\">obs-shaderfilter</a> (" PROJECT_VERSION
		") by <a href=\"https://www.exeldro.com\">Exeldro</a>",
		OBS_TEXT_INFO);
	return props;
}

static void shader_filter_update(void *data, obs_data_t *settings)
{
	struct shader_filter_data *filter = data;

	// Get expansions. Will be used in the video_tick() callback.

	filter->expand_left = (int)obs_data_get_int(settings, "expand_left");
	filter->expand_right = (int)obs_data_get_int(settings, "expand_right");
	filter->expand_top = (int)obs_data_get_int(settings, "expand_top");
	filter->expand_bottom = (int)obs_data_get_int(settings, "expand_bottom");
	filter->rand_activation_f = (float)((double)rand_interval(0, 10000) / (double)10000);

	if (filter->reload_effect) {
		filter->reload_effect = false;
		shader_filter_reload_effect(filter);
		obs_source_update_properties(filter->context);
	}

	if (uses_audio(filter)) {
		const char *audio_source_name = obs_data_get_string(settings, "audio_source");
		if (!filter->audio_source_name || strcmp(filter->audio_source_name, audio_source_name) != 0 ||
		    !filter->audio_analysis) {
			obs_source_t *audio_source = strlen(audio_source_name) > 0 ? obs_get_source_by_name(audio_source_name)
										   : NULL;
			if (audio_source && ((obs_source_get_output_flags(audio_source) & OBS_SOURCE_AUDIO) == 0)) {
				obs_source_release(audio_source);
				audio_source = NULL;
			}
			if (!audio_source) {
				audio_source = obs_source_get_ref(obs_filter_get_parent(filter->context));
				if (audio_source && ((obs_source_get_output_flags(audio_source) & OBS_SOURCE_AUDIO) == 0)) {
					obs_source_release(audio_source);
					audio_source = NULL;
				}
			}
			if (audio_source) {
				if (filter->audio_source_name)
					bfree(filter->audio_source_name);
				filter->audio_source_name = bstrdup(audio_source_name);
				shader_filter_attach_audio_analysis(filter, audio_source);
				obs_source_release(audio_source);
			} else {
				shader_filter_release_audio_analysis(filter);
				if (filter->audio_source_name) {
					bfree(filter->audio_source_name);
					filter->audio_source_name = NULL;
				}
			}
		}
	} else {
		shader_filter_release_audio_analysis(filter);
		if (filter->audio_source_name) {
			bfree(filter->audio_source_name);
			filter->audio_source_name = NULL;
		}
	}

	shader_filter_sync_spectrum(filter);

	size_t param_count = filter->stored_param_list.num;
	for (size_t param_index = 0; param_index < param_count; param_index++) {
		struct effect_param_data *param = (filter->stored_param_list.array + param_index);
		//gs_eparam_t *annot = gs_param_get_annotation_by_idx(param->param, param_index);
		const char *param_name = param->name.array;
		struct dstr sources_name = {0};
		obs_source_t *source = NULL;
		void *default_value = gs_effect_get_default_val(param->param);
		param->has_default = false;
		switch (param->type) {
		case GS_SHADER_PARAM_BOOL:
			if (default_value != NULL) {
				obs_data_set_default_bool(settings, param_name, *(bool *)default_value);
				param->default_value.i = *(bool *)default_value;
				param->has_default = true;
			}
			param->value.i = obs_data_get_bool(settings, param_name);
			break;
		case GS_SHADER_PARAM_FLOAT:
			if (default_value != NULL) {
				obs_data_set_default_double(settings, param_name, *(float *)default_value);
				param->default_value.f = *(float *)default_value;
				param->has_default = true;
			}
			param->value.f = obs_data_get_double(settings, param_name);
			break;
		case GS_SHADER_PARAM_INT:
			if (default_value != NULL) {
				obs_data_set_default_int(settings, param_name, *(int *)default_value);
				param->default_value.i = *(int *)default_value;
				param->has_default = true;
			}
			param->value.i = obs_data_get_int(settings, param_name);
			break;
		case GS_SHADER_PARAM_VEC2: {
			struct vec2 *xy = default_value;

			for (size_t i = 0; i < 2; i++) {
				dstr_printf(&sources_name, "%s_%zu", param_name, i);
				if (xy != NULL) {
					obs_data_set_default_double(settings, sources_name.array, xy->ptr[i]);
					param->default_value.vec2.ptr[i] = xy->ptr[i];
					param->has_default = true;
				}
				param->value.vec2.ptr[i] = (float)obs_data_get_double(settings, sources_name.array);
			}
			dstr_free(&sources_name);
			break;
		}
		case GS_SHADER_PARAM_VEC3: {
			struct vec3 *rgb = default_value;
			if (param->widget_type.array && strcmp(param->widget_type.array, "slider") == 0) {
				for (size_t i = 0; i < 3; i++) {
					dstr_printf(&sources_name, "%s_%zu", param_name, i);
					if (rgb != NULL) {
						obs_data_set_default_double(settings, sources_name.array, rgb->ptr[i]);
						param->default_value.vec3.ptr[i] = rgb->ptr[i];
						param->has_default = true;
					}
					param->value.vec3.ptr[i] = (float)obs_data_get_double(settings, sources_name.array);
				}
				dstr_free(&sources_name);
			} else {
				if (rgb != NULL) {
					struct vec4 rgba;
					vec4_from_vec3(&rgba, rgb);
					obs_data_set_default_int(settings, param_name, vec4_to_rgba(&rgba));
					param->default_value.vec4 = rgba;
					param->has_default = true;
				} else {
					// Hack to ensure we have a default...(white)
					obs_data_set_default_int(settings, param_name, 0xffffffff);
				}
				vec4_from_rgba(&param->value.vec4, (uint32_t)obs_data_get_int(settings, param_name));
			}
			break;
		}
		case GS_SHADER_PARAM_VEC4: {
			struct vec4 *rgba = default_value;
			if (param->widget_type.array && strcmp(param->widget_type.array, "slider") == 0) {
				for (size_t i = 0; i < 4; i++) {
					dstr_printf(&sources_name, "%s_%zu", param_name, i);
					if (rgba != NULL) {
						obs_data_set_default_double(settings, sources_name.array, rgba->ptr[i]);
						param->default_value.vec4.ptr[i] = rgba->ptr[i];
						param->has_default = true;
					}
					param->value.vec4.ptr[i] = (float)obs_data_get_double(settings, sources_name.array);
				}
				dstr_free(&sources_name);
			} else {
				if (rgba != NULL) {
					obs_data_set_default_int(settings, param_name, vec4_to_rgba(rgba));
					param->default_value.vec4 = *rgba;
					param->has_default = true;
				} else {
					// Hack to ensure we have a default...(white)
					obs_data_set_default_int(settings, param_name, 0xffffffff);
				}
				vec4_from_rgba(&param->value.vec4, (uint32_t)obs_data_get_int(settings, param_name));
			}
			break;
		}
		case GS_SHADER_PARAM_TEXTURE:
			dstr_init_copy_dstr(&sources_name, &param->name);
			dstr_cat(&sources_name, "_source");
			const char *sn = obs_data_get_string(settings, sources_name.array);
			dstr_free(&sources_name);
			source = obs_weak_source_get_source(param->source);
			if (source && strcmp(obs_source_get_name(source), sn) != 0) {
				obs_source_release(source);
				source = NULL;
			}
			if (!source)
				source = (sn && strlen(sn)) ? obs_get_source_by_name(sn) : NULL;
			if (source) {
				if (!obs_weak_source_references_source(param->source, source)) {
					if ((!filter->transition || filter->prev_transitioning) &&
					    obs_source_active(filter->context))
						obs_source_inc_active(source);
					if ((!filter->transition || filter->prev_transitioning) &&
					    obs_source_showing(filter->context))
						obs_source_inc_showing(source);

					obs_source_t *old_source = obs_weak_source_get_source(param->source);
					if (old_source) {
						if ((!filter->transition || filter->prev_transitioning) &&
						    obs_source_active(filter->context))
							obs_source_dec_active(old_source);
						if ((!filter->transition || filter->prev_transitioning) &&
						    obs_source_showing(filter->context))
							obs_source_dec_showing(old_source);
						obs_source_release(old_source);
					}
					obs_weak_source_release(param->source);
					param->source = obs_source_get_weak_source(source);
				}
				obs_source_release(source);
				if (param->image) {
					obs_enter_graphics();
					gs_image_file_free_func(param->image);
					obs_leave_graphics();
					bfree(param->image);
					param->image = NULL;
				}
				dstr_free(&param->path);
			} else {
				const char *path = default_value;
				if (!obs_data_has_user_value(settings, param_name) && path && strlen(path)) {
					if (os_file_exists(path)) {
						char *abs_path = os_get_abs_path_ptr(path);
						obs_data_set_default_string(settings, param_name, abs_path);
						bfree(abs_path);
						param->has_default = true;
					} else {
						struct dstr texture_path = {0};
						dstr_init(&texture_path);
						dstr_cat(&texture_path, obs_get_module_data_path(obs_current_module()));
						dstr_cat(&texture_path, "/textures/");
						dstr_cat(&texture_path, path);
						char *abs_path = os_get_abs_path_ptr(texture_path.array);
						if (os_file_exists(abs_path)) {
							obs_data_set_default_string(settings, param_name, abs_path);
							param->has_default = true;
						}
						bfree(abs_path);
						dstr_free(&texture_path);
					}
				}
				path = obs_data_get_string(settings, param_name);
				bool n = false;
				if (param->image == NULL && gs_image_file_create_func) {
					param->image = gs_image_file_create_func();
					n = true;
				}
				if (n || !path || !param->path.array || strcmp(path, param->path.array) != 0) {
					if (param->image) {
						if (!n) {
							obs_enter_graphics();
							gs_image_file_free_func(param->image);
							obs_leave_graphics();
						}
						gs_image_file_init_func(param->image, path, GS_IMAGE_ALPHA_PREMULTIPLY_SRGB);
						dstr_copy(&param->path, path);
						obs_enter_graphics();
						gs_image_file_init_texture_func(param->image);
						obs_leave_graphics();
					}
				}
				obs_source_t *old_source = obs_weak_source_get_source(param->source);
				if (old_source) {
					if ((!filter->transition || filter->prev_transitioning) &&
					    obs_source_active(filter->context))
						obs_source_dec_active(old_source);
					if ((!filter->transition || filter->prev_transitioning) &&
					    obs_source_showing(filter->context))
						obs_source_dec_showing(old_source);
					obs_source_release(old_source);
				}
				obs_weak_source_release(param->source);
				param->source = NULL;
			}
			break;
		case GS_SHADER_PARAM_STRING:
			if (default_value != NULL) {
				obs_data_set_default_string(settings, param_name, (const char *)default_value);
				param->has_default = true;
			}
			param->value.string = (char *)obs_data_get_string(settings, param_name);
			break;
		default:;
		}
		bfree(default_value);
	}
}

static void shader_filter_tick(void *data, float seconds)
{
	struct shader_filter_data *filter = data;
	obs_source_t *target = filter->transition ? filter->context : obs_filter_get_target(filter->context);
	if (!target)
		return;
	// Determine offsets from expansion values.
	int base_width = obs_source_get_base_width(target);
	int base_height = obs_source_get_base_height(target);

	filter->total_width = filter->expand_left + base_width + filter->expand_right;
	filter->total_height = filter->expand_top + base_height + filter->expand_bottom;

	filter->uv_size.x = (float)filter->total_width;
	filter->uv_size.y = (float)filter->total_height;

	filter->uv_scale.x = (float)filter->total_width / base_width;
	filter->uv_scale.y = (float)filter->total_height / base_height;

	filter->uv_offset.x = (float)(-filter->expand_left) / base_width;
	filter->uv_offset.y = (float)(-filter->expand_top) / base_height;

	filter->uv_pixel_interval.x = 1.0f / base_width;
	filter->uv_pixel_interval.y = 1.0f / base_height;

	if (filter->shader_start_time == 0.0f) {
		filter->shader_start_time = filter->elapsed_time + seconds;
	}
	filter->elapsed_time += seconds;
	filter->elapsed_time_loop += seconds;
	if (filter->elapsed_time_loop > 1.0f) {
		filter->elapsed_time_loop -= 1.0f;

		// Loops
		filter->loops += 1;
		if (filter->loops >= 4194304)
			filter->loops = -filter->loops;
	}
	filter->local_time = (float)(os_gettime_ns() / 1000000000.0);
	if (filter->enabled != obs_source_enabled(filter->context)) {
		filter->enabled = !filter->enabled;
		if (filter->enabled)
			filter->shader_enable_time = filter->elapsed_time;
	}
	obs_source_t *parent = obs_filter_get_parent(filter->context);
	if (obs_source_enabled(filter->context) && parent && obs_source_active(parent)) {
		filter->shader_active_time += seconds;
	} else {
		filter->shader_active_time = 0.0f;
	}
	if (obs_source_enabled(filter->context) && parent && obs_source_showing(parent)) {
		filter->shader_show_time += seconds;
	} else {
		filter->shader_show_time = 0.0f;
	}

	// undecided between this and "rand_float(1);"
	filter->rand_f = (float)((double)rand_interval(0, 10000) / (double)10000);

	if (filter->audio_analysis) {
		shader_filter_copy_audio_analysis(filter);
	} else {
		shader_filter_clear_audio_snapshot(filter);
	}

	filter->output_rendered = false;
	filter->input_rendered = false;

	if (gs_image_file_tick_func) {
		uint64_t frame_time = obs_get_video_frame_time();
		if (filter->last_frame_time) {
			bool g = false;
			uint64_t elapsed = frame_time - filter->last_frame_time;
			size_t param_count = filter->stored_param_list.num;
			for (size_t param_index = 0; param_index < param_count; param_index++) {
				struct effect_param_data *param = (filter->stored_param_list.array + param_index);
				if (!param->image)
					continue;

				bool updated = gs_image_file_tick_func(param->image, elapsed);
				if (updated) {
					if (!g) {
						obs_enter_graphics();
						g = true;
					}
					gs_image_file_update_texture_func(param->image);
				}
			}
			if (g) {
				obs_leave_graphics();
			}
		}
		filter->last_frame_time = frame_time;
	}
}

gs_texrender_t *create_or_reset_texrender(gs_texrender_t *render, const enum gs_color_format format)
{
	if (render && gs_texrender_get_format(render) != format) {
		gs_texrender_destroy(render);
		render = NULL;
	}
	if (!render) {
		render = gs_texrender_create(format, GS_ZS_NONE);
	} else {
		gs_texrender_reset(render);
	}
	return render;
}

static void get_input_source(struct shader_filter_data *filter)
{
	if (filter->input_rendered)
		return;

	// Use the OBS default effect file as our effect.
	gs_effect_t *pass_through = obs_get_base_effect(OBS_EFFECT_DEFAULT);

	// Set up our color space info.
	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};

	const enum gs_color_space source_space =
		obs_source_get_color_space(obs_filter_get_target(filter->context), OBS_COUNTOF(preferred_spaces), preferred_spaces);

	const enum gs_color_format format = gs_get_format_from_space(source_space);

	if (filter->param_previous_image) {
		gs_texrender_t *temp = filter->input_texrender;
		filter->input_texrender = filter->previous_input_texrender;
		filter->previous_input_texrender = temp;
	}

	// Set up our input_texrender to catch the output texture.
	filter->input_texrender = create_or_reset_texrender(filter->input_texrender, format);

	// Start the rendering process with our correct color space params,
	// And set up your texrender to recieve the created texture.
	if (!filter->transition &&
	    !obs_source_process_filter_begin_with_color_space(filter->context, format, source_space, OBS_NO_DIRECT_RENDERING))
		return;

	if (gs_texrender_begin(filter->input_texrender, filter->total_width, filter->total_height)) {

		gs_blend_state_push();
		gs_reset_blend_state();
		gs_enable_blending(false);
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		gs_ortho(0.0f, (float)filter->total_width, 0.0f, (float)filter->total_height, -100.0f, 100.0f);
		// The incoming source is pre-multiplied alpha, so use the
		// OBS default effect "DrawAlphaDivide" technique to convert
		// the colors back into non-pre-multiplied space. If the shader
		// file has #define USE_PM_ALPHA 1, then use normal "Draw"
		// technique.
		const char *technique = filter->use_pm_alpha ? "Draw" : "DrawAlphaDivide";
		if (!filter->transition)
			obs_source_process_filter_tech_end(filter->context, pass_through, filter->total_width, filter->total_height,
							   technique);
		gs_texrender_end(filter->input_texrender);
		gs_blend_state_pop();
		filter->input_rendered = true;
	}
}

static void draw_output(struct shader_filter_data *filter)
{
	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};

	const enum gs_color_space source_space =
		obs_source_get_color_space(obs_filter_get_target(filter->context), OBS_COUNTOF(preferred_spaces), preferred_spaces);

	const enum gs_color_format format = gs_get_format_from_space(source_space);

	if (!obs_source_process_filter_begin_with_color_space(filter->context, format, source_space, OBS_NO_DIRECT_RENDERING)) {
		return;
	}

	gs_texture_t *texture = gs_texrender_get_texture(filter->output_texrender);
	gs_effect_t *pass_through = filter->output_effect;
	if (!pass_through)
		pass_through = obs_get_base_effect(OBS_EFFECT_DEFAULT);

	if (filter->param_output_image) {
		gs_effect_set_texture(filter->param_output_image, texture);
	}

	obs_source_process_filter_end(filter->context, pass_through, filter->total_width, filter->total_height);
}

void shader_filter_set_effect_params(struct shader_filter_data *filter)
{
	if (filter->param_spec) {
		// Read at draw time so an FFT completed after video_tick is visible in
		// this frame, without another frame of snapshot delay.
		struct audio_source_analysis *analysis = filter->audio_analysis;
		if (analysis)
			pthread_mutex_lock(&analysis->mutex);
		filter->spec_revision = sf_spectrum_read(analysis ? analysis->spectrum : NULL, filter->spec_values, os_gettime_ns());
		if (analysis)
			pthread_mutex_unlock(&analysis->mutex);
		if (!filter->spec_texture) {
			const uint8_t *pixels = (const uint8_t *)filter->spec_values;
			filter->spec_texture = gs_texture_create(SF_SPECTRUM_BANDS, SF_SPECTRUM_ROWS, GS_R32F, 1, &pixels, GS_DYNAMIC);
			filter->spec_uploaded_revision = filter->spec_revision;
		} else if (filter->spec_uploaded_revision != filter->spec_revision) {
			gs_texture_set_image(filter->spec_texture, (const uint8_t *)filter->spec_values,
					     SF_SPECTRUM_BANDS * sizeof(float), false);
			filter->spec_uploaded_revision = filter->spec_revision;
		}
		gs_effect_set_texture(filter->param_spec, filter->spec_texture);
	}
	if (filter->param_gamepad_available || filter->param_gamepad_info || filter->param_gamepad_buttons ||
	    filter->param_gamepad_presses || filter->param_gamepad_axes || filter->param_gamepad_axes_c ||
	    filter->param_gamepad_axes_clamped || filter->param_gamepad_data) {
		struct sf_gamepad_snapshot gamepads;
		sf_gamepad_get_snapshot(&gamepads);
		if (filter->param_gamepad_data) {
			const uint64_t frame = obs_get_video_frame_time();
			if (!gamepad_texture || frame != gamepad_texture_frame) {
				float pixels[SF_GAMEPAD_COUNT * SF_GAMEPAD_TEXTURE_WIDTH * 4];
				sf_gamepad_pack_snapshot(&gamepads, pixels);
				if (!gamepad_texture) {
					const uint8_t *initial_data = (const uint8_t *)pixels;
					gamepad_texture = gs_texture_create(SF_GAMEPAD_TEXTURE_WIDTH, SF_GAMEPAD_COUNT,
								    GS_RGBA32F, 1, &initial_data, GS_DYNAMIC);
				} else {
					gs_texture_set_image(gamepad_texture, (const uint8_t *)pixels,
							     SF_GAMEPAD_TEXTURE_WIDTH * 4 * sizeof(float), false);
				}
				gamepad_texture_frame = frame;
			}
			gs_effect_set_texture(filter->param_gamepad_data, gamepad_texture);
		}
		if (filter->param_gamepad_available)
			gs_effect_set_int(filter->param_gamepad_available, gamepads.available);
		if (filter->param_gamepad_info)
			gs_effect_set_val(filter->param_gamepad_info, gamepads.info, sizeof(gamepads.info));
		if (filter->param_gamepad_buttons)
			gs_effect_set_val(filter->param_gamepad_buttons, gamepads.buttons, sizeof(gamepads.buttons));
		if (filter->param_gamepad_presses)
			gs_effect_set_val(filter->param_gamepad_presses, gamepads.presses, sizeof(gamepads.presses));
		if (filter->param_gamepad_axes)
			gs_effect_set_val(filter->param_gamepad_axes, gamepads.axes, sizeof(gamepads.axes));
		if (filter->param_gamepad_axes_c)
			gs_effect_set_val(filter->param_gamepad_axes_c, gamepads.axes_c, sizeof(gamepads.axes_c));
		if (filter->param_gamepad_axes_clamped)
			gs_effect_set_val(filter->param_gamepad_axes_clamped, gamepads.axes_clamped, sizeof(gamepads.axes_clamped));
	}

	// Update audio timeseries textures if audio is being used
	if (uses_audio_bands(filter)) {
		shader_filter_render_audio_timeseries_textures(filter);
	}

	if (filter->param_uv_scale != NULL) {
		gs_effect_set_vec2(filter->param_uv_scale, &filter->uv_scale);
	}
	if (filter->param_uv_offset != NULL) {
		gs_effect_set_vec2(filter->param_uv_offset, &filter->uv_offset);
	}
	if (filter->param_uv_pixel_interval != NULL) {
		gs_effect_set_vec2(filter->param_uv_pixel_interval, &filter->uv_pixel_interval);
	}
	if (filter->param_current_time_ms != NULL) {
#ifdef _WIN32
		SYSTEMTIME system_time;
		GetSystemTime(&system_time);
		gs_effect_set_int(filter->param_current_time_ms, system_time.wMilliseconds);
#else
		struct timeval tv;
		gettimeofday(&tv, NULL);
		gs_effect_set_int(filter->param_current_time_ms, tv.tv_usec / 1000);
#endif
	}
	if (filter->param_current_time_sec != NULL || filter->param_current_time_min != NULL ||
	    filter->param_current_time_hour != NULL || filter->param_current_time_day_of_week != NULL ||
	    filter->param_current_time_day_of_month != NULL || filter->param_current_time_month != NULL ||
	    filter->param_current_time_day_of_year != NULL || filter->param_current_time_year != NULL) {
		time_t t = time(NULL);
		struct tm *lt = localtime(&t);
		if (filter->param_current_time_sec != NULL)
			gs_effect_set_int(filter->param_current_time_sec, lt->tm_sec);
		if (filter->param_current_time_min != NULL)
			gs_effect_set_int(filter->param_current_time_min, lt->tm_min);
		if (filter->param_current_time_hour != NULL)
			gs_effect_set_int(filter->param_current_time_hour, lt->tm_hour);
		if (filter->param_current_time_day_of_week != NULL)
			gs_effect_set_int(filter->param_current_time_day_of_week, lt->tm_wday);
		if (filter->param_current_time_day_of_month != NULL)
			gs_effect_set_int(filter->param_current_time_day_of_month, lt->tm_mday);
		if (filter->param_current_time_month != NULL)
			gs_effect_set_int(filter->param_current_time_month, lt->tm_mon);
		if (filter->param_current_time_day_of_year != NULL)
			gs_effect_set_int(filter->param_current_time_day_of_year, lt->tm_yday);
		if (filter->param_current_time_year != NULL)
			gs_effect_set_int(filter->param_current_time_year, lt->tm_year);
	}
	if (filter->param_elapsed_time != NULL) {
		gs_effect_set_float(filter->param_elapsed_time, filter->elapsed_time);
	}
	if (filter->param_elapsed_time_start != NULL) {
		gs_effect_set_float(filter->param_elapsed_time_start, filter->elapsed_time - filter->shader_start_time);
	}
	if (filter->param_elapsed_time_show != NULL) {
		gs_effect_set_float(filter->param_elapsed_time_show, filter->shader_show_time);
	}
	if (filter->param_elapsed_time_active != NULL) {
		gs_effect_set_float(filter->param_elapsed_time_active, filter->shader_active_time);
	}
	if (filter->param_elapsed_time_enable != NULL) {
		gs_effect_set_float(filter->param_elapsed_time_enable, filter->elapsed_time - filter->shader_enable_time);
	}
	if (filter->param_uv_size != NULL) {
		gs_effect_set_vec2(filter->param_uv_size, &filter->uv_size);
	}
	if (filter->param_local_time != NULL) {
		gs_effect_set_float(filter->param_local_time, filter->local_time);
	}
	if (filter->param_audio_peak != NULL) {
		gs_effect_set_float(filter->param_audio_peak, filter->audio_peak);
	}
	if (filter->param_audio_magnitude != NULL) {
		gs_effect_set_float(filter->param_audio_magnitude, filter->audio_magnitude);
	}
	if (filter->param_energy != NULL)
		gs_effect_set_float(filter->param_energy, filter->energy);
	if (filter->param_e_bass != NULL)
		gs_effect_set_float(filter->param_e_bass, filter->e_bass);
	if (filter->param_e_lows != NULL)
		gs_effect_set_float(filter->param_e_lows, filter->e_lows);
	if (filter->param_e_mids != NULL)
		gs_effect_set_float(filter->param_e_mids, filter->e_mids);
	if (filter->param_e_treb != NULL)
		gs_effect_set_float(filter->param_e_treb, filter->e_treb);
	if (filter->param_bass != NULL) {
		gs_effect_set_float(filter->param_bass, filter->bass);
	}
	if (filter->param_lows != NULL) {
		gs_effect_set_float(filter->param_lows, filter->lows);
	}
	if (filter->param_mids != NULL) {
		gs_effect_set_float(filter->param_mids, filter->mids);
	}
	if (filter->param_treb != NULL) {
		gs_effect_set_float(filter->param_treb, filter->treb);
	}
	if (filter->param_bass_c != NULL) {
		gs_effect_set_float(filter->param_bass_c, filter->bass_c);
	}
	if (filter->param_lows_c != NULL) {
		gs_effect_set_float(filter->param_lows_c, filter->lows_c);
	}
	if (filter->param_mids_c != NULL) {
		gs_effect_set_float(filter->param_mids_c, filter->mids_c);
	}
	if (filter->param_treb_c != NULL) {
		gs_effect_set_float(filter->param_treb_c, filter->treb_c);
	}
	if (filter->param_t_bass != NULL && filter->t_bass_render) {
		gs_texture_t *t = gs_texrender_get_texture(filter->t_bass_render);
		if (t)
			gs_effect_set_texture(filter->param_t_bass, t);
	}
	if (filter->param_t_lows != NULL && filter->t_lows_render) {
		gs_texture_t *t = gs_texrender_get_texture(filter->t_lows_render);
		if (t)
			gs_effect_set_texture(filter->param_t_lows, t);
	}
	if (filter->param_t_mids != NULL && filter->t_mids_render) {
		gs_texture_t *t = gs_texrender_get_texture(filter->t_mids_render);
		if (t)
			gs_effect_set_texture(filter->param_t_mids, t);
	}
	if (filter->param_t_treb != NULL && filter->t_treb_render) {
		gs_texture_t *t = gs_texrender_get_texture(filter->t_treb_render);
		if (t)
			gs_effect_set_texture(filter->param_t_treb, t);
	}
	// Set exponential falloff preset floats
	if (filter->param_ef_bass_fast != NULL)
		gs_effect_set_float(filter->param_ef_bass_fast, filter->ef_bass_fast);
	if (filter->param_ef_bass_medium != NULL)
		gs_effect_set_float(filter->param_ef_bass_medium, filter->ef_bass_medium);
	if (filter->param_ef_bass_slow != NULL)
		gs_effect_set_float(filter->param_ef_bass_slow, filter->ef_bass_slow);
	if (filter->param_ef_bass_slug != NULL)
		gs_effect_set_float(filter->param_ef_bass_slug, filter->ef_bass_slug);

	if (filter->param_ef_lows_fast != NULL)
		gs_effect_set_float(filter->param_ef_lows_fast, filter->ef_lows_fast);
	if (filter->param_ef_lows_medium != NULL)
		gs_effect_set_float(filter->param_ef_lows_medium, filter->ef_lows_medium);
	if (filter->param_ef_lows_slow != NULL)
		gs_effect_set_float(filter->param_ef_lows_slow, filter->ef_lows_slow);
	if (filter->param_ef_lows_slug != NULL)
		gs_effect_set_float(filter->param_ef_lows_slug, filter->ef_lows_slug);

	if (filter->param_ef_mids_fast != NULL)
		gs_effect_set_float(filter->param_ef_mids_fast, filter->ef_mids_fast);
	if (filter->param_ef_mids_medium != NULL)
		gs_effect_set_float(filter->param_ef_mids_medium, filter->ef_mids_medium);
	if (filter->param_ef_mids_slow != NULL)
		gs_effect_set_float(filter->param_ef_mids_slow, filter->ef_mids_slow);
	if (filter->param_ef_mids_slug != NULL)
		gs_effect_set_float(filter->param_ef_mids_slug, filter->ef_mids_slug);

	if (filter->param_ef_treb_fast != NULL)
		gs_effect_set_float(filter->param_ef_treb_fast, filter->ef_treb_fast);
	if (filter->param_ef_treb_medium != NULL)
		gs_effect_set_float(filter->param_ef_treb_medium, filter->ef_treb_medium);
	if (filter->param_ef_treb_slow != NULL)
		gs_effect_set_float(filter->param_ef_treb_slow, filter->ef_treb_slow);
	if (filter->param_ef_treb_slug != NULL)
		gs_effect_set_float(filter->param_ef_treb_slug, filter->ef_treb_slug);

	// Set linear falloff preset floats
	if (filter->param_lf_bass_fast != NULL)
		gs_effect_set_float(filter->param_lf_bass_fast, filter->lf_bass_fast);
	if (filter->param_lf_bass_medium != NULL)
		gs_effect_set_float(filter->param_lf_bass_medium, filter->lf_bass_medium);
	if (filter->param_lf_bass_slow != NULL)
		gs_effect_set_float(filter->param_lf_bass_slow, filter->lf_bass_slow);
	if (filter->param_lf_bass_slug != NULL)
		gs_effect_set_float(filter->param_lf_bass_slug, filter->lf_bass_slug);

	if (filter->param_lf_lows_fast != NULL)
		gs_effect_set_float(filter->param_lf_lows_fast, filter->lf_lows_fast);
	if (filter->param_lf_lows_medium != NULL)
		gs_effect_set_float(filter->param_lf_lows_medium, filter->lf_lows_medium);
	if (filter->param_lf_lows_slow != NULL)
		gs_effect_set_float(filter->param_lf_lows_slow, filter->lf_lows_slow);
	if (filter->param_lf_lows_slug != NULL)
		gs_effect_set_float(filter->param_lf_lows_slug, filter->lf_lows_slug);

	if (filter->param_lf_mids_fast != NULL)
		gs_effect_set_float(filter->param_lf_mids_fast, filter->lf_mids_fast);
	if (filter->param_lf_mids_medium != NULL)
		gs_effect_set_float(filter->param_lf_mids_medium, filter->lf_mids_medium);
	if (filter->param_lf_mids_slow != NULL)
		gs_effect_set_float(filter->param_lf_mids_slow, filter->lf_mids_slow);
	if (filter->param_lf_mids_slug != NULL)
		gs_effect_set_float(filter->param_lf_mids_slug, filter->lf_mids_slug);

	if (filter->param_lf_treb_fast != NULL)
		gs_effect_set_float(filter->param_lf_treb_fast, filter->lf_treb_fast);
	if (filter->param_lf_treb_medium != NULL)
		gs_effect_set_float(filter->param_lf_treb_medium, filter->lf_treb_medium);
	if (filter->param_lf_treb_slow != NULL)
		gs_effect_set_float(filter->param_lf_treb_slow, filter->lf_treb_slow);
	if (filter->param_lf_treb_slug != NULL)
		gs_effect_set_float(filter->param_lf_treb_slug, filter->lf_treb_slug);
	if (filter->param_loops != NULL) {
		gs_effect_set_int(filter->param_loops, filter->loops);
	}
	if (filter->param_loop_second != NULL) {
		gs_effect_set_float(filter->param_loop_second, filter->elapsed_time_loop);
	}
	if (filter->param_rand_f != NULL) {
		gs_effect_set_float(filter->param_rand_f, filter->rand_f);
	}
	if (filter->param_rand_activation_f != NULL) {
		gs_effect_set_float(filter->param_rand_activation_f, filter->rand_activation_f);
	}
	if (filter->param_rand_instance_f != NULL) {
		gs_effect_set_float(filter->param_rand_instance_f, filter->rand_instance_f);
	}

	size_t param_count = filter->stored_param_list.num;
	for (size_t param_index = 0; param_index < param_count; param_index++) {
		struct effect_param_data *param = (filter->stored_param_list.array + param_index);
		if (!param->param)
			continue;
		obs_source_t *source = NULL;

		switch (param->type) {
		case GS_SHADER_PARAM_BOOL:
			gs_effect_set_bool(param->param, param->value.i);
			break;
		case GS_SHADER_PARAM_FLOAT:
			gs_effect_set_float(param->param, (float)param->value.f);
			break;
		case GS_SHADER_PARAM_INT:
			gs_effect_set_int(param->param, (int)param->value.i);
			break;
		case GS_SHADER_PARAM_VEC2:
			gs_effect_set_vec2(param->param, &param->value.vec2);
			break;
		case GS_SHADER_PARAM_VEC3:
			gs_effect_set_vec3(param->param, &param->value.vec3);
			break;
		case GS_SHADER_PARAM_VEC4:
			gs_effect_set_vec4(param->param, &param->value.vec4);
			break;
		case GS_SHADER_PARAM_TEXTURE:
			source = obs_weak_source_get_source(param->source);
			if (source) {
				const enum gs_color_space preferred_spaces[] = {
					GS_CS_SRGB,
					GS_CS_SRGB_16F,
					GS_CS_709_EXTENDED,
				};
				const enum gs_color_space space =
					obs_source_get_color_space(source, OBS_COUNTOF(preferred_spaces), preferred_spaces);
				const enum gs_color_format format = gs_get_format_from_space(space);
				if (!param->render || gs_texrender_get_format(param->render) != format) {
					gs_texrender_destroy(param->render);
					param->render = gs_texrender_create(format, GS_ZS_NONE);
				} else {
					gs_texrender_reset(param->render);
				}
				uint32_t base_width = obs_source_get_base_width(source);
				uint32_t base_height = obs_source_get_base_height(source);
				gs_blend_state_push();
				gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
				if (gs_texrender_begin_with_color_space(param->render, base_width, base_height, space)) {
					const float w = (float)base_width;
					const float h = (float)base_height;
					uint32_t flags = obs_source_get_output_flags(source);
					const bool custom_draw = (flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
					const bool async = (flags & OBS_SOURCE_ASYNC) != 0;
					struct vec4 clear_color;

					vec4_zero(&clear_color);
					gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
					gs_ortho(0.0f, w, 0.0f, h, -100.0f, 100.0f);

					if (!custom_draw && !async)
						obs_source_default_render(source);
					else
						obs_source_video_render(source);
					gs_texrender_end(param->render);
				}
				gs_blend_state_pop();
				obs_source_release(source);
				gs_texture_t *tex = gs_texrender_get_texture(param->render);
				gs_effect_set_texture(param->param, tex);
			} else if (param->image) {
				gs_effect_set_texture(param->param, param->image->texture);
			} else {
				gs_effect_set_texture(param->param, NULL);
			}

			break;
		case GS_SHADER_PARAM_STRING:
			gs_effect_set_val(param->param, (param->value.string ? param->value.string : NULL),
					  gs_effect_get_val_size(param->param));
			break;
		default:;
		}
	}
}

static void build_sprite(struct gs_vb_data *data, float fcx, float fcy, float start_u, float end_u, float start_v, float end_v)
{
	struct vec2 *tvarray = data->tvarray[0].array;

	vec3_zero(data->points);
	vec3_set(data->points + 1, fcx, 0.0f, 0.0f);
	vec3_set(data->points + 2, 0.0f, fcy, 0.0f);
	vec3_set(data->points + 3, fcx, fcy, 0.0f);
	vec2_set(tvarray, start_u, start_v);
	vec2_set(tvarray + 1, end_u, start_v);
	vec2_set(tvarray + 2, start_u, end_v);
	vec2_set(tvarray + 3, end_u, end_v);
}

static inline void build_sprite_norm(struct gs_vb_data *data, float fcx, float fcy)
{
	build_sprite(data, fcx, fcy, 0.0f, 1.0f, 0.0f, 1.0f);
}

static enum gs_color_space shader_filter_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);
	struct shader_filter_data *filter = data;
	obs_source_t *target = obs_filter_get_target(filter->context);
	const enum gs_color_space potential_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};
	return obs_source_get_color_space(target, OBS_COUNTOF(potential_spaces), potential_spaces);
}

static void render_shader(struct shader_filter_data *filter, float f, obs_source_t *filter_to)
{
	gs_texture_t *texture = gs_texrender_get_texture(filter->input_texrender);
	if (!texture) {
		return;
	}

	if (filter->param_previous_output) {
		gs_texrender_t *temp = filter->output_texrender;
		filter->output_texrender = filter->previous_output_texrender;
		filter->previous_output_texrender = temp;
	}

	enum gs_color_space space = shader_filter_get_color_space(filter, 0, NULL);
	enum gs_color_format format = gs_get_format_from_space(space);
	filter->output_texrender = create_or_reset_texrender(filter->output_texrender, format);

	if (filter->param_image)
		gs_effect_set_texture(filter->param_image, texture);
	if (filter->param_previous_image)
		gs_effect_set_texture(filter->param_previous_image, gs_texrender_get_texture(filter->previous_input_texrender));
	if (filter->param_previous_output)
		gs_effect_set_texture(filter->param_previous_output, gs_texrender_get_texture(filter->previous_output_texrender));

	shader_filter_set_effect_params(filter);

	if (f > 0.0f) {
		if (filter_to) {
			struct shader_filter_data *filter2 = obs_obj_get_data(filter_to);
			for (size_t i = 0; i < filter->stored_param_list.num; i++) {
				struct effect_param_data *param = (filter->stored_param_list.array + i);
				if (!param->param)
					continue;

				for (size_t j = 0; j < filter->stored_param_list.num; j++) {
					struct effect_param_data *param2 = (filter2->stored_param_list.array + i);
					if (!param2->param)
						continue;
					if (param->type != param2->type)
						continue;
					if (strcmp(param->name.array, param2->name.array) != 0)
						continue;

					switch (param->type) {
					case GS_SHADER_PARAM_FLOAT:
						gs_effect_set_float(param->param, (float)param2->value.f * f +
											  (float)param->value.f * (1.0f - f));
						break;
					case GS_SHADER_PARAM_INT:
						gs_effect_set_int(param->param, (int)((double)param2->value.i * f +
										      (double)param->value.i * (1.0f - f)));
						break;
					case GS_SHADER_PARAM_VEC2: {
						struct vec2 v2;
						v2.x = (float)param2->value.vec2.x * f + (float)param->value.vec2.x * (1.0f - f);
						v2.y = (float)param2->value.vec2.y * f + (float)param->value.vec2.y * (1.0f - f);
						gs_effect_set_vec2(param->param, &v2);
						break;
					}
					case GS_SHADER_PARAM_VEC3: {
						struct vec3 v3;
						v3.x = (float)param2->value.vec3.x * f + (float)param->value.vec3.x * (1.0f - f);
						v3.y = (float)param2->value.vec3.y * f + (float)param->value.vec3.y * (1.0f - f);
						v3.z = (float)param2->value.vec3.z * f + (float)param->value.vec3.z * (1.0f - f);
						gs_effect_set_vec3(param->param, &v3);
						break;
					}
					case GS_SHADER_PARAM_VEC4: {
						struct vec4 v4;
						v4.x = (float)param2->value.vec4.x * f + (float)param->value.vec4.x * (1.0f - f);
						v4.y = (float)param2->value.vec4.y * f + (float)param->value.vec4.y * (1.0f - f);
						v4.z = (float)param2->value.vec4.z * f + (float)param->value.vec4.z * (1.0f - f);
						v4.w = (float)param2->value.vec4.w * f + (float)param->value.vec4.w * (1.0f - f);
						gs_effect_set_vec4(param->param, &v4);
						break;
					}
					default:;
					}
					break;
				}
			}
		} else {
			for (size_t i = 0; i < filter->stored_param_list.num; i++) {
				struct effect_param_data *param = (filter->stored_param_list.array + i);
				if (!param->param || !param->has_default)
					continue;

				switch (param->type) {
				case GS_SHADER_PARAM_FLOAT:
					gs_effect_set_float(param->param,
							    (float)param->default_value.f * f + (float)param->value.f * (1.0f - f));
					break;
				case GS_SHADER_PARAM_INT:
					gs_effect_set_int(param->param, (int)((double)param->default_value.i * f +
									      (double)param->value.i * (1.0f - f)));
					break;
				case GS_SHADER_PARAM_VEC2: {
					struct vec2 v2;
					v2.x = param->default_value.vec2.x * f + param->value.vec2.x * (1.0f - f);
					v2.y = param->default_value.vec2.y * f + param->value.vec2.y * (1.0f - f);
					gs_effect_set_vec2(param->param, &v2);
					break;
				}
				case GS_SHADER_PARAM_VEC3: {
					struct vec3 v3;
					v3.x = param->default_value.vec3.x * f + param->value.vec3.x * (1.0f - f);
					v3.y = param->default_value.vec3.y * f + param->value.vec3.y * (1.0f - f);
					v3.z = param->default_value.vec3.z * f + param->value.vec3.z * (1.0f - f);
					gs_effect_set_vec3(param->param, &v3);
					break;
				}
				case GS_SHADER_PARAM_VEC4: {
					struct vec4 v4;
					v4.x = param->default_value.vec4.x * f + param->value.vec4.x * (1.0f - f);
					v4.y = param->default_value.vec4.y * f + param->value.vec4.y * (1.0f - f);
					v4.z = param->default_value.vec4.z * f + param->value.vec4.z * (1.0f - f);
					v4.w = param->default_value.vec4.w * f + param->value.vec4.w * (1.0f - f);
					gs_effect_set_vec4(param->param, &v4);
					break;
				}
				default:;
				}
			}
		}
	}

	gs_blend_state_push();
	gs_reset_blend_state();
	gs_enable_blending(false);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(filter->output_texrender, filter->total_width, filter->total_height)) {
		gs_ortho(0.0f, (float)filter->total_width, 0.0f, (float)filter->total_height, -100.0f, 100.0f);
		while (gs_effect_loop(filter->effect, "Draw")) {
			if (filter->use_template) {
				gs_draw_sprite(texture, 0, filter->total_width, filter->total_height);
			} else {
				if (!filter->sprite_buffer)
					load_sprite_buffer(filter);

				struct gs_vb_data *data = gs_vertexbuffer_get_data(filter->sprite_buffer);
				build_sprite_norm(data, (float)filter->total_width, (float)filter->total_height);
				gs_vertexbuffer_flush(filter->sprite_buffer);
				gs_load_vertexbuffer(filter->sprite_buffer);
				gs_load_indexbuffer(NULL);
				gs_draw(GS_TRISTRIP, 0, 0);
			}
		}
		gs_texrender_end(filter->output_texrender);
	}

	gs_blend_state_pop();
}

static void shader_filter_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	struct shader_filter_data *filter = data;

	float f = 0.0f;
	obs_source_t *filter_to = NULL;
	if (move_get_transition_filter)
		f = move_get_transition_filter(filter->context, &filter_to);

	if (f == 0.0f && filter->output_rendered) {
		draw_output(filter);
		return;
	}

	if (filter->effect == NULL || filter->rendering) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	get_input_source(filter);

	filter->rendering = true;
	render_shader(filter, f, filter_to);
	draw_output(filter);
	if (f == 0.0f)
		filter->output_rendered = true;
	filter->rendering = false;
}

static uint32_t shader_filter_getwidth(void *data)
{
	struct shader_filter_data *filter = data;

	return filter->total_width;
}

static uint32_t shader_filter_getheight(void *data)
{
	struct shader_filter_data *filter = data;

	return filter->total_height;
}

static void shader_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "shader_text", effect_template_default_image_shader);
}

void shader_filter_param_source_action(void *data, void (*action)(obs_source_t *source))
{
	struct shader_filter_data *filter = data;
	size_t param_count = filter->stored_param_list.num;
	for (size_t param_index = 0; param_index < param_count; param_index++) {
		struct effect_param_data *param = (filter->stored_param_list.array + param_index);
		if (!param->source)
			continue;
		obs_source_t *source = obs_weak_source_get_source(param->source);
		if (!source)
			continue;
		action(source);
		obs_source_release(source);
	}
}

void shader_filter_activate(void *data)
{
	shader_filter_param_source_action(data, obs_source_inc_active);
}

void shader_filter_deactivate(void *data)
{
	shader_filter_param_source_action(data, obs_source_dec_active);
}

void shader_filter_show(void *data)
{
	shader_filter_param_source_action(data, obs_source_inc_showing);
}

void shader_filter_hide(void *data)
{
	shader_filter_param_source_action(data, obs_source_dec_showing);
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct shader_filter_data *filter = src;
	const char *setting_name = data;
	obs_data_t *settings = obs_source_get_settings(filter->context);
	obs_data_set_string(settings, setting_name, new_path);
	obs_source_update(filter->context, settings);
	obs_data_release(settings);
}

obs_missing_files_t *shader_filter_missing_files(void *data)
{
	struct shader_filter_data *filter = data;
	obs_missing_files_t *files = obs_missing_files_create();
	obs_data_t *settings = obs_source_get_settings(filter->context);
	if (obs_data_get_bool(settings, "from_file")) {
		const char *file_name = obs_data_get_string(settings, "shader_file_name");
		if (file_name && strlen(file_name) > 0 && !os_file_exists(file_name)) {
			obs_missing_file_t *file = obs_missing_file_create(
				file_name, missing_file_callback, OBS_MISSING_FILE_SOURCE, filter->context, "shader_file_name");
			obs_missing_files_add_file(files, file);
		}
	}
	size_t param_count = filter->stored_param_list.num;
	for (size_t param_index = 0; param_index < param_count; param_index++) {
		struct effect_param_data *param = (filter->stored_param_list.array + param_index);
		if (param->type != GS_SHADER_PARAM_TEXTURE)
			continue;
		const char *widget_type = param->widget_type.array;
		if (widget_type && strcmp(widget_type, "source") == 0)
			continue;
		const char *param_name = param->name.array;
		const char *path = obs_data_get_string(settings, param_name);
		if (path && strlen(path) > 0 && !os_file_exists(path)) {
			obs_missing_file_t *file = obs_missing_file_create(path, missing_file_callback, OBS_MISSING_FILE_SOURCE,
									   filter->context, param->name.array);

			obs_missing_files_add_file(files, file);
		}
	}
	obs_data_release(settings);
	return files;
}

struct obs_source_info shader_filter = {
	.id = "shader_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB | OBS_SOURCE_CUSTOM_DRAW,
	.create = shader_filter_create,
	.destroy = shader_filter_destroy,
	.update = shader_filter_update,
	.load = shader_filter_update,
	.video_tick = shader_filter_tick,
	.get_name = shader_filter_get_name,
	.get_defaults = shader_filter_defaults,
	.get_width = shader_filter_getwidth,
	.get_height = shader_filter_getheight,
	.video_render = shader_filter_render,
	.get_properties = shader_filter_properties,
	.video_get_color_space = shader_filter_get_color_space,
	.activate = shader_filter_activate,
	.deactivate = shader_filter_deactivate,
	.show = shader_filter_show,
	.hide = shader_filter_hide,
	.missing_files = shader_filter_missing_files,
};

static void *shader_transition_create(obs_data_t *settings, obs_source_t *source)
{
	struct shader_filter_data *filter = bzalloc(sizeof(struct shader_filter_data));
	filter->context = source;
	filter->reload_effect = true;
	filter->transition = true;

	dstr_init(&filter->last_path);
	dstr_copy(&filter->last_path, obs_data_get_string(settings, "shader_file_name"));
	filter->last_from_file = obs_data_get_bool(settings, "from_file");
	filter->rand_instance_f = (float)((double)rand_interval(0, 10000) / (double)10000);
	filter->rand_activation_f = (float)((double)rand_interval(0, 10000) / (double)10000);

	da_init(filter->stored_param_list);

	obs_source_update(source, settings);

	return filter;
}

static const char *shader_transition_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ShaderFilter");
}

static float mix_a(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return 1.0f - t;
}

static float mix_b(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return t;
}

static bool shader_transition_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio, uint32_t mixers,
					   size_t channels, size_t sample_rate)
{
	struct shader_filter_data *filter = data;
	return obs_transition_audio_render(filter->context, ts_out, audio, mixers, channels, sample_rate, mix_a, mix_b);
}

static void shader_transition_video_callback(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy)
{
	if (!a && !b)
		return;

	struct shader_filter_data *filter = data;
	if (filter->effect == NULL || filter->rendering)
		return;

	if (!filter->prev_transitioning) {
		if (obs_source_active(filter->context))
			shader_filter_param_source_action(data, obs_source_inc_active);
		if (obs_source_showing(filter->context))
			shader_filter_param_source_action(data, obs_source_inc_showing);
	}
	filter->transitioning = true;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	if (gs_get_color_space() == GS_CS_SRGB) {
		if (filter->param_image_a != NULL)
			gs_effect_set_texture(filter->param_image_a, a);
		if (filter->param_image_b != NULL)
			gs_effect_set_texture(filter->param_image_b, b);
		if (filter->param_image != NULL)
			gs_effect_set_texture(filter->param_image, t < 0.5 ? a : b);
		if (filter->param_convert_linear)
			gs_effect_set_bool(filter->param_convert_linear, true);
	} else {
		if (filter->param_image_a != NULL)
			gs_effect_set_texture_srgb(filter->param_image_a, a);
		if (filter->param_image_b != NULL)
			gs_effect_set_texture_srgb(filter->param_image_b, b);
		if (filter->param_image != NULL)
			gs_effect_set_texture_srgb(filter->param_image, t < 0.5 ? a : b);
		if (filter->param_convert_linear)
			gs_effect_set_bool(filter->param_convert_linear, false);
	}
	if (filter->param_transition_time != NULL)
		gs_effect_set_float(filter->param_transition_time, t);

	shader_filter_set_effect_params(filter);

	while (gs_effect_loop(filter->effect, "Draw"))
		gs_draw_sprite(NULL, 0, cx, cy);

	gs_enable_framebuffer_srgb(previous);
}

static void shader_transition_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	const bool previous = gs_set_linear_srgb(true);

	struct shader_filter_data *filter = data;
	filter->transitioning = false;
	obs_transition_video_render2(filter->context, shader_transition_video_callback, NULL);
	if (!filter->transitioning && filter->prev_transitioning) {
		if (obs_source_active(filter->context))
			shader_filter_param_source_action(data, obs_source_dec_active);
		if (obs_source_showing(filter->context))
			shader_filter_param_source_action(data, obs_source_dec_showing);
	}
	filter->prev_transitioning = filter->transitioning;
	gs_set_linear_srgb(previous);
}

static void shader_transition_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "shader_text", effect_template_default_transition_image_shader);
}

static enum gs_color_space shader_transition_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces)
{
	struct shader_filter_data *filter = data;
	const enum gs_color_space transition_space = obs_transition_video_get_color_space(filter->context);

	enum gs_color_space space = transition_space;
	for (size_t i = 0; i < count; ++i) {
		space = preferred_spaces[i];
		if (space == transition_space)
			break;
	}

	return space;
}

struct obs_source_info shader_transition = {
	.id = "shader_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.create = shader_transition_create,
	.destroy = shader_filter_destroy,
	.update = shader_filter_update,
	.load = shader_filter_update,
	.video_tick = shader_filter_tick,
	.get_name = shader_transition_get_name,
	.audio_render = shader_transition_audio_render,
	.get_defaults = shader_transition_defaults,
	.video_render = shader_transition_video_render,
	.get_properties = shader_filter_properties,
	.video_get_color_space = shader_transition_get_color_space,
	.missing_files = shader_filter_missing_files,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-shaderfilter", "en-US")

bool obs_module_load(void)
{
	blog(LOG_INFO, "[obs-shaderfilter] loaded version %s", PROJECT_VERSION);

#ifdef _WIN32
	void *dl = os_dlopen("obs");
#else
	void *dl = dlopen(nullptr, RTLD_LAZY);
#endif
	if (dl) {
		if (obs_get_version() >= MAKE_SEMANTIC_VERSION(33, 0, 0)) {
			gs_image_file_create_func = gs_image_file_ex_create;
			gs_image_file_free_func = os_dlsym(dl, "gs_image_file_ex_free");
			gs_image_file_init_func = os_dlsym(dl, "gs_image_file_ex_init");
			gs_image_file_init_texture_func = os_dlsym(dl, "gs_image_file_ex_init_texture");
			gs_image_file_tick_func = os_dlsym(dl, "gs_image_file_ex_tick");
			gs_image_file_update_texture_func = os_dlsym(dl, "gs_image_file_ex_update_texture");
		} else {
			gs_image_file_create_func = gs_image_file4_create;
			/* The v4 free/init_texture helpers are inline, not exported.
			 * Their base image is the first member, so use the base API. */
			gs_image_file_free_func = os_dlsym(dl, "gs_image_file_free");
			gs_image_file_init_func = os_dlsym(dl, "gs_image_file4_init");
			gs_image_file_init_texture_func = os_dlsym(dl, "gs_image_file_init_texture");
			gs_image_file_tick_func = os_dlsym(dl, "gs_image_file4_tick");
			gs_image_file_update_texture_func = os_dlsym(dl, "gs_image_file4_update_texture");
		}
		os_dlclose(dl);
	}
	if (!gs_image_file_create_func || !gs_image_file_free_func || !gs_image_file_init_func ||
	    !gs_image_file_init_texture_func || !gs_image_file_tick_func || !gs_image_file_update_texture_func) {
		blog(LOG_ERROR, "[obs-shaderfilter] Required OBS image API is unavailable");
		return false;
	}
	if (!sf_gamepad_init())
		blog(LOG_WARNING, "[obs-shaderfilter] Continuing with neutral gamepad input");
	obs_register_source(&shader_filter);
	obs_register_source(&shader_transition);
	return true;
}

void obs_module_unload(void)
{
	sf_tools_shutdown();
	sf_gamepad_shutdown();
	if (gamepad_texture) {
		obs_enter_graphics();
		gs_texture_destroy(gamepad_texture);
		gamepad_texture = NULL;
		gamepad_texture_frame = UINT64_MAX;
		obs_leave_graphics();
	}
}

void obs_module_post_load()
{
	sf_tools_init();
	if (obs_get_module("move-transition") == NULL)
		return;
	proc_handler_t *ph = obs_get_proc_handler();
	struct calldata cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "filter_id", shader_filter.id);
	if (proc_handler_call(ph, "move_get_transition_filter_function", &cd)) {
		move_get_transition_filter = calldata_ptr(&cd, "callback");
	}
	calldata_free(&cd);
}
