#include "shader-gamepad.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <util/base.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

// A deterministic hardware boundary; the real bridge, timer, mutex, and C API
// are compiled unchanged. No physical device is required for hotplug coverage.
static bool init_result = true;
static bool present[SF_GAMEPAD_COUNT] = {};
static GLFWgamepadstate hardware[SF_GAMEPAD_COUNT] = {};
static GLFWjoystickfun hotplug = nullptr;
static int polls[SF_GAMEPAD_COUNT] = {};
static int terminations = 0;
static int connections = 0;
static int disconnections = 0;
static int button_presses = 0;
static int input_logs = 0;

static void capture_log(int, const char *format, va_list args, void *)
{
	char message[512];
	std::vsnprintf(message, sizeof(message), format, args);
	if (!std::strstr(message, "[obs-shaderfilter] Controller"))
		return;
	++input_logs;
	if (std::strstr(message, " connected"))
		++connections;
	if (std::strstr(message, " disconnected"))
		++disconnections;
	if (std::strstr(message, " pressed"))
		++button_presses;
	// OBS handlers may read input: logging must happen outside the state mutex.
	(void)sf_gamepad_connected(15);
}

extern "C" {
int glfwInit(void)
{
	return init_result;
}
void glfwTerminate(void)
{
	++terminations;
}
void glfwInitHint(int, int) {}
void glfwPollEvents(void) {}
int glfwGetError(const char **description)
{
	*description = "Injected init failure";
	return GLFW_PLATFORM_ERROR;
}
GLFWjoystickfun glfwSetJoystickCallback(GLFWjoystickfun callback)
{
	auto previous = hotplug;
	hotplug = callback;
	return previous;
}
int glfwJoystickIsGamepad(int controller)
{
	return present[controller];
}
int glfwJoystickPresent(int controller)
{
	return present[controller];
}
int glfwGetGamepadState(int controller, GLFWgamepadstate *state)
{
	++polls[controller];
	*state = hardware[controller];
	return present[controller];
}
}

static void check(bool condition, const char *message)
{
	if (!condition) {
		std::fprintf(stderr, "%s\n", message);
		std::exit(1);
	}
}

static void tick()
{
	QEventLoop loop;
	QTimer::singleShot(30, &loop, &QEventLoop::quit);
	loop.exec();
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	base_set_log_handler(capture_log, nullptr);
	check(!sf_gamepad_available(), "Unexpected initial availability");
	init_result = false;
	check(!sf_gamepad_init() && !sf_gamepad_available(), "Failed init did not remain neutral");
	init_result = true;
	check(sf_gamepad_init() && sf_gamepad_init(), "Init/idempotency failed");
	tick();
	for (int count : polls)
		check(count == 0, "Disconnected device was polled");
	check(sf_gamepad_last_button(-1) == -1 && sf_gamepad_axis(16, 0) == 0.0f, "Invalid index handling");
	check(sf_gamepad_press_count(0, 15) == 0 && !sf_gamepad_button(0, -1), "Invalid button handling");
	check(sf_gamepad_axis_interval(0, 0, 0) == 0 && sf_gamepad_axis_acc(0, 0, NAN) == 0, "Invalid interval handling");
	check(sf_gamepad_axis_clamped(-1, 0) == 0 && sf_gamepad_axis_clamped(0, 6) == 0, "Invalid clamped axis");
	check(sf_gamepad_axis_clamped(0, 0) == 0.5f && sf_gamepad_axis_clamped(0, 4) == 0.0f, "Initial clamped axis");
	present[15] = true;
	hardware[15].axes[4] = hardware[15].axes[5] = -1.0f;
	hotplug(15, GLFW_CONNECTED);
	tick();
	check(sf_gamepad_connected(15) && sf_gamepad_axis(15, 5) == -1.0f, "Connect failed");
	check(connections == 1 && input_logs == 1, "Connection log missing or duplicated");
	hardware[15].buttons[14] = GLFW_PRESS;
	hardware[15].axes[0] = 1.0f;
	tick();
	check(sf_gamepad_button(15, 14) && sf_gamepad_press_count(15, 14) == 1 && sf_gamepad_last_button(15) == 14,
	      "Button edge failed");
	check(sf_gamepad_axis_deadzone(15, 0, 0.15f) == 1.0f && sf_gamepad_axis_cumulative(15, 0) > 0.0f,
	      "Axis integration failed");
	check(sf_gamepad_axis_cumulative(15, 5) == 0.0f, "Resting trigger drift");
	std::thread reader([] {
		for (int i = 0; i < 1000; ++i) {
			sf_gamepad_snapshot snapshot;
			sf_gamepad_get_snapshot(&snapshot);
			check(snapshot.info[15][0] == 1 && snapshot.presses[63][2] == 1 && snapshot.buttons[63][2] == 1,
			      "Threaded snapshot/packing failed");
		}
	});
	tick();
	reader.join();
	check(sf_gamepad_press_count(15, 14) == 1, "Held button counted repeatedly");
	hardware[15].axes[0] = -0.75f;
	tick();
	check(button_presses == 1 && input_logs == 2, "Held input or axis motion produced extra logs");
	present[15] = false;
	hotplug(15, GLFW_DISCONNECTED);
	const int saved_polls = polls[15];
	tick();
	check(!sf_gamepad_connected(15) && !sf_gamepad_button(15, 14) && sf_gamepad_axis(15, 0) == 0.0f,
	      "Disconnect retained live input");
	check(polls[15] == saved_polls && sf_gamepad_press_count(15, 14) == 1, "Disconnect polling/history failed");
	check(disconnections == 1 && input_logs == 3, "Disconnect log missing or duplicated");
	present[15] = true;
	hotplug(15, GLFW_CONNECTED);
	tick();
	check(sf_gamepad_press_count(15, 14) == 1, "Reconnect invented a press");
	check(connections == 2 && button_presses == 1 && input_logs == 4, "Reconnect logging failed");
	hardware[15].buttons[14] = GLFW_RELEASE;
	tick();
	hardware[15].buttons[14] = GLFW_PRESS;
	tick();
	check(button_presses == 2 && input_logs == 5, "Second button press was not logged exactly once");
	// Both UI/native diagnostics and shader uploads share bounded signed axes.
	hardware[15].axes[0] = -5.0f;
	hardware[15].axes[1] = 5.0f;
	hardware[15].axes[2] = NAN;
	hardware[15].axes[3] = -0.25f;
	hardware[15].axes[4] = 5.0f;
	hardware[15].axes[5] = -5.0f;
	tick();
	sf_gamepad_snapshot shader;
	sf_gamepad_controller_state diagnostic;
	sf_gamepad_get_snapshot(&shader);
	sf_gamepad_get_controller_state(15, &diagnostic);
	check(diagnostic.connected && diagnostic.last_button == 14 && diagnostic.presses[14] == 2 && diagnostic.buttons[14],
	      "Native controller snapshot lost button history");
	const float expected_axes[] = {-1, 1, 0, -0.25f, 1, -1};
	for (int axis = 0; axis < SF_GAMEPAD_AXIS_COUNT; ++axis) {
		check(diagnostic.axes[axis] == expected_axes[axis] &&
			      shader.axes[15 * 2 + axis / 4][axis % 4] == expected_axes[axis],
		      "Native and shader axes disagree or exceed [-1,1]");
		check(diagnostic.axes_c[axis] >= 0.0 && diagnostic.axes_c[axis] < 1.0, "Native cumulative range escaped [0,1)");
		check(diagnostic.axes_clamped[axis] >= 0.0 && diagnostic.axes_clamped[axis] <= 1.0 &&
			      shader.axes_clamped[15 * 2 + axis / 4][axis % 4] == float(diagnostic.axes_clamped[axis]) &&
			      sf_gamepad_axis_clamped(15, axis) == float(diagnostic.axes_clamped[axis]),
		      "Clamped native and shader snapshots disagree");
	}
	sf_gamepad_get_controller_state(-1, &diagnostic);
	check(!diagnostic.connected && diagnostic.last_button == -1 && diagnostic.presses[14] == 0,
	      "Invalid diagnostic index was not neutral");
	sf_gamepad_get_controller_state(0, nullptr);
	sf_gamepad_shutdown();
	sf_gamepad_shutdown();
	check(terminations == 1 && !sf_gamepad_available() && sf_gamepad_last_button(15) == -1, "Shutdown failed");
	const int stopped_polls = polls[15];
	tick();
	check(polls[15] == stopped_polls, "Timer survived shutdown");
	sf_gamepad_get_snapshot(nullptr);
	std::puts("Gamepad C API, timer, hotplug, logging, and thread tests passed");
}
