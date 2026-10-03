# Shaderfilter variable and helper reference

This reference was checked against the implementation on 2026-09-25. It covers
the plugin's shader-visible inputs, provided functions, shader annotations, and
relevant application settings. Names are case-sensitive. It does not catalog
private C/C++ implementation locals.

Reviewed sources: [preamble, parameter binding, audio processing and rendering](obs-shaderfilter.c),
[color/noise helpers](data/internal/shader-helpers.effect),
[gamepad shader interface](data/internal/gamepad.effect),
[spectrum analysis](shader-spectrum.cpp), [spectrum helper](data/internal/spectrum.effect),
[controller state and normalization](shader-gamepad-state.hpp),
[native controller API](shader-gamepad.h), and [controller Tools menu](shader-tools.cpp).
Declaration overhead was also checked against OBS's
[effect parameter compilation](../../libobs/graphics/effect-parser.c).

## Complete definition table

This is the single consolidated lookup table for **all plugin-provided shader
uniforms and helper signatures**, including the original plugin's definitions.
Each uniform and each function overload has its own row. It also collects the
shader structures, preprocessing controls, annotations, settings, and public
native header APIs documented below. The **Scope** column distinguishes shader
inputs from application settings and C/C++ definitions.

Coverage checked against source: **92 recognized shader uniforms and 55 shader
function signatures**, including the supplied vertex transform and the pixel
entry-point contract. The original helpers such as `acc`, `ts`, `cu_*`,
`expfoff`, `linfoff`, `rgb2hsv`, `hsv2rgb`, and the sRGB transfer functions are
included alongside the newer functions.

**Preamble** means use it directly without redeclaring it; **Declare** means add
the displayed uniform declaration. **Gamepad preamble** is appended when shader
text contains `gamepad_`. Complete-effect overrides supply their own declarations.
The **Spectrum helper** is supplied only when the user declares `uniform texture2d spec;`,
including in complete-effect overrides. `spec` is never predeclared.
See [declaration and cost rules](#how-declarations-and-costs-work) for the CPU
costs that can remain even when a GPU shader does not read a declared parameter.
Standard HLSL/OBS intrinsics, example-specific user uniforms, and private C/C++
implementation locals are outside this plugin-definition table.

The **Recommended access** column distinguishes the intended shader interface
from exposed storage:

- **Direct input / public helper**: use the variable or function normally.
- **Raw counter / history carrier**: lower-level storage behind a more convenient
  helper. Audio counters use `acc(speed)` or `cu_bass(speed)`/`cu_lows(speed)`/
  `cu_mids(speed)`/`cu_treb(speed)`; history textures can use `ts(x)`.
- **Preset carrier**: a named falloff anchor used by `expfoff(tails)` or
  `linfoff(tails)`. Read the variable directly when that exact preset is wanted;
  use the function for an adjustable tail. Here `tails` is a `float4` in
  bass/lows/mids/treb order, with each component in `[0,1]`.
- **Backend carrier / support state**: packed vectors or encoded textures whose
  layout depends on the graphics backend. Use public `gamepad_*` accessors for
  index checks, count reconstruction, and consistent D3D/OpenGL behavior.
- **Pipeline input / definition**: used by the generated vertex/texture pipeline;
  direct access is appropriate when customizing that pipeline.
- **Implementation helper**: a lower-level function used by the public helpers.
  The row points to the usual higher-level entry point.

These labels describe abstraction level, not removal or deprecation. Raw reads
are still possible for custom math. Helpers do not eliminate the underlying
declaration/upload costs: `ts(x)` samples all four histories, while sampling one
history texture directly can cost fewer texture reads. Likewise, reading an exact
falloff preset avoids interpolation. Native upload carriers are marked separately
from the typed native controller snapshot API.

| Scope | Definition / full signature | Type / return type | Availability / declaration | Recommended access | Meaning | Declaration / use overhead |
| --- | --- | --- | --- | --- | --- | --- |
| Shader uniform | `uniform texture2d spec;` | `texture2d` | Declare explicitly | **History/data carrier — prefer helper:** `spectrum(pitch, falloff)` | 64 × 4 R32F spectrum: instant, short intermediate, long intermediate, long-window rows. A-weighted, equal-ratio bands from 20 Hz to 30000 Hz, normalized to [0,1]. | Declaration opts into shared background FFT; texture allocated on first render and uploaded on revision changes. No FFT or spectrum texture without declaration. |
| Shader helper | `float spectrum(float pitch)` | `float` | Spectrum helper; requires explicit `spec` declaration | **Public helper:** call directly. | Samples the instant row at `float2(saturate(pitch), 0.125)`, with linear interpolation and clamp addressing; returns red channel. | One texture sample. |
| Shader helper | `float spectrum(float pitch, float falloff)` | `float` | Spectrum helper; requires explicit `spec` declaration | **Public helper:** call directly. | Samples `float2(saturate(pitch), 0.125 + 0.75 * saturate(falloff))`. Falloff 0, ⅓, ⅔, 1 hits the four row centers, from instant to long window. | One texture sample; four cached FFT windows. |
| Shader structure/state | `sampler_state sf_spectrum_sampler` | sampler state | Spectrum helper; requires explicit `spec` declaration | **Implementation state:** use `spectrum(pitch)`. | Linear filtering and clamp addressing. | Sampler binding when used. |
| Shader uniform | `uniform float energy;` | `float` | Preamble | **Direct input:** use this variable directly. | Three-second rolling RMS of all captured channels, dB-mapped to [0,1]. | Shared rolling power history + scalar assignment. |
| Shader uniform | `uniform float e_bass;` | `float` | Preamble | **Direct input:** use this variable directly. | Three-second rolling weighted bass RMS, approximately 30–120 Hz. | Shared rolling power history + scalar assignment. |
| Shader uniform | `uniform float e_lows;` | `float` | Preamble | **Direct input:** use this variable directly. | Three-second rolling weighted lows RMS, approximately 120–500 Hz. | Shared rolling power history + scalar assignment. |
| Shader uniform | `uniform float e_mids;` | `float` | Preamble | **Direct input:** use this variable directly. | Three-second rolling weighted mids RMS, approximately 500–3000 Hz. | Shared rolling power history + scalar assignment. |
| Shader uniform | `uniform float e_treb;` | `float` | Preamble | **Direct input:** use this variable directly. | Three-second rolling weighted treble RMS, approximately 3000–13000 Hz. | Shared rolling power history + scalar assignment. |
| Shader uniform | `uniform float4 gamepad_axes_clamped[32];` | `float4[32]` | Gamepad preamble, D3D only | **Backend carrier — use helper:** `gamepad_axis_clamped(c,a)` | Two packed vectors per slot, 6 clamped positions in [0,1]. | Cached input snapshot and packed-vector assignment. |
| Shader helper | `float gamepad_axis_clamped(int controller, int axis)` | `float` | Gamepad preamble | **Public helper:** call directly. | Clamped position; full deflection traverses [0,1] in two seconds. | Vector lookup / one fetch. |
| Native C/C++ API | `float sf_gamepad_axis_clamped(int controller, int axis)` | `float` | Declared in `shader-gamepad.h`; not shader-callable | **Native API:** call from C/C++. | Cached clamped position under the mutex; invalid indices return zero. | Synchronized snapshot read. |
| Shader uniform | `uniform float audio_magnitude;` | `float` | Preamble | **Direct input:** use this variable directly. | RMS amplitude across captured frames/channels with the same dB mapping. Not raw RMS. | Audio analysis and scalar assignment. |
| Shader uniform | `uniform float audio_peak;` | `float` | Preamble | **Direct input:** use this variable directly. | Peak absolute amplitude across captured channels, mapped from -60 dB to 0 dB into `[0,1]`. | Audio analysis and scalar assignment. |
| Shader uniform | `uniform float bass;` | `float` | Preamble | **Direct input:** use this variable directly. | Instant block RMS with band-center A-weighting, approximately 30–120 Hz. | Audio. |
| Shader uniform | `uniform float bass_c;` | `float` | Preamble | **Raw counter — prefer helper:** `cu_bass(speed)` or `acc(speed).x`. | Integrated bass counter in `[0,166320)`. | Shared audio integration + scalar assignment. |
| Shader uniform | `uniform bool convert_linear = true;` | `bool` | Declare in custom transition | **Direct input:** use this variable directly. | Whether the default transition shader should apply its nonlinear-to-linear RGB conversion. | Bool assignment; conversion work only if your shader implements it. |
| Shader uniform | `uniform int current_time_day_of_month;` | `int` | Declare | **Direct input:** use this variable directly. | 1–31. | Calendar. |
| Shader uniform | `uniform int current_time_day_of_week;` | `int` | Declare | **Direct input:** use this variable directly. | 0 = Sunday through 6 = Saturday. | Calendar. |
| Shader uniform | `uniform int current_time_day_of_year;` | `int` | Declare | **Direct input:** use this variable directly. | **0-based**, 0–365. | Calendar. |
| Shader uniform | `uniform int current_time_hour;` | `int` | Declare | **Direct input:** use this variable directly. | Local hour, 0–23. | Calendar. |
| Shader uniform | `uniform int current_time_min;` | `int` | Declare | **Direct input:** use this variable directly. | Local minute, 0–59. | Calendar. |
| Shader uniform | `uniform int current_time_month;` | `int` | Declare | **Direct input:** use this variable directly. | **0 = January**, 11 = December; add 1 for display. | Calendar. |
| Shader uniform | `uniform int current_time_ms;` | `int` | Declare | **Direct input:** use this variable directly. | Millisecond component, 0–999. | Separate system-time lookup + int assignment. |
| Shader uniform | `uniform int current_time_sec;` | `int` | Declare | **Direct input:** use this variable directly. | Local `tm_sec`, normally 0–59; platform leap-second semantics apply. | Shared calendar conversion + int assignment. |
| Shader uniform | `uniform int current_time_year;` | `int` | Declare | **Direct input:** use this variable directly. | **Years since 1900**; add 1900 for the calendar year. | Calendar. |
| Shader uniform | `uniform float ef_bass_fast;` | `float` | Preamble | **Preset carrier:** `expfoff(tails).x` for adjustable falloff; direct read is valid for this exact preset. | Exponential bass, fast | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float ef_bass_medium;` | `float` | Preamble | **Preset carrier:** `expfoff(tails).x` for adjustable falloff; direct read is valid for this exact preset. | Exponential bass, medium | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float ef_bass_slow;` | `float` | Preamble | **Preset carrier:** `expfoff(tails).x` for adjustable falloff; direct read is valid for this exact preset. | Exponential bass, slow | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float ef_bass_slug;` | `float` | Preamble | **Preset carrier:** `expfoff(tails).x` for adjustable falloff; direct read is valid for this exact preset. | Exponential bass, slug | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float ef_lows_fast;` | `float` | Preamble | **Preset carrier:** `expfoff(tails).y` for adjustable falloff; direct read is valid for this exact preset. | Exponential lows, fast | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float ef_lows_medium;` | `float` | Preamble | **Preset carrier:** `expfoff(tails).y` for adjustable falloff; direct read is valid for this exact preset. | Exponential lows, medium | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float ef_lows_slow;` | `float` | Preamble | **Preset carrier:** `expfoff(tails).y` for adjustable falloff; direct read is valid for this exact preset. | Exponential lows, slow | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float ef_lows_slug;` | `float` | Preamble | **Preset carrier:** `expfoff(tails).y` for adjustable falloff; direct read is valid for this exact preset. | Exponential lows, slug | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float ef_mids_fast;` | `float` | Preamble | **Preset carrier:** `expfoff(tails).z` for adjustable falloff; direct read is valid for this exact preset. | Exponential mids, fast | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float ef_mids_medium;` | `float` | Preamble | **Preset carrier:** `expfoff(tails).z` for adjustable falloff; direct read is valid for this exact preset. | Exponential mids, medium | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float ef_mids_slow;` | `float` | Preamble | **Preset carrier:** `expfoff(tails).z` for adjustable falloff; direct read is valid for this exact preset. | Exponential mids, slow | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float ef_mids_slug;` | `float` | Preamble | **Preset carrier:** `expfoff(tails).z` for adjustable falloff; direct read is valid for this exact preset. | Exponential mids, slug | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float ef_treb_fast;` | `float` | Preamble | **Preset carrier:** `expfoff(tails).w` for adjustable falloff; direct read is valid for this exact preset. | Exponential treb, fast | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float ef_treb_medium;` | `float` | Preamble | **Preset carrier:** `expfoff(tails).w` for adjustable falloff; direct read is valid for this exact preset. | Exponential treb, medium | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float ef_treb_slow;` | `float` | Preamble | **Preset carrier:** `expfoff(tails).w` for adjustable falloff; direct read is valid for this exact preset. | Exponential treb, slow | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float ef_treb_slug;` | `float` | Preamble | **Preset carrier:** `expfoff(tails).w` for adjustable falloff; direct read is valid for this exact preset. | Exponential treb, slug | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float elapsed_time;` | `float` | Preamble | **Direct input:** use this variable directly. | Seconds accumulated by this filter's video ticks since creation; not reset by shader reload. | Baseline. |
| Shader uniform | `uniform float elapsed_time_active;` | `float` | Preamble | **Direct input:** use this variable directly. | Consecutive seconds enabled with its parent active; resets to zero otherwise. | Baseline. |
| Shader uniform | `uniform float elapsed_time_enable;` | `float` | Preamble | **Direct input:** use this variable directly. | `elapsed_time` minus the timestamp of the most recent observed enable; continues to advance while disabled if ticks occur. | Baseline. |
| Shader uniform | `uniform float elapsed_time_show;` | `float` | Preamble | **Direct input:** use this variable directly. | Consecutive seconds enabled with its parent showing; resets to zero otherwise. | Baseline. |
| Shader uniform | `uniform float elapsed_time_start;` | `float` | Preamble | **Direct input:** use this variable directly. | Seconds since the first video tick following shader load/reload. | Baseline. |
| Shader uniform | `uniform int gamepad_available;` | `int` | Gamepad preamble | **Direct input:** use this variable directly. | 1 when GLFW initialization succeeded; not a controller count. | Gamepad snapshot/binding. |
| Shader uniform | `uniform float4 gamepad_axes[32];` | `float4[32]` | Gamepad preamble, D3D only | **Backend carrier — use helper:** `gamepad_axis(c,a)` / `gamepad_axis_deadzone(c,a,d)` | Two packed vectors per slot, 6 normalized signed axes. | Yes: cached input snapshot and packed-vector assignment per effect update. |
| Shader uniform | `uniform float4 gamepad_axes_c[32];` | `float4[32]` | Gamepad preamble, D3D only | **Backend carrier — use helper:** `gamepad_axis_cumulative(c,a)` / `gamepad_axis_acc(c,a,rate)` / `gamepad_axis_interval(c,a,interval)` | Two packed vectors per slot, 6 normalized integrated phases. | Yes: cached input snapshot and packed-vector assignment per effect update. |
| Shader uniform | `uniform int4 gamepad_buttons[64];` | `int4[64]` | Gamepad preamble, D3D only | **Backend carrier — use helper:** `gamepad_button(c,b)` | Four packed vectors per slot, 15 button states. | Yes: cached input snapshot and packed-vector assignment per effect update. |
| Shader uniform | `uniform texture2d gamepad_data;` | `texture2d` | Gamepad preamble, OpenGL only | **Backend carrier — use helper:** Public `gamepad_*` accessors; they decode the OpenGL texture. | Shared 19 × 16 RGBA32F texture; at most one upload per OBS video frame. Allocated on first use and retained to module unload. | Yes: shared texture allocation on first use; at most one upload per frame, plus sampling. |
| Shader uniform | `uniform int4 gamepad_info[16];` | `int4[16]` | Gamepad preamble, D3D only | **Backend carrier — use helper:** `gamepad_connected(c)` / `gamepad_last_button(c)` | Per slot: connected, last button, 0, 0. Uploaded per effect parameter update. | Yes: cached input snapshot and packed-vector assignment per effect update. |
| Shader uniform | `uniform int4 gamepad_presses[64];` | `int4[64]` | Gamepad preamble, D3D only | **Backend carrier — use helper:** `gamepad_press_count(c,b)` | Packed press counts, saturated to signed int32. | Yes: cached input snapshot and packed-vector assignment per effect update. |
| Shader uniform | `uniform texture2d image;` | `texture2d` | Preamble | **Direct input:** use this variable directly. | Current filter input. Normally straight alpha; see `USE_PM_ALPHA`. In a transition, selects A before halfway and B from halfway onward. | Baseline input rendering/binding; GPU texture read when sampled. |
| Shader uniform | `uniform texture2d image_a;` | `texture2d` | Declare in custom transition | **Direct input:** use this variable directly. | Transition source A. | Texture binding; supplied by OBS transition rendering. |
| Shader uniform | `uniform texture2d image_b;` | `texture2d` | Declare in custom transition | **Direct input:** use this variable directly. | Transition source B. | Texture binding; supplied by OBS transition rendering. |
| Shader uniform | `uniform float lf_bass_fast;` | `float` | Preamble | **Preset carrier:** `linfoff(tails).x` for adjustable falloff; direct read is valid for this exact preset. | Linear bass, fast | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float lf_bass_medium;` | `float` | Preamble | **Preset carrier:** `linfoff(tails).x` for adjustable falloff; direct read is valid for this exact preset. | Linear bass, medium | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float lf_bass_slow;` | `float` | Preamble | **Preset carrier:** `linfoff(tails).x` for adjustable falloff; direct read is valid for this exact preset. | Linear bass, slow | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float lf_bass_slug;` | `float` | Preamble | **Preset carrier:** `linfoff(tails).x` for adjustable falloff; direct read is valid for this exact preset. | Linear bass, slug | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float lf_lows_fast;` | `float` | Preamble | **Preset carrier:** `linfoff(tails).y` for adjustable falloff; direct read is valid for this exact preset. | Linear lows, fast | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float lf_lows_medium;` | `float` | Preamble | **Preset carrier:** `linfoff(tails).y` for adjustable falloff; direct read is valid for this exact preset. | Linear lows, medium | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float lf_lows_slow;` | `float` | Preamble | **Preset carrier:** `linfoff(tails).y` for adjustable falloff; direct read is valid for this exact preset. | Linear lows, slow | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float lf_lows_slug;` | `float` | Preamble | **Preset carrier:** `linfoff(tails).y` for adjustable falloff; direct read is valid for this exact preset. | Linear lows, slug | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float lf_mids_fast;` | `float` | Preamble | **Preset carrier:** `linfoff(tails).z` for adjustable falloff; direct read is valid for this exact preset. | Linear mids, fast | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float lf_mids_medium;` | `float` | Preamble | **Preset carrier:** `linfoff(tails).z` for adjustable falloff; direct read is valid for this exact preset. | Linear mids, medium | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float lf_mids_slow;` | `float` | Preamble | **Preset carrier:** `linfoff(tails).z` for adjustable falloff; direct read is valid for this exact preset. | Linear mids, slow | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float lf_mids_slug;` | `float` | Preamble | **Preset carrier:** `linfoff(tails).z` for adjustable falloff; direct read is valid for this exact preset. | Linear mids, slug | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float lf_treb_fast;` | `float` | Preamble | **Preset carrier:** `linfoff(tails).w` for adjustable falloff; direct read is valid for this exact preset. | Linear treb, fast | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float lf_treb_medium;` | `float` | Preamble | **Preset carrier:** `linfoff(tails).w` for adjustable falloff; direct read is valid for this exact preset. | Linear treb, medium | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float lf_treb_slow;` | `float` | Preamble | **Preset carrier:** `linfoff(tails).w` for adjustable falloff; direct read is valid for this exact preset. | Linear treb, slow | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float lf_treb_slug;` | `float` | Preamble | **Preset carrier:** `linfoff(tails).w` for adjustable falloff; direct read is valid for this exact preset. | Linear treb, slug | Shared Audio computation + scalar assignment. |
| Shader uniform | `uniform float local_time;` | `float` | Preamble | **Direct input:** use this variable directly. | `os_gettime_ns()/1e9`, a system monotonic time value converted to float. Not local wall-clock time or a Unix timestamp. | Baseline clock read; float precision decreases as magnitude grows. |
| Shader uniform | `uniform float loop_second;` | `float` | Preamble | **Direct input:** use this variable directly. | Remainder of the one-second loop timer. Normally 0–1; code subtracts once only when `> 1`, so exactly 1 and values after very large ticks are possible. | Baseline. |
| Shader uniform | `uniform int loops;` | `int` | Preamble | **Direct input:** use this variable directly. | Count of one-second crossings; when it reaches 4194304 it is negated to -4194304 and continues upward. | Baseline. |
| Shader uniform | `uniform float lows;` | `float` | Preamble | **Direct input:** use this variable directly. | Instant block RMS with band-center A-weighting, approximately 120–500 Hz. | Audio. |
| Shader uniform | `uniform float lows_c;` | `float` | Preamble | **Raw counter — prefer helper:** `cu_lows(speed)` or `acc(speed).y`. | Integrated lows counter in `[0,166320)`. | Shared audio integration + scalar assignment. |
| Shader uniform | `uniform float mids;` | `float` | Preamble | **Direct input:** use this variable directly. | Instant block RMS with band-center A-weighting, approximately 500–3000 Hz. | Audio. |
| Shader uniform | `uniform float mids_c;` | `float` | Preamble | **Raw counter — prefer helper:** `cu_mids(speed)` or `acc(speed).z`. | Integrated mids counter in `[0,166320)`. | Shared audio integration + scalar assignment. |
| Shader uniform | `uniform texture2d previous_image;` | `texture2d` | Declare | **Direct input:** use this variable directly. | Previous captured input to this filter. | Yes: retained extra input render target + ping-pong state. |
| Shader uniform | `uniform texture2d previous_output;` | `texture2d` | Declare | **Direct input:** use this variable directly. | Previous rendered output of this filter; useful for feedback. | Yes: retained extra output render target + ping-pong state. |
| Shader uniform | `uniform float rand_activation_f;` | `float` | Preamble | **Direct input:** use this variable directly. | Random value selected at creation and every settings update. It is not exclusively tied to an OBS activation event. | RNG on settings update, scalar binding during rendering. |
| Shader uniform | `uniform float rand_f;` | `float` | Preamble | **Direct input:** use this variable directly. | New pseudorandom value each video tick. | Baseline RNG call. |
| Shader uniform | `uniform float rand_instance_f;` | `float` | Preamble | **Direct input:** use this variable directly. | Random value selected at filter creation. | Baseline scalar binding. |
| Shader uniform | `uniform texture2d t_bass;` | `texture2d` | Preamble | **History carrier — helper available:** `ts(x).x`. Direct sampling is valid for custom/single-band access. | Bass history, 512 × 1, single-channel R32F. | Audio history allocation/upload; texture sampling if used. |
| Shader uniform | `uniform texture2d t_lows;` | `texture2d` | Preamble | **History carrier — helper available:** `ts(x).y`. Direct sampling is valid for custom/single-band access. | Lows history, 512 × 1, R32F. | Audio history allocation/upload; texture sampling if used. |
| Shader uniform | `uniform texture2d t_mids;` | `texture2d` | Preamble | **History carrier — helper available:** `ts(x).z`. Direct sampling is valid for custom/single-band access. | Mids history, 512 × 1, R32F. | Audio history allocation/upload; texture sampling if used. |
| Shader uniform | `uniform texture2d t_treb;` | `texture2d` | Preamble | **History carrier — helper available:** `ts(x).w`. Direct sampling is valid for custom/single-band access. | Treble history, 512 × 1, R32F. | Audio history allocation/upload; texture sampling if used. |
| Shader uniform | `uniform float transition_time = 0.5;` | `float` | Declare in custom transition | **Direct input:** use this variable directly. | Transition progress `t`, normally 0–1. | Scalar assignment. |
| Shader uniform | `uniform float treb;` | `float` | Preamble | **Direct input:** use this variable directly. | Instant block RMS with band-center A-weighting, approximately 3000–13000 Hz. The name is `treb`, not `treble`. | Audio. |
| Shader uniform | `uniform float treb_c;` | `float` | Preamble | **Raw counter — prefer helper:** `cu_treb(speed)` or `acc(speed).w`. | Integrated treble counter in `[0,166320)`. | Shared audio integration + scalar assignment. |
| Shader uniform | `uniform float2 uv_offset;` | `float2` | Preamble | **Direct input:** use this variable directly. | `(-expand_left/base_width, -expand_top/base_height)`. | Baseline. |
| Shader uniform | `uniform float2 uv_pixel_interval;` | `float2` | Preamble | **Direct input:** use this variable directly. | `(1/base_width, 1/base_height)`; one source pixel in normalized UV units. | Baseline. |
| Shader uniform | `uniform float2 uv_scale;` | `float2` | Preamble | **Direct input:** use this variable directly. | Expanded output size divided by input base size. | Baseline. |
| Shader uniform | `uniform float2 uv_size;` | `float2` | Preamble | **Direct input:** use this variable directly. | Expanded output dimensions in pixels. | Baseline. |
| Shader uniform | `uniform float4x4 ViewProj;` | `float4x4` | Preamble | **Pipeline input:** used by `mainTransform(v_in)`; read directly for a custom vertex transform. | OBS view/projection matrix used by the vertex transform. | Baseline matrix binding; vertex multiplication when used. |
| Shader helper | `float4 acc(float rate)` | `float4` | Preamble | **Public helper:** call directly. | `frac(float4(bass_c,lows_c,mids_c,treb_c) / pow(166320, saturate(1-rate)))`. | Audio plus GPU power/division/fraction. |
| Shader helper | `float cu_bass(float speed)` | `float` | Preamble | **Public helper:** call directly. | `acc(speed).x`. | Same phase calculation. |
| Shader helper | `float cu_lows(float speed)` | `float` | Preamble | **Public helper:** call directly. | `acc(speed).y`. | Same phase calculation. |
| Shader helper | `float cu_mids(float speed)` | `float` | Preamble | **Public helper:** call directly. | `acc(speed).z`. | Same phase calculation. |
| Shader helper | `float cu_treb(float speed)` | `float` | Preamble | **Public helper:** call directly. | `acc(speed).w`. | Same phase calculation. |
| Shader helper | `float3 curl_noise(float4 p)` | `float3` | Preamble | **Public helper:** call directly. | Spatial curl; three analytic simplex-derivative evaluations. Deliberately not unit-normalized; magnitude is not bounded to [0,1]. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float4 expfoff(float tailLength)` | `float4` | Preamble (scalar/vector macro wrapper) | **Public helper:** call directly. | Same tail for all bands; the wrapper broadcasts scalars to all four components. | Same as vector form. |
| Shader helper | `float4 expfoff(float4 tailLengths)` | `float4` | Preamble | **Public helper:** call directly. | Per-band interpolation across current/fast/medium/slow/slug values, clamping tails to `[0,1]`. | Audio plus four interpolations/branches. |
| Shader helper | `float gamepad_axis(int controller, int axis)` | `float` | Gamepad preamble | **Public helper:** call directly. | Latest normalized raw signed axis, before deadzone. | Vector lookup / one fetch. |
| Shader helper | `float gamepad_axis_acc(int controller, int axis, float rate)` | `float` | Gamepad preamble | **Public helper:** call directly. | Audio-style rate mapped to `pow(166320, saturate(1-rate))` interval. | Phase access plus power/arithmetic. |
| Shader helper | `float gamepad_axis_cumulative(int controller, int axis)` | `float` | Gamepad preamble | **Public helper:** call directly. | Integrated phase, already in [0,1). | Vector lookup / one fetch. |
| Shader helper | `float gamepad_axis_deadzone(int controller, int axis, float deadzone)` | `float` | Gamepad preamble | **Public helper:** call directly. | Removes center deadzone and rescales remaining signed magnitude; deadzone clamped to [0,1]. | Axis access plus arithmetic. |
| Shader helper | `float gamepad_axis_interval(int controller, int axis, float interval)` | `float` | Gamepad preamble | **Public helper:** call directly. | `frac(phase * 166320 / interval)`; valid intervals (0,166320], otherwise zero. | Phase access plus arithmetic. |
| Shader helper | `int gamepad_button(int controller, int button)` | `int` | Gamepad preamble | **Public helper:** call directly. | 0 released, 1 held. | Vector lookup / one fetch. |
| Shader helper | `bool gamepad_connected(int controller)` | `bool` | Gamepad preamble | **Public helper:** call directly. | Whether that mapped slot is connected. | D3D vector lookup; OpenGL one texture fetch. |
| Shader helper | `int gamepad_last_button(int controller)` | `int` | Gamepad preamble | **Public helper:** call directly. | Most recent sampled rising-edge button ID, initially -1. | Vector lookup / one fetch. |
| Shader helper | `int gamepad_press_count(int controller, int button)` | `int` | Gamepad preamble | **Public helper:** call directly. | Cumulative rising-edge presses, saturating at 2147483647. | Vector lookup / two fetches to reconstruct exact int32. |
| Shader helper | `float3 hsl2hsv(float3 hsl)` | `float3` | Preamble | **Public helper:** call directly. | Direct saturation/value conversion. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float3 hsl2oklch(float3 hsl)` | `float3` | Preamble | **Public helper:** call directly. | HSL → nonlinear RGB → OKLCH. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float3 hsl2rgb(float3 hsl)` | `float3` | Preamble | **Public helper:** call directly. | HSL to RGB through HSV. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float3 hsv2hsl(float3 hsv)` | `float3` | Preamble | **Public helper:** call directly. | Direct saturation/lightness conversion. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float3 hsv2oklch(float3 hsv)` | `float3` | Preamble | **Public helper:** call directly. | HSV → nonlinear RGB → OKLCH. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float3 hsv2rgb(float3 hsv)` | `float3` | Preamble | **Public helper:** call directly. | HSV to RGB; hue-sector arithmetic. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float interp_falloff5(float t, float p0, float p1, float p2, float p3, float p4)` | `float` | Preamble, implementation helper | **Implementation helper:** usually use `expfoff(tails)` / `linfoff(tails)` | Piecewise interpolation through five anchors at t = 0, .25, .5, .75, 1. | GPU arithmetic/branches. |
| Shader helper | `float4 linfoff(float tailLength)` | `float4` | Preamble (scalar/vector macro wrapper) | **Public helper:** call directly. | Same tail for all bands; scalar and vector arguments work on every backend. | Same as vector form. |
| Shader helper | `float4 linfoff(float4 tailLengths)` | `float4` | Preamble | **Public helper:** call directly. | As above, using linear presets. | Audio plus four interpolations/branches. |
| Shader helper | `float3 linrgb2oklch(float3 rgb)` | `float3` | Preamble | **Public helper:** call directly. | Linear-light sRGB to OKLCH; skips the transfer curve. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `VertData mainTransform(VertData v_in)` | `VertData` | Preamble | **Public helper:** call directly. | Standard vertex transform. | Vertex-stage matrix/UV arithmetic. |
| Shader helper | `float3 oklch2hsl(float3 lch)` | `float3` | Preamble | **Public helper:** call directly. | OKLCH → nonlinear RGB, clipped to [0,1] → HSL. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float3 oklch2hsv(float3 lch)` | `float3` | Preamble | **Public helper:** call directly. | OKLCH → nonlinear RGB, clipped to [0,1] → HSV. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float3 oklch2linrgb(float3 lch)` | `float3` | Preamble | **Public helper:** call directly. | OKLCH to linear-light sRGB; skips the transfer curve. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float3 oklch2rgb(float3 lch)` | `float3` | Preamble | **Public helper:** call directly. | OKLCH to nonlinear sRGB; trig, matrices and transfer curves. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float perlin_noise(float4 p)` | `float` | Preamble | **Public helper:** call directly. | Signed gradient noise; 16 corners with quintic interpolation. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float perlin_noise_fbm(float4 p, int octaves, float frequency, float lacunarity, float persistence)` | `float` | Preamble | **Public helper:** call directly. | Amplitude-normalized fractal sum; 1–8 Perlin evaluations. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float3 rgb2hsl(float3 rgb)` | `float3` | Preamble | **Public helper:** call directly. | RGB to HSL through HSV. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float3 rgb2hsv(float3 rgb)` | `float3` | Preamble | **Public helper:** call directly. | RGB to HSV; min/max, divisions and branches. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float3 rgb2oklch(float3 rgb)` | `float3` | Preamble | **Public helper:** call directly. | Nonlinear sRGB to OKLCH; transfer curves, matrices, cube root and atan2. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float4 sf_gamepad_fetch(int controller, int column)` | `float4` | Gamepad preamble, OpenGL only | **Implementation helper:** usually use Public `gamepad_*` accessors, which validate indices and handle the backend. | Internal texture lookup; public accessors perform index validation. | No computation from the function definition alone; one texture fetch per call and shared gamepad upload costs. |
| Shader helper | `float4 sf_noise_gradient(float4 cell)` | `float4` | Preamble | **Implementation helper:** usually use `simplex_noise(p)` / `perlin_noise(p)` / `perlin_noise_fbm(...)`. | Gradient hashing/selection. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float sf_noise_hash(float4 cell)` | `float` | Preamble | **Implementation helper:** usually use Public noise functions. | Nested permutations. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float sf_noise_permute(float x)` | `float` | Preamble | **Implementation helper:** usually use `simplex_noise(p)`, `perlin_noise(p)`, `curl_noise(p)`, or `voronoi_noise(p)`. | Arithmetic lattice permutation. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float sf_oklch_decode_srgb(float channel)` | `float` | Preamble | **Implementation helper:** usually use `rgb2oklch(rgb)` for the complete color conversion. | Signed sRGB decoding. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float sf_oklch_encode_srgb(float channel)` | `float` | Preamble | **Implementation helper:** usually use `oklch2rgb(lch)` for the complete color conversion. | Signed sRGB encoding. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float4 sf_simplex_corner(float4 cell, float4 offset)` | `float4` | Preamble | **Implementation helper:** usually use `simplex_noise(p)` / `curl_noise(p)`. | Analytic corner contribution: xyz derivative, w scalar. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float4 sf_simplex_derivative(float4 p)` | `float4` | Preamble | **Implementation helper:** usually use `simplex_noise(p)` / `curl_noise(p)`; direct use is valid when you specifically need the analytic derivative. | Five-corner analytic derivative/value sum. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float4 sf_voronoi_point(float4 cell)` | `float4` | Preamble | **Implementation helper:** usually use `voronoi_noise(p)`. | Hash-derived point offset within the cell. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float simplex_noise(float4 p)` | `float` | Preamble | **Public helper:** call directly. | Signed noise, approximately [-1,1]; five simplex corners. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float3 srgb_nonlinear_to_linear(float3 v)` | `float3` | Preamble | **Public helper:** call directly. | Legacy sRGB transfer helper, used by the default transition. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float srgb_nonlinear_to_linear_channel(float u)` | `float` | Preamble | **Public helper:** call directly. | Scalar legacy transfer curve. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader helper | `float4 ts(float x)` | `float4` | Preamble | **Public helper:** call directly. | Sample all four history textures at normalized coordinate `(x,0.5)`. Order: bass, lows, mids, treb. | Audio plus four texture reads. |
| Shader helper | `float voronoi_noise(float4 p)` | `float` | Preamble | **Public helper:** call directly. | Euclidean F1 distance; 81 cells, one square root. Feature jitter is bounded to each cell's middle third. | No CPU resource allocation from the function definition; GPU computation on calls, as described. |
| Shader entry point | `float4 mainImage(VertData v_in) : TARGET` | `float4` | Implement in your shader | **Entry point:** implement this function. | Required pixel entry point used by the generated `Draw` technique. | Your shader's pixel work. |
| Shader structure/state | `sampler_state gamepad_sampler` | `sampler_state` | Gamepad preamble, OpenGL only | **Backend support state:** accessed through public `gamepad_*` helpers. | Point filtering and clamp addressing. | Sampler declaration only; texture work occurs when sampled. |
| Shader structure/state | `sampler_state textureSampler` | `sampler_state` | Preamble | **Direct sampling API:** `image.Sample(textureSampler, v_in.uv)`; also used by `ts(x)`. | Linear filtering; transparent-black border addressing in U/V. | Sampling state, used only with texture sampling. |
| Shader structure/state | `struct VertData` | struct | Preamble | **Pipeline definition:** use the normal shader entry point/transform; direct use is valid for custom pipeline work. | Vertex/pixel input structure containing `pos` and `uv`. | Structure declaration itself has no per-pixel work. |
| Shader structure/state | `technique Draw` | technique | Generated template suffix | **Pipeline definition:** use the normal shader entry point/transform; direct use is valid for custom pipeline work. | Uses `mainTransform` and `mainImage`. | Normal effect draw. |
| Shader structure/state | `VertData.pos` | `float4`, `POSITION` | Field of `VertData` | **Pipeline definition:** use the normal shader entry point/transform; direct use is valid for custom pipeline work. | Position supplied through the pipeline; the vertex transform applies `ViewProj`. | Baseline varying. |
| Shader structure/state | `VertData.uv` | `float2`, `TEXCOORD0` | Field of `VertData` | **Pipeline definition:** use the normal shader entry point/transform; direct use is valid for custom pipeline work. | Interpolated UV after `uv_scale` and `uv_offset`; expansion may put it outside `[0,1]`. | Baseline varying. |
| Shader preprocessing | `#include "path"` | file-loader directive | Optional in loaded shader files | **Preprocessor control:** use as documented, not as a uniform. | Loads/inlines another shader text file relative to the including file; I/O at load/reload. | Compile/load-time control; no uniform upload. Included files require I/O at load/reload. |
| Shader preprocessing | `OPENGL` | preprocessor define | Automatically defined as 1 on OpenGL | **Preprocessor control:** use as documented, not as a uniform. | Selects backend-specific code; absent on D3D. No runtime uniform. | Compile/load-time control; no uniform upload. Included files require I/O at load/reload. |
| Shader preprocessing | `USE_PM_ALPHA` | preprocessor define | Optional: `#define USE_PM_ALPHA 1` | **Preprocessor control:** use as documented, not as a uniform. | Changes the filter's input/output alpha handling to premultiplied-alpha behavior. No uniform upload. Detection uses the literal define text in the assembled effect. | Compile/load-time control; no uniform upload. Included files require I/O at load/reload. |
| Custom uniform type | `uniform bool <your_name>;` | `bool` | User-defined | **Custom input:** declare your own name and read it directly. | Checkbox. | Settings/property storage and per-render assignment. |
| Custom uniform type | `uniform float <your_name>;` | `float` | User-defined | **Custom input:** declare your own name and read it directly. | Number or slider. | Settings/property storage and per-render assignment. |
| Custom uniform type | `uniform float2 <your_name>;` | `float2` | User-defined | **Custom input:** declare your own name and read it directly. | Two numeric fields/sliders. | Settings/property storage and vector assignment. |
| Custom uniform type | `uniform float3 <your_name>;` | `float3` | User-defined | **Custom input:** declare your own name and read it directly. | RGB color by default, or three component sliders. | Settings/property storage and vector assignment. |
| Custom uniform type | `uniform float4 <your_name>;` | `float4` | User-defined | **Custom input:** declare your own name and read it directly. | RGBA color by default, or four component sliders. | Settings/property storage and vector assignment. |
| Custom uniform type | `uniform int <your_name>;` | `int` | User-defined | **Custom input:** declare your own name and read it directly. | Number, slider, or select list. | Settings/property storage and per-render assignment. |
| Custom uniform type | `uniform string <your_name>;` | `string` | User-defined | **Custom input:** declare your own name and read it directly. | Multiline text or info text. | CPU settings/string handling; not a native GPU string value for shader math. |
| Custom uniform type | `uniform texture2d <your_name>;` | `texture2d` | User-defined | **Custom input:** declare your own name and read it directly. | Source/scene selector and/or image file picker. | Yes; selected source may stay active/showing and is rendered to its own target. Selected image is loaded/allocated. |
| Uniform annotation | `int option_N` | `int` | Optional annotation | **Annotation:** metadata on a custom uniform, not a shader value. | Integer value for select option N; use sequential zero-based entries. | Annotation metadata/property work; no independent uniform or per-pixel operation. |
| Uniform annotation | `int or float maximum` | `int` or `float` | Optional annotation | **Annotation:** metadata on a custom uniform, not a shader value. | UI upper bound; not an automatic shader clamp. | Annotation metadata/property work; no independent uniform or per-pixel operation. |
| Uniform annotation | `int or float minimum` | `int` or `float` | Optional annotation | **Annotation:** metadata on a custom uniform, not a shader value. | UI lower bound. Use scalar float for float/vector controls, int for int controls. Not an automatic shader clamp. | Annotation metadata/property work; no independent uniform or per-pixel operation. |
| Uniform annotation | `int or float step` | `int` or `float` | Optional annotation | **Annotation:** metadata on a custom uniform, not a shader value. | UI increment. | Annotation metadata/property work; no independent uniform or per-pixel operation. |
| Uniform annotation | `string group` | `string` | Optional annotation | **Annotation:** metadata on a custom uniform, not a shader value. | Groups properties by name. | Annotation metadata/property work; no independent uniform or per-pixel operation. |
| Uniform annotation | `string label` | `string` | Optional annotation | **Annotation:** metadata on a custom uniform, not a shader value. | Same display-label field as `name`; later parsed value wins. | Annotation metadata/property work; no independent uniform or per-pixel operation. |
| Uniform annotation | `string name` | `string` | Optional annotation | **Annotation:** metadata on a custom uniform, not a shader value. | Property display label; metadata/UI work. | Annotation metadata/property work; no independent uniform or per-pixel operation. |
| Uniform annotation | `string option_N` | `string` | Optional annotation | **Annotation:** metadata on a custom uniform, not a shader value. | Select label or vector component label N. Pair int/string entries for select controls. | Annotation metadata/property work; no independent uniform or per-pixel operation. |
| Uniform annotation | `string widget_type` | `string` | Optional annotation | **Annotation:** metadata on a custom uniform, not a shader value. | Recognized values: `slider`, `select` (int), `source`/`file` (texture), `info` (string). UI work; selected texture sources/files have costs above. | Annotation metadata/property work; no independent uniform or per-pixel operation. |
| Application setting | `<uniform>_0` | number | Generated vector-component settings; not a shader input | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | Numeric components for applicable vector controls. | Settings/UI or load-time work; a same-named shader uniform does not expose this value. |
| Application setting | `<uniform>_1` | number | Generated vector-component settings; not a shader input | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | Numeric components for applicable vector controls. | Settings/UI or load-time work; a same-named shader uniform does not expose this value. |
| Application setting | `<uniform>_2` | number | Generated vector-component settings; not a shader input | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | Numeric components for applicable vector controls. | Settings/UI or load-time work; a same-named shader uniform does not expose this value. |
| Application setting | `<uniform>_3` | number | Generated vector-component settings; not a shader input | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | Numeric components for applicable vector controls. | Settings/UI or load-time work; a same-named shader uniform does not expose this value. |
| Application setting | `<uniform>_source` | string source name | Generated custom texture setting; not a shader input | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | Source selected for a custom texture uniform; can activate/render another source. | Can keep a selected source active and render it into a texture. |
| Application setting | `audio_source` | string source name | Filter/transition setting; not a shader input | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | Audio capture source, with parent fallback described above. | Settings/UI or load-time work; a same-named shader uniform does not expose this value. |
| Application setting | `expand_bottom` | integer pixels | Filter properties; not shader declarations; not a shader input | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | Increase render dimensions, changing texture memory and pixel workload. | Larger render targets increase memory and pixel work. |
| Application setting | `expand_left` | integer pixels | Filter properties; not shader declarations; not a shader input | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | Increase render dimensions, changing texture memory and pixel workload. | Larger render targets increase memory and pixel work. |
| Application setting | `expand_right` | integer pixels | Filter properties; not shader declarations; not a shader input | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | Increase render dimensions, changing texture memory and pixel workload. | Larger render targets increase memory and pixel work. |
| Application setting | `expand_top` | integer pixels | Filter properties; not shader declarations; not a shader input | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | Increase render dimensions, changing texture memory and pixel workload. | Larger render targets increase memory and pixel work. |
| Application setting | `from_file` | bool | Filter/transition setting; not a shader input | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | Loads shader text from a file. | Settings/UI or load-time work; a same-named shader uniform does not expose this value. |
| Application setting | `last_error` | string | Generated settings/UI field; not a shader input | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | Last shader parser/compiler error; not a shader input. | Settings/UI or load-time work; a same-named shader uniform does not expose this value. |
| Application setting | `override_entire_effect` | bool | File-loading setting; not a shader input | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | Disables the normal shader template for a loaded complete effect. | Settings/UI or load-time work; a same-named shader uniform does not expose this value. |
| Application setting | `shader_file_name` | string path | Filter/transition setting; not a shader input | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | File used when `from_file` is true; file I/O on load/reload. | Settings/UI or load-time work; a same-named shader uniform does not expose this value. |
| Application setting | `shader_text` | string | Filter/transition setting; not a shader input | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | Pasted shader text / default shader. Compilation on reload. | Settings/UI or load-time work; a same-named shader uniform does not expose this value. |
| Application UI identifier | `plugin_info` | Property ID | Defined in plugin UI; not a shader variable | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | Read-only plugin information shown in properties. | Button work on invocation; info text is UI-only. |
| Application UI identifier | `reload_effect` | Property ID | Defined in plugin UI; not a shader variable | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | Property button: reload/compile the shader. | Button work on invocation; info text is UI-only. |
| Application UI identifier | `shader_convert` | Property ID | Defined in plugin UI; not a shader variable | **Settings/UI:** access through the application settings or UI, not a same-named shader uniform. | Property button: convert supported external shader syntax. | Button work on invocation; info text is UI-only. |
| Native C/C++ API | `QDialog * sf_create_controller_dialog(QWidget *parent)` | `QDialog *` | Declared in `shader-controller-dialog.hpp`; not shader-callable | **Native API:** call from C/C++, not shader code. | Create the parent-owned, delete-on-close Qt controller inspector. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native C/C++ API | `bool sf_gamepad_available(void)` | `bool` | Declared in `shader-gamepad.h`; not shader-callable | **Native API:** call from C/C++, not shader code. | Cached status under the shared mutex. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native C/C++ API | `float sf_gamepad_axis(int controller, int axis)` | `float` | Declared in `shader-gamepad.h`; not shader-callable | **Native API:** call from C/C++, not shader code. | Normalized cached signed axis; optional deadzone calculation. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native C/C++ API | `float sf_gamepad_axis_acc(int controller, int axis, float rate)` | `float` | Declared in `shader-gamepad.h`; not shader-callable | **Native API:** call from C/C++, not shader code. | Native phase helpers; invalid/nonfinite interval/rate returns zero. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native C/C++ API | `float sf_gamepad_axis_cumulative(int controller, int axis)` | `float` | Declared in `shader-gamepad.h`; not shader-callable | **Native API:** call from C/C++, not shader code. | Normalized cached phase, rounded down if float conversion would reach 1. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native C/C++ API | `float sf_gamepad_axis_deadzone(int controller, int axis, float deadzone)` | `float` | Declared in `shader-gamepad.h`; not shader-callable | **Native API:** call from C/C++, not shader code. | Normalized cached signed axis; optional deadzone calculation. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native C/C++ API | `float sf_gamepad_axis_interval(int controller, int axis, float interval)` | `float` | Declared in `shader-gamepad.h`; not shader-callable | **Native API:** call from C/C++, not shader code. | Native phase helpers; invalid/nonfinite interval/rate returns zero. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native C/C++ API | `bool sf_gamepad_button(int controller, int button)` | `bool` | Declared in `shader-gamepad.h`; not shader-callable | **Native API:** call from C/C++, not shader code. | Cached button state under the mutex. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native C/C++ API | `bool sf_gamepad_connected(int controller)` | `bool` | Declared in `shader-gamepad.h`; not shader-callable | **Native API:** call from C/C++, not shader code. | Cached status under the shared mutex. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native C/C++ API | `void sf_gamepad_get_controller_state(int controller, struct sf_gamepad_controller_state *state)` | `void` | Declared in `shader-gamepad.h`; not shader-callable | **Native API:** call from C/C++, not shader code. | One synchronized native controller snapshot for the inspector; full uint64 counts, double phases, and double clamped positions. Invalid slot yields neutral values and last_button=-1. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native C/C++ API | `void sf_gamepad_get_snapshot(struct sf_gamepad_snapshot *snapshot)` | `void` | Declared in `shader-gamepad.h`; not shader-callable | **Native API:** call from C/C++, not shader code. | One synchronized packed shader snapshot; press counts narrowed to saturated int32. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native C/C++ API | `bool sf_gamepad_init(void)` | `bool` | Declared in `shader-gamepad.h`; not shader-callable | **Native API:** call from C/C++, not shader code. | UI-thread GLFW/timer lifecycle; init reports success. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native C/C++ API | `int sf_gamepad_last_button(int controller)` | `int` | Declared in `shader-gamepad.h`; not shader-callable | **Native API:** call from C/C++, not shader code. | Last sampled button ID; -1 initially/invalid. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native C/C++ API | `void sf_gamepad_pack_snapshot(const struct sf_gamepad_snapshot *snapshot, float *pixels)` | `void` | Declared in `shader-gamepad.h`; not shader-callable | **Native upload utility:** for the renderer; use typed native getters/snapshots for ordinary controller reads. | CPU conversion into the 19×16 OpenGL texture layout; caller supplies the buffer. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native C/C++ API | `uint64_t sf_gamepad_press_count(int controller, int button)` | `uint64_t` | Declared in `shader-gamepad.h`; not shader-callable | **Native API:** call from C/C++, not shader code. | Full native count, saturating at UINT64_MAX. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native C/C++ API | `void sf_gamepad_shutdown(void)` | `void` | Declared in `shader-gamepad.h`; not shader-callable | **Native API:** call from C/C++, not shader code. | UI-thread GLFW/timer lifecycle; init reports success. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native C/C++ API | `void sf_tools_init(void)` | `void` | Declared in `shader-tools.h`; not shader-callable | **Native API:** call from C/C++, not shader code. | Controller menu/window lifecycle on the UI thread. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native C/C++ API | `void sf_tools_shutdown(void)` | `void` | Declared in `shader-tools.h`; not shader-callable | **Native API:** call from C/C++, not shader code. | Controller menu/window lifecycle on the UI thread. | Header declaration alone has no runtime work; calls perform the read/calculation/lifecycle work described. |
| Native constant | `#define SF_GAMEPAD_AXIS_COUNT 6` | integer constant | Defined in shader-gamepad.h; not injected into shaders | **Native constant:** use directly in C/C++; not a shader input. | Controller dimensions, layout, counter period, or integration deadzone. | Constant substitution only; no work from including the definition. |
| Native constant | `#define SF_GAMEPAD_BUTTON_COUNT 15` | integer constant | Defined in shader-gamepad.h; not injected into shaders | **Native constant:** use directly in C/C++; not a shader input. | Controller dimensions, layout, counter period, or integration deadzone. | Constant substitution only; no work from including the definition. |
| Native constant | `#define SF_GAMEPAD_COUNT 16` | integer constant | Defined in shader-gamepad.h; not injected into shaders | **Native constant:** use directly in C/C++; not a shader input. | Controller dimensions, layout, counter period, or integration deadzone. | Constant substitution only; no work from including the definition. |
| Native constant | `#define SF_GAMEPAD_COUNTER_WRAP 166320.0` | double constant | Defined in shader-gamepad.h; not injected into shaders | **Native constant:** use directly in C/C++; not a shader input. | Controller dimensions, layout, counter period, or integration deadzone. | Constant substitution only; no work from including the definition. |
| Native constant | `#define SF_GAMEPAD_INTEGRATION_DEADZONE 0.15f` | float constant | Defined in shader-gamepad.h; not injected into shaders | **Native constant:** use directly in C/C++; not a shader input. | Controller dimensions, layout, counter period, or integration deadzone. | Constant substitution only; no work from including the definition. |
| Native constant | `#define SF_GAMEPAD_TEXTURE_WIDTH 19` | integer constant | Defined in shader-gamepad.h; not injected into shaders | **Native constant:** use directly in C/C++; not a shader input. | Controller dimensions, layout, counter period, or integration deadzone. | Constant substitution only; no work from including the definition. |
| Native structure | `struct sf_gamepad_controller_state` | struct | Declared in shader-gamepad.h; C/C++ only | **Native snapshot API:** fill with `sf_gamepad_get_controller_state(c,&state)`, then read these typed fields directly. | Full-precision native per-controller diagnostic snapshot. | Type declaration alone has no runtime work; allocating/filling a value uses storage/copy work. |
| Native structure | `struct sf_gamepad_snapshot` | struct | Declared in shader-gamepad.h; C/C++ only | **Native upload carrier:** fill via `sf_gamepad_get_snapshot`; prefer `sf_gamepad_get_controller_state` or typed getters for application/inspector reads. | Packed cross-backend shader upload snapshot. | Type declaration alone has no runtime work; allocating/filling a value uses storage/copy work. |
| Native structure field | `sf_gamepad_controller_state.axes` | `float[SF_GAMEPAD_AXIS_COUNT]` | Field in `sf_gamepad_controller_state`; not a shader variable | **Native snapshot API:** fill with `sf_gamepad_get_controller_state(c,&state)`, then read these typed fields directly. | Normalized signed instantaneous axes. | Storage/copy as part of its snapshot; no extra polling. |
| Native structure field | `sf_gamepad_controller_state.axes_c` | `double[SF_GAMEPAD_AXIS_COUNT]` | Field in `sf_gamepad_controller_state`; not a shader variable | **Native snapshot API:** fill with `sf_gamepad_get_controller_state(c,&state)`, then read these typed fields directly. | Normalized integrated phases in [0,1). | Storage/copy as part of its snapshot; no extra polling. |
| Native structure field | `sf_gamepad_controller_state.buttons` | `bool[SF_GAMEPAD_BUTTON_COUNT]` | Field in `sf_gamepad_controller_state`; not a shader variable | **Native snapshot API:** fill with `sf_gamepad_get_controller_state(c,&state)`, then read these typed fields directly. | Held/released button states. | Storage/copy as part of its snapshot; no extra polling. |
| Native structure field | `sf_gamepad_controller_state.connected` | `bool` | Field in `sf_gamepad_controller_state`; not a shader variable | **Native snapshot API:** fill with `sf_gamepad_get_controller_state(c,&state)`, then read these typed fields directly. | Mapped slot connected. | Storage/copy as part of its snapshot; no extra polling. |
| Native structure field | `sf_gamepad_controller_state.last_button` | `int` | Field in `sf_gamepad_controller_state`; not a shader variable | **Native snapshot API:** fill with `sf_gamepad_get_controller_state(c,&state)`, then read these typed fields directly. | Most recent sampled button ID; initially -1. | Storage/copy as part of its snapshot; no extra polling. |
| Native structure field | `sf_gamepad_controller_state.presses` | `uint64_t[SF_GAMEPAD_BUTTON_COUNT]` | Field in `sf_gamepad_controller_state`; not a shader variable | **Native snapshot API:** fill with `sf_gamepad_get_controller_state(c,&state)`, then read these typed fields directly. | Cumulative press counts; int32 shader counts saturate, uint64 native counts preserve full precision. | Storage/copy as part of its snapshot; no extra polling. |
| Native structure field | `sf_gamepad_snapshot.available` | `int32_t` | Field in `sf_gamepad_snapshot`; not a shader variable | **Native upload carrier:** fill via `sf_gamepad_get_snapshot`; prefer `sf_gamepad_get_controller_state` or typed getters for application/inspector reads. | GLFW initialized flag. | Storage/copy as part of its snapshot; no extra polling. |
| Native structure field | `sf_gamepad_snapshot.axes` | `float[SF_GAMEPAD_COUNT * 2][4]` | Field in `sf_gamepad_snapshot`; not a shader variable | **Native upload carrier:** fill via `sf_gamepad_get_snapshot`; prefer `sf_gamepad_get_controller_state` or typed getters for application/inspector reads. | Normalized signed instantaneous axes. | Storage/copy as part of its snapshot; no extra polling. |
| Native structure field | `sf_gamepad_snapshot.axes_c` | `float[SF_GAMEPAD_COUNT * 2][4]` | Field in `sf_gamepad_snapshot`; not a shader variable | **Native upload carrier:** fill via `sf_gamepad_get_snapshot`; prefer `sf_gamepad_get_controller_state` or typed getters for application/inspector reads. | Normalized integrated phases in [0,1). | Storage/copy as part of its snapshot; no extra polling. |
| Native structure field | `sf_gamepad_snapshot.buttons` | `int32_t[SF_GAMEPAD_COUNT * 4][4]` | Field in `sf_gamepad_snapshot`; not a shader variable | **Native upload carrier:** fill via `sf_gamepad_get_snapshot`; prefer `sf_gamepad_get_controller_state` or typed getters for application/inspector reads. | Held/released button states. | Storage/copy as part of its snapshot; no extra polling. |
| Native structure field | `sf_gamepad_snapshot.info` | `int32_t[SF_GAMEPAD_COUNT][4]` | Field in `sf_gamepad_snapshot`; not a shader variable | **Native upload carrier:** fill via `sf_gamepad_get_snapshot`; prefer `sf_gamepad_get_controller_state` or typed getters for application/inspector reads. | Packed connected/last-button values per slot. | Storage/copy as part of its snapshot; no extra polling. |
| Native structure field | `sf_gamepad_snapshot.presses` | `int32_t[SF_GAMEPAD_COUNT * 4][4]` | Field in `sf_gamepad_snapshot`; not a shader variable | **Native upload carrier:** fill via `sf_gamepad_get_snapshot`; prefer `sf_gamepad_get_controller_state` or typed getters for application/inspector reads. | Cumulative press counts; int32 shader counts saturate, uint64 native counts preserve full precision. | Storage/copy as part of its snapshot; no extra polling. |

## How declarations and costs work

The **Declaration** column below describes a normal template-based `.shader`:

- **Preamble**: already declared; use the name directly. Do not redeclare it.
- **Declare**: add the indicated `uniform TYPE name;` yourself.
- **Gamepad preamble**: automatically appended when the loaded shader text
  contains `gamepad_`, including in a comment. Use the helpers without
  redeclaring them.
- **User-defined**: choose your own name and declare it.

With **Use Effect File (.effect)** / `override_entire_effect` enabled for a loaded
file, **none of the normal preamble is appended**. Supply your own uniforms,
samplers, vertex/pixel entry points, helpers, and technique. Recognized uniform
names still receive the values described here. Merely using a `.effect` filename
does not change the rule: the override setting controls it. Pasted shader text
always uses the template.

**Declaring an optional uniform can introduce work even if your pixel shader
never reads it.** OBS retains declared effect parameters, and this plugin binds
resources and updates values based on their presence. GPU dead-code elimination
does not remove that CPU-side bookkeeping. Ordinary arithmetic or function calls,
on the other hand, cost GPU work only when used by a shader stage.

Costs in the charts are qualitative, based on code paths, not benchmark timings:

| Cost | Actual work |
| --- | --- |
| Baseline / scalar | Small CPU value update and parameter assignment during rendering; preamble declarations already incur this path. |
| Calendar | OS clock lookup; calendar fields share one `time()` + `localtime()` conversion per parameter update. |
| Previous frame | An additional retained full-size render target and ping-pong handling per requested history texture, plus sampling if used. |
| Audio | Shared per-source CPU audio analysis, result/history copying to each filter, and history texture uploads when audio text is detected; details below. |
| Gamepad | One shared 8 ms UI-thread polling timer for connected mapped devices; effects using the gamepad preamble additionally copy/upload cached input. |
| Custom texture | Image allocation/loading, or a separate source render into a texture each effect update; sampling costs GPU work. |

## Image, geometry, and shader entry points

| Name | Type | Declaration | Meaning | Declaration/use overhead |
| --- | --- | --- | --- | --- |
| `ViewProj` | `float4x4` | Preamble | OBS view/projection matrix used by the vertex transform. | Baseline matrix binding; vertex multiplication when used. |
| `image` | `texture2d` | Preamble | Current filter input. Normally straight alpha; see `USE_PM_ALPHA`. In a transition, selects A before halfway and B from halfway onward. | Baseline input rendering/binding; GPU texture read when sampled. |
| `uv_offset` | `float2` | Preamble | `(-expand_left/base_width, -expand_top/base_height)`. | Baseline. |
| `uv_scale` | `float2` | Preamble | Expanded output size divided by input base size. | Baseline. |
| `uv_pixel_interval` | `float2` | Preamble | `(1/base_width, 1/base_height)`; one source pixel in normalized UV units. | Baseline. |
| `uv_size` | `float2` | Preamble | Expanded output dimensions in pixels. | Baseline. |
| `textureSampler` | `sampler_state` | Preamble | Linear filtering; transparent-black border addressing in U/V. | Sampling state, used only with texture sampling. |
| `VertData` | struct | Preamble | Vertex/pixel input structure containing `pos` and `uv`. | Structure declaration itself has no per-pixel work. |
| `v_in.pos` | `float4`, `POSITION` | Field of `VertData` | Position supplied through the pipeline; the vertex transform applies `ViewProj`. | Baseline varying. |
| `v_in.uv` | `float2`, `TEXCOORD0` | Field of `VertData` | Interpolated UV after `uv_scale` and `uv_offset`; expansion may put it outside `[0,1]`. | Baseline varying. |
| `mainTransform(VertData v_in)` | returns `VertData` | Preamble | Standard vertex transform. | Vertex-stage matrix/UV arithmetic. |
| `mainImage(VertData v_in) : TARGET` | returns `float4` | Implement in your shader | Required pixel entry point used by the generated `Draw` technique. | Your shader's pixel work. |
| `Draw` | technique | Generated template suffix | Uses `mainTransform` and `mainImage`. | Normal effect draw. |

`main_ps` is **not** the entry point used by the normal template.
For a complete effect override, entry-point names are your choice, but define
the technique your rendering path uses.

## Time, loops, and random values

| Name | Type | Declaration | Meaning | Declaration/use overhead |
| --- | --- | --- | --- | --- |
| `elapsed_time` | `float` | Preamble | Seconds accumulated by this filter's video ticks since creation; not reset by shader reload. | Baseline. |
| `elapsed_time_start` | `float` | Preamble | Seconds since the first video tick following shader load/reload. | Baseline. |
| `elapsed_time_show` | `float` | Preamble | Consecutive seconds enabled with its parent showing; resets to zero otherwise. | Baseline. |
| `elapsed_time_active` | `float` | Preamble | Consecutive seconds enabled with its parent active; resets to zero otherwise. | Baseline. |
| `elapsed_time_enable` | `float` | Preamble | `elapsed_time` minus the timestamp of the most recent observed enable; continues to advance while disabled if ticks occur. | Baseline. |
| `loops` | `int` | Preamble | Count of one-second crossings; when it reaches 4194304 it is negated to -4194304 and continues upward. | Baseline. |
| `loop_second` | `float` | Preamble | Remainder of the one-second loop timer. Normally 0–1; code subtracts once only when `> 1`, so exactly 1 and values after very large ticks are possible. | Baseline. |
| `local_time` | `float` | Preamble | `os_gettime_ns()/1e9`, a system monotonic time value converted to float. Not local wall-clock time or a Unix timestamp. | Baseline clock read; float precision decreases as magnitude grows. |
| `rand_f` | `float` | Preamble | New pseudorandom value each video tick. | Baseline RNG call. |
| `rand_instance_f` | `float` | Preamble | Random value selected at filter creation. | Baseline scalar binding. |
| `rand_activation_f` | `float` | Preamble | Random value selected at creation and every settings update. It is not exclusively tied to an OBS activation event. | RNG on settings update, scalar binding during rendering. |

All three random values can include **both 0 and 1** and are quantized in steps
of `1/10000`. These are not independent per-pixel noise.

### Optional calendar uniforms

Declare these as `uniform int name;`. Each optional declaration adds the
corresponding clock/parameter work, even when unused in the pixel shader.

| Name | Type | Declaration | Actual range / interpretation | Declaration overhead |
| --- | --- | --- | --- | --- |
| `current_time_ms` | `int` | Declare | Millisecond component, 0–999. | Separate system-time lookup + int assignment. |
| `current_time_sec` | `int` | Declare | Local `tm_sec`, normally 0–59; platform leap-second semantics apply. | Shared calendar conversion + int assignment. |
| `current_time_min` | `int` | Declare | Local minute, 0–59. | Calendar. |
| `current_time_hour` | `int` | Declare | Local hour, 0–23. | Calendar. |
| `current_time_day_of_week` | `int` | Declare | 0 = Sunday through 6 = Saturday. | Calendar. |
| `current_time_day_of_month` | `int` | Declare | 1–31. | Calendar. |
| `current_time_month` | `int` | Declare | **0 = January**, 11 = December; add 1 for display. | Calendar. |
| `current_time_day_of_year` | `int` | Declare | **0-based**, 0–365. | Calendar. |
| `current_time_year` | `int` | Declare | **Years since 1900**; add 1900 for the calendar year. | Calendar. |

## Optional previous frames and transition inputs

| Name | Type | Declaration | Meaning | Declaration overhead |
| --- | --- | --- | --- | --- |
| `previous_image` | `texture2d` | Declare | Previous captured input to this filter. | Yes: retained extra input render target + ping-pong state. |
| `previous_output` | `texture2d` | Declare | Previous rendered output of this filter; useful for feedback. | Yes: retained extra output render target + ping-pong state. |
| `image_a` | `texture2d` | Declare in custom transition | Transition source A. | Texture binding; supplied by OBS transition rendering. |
| `image_b` | `texture2d` | Declare in custom transition | Transition source B. | Texture binding; supplied by OBS transition rendering. |
| `transition_time` | `float` | Declare in custom transition | Transition progress `t`, normally 0–1. | Scalar assignment. |
| `convert_linear` | `bool` | Declare in custom transition | Whether the default transition shader should apply its nonlinear-to-linear RGB conversion. | Bool assignment; conversion work only if your shader implements it. |

The four transition-specific uniforms are included in the **default transition
shader text**, not in the shared preamble. Keep or add their declarations when
writing a replacement transition. Outside a transition they are ordinary custom
parameters, not live transition data.

Previous-frame textures may be absent before the first frame has been retained;
they are not guaranteed to contain a valid history frame immediately after load.
The explicit previous-frame ping-pong path is the filter rendering path.

## Audio inputs

| Name | Type | Declaration | Meaning | Declaration/use overhead |
| --- | --- | --- | --- | --- |
| `energy` | `float` | Preamble | Three-second rolling RMS of all captured channels, dB-mapped to [0,1]. | Shared rolling power history + scalar assignment. |
| `e_bass` | `float` | Preamble | Three-second rolling weighted bass RMS, approximately 30–120 Hz. | Shared rolling power history + scalar assignment. |
| `e_lows` | `float` | Preamble | Three-second rolling weighted lows RMS, approximately 120–500 Hz. | Shared rolling power history + scalar assignment. |
| `e_mids` | `float` | Preamble | Three-second rolling weighted mids RMS, approximately 500–3000 Hz. | Shared rolling power history + scalar assignment. |
| `e_treb` | `float` | Preamble | Three-second rolling weighted treble RMS, approximately 3000–13000 Hz. | Shared rolling power history + scalar assignment. |
| `audio_peak` | `float` | Preamble | Peak absolute amplitude across captured channels, mapped from -60 dB to 0 dB into `[0,1]`. | Audio analysis and scalar assignment. |
| `audio_magnitude` | `float` | Preamble | RMS amplitude across captured frames/channels with the same dB mapping. Not raw RMS. | Audio analysis and scalar assignment. |
| `bass` | `float` | Preamble | Instant block RMS with band-center A-weighting, approximately 30–120 Hz. | Audio. |
| `lows` | `float` | Preamble | Instant block RMS with band-center A-weighting, approximately 120–500 Hz. | Audio. |
| `mids` | `float` | Preamble | Instant block RMS with band-center A-weighting, approximately 500–3000 Hz. | Audio. |
| `treb` | `float` | Preamble | Instant block RMS with band-center A-weighting, approximately 3000–13000 Hz. The name is `treb`, not `treble`. | Audio. |
| `bass_c` | `float` | Preamble | Integrated bass counter in `[0,166320)`. | Shared audio integration + scalar assignment. |
| `lows_c` | `float` | Preamble | Integrated lows counter in `[0,166320)`. | Shared audio integration + scalar assignment. |
| `mids_c` | `float` | Preamble | Integrated mids counter in `[0,166320)`. | Shared audio integration + scalar assignment. |
| `treb_c` | `float` | Preamble | Integrated treble counter in `[0,166320)`. | Shared audio integration + scalar assignment. |
| `t_bass` | `texture2d` | Preamble | Bass history, 512 × 1, single-channel R32F. | Audio history allocation/upload; texture sampling if used. |
| `t_lows` | `texture2d` | Preamble | Lows history, 512 × 1, R32F. | Audio history allocation/upload; texture sampling if used. |
| `t_mids` | `texture2d` | Preamble | Mids history, 512 × 1, R32F. | Audio history allocation/upload; texture sampling if used. |
| `t_treb` | `texture2d` | Preamble | Treble history, 512 × 1, R32F. | Audio history allocation/upload; texture sampling if used. |

The band values are bounded `[0,1]`, computed from the RMS of each audio block,
averaged across channel powers, with no attack/release envelope. Differences of
one-pole low-pass filters provide the approximate frequency ranges above. Only
the crossover filters retain sample history; there is no added loudness smoothing.
Each band's RMS is weighted by the same A-weighting curve as the spectrum,
evaluated at its geometric center (60, 245, 1225, and 6245 Hz approximately).
This is a band-center hearing-sensitivity approximation, not full within-band
frequency weighting or calibrated LUFS. The result uses the -60 to 0 dB mapping.
Muted input clears the base values. Missing callbacks clear them after 50 ms or
two block durations, whichever is longer. The global falloff slider was removed;
old `audio_release_ms` settings are ignored.

The cumulative counters add `band * delta_seconds * 100` and wrap at 166320.
They are **not already normalized**. The `acc`/`cu_*` helpers provide normalized
phases. These are the **raw counter** rows in the complete table. For example,
`cu_bass(speed)` (equivalently `acc(speed).x`) returns a usable bass phase in
`[0,1)`; `bass_c` is its underlying wrapped counter in `[0,166320)`. The other
components are lows `.y`, mids `.z`, and treb `.w`. A direct counter read is
appropriate if you intentionally want different scaling or wrap math.

The `t_*` textures are **history carriers**; `ts(x)` is the normal four-band
sampling interface. History receives one sample per audio callback with additional 150 ms
smoothing. Its duration depends on callback size/sample rate, not shader FPS;
512 samples do **not** mean a fixed one second. Upload order is oldest to newest.

### Three-second energy

Use `energy` for overall sustained audio activity, or `e_bass`, `e_lows`,
`e_mids`, and `e_treb` for each band. All five are predeclared and bounded
to `[0,1]`. Select an **Audio source** just as for the existing band inputs.

The analyzer averages squared samples across channels and over the last three
seconds, takes the square root, then applies the existing -60 dB to 0 dB mapping:
`saturate((20 * log10(rms * weight) + 60) / 60)`. Silence and RMS at or below
0.001 after weighting map to zero; RMS of 1 or above maps to one. Overall weight
is 1; band weights use the same band-center A-weighting as the base values.
These are normalized RMS energy indicators, not linear power or LUFS measurements.

The window uses capture duration, independently of shader frame rate, and treats
each audio block's mean power as constant over that block. A fixed ring of 300
ten-millisecond buckets stores power integrals; the oldest bucket is prorated
at the window boundary. It uses about 12 KiB per shared audio source. Startup
history is padded with silence. Muted audio and timestamp gaps add silence;
when callbacks stop, energy expires after three seconds beyond the last block.
A sample-rate change or backward timestamp discontinuity resets the history.
The falloff presets do not affect these measures.

```hlsl
float brightness = lerp(0.2, 1.0, energy);
float3 band_color = float3(e_bass, e_mids, e_treb);
```

### Optional 64-band FFT spectrum

```hlsl
uniform texture2d spec; // Required opt-in; place before functions that use spectrum().

float4 mainImage(VertData v_in) : TARGET
{
    float level = spectrum(v_in.uv.x);
    return float4(level, level, level, 1.0);
}
```

Select an **Audio source** in the filter properties. The four existing time-domain
bands remain available independently. `spec` is a 64 × 4 single-channel **R32F**
texture, with low frequencies on the left and high frequencies on the right.
`spectrum(float pitch)` returns the instant spectrum in `[0,1]`, sampling at
`float2(saturate(pitch), 0.125)`. The two-argument form,
`spectrum(float pitch, float falloff)`, samples at
`float2(saturate(pitch), 0.125 + 0.75 * saturate(falloff))` and returns the red channel.
The sampler uses linear interpolation and clamp addressing. To read bin `i`
without interpolating its neighbors, use `spectrum((i + 0.5) / 64.0)`.

| Row | Texture y region | Row center | Stored value |
| --- | --- | --- | --- |
| 0 | 0–0.25 | 0.125 | Instant spectrum |
| 1 | 0.25–0.5 | 0.375 | 8192-sample Hann window (~171 ms at 48 kHz) |
| 2 | 0.5–0.75 | 0.625 | 32768-sample Hann window (~683 ms at 48 kHz) |
| 3 | 0.75–1 | 0.875 | Original long-window spectrum |

Falloff values 0, ⅓, ⅔, and 1 map to the four row centers; linear filtering blends
between them. `spectrum(pitch, 0.0)` is fully instant, `spectrum(pitch, 0.5)` is an equal
blend of the two intermediate windows, and `spectrum(pitch, 1.0)` is fully slow.
The middle rows use independent FFTs with windows 1/16 and 1/4 of the longest
window. Only sampling between rows interpolates their normalized levels.

Band edges are `20 * pow(1500, i / 64.0)` Hz for `i = 0..64`.
Every adjacent edge has ratio `pow(1500, 1/64)`, about 1.121. These are frequency
regions, not musical note names. Each texel's representative frequency is the
geometric mean of its lower and upper edges.

The instant endpoint uses PFFFT's SIMD real FFT with a **short Hann window**, sized to
20 ms rounded up to a power of two. At 48 kHz it uses **1024 audio samples
(21.33 ms)**, zero-padded to a 4096-point FFT. This gives 2049 unique frequency
samples including DC and Nyquist, spaced about 11.72 Hz apart. Padding makes
pooling into log regions denser; it does not improve the underlying frequency
resolution. Nearby bass regions will overlap because a short window cannot
distinguish tones only a few Hz apart.

The instant endpoint has **no attack/release smoothing, peak hold, or averaging
across FFT results**. Each audio callback schedules analysis of the newest window, even
before a complete window has arrived; startup history is zero-filled. Pending
callbacks are coalesced rather than queued. The renderer reads the newest result
at draw time. A volume change completely replaces previous audio within one
window (21.33 ms at 48 kHz), plus worker and display scheduling time. The separate
`energy` / `e_*` variables still measure their documented three-second history.

The slow endpoint restores the original Hann window: FFT size is the next power
of two giving at least four linear bins across the narrowest log region. At
48 kHz this is **131072 samples (2.73 seconds)**, with 65537 unique frequency
bins spaced about 0.366 Hz apart and roughly 1.37 seconds of window delay. It
updates after each additional 1/30 second of audio, along with both intermediate
windows. All four rows use the same frequency regions and normalization.

Each channel is transformed independently, then channel powers are averaged, so
opposite-phase stereo signals do not cancel. FFT powers are corrected for window
energy and weighted by the squared **A-weighting** magnitude at each frequency
before pooling into the 64 regions. Fractional overlap distributes bins crossing
a region boundary. This is a hearing-sensitivity approximation, not a calibrated
sound-pressure or LUFS meter. The normalized output is
`saturate((10 * log10(weighted_mean_square) + 80) / 80)`; power at or below
`1e-8` maps to zero and power at or above 1 maps to one.
See the [PFFFT API](https://bitbucket.org/jpommier/pffft/) and
[A-weighting overview](https://www.mathworks.com/help/audio/ref/weightingfilter-system-object.html).

The frequency axis always ends at 30000 Hz. Entire regions above the source's
Nyquist frequency are **zero**; a region crossing Nyquist includes only its
available frequencies. At 48 kHz, input ends at 24 kHz. To capture the complete
requested range, use a sample rate of at least 60 kHz (normally 96 kHz).

Only an active, scalar `uniform texture2d spec;` declaration enables this feature.
OBS preprocessing is applied first: comments, strings, disabled `#ifdef` branches,
other names, other types, and arrays do not opt in. Macro-expanded declarations
are supported. OBS does not support general `#if` expressions; use `#ifdef` /
`#ifndef`. Without the declaration the helper is absent, and calling it causes a
shader compilation error. Declaring it enables analysis even if the shader does
not sample it; remove the declaration to disable the feature.

Each subscribed audio source owns one worker shared by all its `spec` filters.
FFT plans, aligned scratch storage, all four Hann windows, and bin weights are cached.
Audio callbacks copy once into a shared bounded ring; the short transform reads
only its newest samples. Transforms run outside the capture and snapshot locks,
and the three longer windows are transformed together at the slower cadence.
Removing the last `spec` subscription joins the worker and frees
its buffers. Mute clears the spectrum immediately; missing callbacks clear the
visible snapshot after 50 ms. Timestamp discontinuities and audio-format changes
reset the window. A filter allocates its spectrum texture only on first render,
uploads it when its revision changes, and frees it on shader reload/destruction.

The supplied [spectrum-bars.shader](data/examples/spectrum-bars.shader) displays
all 64 regions. PFFFT is installed through the pinned [vcpkg manifest](vcpkg.json)
and linked by CMake as `pffft::pffft`; see [build instructions](SHADER_HELPERS.md#building-and-checking).

### Audio declaration and runtime cost in this implementation

`shader_text_uses_audio` uses **substring matching** on the user's shader text
(or loaded file text), not shader dependency analysis. Audio names in comments
can activate it. It looks for band/texture/preset names and strings such as
`acc(`, `ts(`, `expfoff(`, and `linfoff(`; spacing or macro indirection can
affect detection. The settings' shader text is preferred when nonempty, before a
file is read. These are current implementation details, not a zero-cost guarantee.
The optional `spec` input instead uses the explicit declaration check above and
effect parameter reflection; mentioning `spectrum` in a comment does not run FFTs.

A matching shader with a valid audio source attaches a shared analysis callback.
Multiple filters on the same audio source share band analysis, integrals, presets,
energy, and history. The last consumer releasing the source destroys that analysis.
The selected source falls back to the filter's audio-capable parent if unavailable
or empty; the properties label “None” does not suppress that parent fallback.

The current render path creates/uploads **all four history textures when audio
text is detected**, even if only `bass` is used. Per-filter CPU history buffers
are allocated at creation and copied/cleared during video ticks. Each audio
callback computes all bands, both falloff families, energy, and history together.
The optional FFT runs on its own worker only while a `spec` consumer is attached.
Using another preset does not create another audio capture, but its uniform and
shader work still have costs. Without usable analysis the live audio snapshot
is zero.

### Exponential and linear falloff variables

All are scalar `float` values in `[0,1]` and already in the preamble.
There are **four presets per band per family**, not 64-entry arrays/textures.
The full variable names are listed below.

These are **preset carriers** for `expfoff(tails)` and `linfoff(tails)`, rather
than an arbitrary-tail interface themselves. Direct reads remain useful and
efficient for a fixed preset. The functions interpolate through the current
band at tail 0, then fast at 0.25, medium at 0.5, slow at 0.75, and slug at 1.

| Name | Type | Declaration | Preset | Declaration/use overhead |
| --- | --- | --- | --- | --- |
| `ef_bass_fast` | `float` | Preamble | Exponential bass, fast | Shared Audio computation + scalar assignment. |
| `ef_bass_medium` | `float` | Preamble | Exponential bass, medium | Shared Audio computation + scalar assignment. |
| `ef_bass_slow` | `float` | Preamble | Exponential bass, slow | Shared Audio computation + scalar assignment. |
| `ef_bass_slug` | `float` | Preamble | Exponential bass, slug | Shared Audio computation + scalar assignment. |
| `ef_lows_fast` | `float` | Preamble | Exponential lows, fast | Shared Audio computation + scalar assignment. |
| `ef_lows_medium` | `float` | Preamble | Exponential lows, medium | Shared Audio computation + scalar assignment. |
| `ef_lows_slow` | `float` | Preamble | Exponential lows, slow | Shared Audio computation + scalar assignment. |
| `ef_lows_slug` | `float` | Preamble | Exponential lows, slug | Shared Audio computation + scalar assignment. |
| `ef_mids_fast` | `float` | Preamble | Exponential mids, fast | Shared Audio computation + scalar assignment. |
| `ef_mids_medium` | `float` | Preamble | Exponential mids, medium | Shared Audio computation + scalar assignment. |
| `ef_mids_slow` | `float` | Preamble | Exponential mids, slow | Shared Audio computation + scalar assignment. |
| `ef_mids_slug` | `float` | Preamble | Exponential mids, slug | Shared Audio computation + scalar assignment. |
| `ef_treb_fast` | `float` | Preamble | Exponential treb, fast | Shared Audio computation + scalar assignment. |
| `ef_treb_medium` | `float` | Preamble | Exponential treb, medium | Shared Audio computation + scalar assignment. |
| `ef_treb_slow` | `float` | Preamble | Exponential treb, slow | Shared Audio computation + scalar assignment. |
| `ef_treb_slug` | `float` | Preamble | Exponential treb, slug | Shared Audio computation + scalar assignment. |
| `lf_bass_fast` | `float` | Preamble | Linear bass, fast | Shared Audio computation + scalar assignment. |
| `lf_bass_medium` | `float` | Preamble | Linear bass, medium | Shared Audio computation + scalar assignment. |
| `lf_bass_slow` | `float` | Preamble | Linear bass, slow | Shared Audio computation + scalar assignment. |
| `lf_bass_slug` | `float` | Preamble | Linear bass, slug | Shared Audio computation + scalar assignment. |
| `lf_lows_fast` | `float` | Preamble | Linear lows, fast | Shared Audio computation + scalar assignment. |
| `lf_lows_medium` | `float` | Preamble | Linear lows, medium | Shared Audio computation + scalar assignment. |
| `lf_lows_slow` | `float` | Preamble | Linear lows, slow | Shared Audio computation + scalar assignment. |
| `lf_lows_slug` | `float` | Preamble | Linear lows, slug | Shared Audio computation + scalar assignment. |
| `lf_mids_fast` | `float` | Preamble | Linear mids, fast | Shared Audio computation + scalar assignment. |
| `lf_mids_medium` | `float` | Preamble | Linear mids, medium | Shared Audio computation + scalar assignment. |
| `lf_mids_slow` | `float` | Preamble | Linear mids, slow | Shared Audio computation + scalar assignment. |
| `lf_mids_slug` | `float` | Preamble | Linear mids, slug | Shared Audio computation + scalar assignment. |
| `lf_treb_fast` | `float` | Preamble | Linear treb, fast | Shared Audio computation + scalar assignment. |
| `lf_treb_medium` | `float` | Preamble | Linear treb, medium | Shared Audio computation + scalar assignment. |
| `lf_treb_slow` | `float` | Preamble | Linear treb, slow | Shared Audio computation + scalar assignment. |
| `lf_treb_slug` | `float` | Preamble | Linear treb, slug | Shared Audio computation + scalar assignment. |

Fast/medium/slow/slug tail durations are **0.12 / 0.4 / 1.0 / 2.7307 seconds**.
For duration `T` and elapsed audio time `dt`, the independently stored presets use:

- Linear: `max(previous - dt / T, current)`. A constant downward slope in the
  normalized shader value; full scale reaches zero after `T` seconds of silence.
- Exponential: `max(previous * exp(-log(100) * dt / T), current)`. It reaches 1%
  of its starting value after `T` seconds, then continues approaching zero.

Both follow rises immediately, and both interpolate to the exact unsmoothed
base band at tail 0. All four presets are stored separately for every band.
The longest duration is comparable to the 2.73-second spectrum window, but an
envelope has a different response from a Hann-window FFT. Intermediate helper
arguments interpolate preset values. Timing uses full audio block durations,
without a global multiplier or the former 100 ms time cap. Missing callbacks
continue the decay from the last capture when values are read; resuming capture
accounts for that elapsed gap without resurrecting an old peak.

### Audio helpers

| Signature | Return type | Declaration | Meaning | Use overhead |
| --- | --- | --- | --- | --- |
| `acc(float rate)` | `float4` | Preamble | `frac(float4(bass_c,lows_c,mids_c,treb_c) / pow(166320, saturate(1-rate)))`. | Audio plus GPU power/division/fraction. |
| `cu_bass(float speed)` | `float` | Preamble | `acc(speed).x`. | Same phase calculation. |
| `cu_lows(float speed)` | `float` | Preamble | `acc(speed).y`. | Same phase calculation. |
| `cu_mids(float speed)` | `float` | Preamble | `acc(speed).z`. | Same phase calculation. |
| `cu_treb(float speed)` | `float` | Preamble | `acc(speed).w`. | Same phase calculation. |
| `ts(float x)` | `float4` | Preamble | Sample all four history textures at normalized coordinate `(x,0.5)`. Order: bass, lows, mids, treb. | Audio plus four texture reads. |
| `expfoff(float4 tailLengths)` | `float4` | Preamble | Per-band interpolation across current/fast/medium/slow/slug values, clamping tails to `[0,1]`. | Audio plus four interpolations/branches. |
| `expfoff(float tailLength)` | `float4` | Preamble (scalar/vector macro wrapper) | Same tail for all bands; the wrapper broadcasts scalars to all four components. | Same as vector form. |
| `linfoff(float4 tailLengths)` | `float4` | Preamble | As above, using linear presets. | Audio plus four interpolations/branches. |
| `linfoff(float tailLength)` | `float4` | Preamble (scalar/vector macro wrapper) | Same tail for all bands; scalar and vector arguments work on every backend. | Same as vector form. |
| `interp_falloff5(float t, float p0, float p1, float p2, float p3, float p4)` | `float` | Preamble, implementation helper | Piecewise interpolation through five anchors at t = 0, .25, .5, .75, 1. | GPU arithmetic/branches. |

`expfoff_multi` and `linfoff_multi` **do not exist** in this implementation.
Pass a `float4` to the names above for independent per-band tails.
`ts(128)` is not a history-index lookup: `ts` takes normalized texture
coordinates. To sample history texel `i`, use `ts((i + 0.5) / 512.0)`.

## Controller inputs

**Tools → Shaderfilter Controller Lookup** shows tabs labeled by connected
controller slot. The selected controller's page scrolls through all 15 buttons
and 6 axes. It shows live button states, full native press counts, normalized raw
axes, integrated phases, the two stick XY pairs, and the last pressed button.
Selection survives other devices connecting/disconnecting. Each controller's
scroll position is remembered while the window exists. Display updates at 20 Hz
only while visible; input acquisition remains the shared 8 ms timer.

Controller slots are **0–15**, not a compact device count. Only mapped gamepads
appear; unmapped joysticks are logged but have no standardized button/axis view.
Slots may be reused by different hardware.

All instantaneous axes are finite and bounded to **[-1,1]** in the shared CPU
state before either UI or shader access. NaN/infinite input becomes zero.
Left XY = axes (0,1); right XY = (2,3). Each pair is in **[-1,1] × [-1,1]**;
the vector is not normalized to unit length (which would discard stick magnitude).
Triggers 4 and 5 also use [-1,1], with -1 at rest and +1 fully pressed.
Deadzone removal is separate from raw axis access.

| Shader name / signature | Type / return type | Declaration | Meaning | Declaration/use overhead |
| --- | --- | --- | --- | --- |
| `gamepad_axis_clamped(int controller, int axis)` | `float` | Gamepad preamble | Position in [0,1]; full deflection traverses the range in two seconds. | Vector lookup / one fetch. |
| `gamepad_available` | `int` | Gamepad preamble | 1 when GLFW initialization succeeded; not a controller count. | Gamepad snapshot/binding. |
| `gamepad_connected(int controller)` | `bool` | Gamepad preamble | Whether that mapped slot is connected. | D3D vector lookup; OpenGL one texture fetch. |
| `gamepad_button(int controller, int button)` | `int` | Gamepad preamble | 0 released, 1 held. | Vector lookup / one fetch. |
| `gamepad_last_button(int controller)` | `int` | Gamepad preamble | Most recent sampled rising-edge button ID, initially -1. | Vector lookup / one fetch. |
| `gamepad_press_count(int controller, int button)` | `int` | Gamepad preamble | Cumulative rising-edge presses, saturating at 2147483647. | Vector lookup / two fetches to reconstruct exact int32. |
| `gamepad_axis(int controller, int axis)` | `float` | Gamepad preamble | Latest normalized raw signed axis, before deadzone. | Vector lookup / one fetch. |
| `gamepad_axis_deadzone(int controller, int axis, float deadzone)` | `float` | Gamepad preamble | Removes center deadzone and rescales remaining signed magnitude; deadzone clamped to [0,1]. | Axis access plus arithmetic. |
| `gamepad_axis_cumulative(int controller, int axis)` | `float` | Gamepad preamble | Integrated phase, already in [0,1). | Vector lookup / one fetch. |
| `gamepad_axis_interval(int controller, int axis, float interval)` | `float` | Gamepad preamble | `frac(phase * 166320 / interval)`; valid intervals (0,166320], otherwise zero. | Phase access plus arithmetic. |
| `gamepad_axis_acc(int controller, int axis, float rate)` | `float` | Gamepad preamble | Audio-style rate mapped to `pow(166320, saturate(1-rate))` interval. | Phase access plus power/arithmetic. |

Build a stick vector with `float2(gamepad_axis(c,0), gamepad_axis(c,1))`.
There is no separately declared stick-vector uniform.

| Button ID | Name | Axis ID | Name |
| --- | --- | --- | --- |
| 0 | A / Cross | 0 | Left stick X |
| 1 | B / Circle | 1 | Left stick Y |
| 2 | X / Square | 2 | Right stick X |
| 3 | Y / Triangle | 3 | Right stick Y |
| 4 | Left bumper | 4 | Left trigger |
| 5 | Right bumper | 5 | Right trigger |
| 6 | Back | — | — |
| 7 | Start | — | — |
| 8 | Guide | — | — |
| 9 | Left stick click | — | — |
| 10 | Right stick click | — | — |
| 11 | D-pad up | — | — |
| 12 | D-pad right | — | — |
| 13 | D-pad down | — | — |
| 14 | D-pad left | — | — |

Both integrators use signed stick velocity and a 0.15 deadzone. Triggers are converted
to [0,1] **for integration only**, so their resting -1 does not drift. A trigger
must report a nonzero raw value once before integration is enabled, preventing
absent GLFW trigger mappings (zero) from drifting. A real trigger connected at
exact midpoint waits until it moves. For the wrapping helpers, full deflection
contributes 100/166320 phase units per second, with monotonic elapsed time capped
at 100 ms per poll. Negative integration wraps into [0,1).

`gamepad_axis_clamped(c,a)` holds a position in the inclusive range `[0,1]`.
Stick axes 0–3 start at `0.5`; trigger axes 4–5 start at `0`. Full deflection
moves at 0.5 units per second: exactly two seconds from one endpoint to the
other (one second from the initial stick midpoint to either endpoint).
Partial deflection moves proportionally after deadzone removal. Releasing the
stick holds the position; reversing direction moves it back immediately on the
next sampled interval. Values stop at the endpoints without wrapping or storing
excess movement. Triggers move only upward while pressed and hold at rest.
Clamped integration uses the full elapsed time between polls, including delayed
polls. Positions are shared by all filters, survive disconnect/reconnect, and
reset on plugin unload. Invalid indices return zero.

```hlsl
float2 position = float2(gamepad_axis_clamped(0, 0), gamepad_axis_clamped(0, 1));
```

Choose an interval dividing **166320** (such as 1, 2, 3, 4, 5, 6, 8, 10, 12,
60, 120, 360) for seamless wrap. Other intervals can jump at the underlying wrap.

Holding a button does not increment its count. Buttons held at connection form
a baseline rather than a new press. If several rise in one poll, the highest
button ID becomes “last.” Axis changes never replace the last-button ID.
Disconnect clears live input but retains slot history until plugin unload.
Invalid indices return zero/false, or -1 for the last-button helper.
Brief complete press/release cycles between polls may be missed.

### Controller backing uniforms

These are reserved implementation data. Prefer the cross-backend functions
above. They are listed for completeness and for full-effect authors.

| Name | Type | Declaration in normal shader | Contents / overhead |
| --- | --- | --- | --- |
| `gamepad_axes_clamped` | `float4[32]` | Gamepad preamble, D3D only | Two packed vectors per slot, 6 clamped positions. |
| `gamepad_info` | `int4[16]` | Gamepad preamble, D3D only | Per slot: connected, last button, 0, 0. Uploaded per effect parameter update. |
| `gamepad_buttons` | `int4[64]` | Gamepad preamble, D3D only | Four packed vectors per slot, 15 button states. |
| `gamepad_presses` | `int4[64]` | Gamepad preamble, D3D only | Packed press counts, saturated to signed int32. |
| `gamepad_axes` | `float4[32]` | Gamepad preamble, D3D only | Two packed vectors per slot, 6 normalized signed axes. |
| `gamepad_axes_c` | `float4[32]` | Gamepad preamble, D3D only | Two packed vectors per slot, 6 normalized integrated phases. |
| `gamepad_data` | `texture2d` | Gamepad preamble, OpenGL only | Shared 19 × 16 RGBA32F texture; at most one upload per OBS video frame. Allocated on first use and retained to module unload. |
| `gamepad_sampler` | `sampler_state` | Gamepad preamble, OpenGL only | Point filtering and clamp addressing. |
| `sf_gamepad_fetch(int controller, int column)` | returns `float4` | Gamepad preamble, OpenGL only | Internal texture lookup; public accessors perform index validation. |

Direct3D packed index is `controller * vectors_per_slot + item/4`, component
`item%4`. OpenGL texture columns: 0 info; 1–4 buttons; 5–8 low 16 bits of
counts; 9–12 high bits; 13–14 raw axes; 15–16 cumulative axes; 17–18 clamped axes.
Each texture row is one controller slot.

Declaring these yourself in a full effect triggers the corresponding
snapshot/upload path when present; it does not start a separate GLFW poller.
Only OpenGL uses the texture path in the generated gamepad preamble.

## Color conversion helpers

All functions below are supplied by the preamble; **do not declare them**.
They have no capture, texture, or CPU integration overhead. Used calls add GPU
arithmetic; unused helper definitions add parsing/compilation text, not a running
pixel computation.

| Function | Argument → return type | Declaration | Interpretation / use overhead |
| --- | --- | --- | --- |
| `rgb2hsv` | `float3 → float3` | Preamble | RGB to HSV; min/max, divisions and branches. |
| `hsv2rgb` | `float3 → float3` | Preamble | HSV to RGB; hue-sector arithmetic. |
| `rgb2hsl` | `float3 → float3` | Preamble | RGB to HSL through HSV. |
| `hsl2rgb` | `float3 → float3` | Preamble | HSL to RGB through HSV. |
| `hsv2hsl` | `float3 → float3` | Preamble | Direct saturation/lightness conversion. |
| `hsl2hsv` | `float3 → float3` | Preamble | Direct saturation/value conversion. |
| `rgb2oklch` | `float3 → float3` | Preamble | Nonlinear sRGB to OKLCH; transfer curves, matrices, cube root and atan2. |
| `oklch2rgb` | `float3 → float3` | Preamble | OKLCH to nonlinear sRGB; trig, matrices and transfer curves. |
| `linrgb2oklch` | `float3 → float3` | Preamble | Linear-light sRGB to OKLCH; skips the transfer curve. |
| `oklch2linrgb` | `float3 → float3` | Preamble | OKLCH to linear-light sRGB; skips the transfer curve. |
| `hsv2oklch` | `float3 → float3` | Preamble | HSV → nonlinear RGB → OKLCH. |
| `oklch2hsv` | `float3 → float3` | Preamble | OKLCH → nonlinear RGB, clipped to [0,1] → HSV. |
| `hsl2oklch` | `float3 → float3` | Preamble | HSL → nonlinear RGB → OKLCH. |
| `oklch2hsl` | `float3 → float3` | Preamble | OKLCH → nonlinear RGB, clipped to [0,1] → HSL. |
| `srgb_nonlinear_to_linear(float3 v)` | `float3 → float3` | Preamble | Legacy sRGB transfer helper, used by the default transition. |
| `srgb_nonlinear_to_linear_channel(float u)` | `float → float` | Preamble | Scalar legacy transfer curve. |

HSV/HSL order is (hue, saturation, value/lightness), with normalized hue turns.
RGB/HSV/HSL conventions expect bounded RGB and do not implicitly linearize it.
HSL is not perceptually uniform. OKLCH order is **(L, C, h)**, with native Oklab
chroma and hue in turns, not degrees. Convert CSS degrees with `h/360`.
OKLCH uses D65 sRGB primaries; alpha/premultiplication is not handled.

RGB↔OKLCH preserves out-of-gamut RGB, including negative channels, using signed
extended transfer curves. HSV/HSL output wrappers clip RGB first, so out-of-gamut
round trips through them lose information. Negative OKLCH chroma is treated as
zero; neutral chroma ≤1e-6 returns C=0 and hue=0.
The two legacy `srgb_nonlinear_to_linear*` helpers do not use that extended
signed negative-channel rule.

## Four-dimensional noise

All are in the preamble, with no CPU state, random seed, or texture allocation.
All four components of `p` are continuous coordinates; `w` can represent time.

| Signature | Return type | Declaration | Result / use overhead |
| --- | --- | --- | --- |
| `simplex_noise(float4 p)` | `float` | Preamble | Signed noise, approximately [-1,1]; five simplex corners. |
| `perlin_noise(float4 p)` | `float` | Preamble | Signed gradient noise; 16 corners with quintic interpolation. |
| `perlin_noise_fbm(float4 p, int octaves, float frequency, float lacunarity, float persistence)` | `float` | Preamble | Amplitude-normalized fractal sum; 1–8 Perlin evaluations. |
| `curl_noise(float4 p)` | `float3` | Preamble | Spatial curl; three analytic simplex-derivative evaluations. Deliberately not unit-normalized; magnitude is not bounded to [0,1]. |
| `voronoi_noise(float4 p)` | `float` | Preamble | Euclidean F1 distance; 81 cells, one square root. Feature jitter is bounded to each cell's middle third. |

Perlin fBm clamps octaves to 1–8, frequency/lacunarity to at least 0.0001,
and persistence to [0,1]. Use `perlin_noise_fbm(p,4,1.0,2.0,0.5)` for a typical
fractal field. The configurable function is **not** an overload of `perlin_noise`.
Noise lattice hashing repeats after 289 lattice cells (skewed for simplex);
very large coordinates lose float precision.

Internal preamble functions also exist; they are implementation helpers, not
additional uniforms or state:

| Signature | Return type | Declaration | Cost when called |
| --- | --- | --- | --- |
| `sf_oklch_decode_srgb(float channel)` | `float` | Preamble | Signed sRGB decoding. |
| `sf_oklch_encode_srgb(float channel)` | `float` | Preamble | Signed sRGB encoding. |
| `sf_noise_permute(float x)` | `float` | Preamble | Arithmetic lattice permutation. |
| `sf_noise_hash(float4 cell)` | `float` | Preamble | Nested permutations. |
| `sf_noise_gradient(float4 cell)` | `float4` | Preamble | Gradient hashing/selection. |
| `sf_simplex_corner(float4 cell, float4 offset)` | `float4` | Preamble | Analytic corner contribution: xyz derivative, w scalar. |
| `sf_simplex_derivative(float4 p)` | `float4` | Preamble | Five-corner analytic derivative/value sum. |
| `sf_voronoi_point(float4 cell)` | `float4` | Preamble | Hash-derived point offset within the cell. |

## Custom uniforms and UI annotations

Unrecognized uniform names become user parameters. These are not predefined
application variables; choose names that do not collide with reserved names above.

| Uniform type | Declaration | UI behavior | Does declaring it introduce overhead? |
| --- | --- | --- | --- |
| `bool` | User-defined | Checkbox. | Settings/property storage and per-render assignment. |
| `int` | User-defined | Number, slider, or select list. | Settings/property storage and per-render assignment. |
| `float` | User-defined | Number or slider. | Settings/property storage and per-render assignment. |
| `float2` | User-defined | Two numeric fields/sliders. | Settings/property storage and vector assignment. |
| `float3` | User-defined | RGB color by default, or three component sliders. | Settings/property storage and vector assignment. |
| `float4` | User-defined | RGBA color by default, or four component sliders. | Settings/property storage and vector assignment. |
| `texture2d` | User-defined | Source/scene selector and/or image file picker. | Yes; selected source may stay active/showing and is rendered to its own target. Selected image is loaded/allocated. |
| `string` | User-defined | Multiline text or info text. | CPU settings/string handling; not a native GPU string value for shader math. |

Other shader types such as arbitrary custom matrices and integer vectors are not
fully handled by this plugin's custom-property/update switch. Built-in reserved
types such as `ViewProj` and packed gamepad arrays follow their own paths.

Annotations are placed **inside the uniform's angle brackets**, not declared
as independent application uniforms. They mainly affect load/settings/UI work,
not per-pixel arithmetic.

| Annotation key | Type | Declaration | Effect / overhead |
| --- | --- | --- | --- |
| `name` | `string` | Optional annotation | Property display label; metadata/UI work. |
| `label` | `string` | Optional annotation | Same display-label field as `name`; later parsed value wins. |
| `widget_type` | `string` | Optional annotation | Recognized values: `slider`, `select` (int), `source`/`file` (texture), `info` (string). UI work; selected texture sources/files have costs above. |
| `group` | `string` | Optional annotation | Groups properties by name. |
| `minimum` | `int` or `float` | Optional annotation | UI lower bound. Use scalar float for float/vector controls, int for int controls. Not an automatic shader clamp. |
| `maximum` | `int` or `float` | Optional annotation | UI upper bound; not an automatic shader clamp. |
| `step` | `int` or `float` | Optional annotation | UI increment. |
| `option_N` | `int` | Optional annotation | Integer value for select option N; use sequential zero-based entries. |
| `option_N` | `string` | Optional annotation | Select label or vector component label N. Pair int/string entries for select controls. |

Custom scalar numeric ranges default to -1000…1000 when minimum equals maximum,
with step 1 for ints and .0001 for floats. An initializer after the annotation
block sets the default value, for example:

```hlsl
uniform float amount <
    string label = "Amount";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 1.0;
    float step = 0.01;
> = 0.5;
```

## Preprocessor controls

| Name | Type | Declaration | Effect / overhead |
| --- | --- | --- | --- |
| `USE_PM_ALPHA` | preprocessor define | Optional: `#define USE_PM_ALPHA 1` | Changes the filter's input/output alpha handling to premultiplied-alpha behavior. No uniform upload. Detection uses the literal define text in the assembled effect. |
| `OPENGL` | preprocessor define | Automatically defined as 1 on OpenGL | Selects backend-specific code; absent on D3D. No runtime uniform. |
| `#include "path"` | file-loader directive | Optional in loaded shader files | Loads/inlines another shader text file relative to the including file; I/O at load/reload. |

Built-in function names, annotations, and controller indices are not uniforms.
Imported Shadertoy names such as `iTime`, `iResolution`, `iMouse`, and
`iChannel0` are not automatically live inputs by those names. The **Convert
Shader** action rewrites supported patterns; notably, its generated `iMouse`
is a manual slider parameter, not live mouse input.

## Application settings (not shader uniforms)

These live in OBS settings or the plugin configuration. Declaring a same-named
shader uniform does not expose the application's setting as an input.

| Key | Type | Where / declaration | Meaning / cost |
| --- | --- | --- | --- |
| `expand_left`, `expand_right`, `expand_top`, `expand_bottom` | integer pixels | Filter properties; not shader declarations | Increase render dimensions, changing texture memory and pixel workload. |
| `from_file` | bool | Filter/transition setting | Loads shader text from a file. |
| `shader_file_name` | string path | Filter/transition setting | File used when `from_file` is true; file I/O on load/reload. |
| `shader_text` | string | Filter/transition setting | Pasted shader text / default shader. Compilation on reload. |
| `override_entire_effect` | bool | File-loading setting | Disables the normal shader template for a loaded complete effect. |
| `audio_source` | string source name | Filter/transition setting | Audio capture source, with parent fallback described above. |
| `last_error` | string | Generated settings/UI field | Last shader parser/compiler error; not a shader input. |
| `<uniform>_source` | string source name | Generated custom texture setting | Source selected for a custom texture uniform; can activate/render another source. |
| `<uniform>_0` … `<uniform>_3` | number | Generated vector-component settings | Numeric components for applicable vector controls. |

`reload_effect` and `shader_convert` are property-button identifiers;
`plugin_info` is a UI info field, not an input.
There is no global audio release setting. Controller count is available through the gamepad helpers.
The existing C struct's `no_repeat` and old locale labels are not implemented
shader inputs/settings in the reviewed paths.

## Native C/C++ access

These are **not callable inside a shader**. Include [shader-gamepad.h](shader-gamepad.h)
or [shader-tools.h](shader-tools.h); declarations are already in those headers.

| Native function | Return / output | Cost / meaning |
| --- | --- | --- |
| `sf_gamepad_axis_clamped(c,a)` | float | Cached clamped position in [0,1]; invalid indices return zero. |
| `sf_gamepad_init()`, `sf_gamepad_shutdown()` | bool / void | UI-thread GLFW/timer lifecycle; init reports success. |
| `sf_gamepad_available()`, `sf_gamepad_connected(c)` | bool | Cached status under the shared mutex. |
| `sf_gamepad_button(c,b)` | bool | Cached button state under the mutex. |
| `sf_gamepad_axis(c,a)`, `sf_gamepad_axis_deadzone(c,a,d)` | float | Normalized cached signed axis; optional deadzone calculation. |
| `sf_gamepad_last_button(c)` | int | Last sampled button ID; -1 initially/invalid. |
| `sf_gamepad_press_count(c,b)` | uint64_t | Full native count, saturating at UINT64_MAX. |
| `sf_gamepad_axis_cumulative(c,a)` | float | Normalized cached phase, rounded down if float conversion would reach 1. |
| `sf_gamepad_axis_interval(c,a,interval)`, `sf_gamepad_axis_acc(c,a,rate)` | float | Native phase helpers; invalid/nonfinite interval/rate returns zero. |
| `sf_gamepad_get_snapshot(out)` | `sf_gamepad_snapshot` via pointer | One synchronized packed shader snapshot; press counts narrowed to saturated int32. |
| `sf_gamepad_get_controller_state(c,out)` | `sf_gamepad_controller_state` via pointer | One synchronized native controller snapshot for the inspector; full uint64 counts, double phases, and double clamped positions. Invalid slot yields neutral values and last_button=-1. |
| `sf_gamepad_pack_snapshot(snapshot,pixels)` | void | CPU conversion into the 19×16 OpenGL texture layout; caller supplies the buffer. |
| `sf_tools_init()`, `sf_tools_shutdown()` | void | Controller menu/window lifecycle on the UI thread. |

The native controller snapshot fields are `connected: bool`, `last_button: int`,
`buttons: bool[15]`, `presses: uint64_t[15]`, `axes: float[6]`,
`axes_c: double[6]`, and `axes_clamped: double[6]`.
These have the same slot/button/axis semantics as the shader
interface. Inspector counts can therefore exceed the shader's int32 limit.

Header constants: `SF_GAMEPAD_COUNT=16`, `SF_GAMEPAD_BUTTON_COUNT=15`,
`SF_GAMEPAD_AXIS_COUNT=6`, `SF_GAMEPAD_TEXTURE_WIDTH=19`,
`SF_GAMEPAD_COUNTER_WRAP=166320.0`,
`SF_GAMEPAD_INTEGRATION_DEADZONE=0.15f`. They are C/C++ macros, not injected
shader constants.

## Minimal usage

```hlsl
// Optional calendar input: this one needs a declaration.
uniform int current_time_hour;

float4 mainImage(VertData v_in) : TARGET
{
    float4 color = image.Sample(textureSampler, v_in.uv);
    // No declarations needed for the gamepad helpers or rgb2oklch/oklch2rgb.
    int controller = 0; // Use the slot shown in Tools → Shaderfilter Controller Lookup.
    if (gamepad_connected(controller)) {
        float2 stick = float2(gamepad_axis(controller, 0), gamepad_axis(controller, 1));
        float3 lch = rgb2oklch(color.rgb);
        lch.z = frac(lch.z + stick.x * 0.1);
        color.rgb = saturate(oklch2rgb(lch));
    }
    return color;
}
```
