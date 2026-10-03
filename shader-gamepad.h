#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SF_GAMEPAD_COUNT 16
#define SF_GAMEPAD_BUTTON_COUNT 15
#define SF_GAMEPAD_AXIS_COUNT 6
#define SF_GAMEPAD_TEXTURE_WIDTH 19
#define SF_GAMEPAD_COUNTER_WRAP 166320.0
#define SF_GAMEPAD_INTEGRATION_DEADZONE 0.15f

/* Packed as float4/int4 arrays for matching D3D and OpenGL uniform layouts.
 * Controller IDs and button/axis IDs are zero-based GLFW gamepad indices.
 * info[c] = {connected with mapping, last button (-1 if none), 0, 0}.
 * Counts exported to shaders saturate at INT32_MAX; C getters use uint64_t. */
struct sf_gamepad_snapshot {
	int32_t available;
	int32_t info[SF_GAMEPAD_COUNT][4];
	int32_t buttons[SF_GAMEPAD_COUNT * 4][4];
	int32_t presses[SF_GAMEPAD_COUNT * 4][4];
	float axes[SF_GAMEPAD_COUNT * 2][4];
	float axes_c[SF_GAMEPAD_COUNT * 2][4];
	float axes_clamped[SF_GAMEPAD_COUNT * 2][4];
};

/* Native diagnostic snapshot: full 64-bit press counts and double integrals,
 * read together under one lock. Unlike the shader upload, counts aren't narrowed. */
struct sf_gamepad_controller_state {
	bool connected;
	int last_button;
	bool buttons[SF_GAMEPAD_BUTTON_COUNT];
	uint64_t presses[SF_GAMEPAD_BUTTON_COUNT];
	float axes[SF_GAMEPAD_AXIS_COUNT];
	double axes_c[SF_GAMEPAD_AXIS_COUNT];
	double axes_clamped[SF_GAMEPAD_AXIS_COUNT];
};

/* OpenGL's OBS shader translator does not preserve uniform-array dimensions.
 * Encode the same snapshot as a small point-sampled RGBA32F texture there.
 * Split counts into exact 16-bit halves to preserve every bit of int32. */
static inline void sf_gamepad_pack_snapshot(const struct sf_gamepad_snapshot *snapshot, float *pixels)
{
	for (int c = 0; c < SF_GAMEPAD_COUNT; ++c) {
		float *row = pixels + c * SF_GAMEPAD_TEXTURE_WIDTH * 4;
		for (int component = 0; component < 4; ++component) {
			row[component] = (float)snapshot->info[c][component];
			for (int pack = 0; pack < 4; ++pack) {
				uint32_t count = (uint32_t)snapshot->presses[c * 4 + pack][component];
				row[(1 + pack) * 4 + component] = (float)snapshot->buttons[c * 4 + pack][component];
				row[(5 + pack) * 4 + component] = (float)(count & 65535);
				row[(9 + pack) * 4 + component] = (float)(count >> 16);
			}
			for (int pack = 0; pack < 2; ++pack) {
				row[(13 + pack) * 4 + component] = snapshot->axes[c * 2 + pack][component];
				row[(15 + pack) * 4 + component] = snapshot->axes_c[c * 2 + pack][component];
				row[(17 + pack) * 4 + component] = snapshot->axes_clamped[c * 2 + pack][component];
			}
		}
	}
}

/* init/shutdown marshal synchronously to the Qt application (OBS UI) thread.
 * Polling uses an 8 ms UI timer. Getters are thread-safe snapshot reads and
 * never call GLFW from the rendering thread. Shutdown before unloading. */
bool sf_gamepad_init(void);
void sf_gamepad_shutdown(void);
bool sf_gamepad_available(void);
bool sf_gamepad_connected(int controller);
void sf_gamepad_get_snapshot(struct sf_gamepad_snapshot *snapshot);
void sf_gamepad_get_controller_state(int controller, struct sf_gamepad_controller_state *state);
bool sf_gamepad_button(int controller, int button);
float sf_gamepad_axis(int controller, int axis);
float sf_gamepad_axis_deadzone(int controller, int axis, float deadzone);
int sf_gamepad_last_button(int controller);
uint64_t sf_gamepad_press_count(int controller, int button);
/* Signed velocity integration, wrapped into [0,1). Triggers are remapped
 * from GLFW [-1,1] to [0,1] for integration only (rest does not drift).
 * Full deflection integrates 100 counter units per second, like audio.
 * rate helper matches acc(rate); interval uses a divisor of 166320 for
 * seamless wrap (e.g. 1, 2, 3, 4, 5, 6, 8, 10, 12, 60, 120, 360). */
float sf_gamepad_axis_cumulative(int controller, int axis);
/* Position in [0,1], moving at half the deadzone-adjusted velocity per second.
 * Sticks start at 0.5; triggers start at 0 and use the same remapping as above.
 * Holds at the endpoints and at rest; retained across reconnects. */
float sf_gamepad_axis_clamped(int controller, int axis);
float sf_gamepad_axis_acc(int controller, int axis, float rate);
float sf_gamepad_axis_interval(int controller, int axis, float interval);

#ifdef __cplusplus
}
#endif
