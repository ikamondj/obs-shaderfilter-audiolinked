#include "shader-gamepad-state.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <util/base.h>

#include <chrono>
#include <mutex>
#include <new>

namespace {
using namespace shaderfilter;
std::mutex state_mutex;
gamepad_state states[SF_GAMEPAD_COUNT];
bool available = false;
QTimer *poll_timer = nullptr; // Only accessed on the UI thread.
std::chrono::steady_clock::time_point last_poll;
bool mapped[SF_GAMEPAD_COUNT] = {}; // UI thread only; maintained by hotplug.
bool attached[SF_GAMEPAD_COUNT] = {};

static_assert(GLFW_JOYSTICK_LAST + 1 == SF_GAMEPAD_COUNT);
static_assert(GLFW_GAMEPAD_BUTTON_LAST + 1 == SF_GAMEPAD_BUTTON_COUNT);
static_assert(GLFW_GAMEPAD_AXIS_LAST + 1 == SF_GAMEPAD_AXIS_COUNT);

void joystick_changed(int controller, int event)
{
	if (!valid_controller(controller))
		return;
	const bool connected = event == GLFW_CONNECTED;
	mapped[controller] = connected && glfwJoystickIsGamepad(controller);
	if (connected != attached[controller]) {
		attached[controller] = connected;
		if (connected)
			blog(LOG_INFO, "[obs-shaderfilter] Controller %d connected (%s)", controller,
			     mapped[controller] ? "gamepad mapping available" : "no gamepad mapping; input unavailable");
		else
			blog(LOG_INFO, "[obs-shaderfilter] Controller %d disconnected", controller);
	}
	if (!mapped[controller]) {
		std::lock_guard<std::mutex> lock(state_mutex);
		states[controller].update(false, nullptr, nullptr, 0.0);
	}
}

void poll_gamepads()
{
	// Qt already dispatches Win32 helper-window and Cocoa run-loop events.
	// Calling glfwPollEvents there would re-enter Qt's native event dispatch.
	// Linux needs GLFW's own display/inotify connection pumped for hotplug.
#if !defined(_WIN32) && !defined(__APPLE__)
	glfwPollEvents();
#endif
	GLFWgamepadstate samples[SF_GAMEPAD_COUNT] = {};
	bool present[SF_GAMEPAD_COUNT] = {};
	for (int controller = 0; controller < SF_GAMEPAD_COUNT; ++controller) {
		if (mapped[controller])
			present[controller] = glfwGetGamepadState(controller, &samples[controller]) == GLFW_TRUE;
	}
	const auto now = std::chrono::steady_clock::now();
	const double seconds = std::chrono::duration<double>(now - last_poll).count();
	last_poll = now;
	unsigned int pressed[SF_GAMEPAD_COUNT] = {};
	{
		std::lock_guard<std::mutex> lock(state_mutex);
		for (int controller = 0; controller < SF_GAMEPAD_COUNT; ++controller) {
			if (present[controller] && states[controller].connected) {
				for (int button = 0; button < SF_GAMEPAD_BUTTON_COUNT; ++button) {
					if (samples[controller].buttons[button] == GLFW_PRESS &&
					    !states[controller].buttons[button])
						pressed[controller] |= 1u << button;
				}
			}
			if (present[controller] || states[controller].connected)
				states[controller].update(present[controller], samples[controller].buttons,
							  samples[controller].axes, seconds);
		}
	}
	// Logging may call external handlers. Never hold the input mutex here.
	for (int controller = 0; controller < SF_GAMEPAD_COUNT; ++controller) {
		if (!pressed[controller])
			continue;
		for (int button = 0; button < SF_GAMEPAD_BUTTON_COUNT; ++button) {
			if (pressed[controller] & (1u << button))
				blog(LOG_INFO, "[obs-shaderfilter] Controller %d button %d pressed", controller, button);
		}
	}
}

bool init_on_ui()
{
	if (poll_timer)
		return true;
#ifdef __APPLE__
	glfwInitHint(GLFW_COCOA_CHDIR_RESOURCES, GLFW_FALSE);
	glfwInitHint(GLFW_COCOA_MENUBAR, GLFW_FALSE);
#endif
	if (!glfwInit()) {
		const char *description = nullptr;
		glfwGetError(&description);
		blog(LOG_WARNING, "[obs-shaderfilter] Gamepads unavailable: %s", description ? description : "GLFW init failed");
		return false;
	}
	poll_timer = new (std::nothrow) QTimer;
	if (!poll_timer) {
		glfwTerminate();
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(state_mutex);
		available = true;
	}
	last_poll = std::chrono::steady_clock::now();
	glfwSetJoystickCallback(joystick_changed);
	for (int controller = 0; controller < SF_GAMEPAD_COUNT; ++controller)
		joystick_changed(controller, glfwJoystickPresent(controller) ? GLFW_CONNECTED : GLFW_DISCONNECTED);
	poll_gamepads();
	poll_timer->setTimerType(Qt::PreciseTimer);
	QObject::connect(poll_timer, &QTimer::timeout, poll_timer, poll_gamepads);
	poll_timer->start(8);
	return true;
}

void shutdown_on_ui()
{
	if (poll_timer) {
		delete poll_timer; // Cancels pending timer callbacks before unloading this module.
		poll_timer = nullptr;
		glfwSetJoystickCallback(nullptr);
		glfwTerminate();
	}
	std::lock_guard<std::mutex> lock(state_mutex);
	available = false;
	std::fill(std::begin(mapped), std::end(mapped), false);
	std::fill(std::begin(attached), std::end(attached), false);
	for (auto &state : states)
		state = gamepad_state{};
}
} // namespace

bool sf_gamepad_init(void)
{
	auto *app = QCoreApplication::instance();
	if (!app)
		return false;
	bool result = false;
	if (QThread::currentThread() == app->thread())
		result = init_on_ui();
	else
		QMetaObject::invokeMethod(app, [&result] { result = init_on_ui(); }, Qt::BlockingQueuedConnection);
	return result;
}

void sf_gamepad_shutdown(void)
{
	auto *app = QCoreApplication::instance();
	if (!app || QThread::currentThread() == app->thread())
		shutdown_on_ui();
	else
		QMetaObject::invokeMethod(app, shutdown_on_ui, Qt::BlockingQueuedConnection);
}

bool sf_gamepad_available(void)
{
	std::lock_guard<std::mutex> lock(state_mutex);
	return available;
}

bool sf_gamepad_connected(int controller)
{
	std::lock_guard<std::mutex> lock(state_mutex);
	return valid_controller(controller) && states[controller].connected;
}

void sf_gamepad_get_snapshot(struct sf_gamepad_snapshot *snapshot)
{
	if (!snapshot)
		return;
	*snapshot = {};
	std::lock_guard<std::mutex> lock(state_mutex);
	snapshot->available = available;
	for (int controller = 0; controller < SF_GAMEPAD_COUNT; ++controller) {
		const auto &state = states[controller];
		snapshot->info[controller][0] = state.connected;
		snapshot->info[controller][1] = state.last_button;
		for (int button = 0; button < SF_GAMEPAD_BUTTON_COUNT; ++button) {
			const int index = controller * 4 + button / 4;
			snapshot->buttons[index][button % 4] = state.buttons[button];
			snapshot->presses[index][button % 4] =
				static_cast<int32_t>(std::min<uint64_t>(state.presses[button], INT32_MAX));
		}
		for (int axis = 0; axis < SF_GAMEPAD_AXIS_COUNT; ++axis) {
			const int index = controller * 2 + axis / 4;
			snapshot->axes[index][axis % 4] = state.axes[axis];
			snapshot->axes_c[index][axis % 4] = unit_float(state.cumulative[axis]);
			snapshot->axes_clamped[index][axis % 4] = static_cast<float>(state.clamped[axis]);
		}
	}
}

bool sf_gamepad_button(int controller, int button)
{
	std::lock_guard<std::mutex> lock(state_mutex);
	return valid_button(controller, button) && states[controller].buttons[button];
}

void sf_gamepad_get_controller_state(int controller, struct sf_gamepad_controller_state *state)
{
	if (!state)
		return;
	*state = {};
	state->last_button = -1;
	std::lock_guard<std::mutex> lock(state_mutex);
	if (!valid_controller(controller))
		return;
	const auto &source = states[controller];
	state->connected = source.connected;
	state->last_button = source.last_button;
	for (int button = 0; button < SF_GAMEPAD_BUTTON_COUNT; ++button) {
		state->buttons[button] = source.buttons[button];
		state->presses[button] = source.presses[button];
	}
	for (int axis = 0; axis < SF_GAMEPAD_AXIS_COUNT; ++axis) {
		state->axes[axis] = source.axes[axis];
		state->axes_c[axis] = source.cumulative[axis];
		state->axes_clamped[axis] = source.clamped[axis];
	}
}

float sf_gamepad_axis(int controller, int axis)
{
	std::lock_guard<std::mutex> lock(state_mutex);
	return valid_axis(controller, axis) ? states[controller].axes[axis] : 0.0f;
}

float sf_gamepad_axis_deadzone(int controller, int axis, float deadzone)
{
	return apply_deadzone(sf_gamepad_axis(controller, axis), deadzone);
}

int sf_gamepad_last_button(int controller)
{
	std::lock_guard<std::mutex> lock(state_mutex);
	return valid_controller(controller) ? states[controller].last_button : -1;
}

uint64_t sf_gamepad_press_count(int controller, int button)
{
	std::lock_guard<std::mutex> lock(state_mutex);
	return valid_button(controller, button) ? states[controller].presses[button] : 0;
}

float sf_gamepad_axis_cumulative(int controller, int axis)
{
	std::lock_guard<std::mutex> lock(state_mutex);
	return valid_axis(controller, axis) ? unit_float(states[controller].cumulative[axis]) : 0.0f;
}

float sf_gamepad_axis_clamped(int controller, int axis)
{
	std::lock_guard<std::mutex> lock(state_mutex);
	return valid_axis(controller, axis) ? static_cast<float>(states[controller].clamped[axis]) : 0.0f;
}

float sf_gamepad_axis_acc(int controller, int axis, float rate)
{
	if (!std::isfinite(rate))
		return 0.0f;
	return sf_gamepad_axis_interval(controller, axis,
					static_cast<float>(std::pow(SF_GAMEPAD_COUNTER_WRAP, 1.0 - std::clamp(rate, 0.0f, 1.0f))));
}

float sf_gamepad_axis_interval(int controller, int axis, float interval)
{
	if (!std::isfinite(interval) || interval <= 0.0f || interval > SF_GAMEPAD_COUNTER_WRAP)
		return 0.0f;
	std::lock_guard<std::mutex> lock(state_mutex);
	return valid_axis(controller, axis) ? unit_float(states[controller].cumulative[axis] * SF_GAMEPAD_COUNTER_WRAP / interval)
					    : 0.0f;
}
