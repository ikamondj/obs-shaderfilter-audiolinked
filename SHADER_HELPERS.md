# Color, noise, and gamepad helpers

See [VARIABLE_REFERENCE.md](VARIABLE_REFERENCE.md) for the comprehensive, code-reviewed
list of shader inputs, types, declaration requirements, and runtime costs.

These functions are appended to the normal `.shader` preamble. Their editable
sources are `data/internal/shader-helpers.effect` and `data/internal/gamepad.effect`;
CMake embeds them into the plugin, so deployment does not depend on reading these
files at runtime. Gamepad declarations are included when the shader uses a
`gamepad_` identifier. Complete `.effect` overrides must supply their own helper
definitions/uniforms, as with the existing audio helpers.

## Color

The RGB/HSV/HSL helpers use normalized units; hue wraps at 1. Their RGB input is
expected in `[0,1]`, normally nonlinear sRGB, with no implicit transfer-function conversion.
HSL is the conventional hue/saturation/lightness model, not a perceptually uniform
space. Its lightness is not luminance.

| Function | Conversion |
| --- | --- |
| `float3 rgb2hsl(float3 rgb)` | RGB to HSL |
| `float3 hsl2rgb(float3 hsl)` | HSL to RGB |
| `float3 hsv2hsl(float3 hsv)` | HSV to HSL directly |
| `float3 hsl2hsv(float3 hsl)` | HSL to HSV directly |

Black, white, and grayscale avoid division by zero. The existing `rgb2hsv` and
`hsv2rgb` retain their behavior (the hue-sector integer cast now also compiles
on OpenGL). Hue/saturation cannot be recovered from achromatic RGB.

### OKLCH

All OKLCH functions take and return `float3`. OKLCH component order is **L, C, h**:
lightness normally in `[0,1]`, nonnegative chroma in native Oklab units (not
normalized saturation), and hue in `[0,1)` turns, consistent with the HSV/HSL
helpers. For a CSS hue in degrees, divide by 360. The supported chroma range
depends on lightness and hue; arbitrary OKLCH values can lie outside sRGB.

| To OKLCH | From OKLCH | RGB interpretation |
| --- | --- | --- |
| `rgb2oklch(float3 rgb)` | `oklch2rgb(float3 lch)` | Nonlinear sRGB; transfer curve handled internally |
| `linrgb2oklch(float3 rgb)` | `oklch2linrgb(float3 lch)` | Linear-light sRGB; no transfer curve |
| `hsv2oklch(float3 hsv)` | `oklch2hsv(float3 lch)` | Conventional HSV over nonlinear sRGB |
| `hsl2oklch(float3 hsl)` | `oklch2hsl(float3 lch)` | Conventional HSL over nonlinear sRGB |

The implementation uses [Ottosson's Oklab matrices](https://bottosson.github.io/posts/oklab/)
and the [extended sRGB transfer curves](https://www.w3.org/TR/css-color-4/#color-conversion-code).
RGB/linear-RGB conversions preserve out-of-gamut values (including negative
channels); use `saturate(oklch2rgb(lch))` when a clipped display color is wanted.
The HSV/HSL output wrappers clip RGB to `[0,1]` before conversion to keep those
representations bounded. This is simple channel clipping, not perceptual gamut
mapping, so out-of-gamut HSV/HSL round trips are lossy.

Hue wraps on input. Negative chroma is treated as zero. Neutral colors with
chroma at or below `1e-6` return chroma and hue as zero, avoiding unstable hue
from floating-point matrix roundoff. No alpha/premultiplication is handled by
these `float3` helpers; pass straight RGB.

```hlsl
float3 lch = rgb2oklch(color.rgb);
lch.z = frac(lch.z + 0.1); // Rotate perceived hue by 36 degrees
color.rgb = saturate(oklch2rgb(lch));
```

## Four-dimensional noise

All four input components are continuous coordinates. For example,
`float4(v_in.uv * 8.0, 0.0, elapsed_time * 0.2)` animates a 2D slice of a 4D field.
The functions are deterministic and need no textures, CPU state, or random seed.
Translate the input coordinates to obtain a different region of the field.

| Function | Result and cost |
| --- | --- |
| `float simplex_noise(float4 p)` | Signed gradient noise, approximately `[-1,1]`; five simplex corners |
| `float perlin_noise(float4 p)` | Signed gradient noise; 16 corners with quintic interpolation |
| `float perlin_noise_fbm(float4 p, int octaves, float frequency, float lacunarity, float persistence)` | Amplitude-normalized fractal Perlin sum |
| `float3 curl_noise(float4 p)` | Spatial curl of three scalar potentials; `.w` animates the field |
| `float voronoi_noise(float4 p)` | Nonnegative Euclidean distance to the nearest feature point |

The Perlin controls are octave count (clamped to 1–8), starting frequency
(minimum `0.0001`), frequency multiplier per octave (minimum `0.0001`), and
amplitude multiplier per octave (clamped to `[0,1]`). A typical invocation is
`perlin_noise_fbm(p, 4, 1.0, 2.0, 0.5)`. Use the single-argument function for the
lowest cost. Very large input coordinates/frequencies lose precision in floats.

Curl uses analytic derivatives: three noise evaluations instead of 18 scalar
evaluations for central differences. The result is deliberately not normalized,
which preserves its divergence-free property. Its magnitude is not confined to
`[0,1]`.

Voronoi visits 81 cells and takes one square root. Feature points occupy the
middle third of each cell; this bounded jitter makes the search exact with that
neighborhood, avoiding a 625-cell search. It returns F1 distance, not cell IDs or
distance to an edge. Noise hashing repeats after 289 lattice cells (simplex uses
a skewed lattice); it is suitable for procedural effects, not random-number security.

## Gamepads

`shader-gamepad.h` is a C-compatible interface implemented by `shader-gamepad.cpp`.
The main C plugin initializes it once at module load, uploads snapshots to effects,
and shuts it down at unload. Initialization failure leaves the rest of the plugin
usable with neutral input and `gamepad_available == 0`.

Polling runs on OBS's Qt UI thread every 8 ms, independently of rendering and the
number of filters. Only connected, mapped gamepads are sampled. Hotplug callbacks
maintain that set. Shader rendering and C getters take synchronized snapshots;
they never call GLFW on the graphics thread. Current input means the latest poll,
not a synchronous hardware query. As with any polled input, a complete press and
release between polls (or while the UI is stalled) can be missed.

OBS logs controller connections/disconnections (including unmapped joysticks)
and each new button press with its controller and button indices. Devices already
attached at startup are logged once. Axis movement, held buttons, and releases
do not produce input logs. Press logging follows the same edge detection as the
counters, so reconnecting with a button held does not invent a press.

Direct3D uses packed vector uniforms. For OpenGL, whose OBS translator drops
uniform-array dimensions, the plugin uses a shared 19×16 point-sampled RGBA32F
texture updated at most once per video frame. Button counts are split into two
16-bit halves for exact reconstruction, including counts larger than float's
integer precision. The shader helpers have the same signatures on both backends.

IDs follow [GLFW's gamepad mapping](https://www.glfw.org/docs/3.4/input_guide.html#gamepad).
Controller IDs are slots `0–15`, not a compact count. Unmapped joysticks are not
exposed as gamepads. Slots may be reused by a different device after disconnect.

Open **Tools → Shaderfilter Controller Lookup** to inspect live input. Tabs show
the actual connected slot IDs, and each controller page scrolls through all buttons
and axes. The inspector includes full 64-bit press counts, double-precision cumulative
phases, stick XY pairs, and the most recently pressed button. Display refreshes at
20 Hz while visible and stops while hidden or closed. Input polling stays shared
with shaders. Both receive axes clamped to `[-1,1]`, with invalid numbers replaced
by zero; each stick pair therefore lies in `[-1,1] × [-1,1]`.

| Button ID | Button |
| --- | --- |
| 0–3 | A / B / X / Y (Cross / Circle / Square / Triangle) |
| 4–5 | Left / right bumper |
| 6–8 | Back / Start / Guide |
| 9–10 | Left / right stick click |
| 11–14 | D-pad up / right / down / left |

| Axis ID | Axis |
| --- | --- |
| 0–1 | Left stick X / Y |
| 2–3 | Right stick X / Y |
| 4–5 | Left / right trigger |

Raw axes use GLFW's `[-1,1]` range. Triggers rest at `-1`. Deadzone helpers remove
the center interval and rescale the remaining signed range to `[-1,1]`. For a
trigger's instantaneous pressure, use `(gamepad_axis(c, 4) + 1.0) * 0.5` after
checking `gamepad_connected(c)`.

| Shader interface | Result |
| --- | --- |
| `gamepad_available` | Integer: GLFW initialized successfully |
| `bool gamepad_connected(int c)` | Slot has a mapped, connected gamepad |
| `int gamepad_button(int c, int b)` | 1 while held; otherwise 0 |
| `float gamepad_axis(int c, int a)` | Latest raw axis value |
| `float gamepad_axis_deadzone(int c, int a, float deadzone)` | Axis with rescaled center deadzone |
| `int gamepad_last_button(int c)` | Most recent rising-edge button ID, initially -1 |
| `int gamepad_press_count(int c, int b)` | Number of rising-edge presses, not held frames |
| `float gamepad_axis_cumulative(int c, int a)` | Shared integrated phase in `[0,1)` |
| `float gamepad_axis_clamped(int c, int a)` | Shared position in `[0,1]`; two seconds end to end at full deflection |
| `float gamepad_axis_acc(int c, int a, float rate)` | Audio-style `acc(rate)` interval selection |
| `float gamepad_axis_interval(int c, int a, float interval)` | Explicit counter interval |

C helpers use the same names with `sf_` prepended, except `sf_gamepad_button`
returns `bool` and `sf_gamepad_press_count` returns `uint64_t`. The C header also
exposes lifecycle functions and `sf_gamepad_get_snapshot` for one consistent read.
Invalid indices return zero/false; the last-button getter returns -1. Disconnected
live inputs are neutral, while history remains available for that slot.

Continuous integration treats the stick as signed velocity with a `0.15` deadzone.
Triggers are remapped to `[0,1]` for integration only. GLFW reports absent mappings
as zero, so a trigger must first report a nonzero value before its integral is
enabled; this prevents absent triggers drifting. A real trigger first connected
at exactly its midpoint starts integrating after it moves away from that value.

Each wrapping integral is stored in double precision modulo 1. At full deflection it adds
`100 / 166320` per second, matching audio's 100 counter units per second. Negative
velocity wraps correctly. Delta time is measured with a monotonic clock and capped
at 100 ms per poll to avoid a large jump after a stalled UI or suspend. Missing or
disconnected axes do not integrate. Filter reloads/creation do not reset shared
state; plugin unload does.

`gamepad_axis_clamped(c,a)` starts stick axes at `0.5` and triggers at `0`.
It integrates the deadzone-adjusted velocity at 0.5 units per second, using
the full elapsed time between polls. Full stick deflection takes two seconds
from 0 to 1 or 1 to 0; partial deflection moves proportionally. It holds at
rest and clamps at both endpoints without wrapping or accumulating excess
movement. Triggers only increase while pressed. These positions also survive
disconnects and shader reloads. See [the variable reference](VARIABLE_REFERENCE.md)
for the new three-second audio inputs `energy` and `e_bass`/`e_lows`/`e_mids`/`e_treb`.

`gamepad_axis_acc(c,a,rate)` uses
`frac(phase * 166320 / pow(166320, 1 - saturate(rate)))`.
`gamepad_axis_interval(c,a,interval)` uses
`frac(phase * 166320 / interval)` for intervals in `(0,166320]`.
Choose a divisor of the highly composite number **166320** for seamless wrapping,
such as 1, 2, 3, 4, 5, 6, 8, 10, 12, 60, 120, or 360. Arbitrary intervals and
intermediate rate values can jump when the underlying phase wraps, just as with
the audio speed helper. Shader phase precision is float; C integrals use double.

Counts advance only on sampled released-to-pressed transitions. Buttons held at
initial connection/reconnection establish a baseline and do not count as presses.
If several buttons rise in the same poll, the highest ID is reported as last.
Axis motion never changes the last-button value. Shader counts saturate at
`2147483647`; C counts saturate at `UINT64_MAX`.

```hlsl
int mode = gamepad_press_count(0, 0) % 4; // A cycles four modes
int movement = gamepad_press_count(0, 12) - gamepad_press_count(0, 14);
int menu = ((movement % 5) + 5) % 5; // D-pad right/left, including negatives
float pan = gamepad_axis_deadzone(0, 0, 0.15);
float rotation = gamepad_axis_interval(0, 0, 360.0) * 6.2831853;
```

## Audio falloff

`bass`, `lows`, `mids`, and `treb` are unsmoothed block RMS values with A-weighting
at each band's geometric center. They have no attack/release envelope or global
falloff setting. Use `linfoff(tails)` or `expfoff(tails)` for explicit tails.
Both accept a scalar for all bands or a `float4` for separate band controls;
zero returns the base bands exactly.

Each band stores independent fast/medium/slow/slug presets with tail durations
0.12, 0.4, 1.0, and 2.7307 seconds. Linear decay subtracts `dt / duration`;
exponential decay multiplies by `exp(-log(100) * dt / duration)`, reaching 1%
after one duration. Rises are immediate. The helpers interpolate these presets.
The longest tail has a similar duration to the slow FFT window, rather than
the former tens-of-seconds or multi-minute decay. See
[the full reference](VARIABLE_REFERENCE.md#exponential-and-linear-falloff-variables).

The former global falloff slider and its saved setting are no longer used.
The Controller Lookup Tools menu remains available.

## Building and checking

Qt6 Core/Widgets and the OBS frontend API provide the timer and Tools dialog.
Standalone builds need both the libobs and obs-frontend-api development packages.
CMake first looks
for GLFW 3.4+ using `find_package(glfw3 CONFIG)`, compatible with vcpkg or Conan's
CMakeDeps generator. If absent, it fetches the pinned GLFW 3.4 commit and links it
statically. Set `SHADERFILTER_FETCH_GLFW=OFF` to require a package instead.
Prefer static GLFW so the plugin owns its lifecycle independently of other plugins.
If selecting a shared package, its runtime DLL/library must also be deployed.

The included pinned `vcpkg.json` declares `glfw3` and `pffft`. PFFFT is required
via `find_package(pffft CONFIG REQUIRED)` / `pffft::pffft`; vcpkg builds it as a
static SIMD library. Its redistribution notice is shipped in
`data/internal/pffft-license.txt`. Pass the vcpkg toolchain on the **first**
configure; when building inside OBS also set `VCPKG_MANIFEST_DIR` to this plugin
directory. On Windows, `VCPKG_TARGET_TRIPLET=x64-windows-static-md` uses static GLFW
with OBS's dynamic MSVC runtime. For an existing OBS build tree, install this
manifest separately and point `pffft_DIR` at the installed triplet's
`share/pffft` directory; GLFW can still use its pinned FetchContent fallback.
For example, from the OBS repository root on Windows:

```powershell
vcpkg install --x-manifest-root=plugins/obs-shaderfilter --triplet=x64-windows-static-md --x-install-root=build_shaderfilter_deps/vcpkg_installed
cmake -S . -B build -Dpffft_DIR="$PWD/build_shaderfilter_deps/vcpkg_installed/x64-windows-static-md/share/pffft"
cmake --build build --config Release --target obs-shaderfilter
```

Set `SHADERFILTER_BUILD_TESTS=ON` and build `shaderfilter-gamepad-test`,
`shaderfilter-gamepad-api-test`, `shaderfilter-tools-test`, and
`shaderfilter-helpers-test`. The first two
cover state transitions, injected initialization failures, hotplug, event logging, timer shutdown,
and concurrent C API reads (run directly or use CTest in the plugin's build
directory). The Tools test runs Qt headlessly with its `minimal` platform plugin,
checking the controller inspector and exit cleanup.
If Qt plugins are outside the default search path, set
`QT_QPA_PLATFORM_PLUGIN_PATH` to the Qt package's `plugins/platforms` directory.
The GPU test takes the OBS graphics backend library path and the
path to `obs-shaderfilter.c`; ensure OBS runtime dependencies are on the library
search path. It compiles the actual preamble and checks GPU float readbacks for
color round trips, all four noise dimensions, continuity, analytic derivatives,
curl divergence, controller uniform packing, invalid indices, and interval helpers.
It also checks spectrum opt-in, the expected missing-helper compile error, and
64 × 4 texture interpolation/clamping on Direct3D and OpenGL.

Build `shaderfilter-audio-energy-test` and `shaderfilter-spectrum-test` for the
rolling energy and FFT regressions. The spectrum tests verify band edges,
short-window response to small blocks and sudden amplitude drops, absence of
smoothed falloff, A-weighting, amplitude normalization, opposite-phase stereo,
Nyquist limits, mute/idle behavior, and worker shutdown. Run them with
CTest from the plugin build directory. `data/examples/spectrum-bars.shader`
demonstrates the optional `uniform texture2d spec;` / `spectrum(pitch, falloff)`
interface. The one-argument form samples the instant row at y = 0.125; the slow
row at y = 0.875 uses the original long window. The middle rows use independent
FFT windows of roughly 171 ms and 683 ms at 48 kHz.

`data/examples/gamepad-noise.shader` demonstrates the color/noise helpers and
controller accumulators: A cycles noise modes, and the left stick changes hue.
