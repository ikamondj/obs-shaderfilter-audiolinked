#pragma once

#include "shader-gamepad.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>

namespace shaderfilter {

inline bool valid_controller(int controller)
{
	return controller >= 0 && controller < SF_GAMEPAD_COUNT;
}

inline bool valid_axis(int controller, int axis)
{
	return valid_controller(controller) && axis >= 0 && axis < SF_GAMEPAD_AXIS_COUNT;
}

inline bool valid_button(int controller, int button)
{
	return valid_controller(controller) && button >= 0 && button < SF_GAMEPAD_BUTTON_COUNT;
}

inline float apply_deadzone(float value, float deadzone)
{
	if (!std::isfinite(value) || !std::isfinite(deadzone))
		return 0.0f;
	deadzone = std::clamp(deadzone, 0.0f, 1.0f);
	if (deadzone >= 1.0f || std::abs(value) <= deadzone)
		return 0.0f;
	return std::copysign((std::min(std::abs(value), 1.0f) - deadzone) / (1.0f - deadzone), value);
}

inline float unit_float(double value)
{
	/* Rounding a double just below 1 to float must not escape [0,1). */
	return std::min(static_cast<float>(value - std::floor(value)), std::nextafter(1.0f, 0.0f));
}

struct gamepad_state {
	bool connected = false;
	int last_button = -1;
	bool buttons[SF_GAMEPAD_BUTTON_COUNT] = {};
	uint64_t presses[SF_GAMEPAD_BUTTON_COUNT] = {};
	float axes[SF_GAMEPAD_AXIS_COUNT] = {};
	double cumulative[SF_GAMEPAD_AXIS_COUNT] = {};
	double clamped[SF_GAMEPAD_AXIS_COUNT] = {0.5, 0.5, 0.5, 0.5, 0.0, 0.0};
	bool trigger_observed[2] = {};

	void update(bool present, const unsigned char *new_buttons, const float *new_axes, double seconds)
	{
		const bool was_connected = connected;
		connected = present;
		if (!present) {
			std::fill(std::begin(buttons), std::end(buttons), false);
			std::fill(std::begin(axes), std::end(axes), 0.0f);
			std::fill(std::begin(trigger_observed), std::end(trigger_observed), false);
			return; // Keep history for this controller slot across reconnects.
		}
		seconds = std::isfinite(seconds) ? std::max(seconds, 0.0) : 0.0;
		for (int button = 0; button < SF_GAMEPAD_BUTTON_COUNT; ++button) {
			const bool pressed = new_buttons[button] != 0;
			// Initial/reconnected held buttons establish a baseline, not a new press.
			if (was_connected && pressed && !buttons[button]) {
				if (presses[button] != std::numeric_limits<uint64_t>::max())
					++presses[button];
				last_button = button; // Highest ID wins simultaneous sampled edges.
			}
			buttons[button] = pressed;
		}
		for (int axis = 0; axis < SF_GAMEPAD_AXIS_COUNT; ++axis) {
			const float previous = axes[axis];
			axes[axis] = std::isfinite(new_axes[axis]) ? std::clamp(new_axes[axis], -1.0f, 1.0f) : 0.0f;
			// Integrate the previously sampled velocity over its actual holding time.
			const float velocity = axis >= 4 ? (previous + 1.0f) * 0.5f : previous;
			if (was_connected && (axis < 4 || trigger_observed[axis - 4]) && velocity != 0.0f) {
				const double speed = apply_deadzone(velocity, SF_GAMEPAD_INTEGRATION_DEADZONE);
				clamped[axis] = std::clamp(clamped[axis] + speed * seconds * 0.5, 0.0, 1.0);
				cumulative[axis] += speed * std::min(seconds, 0.1) * (100.0 / SF_GAMEPAD_COUNTER_WRAP);
				cumulative[axis] -= std::floor(cumulative[axis]);
			}
			// GLFW reports zero for missing mappings, including absent triggers.
			// A real trigger must leave exact midpoint before integrating it.
			if (axis >= 4 && axes[axis] != 0.0f)
				trigger_observed[axis - 4] = true;
		}
	}
};

} // namespace shaderfilter
