#include <graphics/graphics.h>
#include <util/bmem.h>
#include "shader-gamepad.h"
#include "shader-helpers.generated.h"
#include "shader-spectrum-effect.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

static void check(bool condition, const char *message)
{
	if (!condition) {
		std::fprintf(stderr, "%s\n", message);
		std::exit(1);
	}
}

// Decode the original C preamble so the test compiles exactly what users get.
static std::string preamble(const char *path)
{
	std::ifstream file(path);
	std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	const auto begin = source.find("static const char *effect_template_begin = \"");
	check(begin != std::string::npos, "Cannot find standard preamble");
	std::string result;
	for (size_t i = source.find('"', begin) + 1; i < source.size() && source[i] != '"'; ++i) {
		if (source[i] != '\\') {
			result += source[i];
			continue;
		}
		++i;
		if (source[i] == '\r')
			++i;
		if (source[i] != '\n')
			result += source[i] == 'n' ? '\n' : source[i];
	}
	return (gs_get_device_type() == GS_DEVICE_OPENGL ? "#define OPENGL 1\n" : "") + result + effect_template_helpers +
	       effect_template_gamepad;
}

static gs_effect_t *compile(const std::string &prefix, const char *body, bool expect_success = true)
{
	const std::string source =
		prefix +
		"\nuniform float4 test_input;\nuniform int test_octaves;\n"
		"float4 sf_test_value(VertData v_in) {\n" +
		body +
		"\n}\n"
		"float4 mainImage(VertData v_in) : TARGET { return sf_test_value(v_in) + (v_in.uv.x - 0.5) * 0.0001; }\n"
		"technique Draw { pass { vertex_shader = mainTransform(v_in); pixel_shader = mainImage(v_in); } }\n";
	char *errors = nullptr;
	dstr effect_text{};
	dstr_copy(&effect_text, source.c_str());
	sf_shader_append_spectrum(&effect_text);
	auto *effect = gs_effect_create(effect_text.array, "shaderfilter-test", &errors);
	dstr_free(&effect_text);
	if (!effect && expect_success)
		std::fprintf(stderr, "%s\n", errors ? errors : "Shader creation failed");
	bfree(errors);
	check((effect != nullptr) == expect_success, "Unexpected shader compilation result");
	return effect;
}

static std::array<float, 4> render(gs_effect_t *effect, const std::array<float, 4> &input)
{
	const float scale[2] = {1, 1}, offset[2] = {0, 0};
	gs_effect_set_val(gs_effect_get_param_by_name(effect, "uv_scale"), scale, sizeof(scale));
	gs_effect_set_val(gs_effect_get_param_by_name(effect, "uv_offset"), offset, sizeof(offset));
	gs_effect_set_val(gs_effect_get_param_by_name(effect, "test_input"), input.data(), sizeof(input));
	gs_effect_set_int(gs_effect_get_param_by_name(effect, "test_octaves"), 4);
	// Unique values across vector and controller boundaries catch packing bugs.
	sf_gamepad_snapshot snapshot{};
	for (int c = 0; c < SF_GAMEPAD_COUNT; ++c) {
		snapshot.info[c][0] = 1;
		snapshot.info[c][1] = c % SF_GAMEPAD_BUTTON_COUNT;
		for (int b = 0; b < SF_GAMEPAD_BUTTON_COUNT; ++b) {
			snapshot.buttons[c * 4 + b / 4][b % 4] = b % 2;
			snapshot.presses[c * 4 + b / 4][b % 4] = 1000 * c + b;
		}
		for (int a = 0; a < SF_GAMEPAD_AXIS_COUNT; ++a) {
			snapshot.axes[c * 2 + a / 4][a % 4] = a * 0.1f;
			snapshot.axes_c[c * 2 + a / 4][a % 4] = 0.25f;
			snapshot.axes_clamped[c * 2 + a / 4][a % 4] = (c * 6 + a) / 95.0f;
		}
	}
	snapshot.presses[63][1] = INT32_MAX;
	gs_effect_set_int(gs_effect_get_param_by_name(effect, "gamepad_available"), 1);
#define UPLOAD(name, member) \
	if (auto *param = gs_effect_get_param_by_name(effect, name)) \
		gs_effect_set_val(param, snapshot.member, sizeof(snapshot.member))
	UPLOAD("gamepad_info", info);
	UPLOAD("gamepad_buttons", buttons);
	UPLOAD("gamepad_presses", presses);
	UPLOAD("gamepad_axes", axes);
	UPLOAD("gamepad_axes_c", axes_c);
	UPLOAD("gamepad_axes_clamped", axes_clamped);
#undef UPLOAD
	gs_texture_t *gamepads = nullptr;
	gs_texture_t *spectrum_texture = nullptr;
	if (auto *param = gs_effect_get_param_by_name(effect, "spec")) {
		float levels[64 * 4];
		for (int row = 0; row < 4; ++row)
			for (int i = 0; i < 64; ++i) {
				const float instant = float(i) / 63.0f;
				levels[row * 64 + i] = instant + (1.0f - 2.0f * instant) * float(row) / 3.0f;
			}
		const uint8_t *data = reinterpret_cast<const uint8_t *>(levels);
		spectrum_texture = gs_texture_create(64, 4, GS_R32F, 1, &data, 0);
		gs_effect_set_texture(param, spectrum_texture);
	}
	if (auto *param = gs_effect_get_param_by_name(effect, "gamepad_data")) {
		float pixels[SF_GAMEPAD_COUNT * SF_GAMEPAD_TEXTURE_WIDTH * 4];
		sf_gamepad_pack_snapshot(&snapshot, pixels);
		const uint8_t *data = reinterpret_cast<const uint8_t *>(pixels);
		gamepads = gs_texture_create(SF_GAMEPAD_TEXTURE_WIDTH, SF_GAMEPAD_COUNT, GS_RGBA32F, 1, &data, 0);
		gs_effect_set_texture(param, gamepads);
	}
	auto *target = gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
	check(gs_texrender_begin(target, 1, 1), "Cannot begin float render");
	gs_ortho(0, 1, 0, 1, -100, 100);
	gs_enable_blending(false);
	gs_enable_depth_test(false);
	gs_set_cull_mode(GS_NEITHER);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(nullptr, 0, 1, 1);
	gs_texrender_end(target);
	auto *stage = gs_stagesurface_create(1, 1, GS_RGBA32F);
	gs_stage_texture(stage, gs_texrender_get_texture(target));
	uint8_t *pixels = nullptr;
	uint32_t stride = 0;
	check(gs_stagesurface_map(stage, &pixels, &stride), "Cannot map float render");
	std::array<float, 4> output;
	std::copy_n(reinterpret_cast<float *>(pixels), 4, output.begin());
	gs_stagesurface_unmap(stage);
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(target);
	gs_texture_destroy(gamepads);
	gs_texture_destroy(spectrum_texture);
	for (float value : output)
		check(std::isfinite(value), "Non-finite shader output");
	return output;
}

static void test_oklch(const std::string &prefix)
{
	// Reference sRGB primaries in OKLCH (D65); hue is degrees / 360.
	const std::array<float, 4> rgb_samples[] = {
		{0, 0, 0, 0}, {1, 1, 1, 0}, {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0.5f, 0.5f, 0.5f, 0},
	};
	const std::array<float, 3> references[] = {
		{0, 0, 0},
		{1, 0, 0},
		{0.62795536f, 0.25768331f, 29.233885f / 360.0f},
		{0.86643961f, 0.29482724f, 142.495339f / 360.0f},
		{0.45201372f, 0.31321437f, 264.052021f / 360.0f},
		{0.59818073f, 0, 0},
	};
	auto *forward = compile(prefix, "return float4(rgb2oklch(test_input.xyz), 1.0);");
	for (size_t i = 0; i < std::size(rgb_samples); ++i) {
		const auto result = render(forward, rgb_samples[i]);
		for (int channel = 0; channel < 3; ++channel)
			check(std::abs(result[channel] - references[i][channel]) < 0.00003f, "OKLCH reference color mismatch");
	}
	gs_effect_destroy(forward);
	auto *round_trip = compile(prefix, "float3 rgb = test_input.xyz;"
					   "return float4(length(oklch2rgb(rgb2oklch(rgb)) - rgb),"
					   "length(oklch2linrgb(linrgb2oklch(rgb)) - rgb),"
					   "length(hsv2rgb(oklch2hsv(hsv2oklch(rgb2hsv(rgb)))) - rgb),"
					   "length(hsl2rgb(oklch2hsl(hsl2oklch(rgb2hsl(rgb)))) - rgb));");
	for (const auto &rgb : rgb_samples) {
		const auto errors = render(round_trip, rgb);
		// HLSL atan2/trig approximation and near-zero RGB cancellation get
		// amplified by sRGB's 12.92 slope (~9e-5 on D3D11's green primary).
		// This bound is under 0.04 of one 8-bit channel step.
		for (float error : errors) {
			if (error >= 0.00015f)
				std::fprintf(stderr, "RGB (%g,%g,%g), round-trip errors: RGB=%g linear=%g HSV=%g HSL=%g\n", rgb[0],
					     rgb[1], rgb[2], errors[0], errors[1], errors[2], errors[3]);
			check(error < 0.00015f, "OKLCH color-space round trip failed");
		}
	}
	for (const auto &rgb : {std::array<float, 4>{0.01f, 0.9f, 0.4f, 0}, {0.02f, 0.04045f, 0.1f, 0}}) {
		for (float error : render(round_trip, rgb))
			check(error < 0.00015f, "OKLCH transfer-curve round trip failed");
	}
	gs_effect_destroy(round_trip);
	auto *linear = compile(prefix, "return float4(linrgb2oklch(test_input.xyz), 1.0);");
	const auto linear_gray = render(linear, {0.125f, 0.125f, 0.125f, 0});
	check(std::abs(linear_gray[0] - 0.5f) < 0.00001f && linear_gray[1] == 0 && linear_gray[2] == 0,
	      "Linear RGB conversion applied an unwanted transfer curve");
	gs_effect_destroy(linear);
	auto *extended = compile(prefix, "return float4(oklch2rgb(rgb2oklch(test_input.xyz)), 1.0);");
	for (const auto &rgb : {std::array<float, 4>{-0.1f, 0.2f, 1.1f, 0}, {-0.2f, -0.3f, -0.4f, 0}}) {
		const auto result = render(extended, rgb);
		for (int channel = 0; channel < 3; ++channel)
			check(std::abs(result[channel] - rgb[channel]) < 0.00004f, "OKLCH clipped extended RGB");
	}
	gs_effect_destroy(extended);
	auto *gamut = compile(prefix, "float3 lch = test_input.xyz; float3 rgb = oklch2rgb(lch);"
				      "return float4(length(rgb - oklch2rgb(lch + float3(0,0,1))),"
				      "length(hsv2rgb(oklch2hsv(lch)) - saturate(rgb)),"
				      "length(hsl2rgb(oklch2hsl(lch)) - saturate(rgb)), min(rgb.r, min(rgb.g, rgb.b)));");
	const auto gamut_result = render(gamut, {0.7f, 0.45f, -0.8f, 0});
	check(gamut_result[0] < 0.00004f && gamut_result[1] < 0.00004f && gamut_result[2] < 0.00004f && gamut_result[3] < 0,
	      "OKLCH hue wrapping or gamut policy failed");
	gs_effect_destroy(gamut);
	auto *neutral = compile(prefix, "return float4(oklch2linrgb(test_input.xyz), 1.0);");
	const auto negative_chroma = render(neutral, {0.5f, -0.2f, 0.9f, 0});
	for (int channel = 0; channel < 3; ++channel)
		check(std::abs(negative_chroma[channel] - 0.125f) < 0.00001f, "Negative OKLCH chroma did not become neutral");
	gs_effect_destroy(neutral);
}

int main(int argc, char **argv)
{
	check(argc == 3, "Usage: shaderfilter-helpers-test <graphics backend DLL> <obs-shaderfilter.c>");
	graphics_t *graphics = nullptr;
	check(gs_create(&graphics, argv[1], 0) == GS_SUCCESS, "Cannot create graphics device");
	gs_enter_context(graphics);
	const auto prefix = preamble(argv[2]);
	auto *color = compile(prefix, "return float4(hsl2rgb(rgb2hsl(test_input.xyz)), 1.0);");
	for (const auto &rgb : {std::array<float, 4>{0, 0, 0, 0},
				{1, 1, 1, 0},
				{1, 0, 0, 0},
				{0.3f, 0.3f, 0.3f, 0},
				{0.01f, 0.9f, 0.4f, 0},
				{0.999f, 0.99f, 0.991f, 0}}) {
		const auto result = render(color, rgb);
		for (int channel = 0; channel < 3; ++channel)
			check(std::abs(result[channel] - rgb[channel]) < 0.00003f, "HSL round trip failed");
	}
	gs_effect_destroy(color);
	auto *hsv = compile(prefix, "return float4(hsl2hsv(hsv2hsl(test_input.xyz)), 1.0);");
	for (const auto &input : {std::array<float, 4>{0.2f, 0.8f, 0.7f, 0}, {0, 0, 0, 0}, {0.9f, 0, 1, 0}}) {
		const auto result = render(hsv, input);
		for (int channel = 0; channel < 3; ++channel)
			check(std::abs(result[channel] - input[channel]) < 0.00003f, "HSV/HSL round trip failed");
	}
	gs_effect_destroy(hsv);
	test_oklch(prefix);
	auto *noise = compile(prefix, "return float4(simplex_noise(test_input), perlin_noise(test_input),"
				      "voronoi_noise(test_input), perlin_noise_fbm(test_input, test_octaves, 1.0, 2.0, 0.5));");
	std::array<float, 4> point{0.37f, -0.83f, 1.21f, 2.49f};
	const auto baseline = render(noise, point);
	for (int dimension = 0; dimension < 4; ++dimension) {
		auto next = point;
		next[dimension] += 0.17f;
		const auto result = render(noise, next);
		for (int channel = 0; channel < 4; ++channel)
			check(std::abs(result[channel] - baseline[channel]) > 1.0e-6f, "Noise ignores an input dimension");
		next = point;
		next[dimension] += 0.0001f;
		const auto near = render(noise, next);
		for (int channel = 0; channel < 4; ++channel)
			check(std::abs(near[channel] - baseline[channel]) < 0.01f, "Noise is discontinuous");
	}
	const auto lattice = render(noise, {0, 0, 0, 0});
	check(std::abs(lattice[0]) < 1.0e-6f && std::abs(lattice[1]) < 1.0e-6f, "Gradient noise lattice mismatch");
	gs_effect_destroy(noise);
	auto *derivative = compile(prefix,
				   "float4 p = test_input; float h = 0.001; float4 d = sf_simplex_derivative(p);"
				   "float dx = (simplex_noise(p + float4(h,0,0,0)) - simplex_noise(p - float4(h,0,0,0))) / (2.0*h);"
				   "float dy = (simplex_noise(p + float4(0,h,0,0)) - simplex_noise(p - float4(0,h,0,0))) / (2.0*h);"
				   "float dz = (simplex_noise(p + float4(0,0,h,0)) - simplex_noise(p - float4(0,0,h,0))) / (2.0*h);"
				   "return float4(abs(d.xyz - float3(dx,dy,dz)), 1.0);");
	const auto errors = render(derivative, point);
	check(errors[0] < 0.003f && errors[1] < 0.003f && errors[2] < 0.003f, "Analytic derivative mismatch");
	gs_effect_destroy(derivative);
	auto *curl = compile(prefix, "return float4(curl_noise(test_input), 1.0);");
	float divergence = 0;
	for (int axis = 0; axis < 3; ++axis) {
		auto before = point, after = point;
		before[axis] -= 0.001f;
		after[axis] += 0.001f;
		divergence += (render(curl, after)[axis] - render(curl, before)[axis]) / 0.002f;
	}
	check(std::abs(divergence) < 0.03f, "Curl has nonzero numerical divergence");
	gs_effect_destroy(curl);
	auto *pad = compile(
		prefix, "int c = int(test_input.x); int b = int(test_input.y); int a = int(test_input.z);"
			"return float4(gamepad_button(c,b), gamepad_press_count(c,b), gamepad_axis(c,a), gamepad_last_button(c));");
	const auto packed = render(pad, {15, 14, 5, 0});
	check(packed[0] == 0 && packed[1] == 15014 && std::abs(packed[2] - 0.5f) < 1.0e-6f && packed[3] == 0,
	      "Controller uniform packing failed");
	const auto invalid = render(pad, {-1, -1, -1, 0});
	check(invalid[0] == 0 && invalid[1] == 0 && invalid[2] == 0 && invalid[3] == -1, "Invalid controller index failed");
	gs_effect_destroy(pad);
	auto *count_bits = compile(prefix, "return float4(gamepad_press_count(15,13) % 97, gamepad_press_count(15,13) / 65536,"
					   "gamepad_connected(15) ? 1.0 : 0.0, gamepad_available);");
	const auto bits = render(count_bits, point);
	check(bits[0] == INT32_MAX % 97 && bits[1] == 32767 && bits[2] == 1 && bits[3] == 1,
	      "Controller count lost integer precision");
	gs_effect_destroy(count_bits);
	auto *interval = compile(prefix, "return float4(gamepad_axis_deadzone(0,5,0.2), gamepad_axis_cumulative(0,5),"
					 "gamepad_axis_interval(0,5,120.0), gamepad_axis_acc(0,5,0.0));");
	const auto phases = render(interval, point);
	check(std::abs(phases[0] - 0.375f) < 1.0e-6f && phases[1] == 0.25f && std::abs(phases[2] - 0.5f) < 0.0001f &&
		      std::abs(phases[3] - 0.25f) < 1.0e-6f,
	      "Controller interval/deadzone mismatch");
	gs_effect_destroy(interval);
	auto *clamped = compile(prefix, "return float4(gamepad_axis_clamped(0,0), gamepad_axis_clamped(15,5),"
					"gamepad_axis_clamped(7,4), gamepad_axis_clamped(0,6));");
	const auto positions = render(clamped, point);
	check(positions[0] == 0 && positions[1] == 1 && std::abs(positions[2] - 46.0f / 95.0f) < 1e-6f && positions[3] == 0,
	      "Clamped controller packing/endpoints/invalid axis mismatch");
	gs_effect_destroy(clamped);
	auto *energy = compile(prefix, "return float4(energy + e_treb, e_bass, e_lows, e_mids);");
	const char *energy_names[] = {"energy", "e_bass", "e_lows", "e_mids", "e_treb"};
	for (int i = 0; i < 5; ++i)
		gs_effect_set_float(gs_effect_get_param_by_name(energy, energy_names[i]), (i + 1) * 0.1f);
	const auto levels = render(energy, point);
	check(std::abs(levels[0] - 0.6f) < 1e-6f && std::abs(levels[1] - 0.2f) < 1e-6f && std::abs(levels[2] - 0.3f) < 1e-6f &&
		      std::abs(levels[3] - 0.4f) < 1e-6f,
	      "Energy preamble/uniform upload mismatch");
	gs_effect_destroy(energy);
	auto *tails = compile(prefix, "return linfoff(0.0) + expfoff(1.0) + linfoff(float4(0.0, 0.25, 0.5, 1.0)) + "
				      "expfoff(float4(0.0, 0.25, 0.5, 1.0));");
	gs_effect_destroy(tails);
	check(sf_shader_declares_spec("uniform /* whitespace */ texture2d\n spec;"), "Spec declaration was missed");
	check(sf_shader_declares_spec("#define TEX texture2d\nuniform TEX spec;"), "Macro spec declaration was missed");
	check(sf_shader_declares_spec("#define ENABLE_SPEC\n#ifdef ENABLE_SPEC\nuniform texture2d spec;\n#endif\n"),
	      "Enabled spec declaration was missed");
	check(!sf_shader_declares_spec("#ifdef DISABLED_SPEC\nuniform texture2d spec;\n#endif\n"),
	      "Disabled spec declaration enabled FFT");
	for (const char *text :
	     {"// uniform texture2d spec;\n", "/* uniform texture2d spec; */", "#if 0\nuniform texture2d spec;\n#endif\n",
	      "uniform texture2d spectrum_texture;", "uniform float spec;", "uniform texture2d spec[2];", "texture2d spec;",
	      "uniform string label = \"uniform texture2d spec;\";"})
		if (sf_shader_declares_spec(text)) {
			std::fprintf(stderr, "Unexpected spectrum declaration in: %s\n", text);
			check(false, "Inactive/unrelated spec declaration enabled FFT");
		}
	compile(prefix, "return float4(spectrum(0.5), 0, 0, 1);", false);
	auto *spectrum = compile(prefix + "\nuniform texture2d spec;\n",
				 "return float4(spectrum(-0.5), spectrum(1.5), spectrum(0.5), spectrum(16.5 / 64.0));");
	const auto bins = render(spectrum, point);
	check(bins[0] == 0 && bins[1] == 1 && std::abs(bins[2] - 0.5f) < 1e-6f && std::abs(bins[3] - 16.0f / 63.0f) < 1e-6f,
	      "Spectrum texture clamping, center sampling or linear interpolation failed");
	gs_effect_destroy(spectrum);
	auto *falloff =
		compile(prefix + "\nuniform texture2d spec;\n",
			"return float4(spectrum(0.0), spectrum(0.0, 1.0 / 3.0), spectrum(0.0, 2.0 / 3.0), spectrum(0.0, 1.0));");
	const auto rows = render(falloff, point);
	check(rows[0] == 0 && std::abs(rows[1] - 1.0f / 3.0f) < 1e-6f && std::abs(rows[2] - 2.0f / 3.0f) < 1e-6f && rows[3] == 1,
	      "Spectrum overloads or row sampling failed");
	gs_effect_destroy(falloff);
	auto *macro_spectrum = compile(prefix + "\n#define SPEC_TYPE texture2d\n#define WITH_SPEC\n#ifdef WITH_SPEC\n"
						"uniform SPEC_TYPE spec < string label = \"Spectrum > bands\"; >;\n#endif\n",
				       "return float4(spectrum(0.5), 0, 0, 1);");
	check(std::abs(render(macro_spectrum, point)[0] - 0.5f) < 1e-6f,
	      "Macro/conditional/annotated spectrum shader did not compile and sample");
	gs_effect_destroy(macro_spectrum);
	auto *unused_spec = compile(prefix + "\nuniform texture2d spec;\n", "return test_input;");
	check(gs_effect_get_param_by_name(unused_spec, "spec") != nullptr, "Declared unused spec was lost to reflection");
	gs_effect_destroy(unused_spec);
	std::string example_path = argv[2];
	example_path.resize(example_path.find_last_of("/\\") + 1);
	example_path += "data/examples/spectrum-bars.shader";
	std::ifstream example_file(example_path);
	check(bool(example_file), "Cannot open spectrum example");
	const std::string example{std::istreambuf_iterator<char>(example_file), std::istreambuf_iterator<char>()};
	auto *demo = compile(prefix + "\n#define mainImage sf_example_main\n" + example + "\n#undef mainImage\n",
			     "return sf_example_main(v_in);");
	(void)render(demo, point);
	gs_effect_destroy(demo);
	gs_leave_context();
	gs_destroy(graphics);
	std::puts("Shader compilation and GPU numeric tests passed");
}
