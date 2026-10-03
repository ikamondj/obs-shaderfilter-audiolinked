#include "shader-gamepad-state.hpp"

#include <cstdio>
#include <cstdlib>

#define CHECK(condition)                                                                                      \
	do {                                                                                                  \
		if (!(condition)) {                                                                           \
			std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition);                            \
			std::exit(1);                                                                          \
		}                                                                                             \
	} while (false)

int main()
{
	using namespace shaderfilter;
	CHECK(!valid_axis(-1, 0) && !valid_axis(16, 0) && !valid_axis(0, 6));
	CHECK(valid_axis(15, 5) && valid_button(15, 14) && !valid_button(0, 15));
	CHECK(apply_deadzone(0.1f, 0.15f) == 0.0f);
	CHECK(apply_deadzone(1.0f, 0.15f) == 1.0f);
	CHECK(apply_deadzone(-1.0f, 0.15f) == -1.0f);
	CHECK(apply_deadzone(0.5f, 1.0f) == 0.0f);
	CHECK(apply_deadzone(0.5f, -1.0f) == 0.5f);
	CHECK(apply_deadzone(NAN, 0.15f) == 0.0f);
	CHECK(apply_deadzone(0.5f, NAN) == 0.0f);
	CHECK(unit_float(1.0 - 1.0e-12) < 1.0f);

	unsigned char buttons[SF_GAMEPAD_BUTTON_COUNT] = {};
	float axes[SF_GAMEPAD_AXIS_COUNT] = {0, 0, 0, 0, -1, -1};
	gamepad_state state;
	CHECK(state.last_button == -1);
	state.update(true, buttons, axes, 0.01);
	buttons[0] = 1;
	state.update(true, buttons, axes, 0.01);
	CHECK(state.presses[0] == 1 && state.last_button == 0);
	for (int i = 0; i < 100; ++i)
		state.update(true, buttons, axes, 0.01);
	CHECK(state.presses[0] == 1 && state.cumulative[4] == 0.0);
	buttons[0] = 0;
	state.update(true, buttons, axes, 0.01);
	buttons[0] = buttons[14] = 1;
	state.update(true, buttons, axes, 0.01);
	CHECK(state.presses[0] == 2 && state.presses[14] == 1 && state.last_button == 14);

	// One second of held input integrates identically at 50 and 125 Hz.
	gamepad_state slow, fast;
	axes[0] = 1.0f;
	slow.update(true, buttons, axes, 0.0);
	fast.update(true, buttons, axes, 0.0);
	for (int i = 0; i < 50; ++i)
		slow.update(true, buttons, axes, 0.02);
	for (int i = 0; i < 125; ++i)
		fast.update(true, buttons, axes, 0.008);
	CHECK(std::abs(slow.cumulative[0] - 100.0 / SF_GAMEPAD_COUNTER_WRAP) < 1.0e-12);
	CHECK(std::abs(slow.cumulative[0] - fast.cumulative[0]) < 1.0e-12);
	CHECK(slow.presses[0] == 0 && slow.last_button == -1);

	// Negative integration wraps and disconnects neutralize live state only.
	axes[0] = -1.0f;
	gamepad_state negative;
	negative.update(true, buttons, axes, 0.0);
	negative.update(true, buttons, axes, 0.01);
	CHECK(negative.cumulative[0] > 0.99 && negative.cumulative[0] < 1.0);
	const double saved = state.cumulative[0];
	state.update(false, nullptr, nullptr, 1.0);
	CHECK(!state.buttons[0] && state.axes[0] == 0 && state.cumulative[0] == saved);
	CHECK(state.presses[0] == 2 && state.last_button == 14);
	state.update(true, buttons, axes, 0.01);
	CHECK(state.presses[0] == 2); // Reconnected held button is not a press.

	gamepad_state absent_triggers;
	axes[4] = axes[5] = 0.0f; // GLFW's unmapped-axis default.
	absent_triggers.update(true, buttons, axes, 0.0);
	absent_triggers.update(true, buttons, axes, 0.01);
	CHECK(absent_triggers.cumulative[4] == 0.0 && absent_triggers.cumulative[5] == 0.0);
	axes[4] = 1.0f;
	absent_triggers.update(true, buttons, axes, 0.01);
	absent_triggers.update(true, buttons, axes, 0.01);
	CHECK(absent_triggers.cumulative[4] > 0.0 && absent_triggers.cumulative[5] == 0.0);

	state.presses[0] = UINT64_MAX;
	buttons[0] = 0;
	state.update(true, buttons, axes, NAN);
	buttons[0] = 1;
	state.update(true, buttons, axes, -1.0);
	CHECK(state.presses[0] == UINT64_MAX);
	CHECK(std::isfinite(state.cumulative[0]));

	// Clamped positions use elapsed time even when polling is delayed.
	gamepad_state position;
	axes[0] = -1.0f;
	axes[1] = 1.0f;
	axes[4] = axes[5] = -1.0f;
	CHECK(position.clamped[0] == 0.5 && position.clamped[4] == 0.0);
	position.update(true, buttons, axes, 0.0);
	position.update(true, buttons, axes, 1.0);
	CHECK(position.clamped[0] == 0.0 && position.clamped[1] == 1.0);
	position.update(true, buttons, axes, 10.0);
	CHECK(position.clamped[0] == 0.0 && position.clamped[1] == 1.0);
	axes[0] = 1.0f;
	axes[1] = -1.0f;
	position.update(true, buttons, axes, 0.0);
	position.update(true, buttons, axes, 1.0);
	CHECK(position.clamped[0] == 0.5 && position.clamped[1] == 0.5);
	position.update(true, buttons, axes, 0.99);
	CHECK(std::abs(position.clamped[0] - 0.995) < 1e-12);
	position.update(true, buttons, axes, 0.01);
	CHECK(std::abs(position.clamped[0] - 1.0) < 1e-12 && std::abs(position.clamped[1]) < 1e-12);
	axes[0] = 0.1f; // Rest/deadzone must preserve position.
	position.update(true, buttons, axes, 0.0);
	position.update(true, buttons, axes, 5.0);
	CHECK(position.clamped[0] == 1.0 && position.clamped[4] == 0.0);
	position.update(false, nullptr, nullptr, 10.0);
	axes[0] = -1.0f;
	position.update(true, buttons, axes, 10.0);
	CHECK(position.clamped[0] == 1.0);
	position.update(true, buttons, axes, NAN);
	position.update(true, buttons, axes, -1.0);
	CHECK(position.clamped[0] == 1.0);
	position.update(true, buttons, axes, 0.5);
	CHECK(position.clamped[0] == 0.75);
	axes[0] = -0.575f; // Half speed after removing the 0.15 deadzone.
	position.update(true, buttons, axes, 0.0);
	position.update(true, buttons, axes, 2.0);
	CHECK(std::abs(position.clamped[0] - 0.25) < 1e-7);
	CHECK(std::abs(slow.clamped[0] - fast.clamped[0]) < 1e-12);
	CHECK(absent_triggers.clamped[4] > 0.0 && absent_triggers.clamped[5] == 0.0);
	std::puts("Gamepad state tests passed");
}
