#include "shader-tools.h"
#include "shader-gamepad.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QEventLoop>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabBar>
#include <QTableWidget>
#include <QLabel>
#include <QTimer>
#include <cmath>
#include <cstdio>
#include <cstdlib>

// Fake only the hosting frontend. Exercise real Qt widgets and controller state.

static QWidget *main_window;

static QPointer<QAction> controller_action;
static sf_gamepad_controller_state controllers[SF_GAMEPAD_COUNT] = {};
static bool gamepads_available = false;
static int snapshot_reads = 0;
static obs_frontend_event_cb event_callback;
static void *event_context;

static int menu_count;

static void check(bool condition, const char *message)
{
	if (!condition) {
		std::fprintf(stderr, "%s\n", message);
		std::exit(1);
	}
}

extern "C" {
const char *obs_module_text(const char *key)
{
	if (qstrcmp(key, "ShaderFilter.Controller.Slot") == 0 || qstrcmp(key, "ShaderFilter.Controller.Connected") == 0)
		return "Controller %1";
	if (qstrcmp(key, "ShaderFilter.Controller.Last") == 0)
		return "Last: %1 (%2)";
	if (qstrcmp(key, "ShaderFilter.Controller.Sticks") == 0)
		return "Left: (%1, %2); Right: (%3, %4)";
	return key;
}
obs_module_t *obs_current_module(void)
{
	return nullptr;
}
void *obs_frontend_get_main_window(void)
{
	return main_window;
}
void *obs_frontend_add_tools_menu_qaction(const char *title)
{
	++menu_count;
	auto *created = new QAction(QString::fromUtf8(title), main_window);
	controller_action = created;
	return created;
}
void obs_frontend_add_event_callback(obs_frontend_event_cb callback, void *context)
{
	check(!event_callback, "Duplicate frontend callback");
	event_callback = callback;
	event_context = context;
}
void obs_frontend_remove_event_callback(obs_frontend_event_cb callback, void *context)
{
	check(callback == event_callback && context == event_context, "Removing unknown frontend callback");
	event_callback = nullptr;
	event_context = nullptr;
}

void sf_gamepad_get_snapshot(sf_gamepad_snapshot *snapshot)
{
	++snapshot_reads;
	*snapshot = {};
	snapshot->available = gamepads_available;
	for (int controller = 0; controller < SF_GAMEPAD_COUNT; ++controller)
		snapshot->info[controller][0] = controllers[controller].connected;
}

void sf_gamepad_get_controller_state(int controller, sf_gamepad_controller_state *state)
{
	*state = {};
	state->last_button = -1;
	if (controller >= 0 && controller < SF_GAMEPAD_COUNT)
		*state = controllers[controller];
}
}

static void test_controller_dialog()
{
	controller_action->trigger();
	controller_action->trigger();
	auto dialogs = main_window->findChildren<QDialog *>("shaderfilterControllerDialog");
	check(dialogs.size() == 1, "Duplicate controller inspector");
	auto *dialog = dialogs[0];
	auto *tabs = dialog->findChild<QTabBar *>("controllerIndex");
	auto *status = dialog->findChild<QLabel *>("controllerStatus");
	auto *timer = dialog->findChild<QTimer *>("controllerRefreshTimer");
	auto *buttons = dialog->findChild<QTableWidget *>("controllerButtons");
	auto *axes = dialog->findChild<QTableWidget *>("controllerAxes");
	auto *scroll = dialog->findChild<QScrollArea *>("controllerScrollArea");
	auto refresh = [timer] {
		QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
	};
	check(tabs && status && timer && buttons && axes && scroll, "Incomplete controller inspector");
	check(tabs->count() == 0 && status->text().contains("Unavailable"), "Unavailable input state missing");
	gamepads_available = true;
	refresh();
	check(tabs->count() == 0 && status->text().contains("None"), "Disconnected input state missing");
	controllers[2].connected = controllers[15].connected = true;
	controllers[2].last_button = -1;
	controllers[15].last_button = 14;
	controllers[15].buttons[14] = true;
	controllers[15].presses[14] = UINT64_MAX;
	controllers[15].axes[0] = -1.0f;
	controllers[15].axes[1] = 1.0f;
	controllers[15].axes[5] = -1.0f;
	controllers[15].axes_c[5] = 0.123456789012345;
	refresh();
	check(tabs->count() == 2 && tabs->tabData(0).toInt() == 2 && tabs->tabData(1).toInt() == 15,
	      "Controller tabs lost sparse slot IDs");
	tabs->setCurrentIndex(1);
	check(tabs->tabText(1) == "Controller 15", "Controller ID not labeled");
	check(buttons->rowCount() == 15 && axes->rowCount() == 6, "Missing controller rows");
	check(buttons->item(14, 3)->text() == "18446744073709551615", "Inspector truncated native button count");
	check(buttons->item(14, 2)->text().contains("Pressed"), "Button state missing");
	check(axes->item(0, 2)->text().toDouble() == -1.0 && axes->item(1, 2)->text().toDouble() == 1.0 &&
		      axes->item(5, 2)->text().toDouble() == -1.0,
	      "Signed axis range changed in UI");
	check(std::abs(axes->item(5, 3)->text().toDouble() - controllers[15].axes_c[5]) < 1e-15,
	      "Inspector lost cumulative precision");
	check(dialog->findChild<QLabel *>("controllerLastButton")->text().startsWith("Last: 14"), "Last button missing");
	dialog->resize(700, 320);
	QCoreApplication::processEvents();
	check(scroll->verticalScrollBar()->maximum() > 0, "Controller page does not scroll in a small window");
	scroll->verticalScrollBar()->setValue(100);
	tabs->setCurrentIndex(0);
	check(scroll->verticalScrollBar()->value() == 0, "Another controller inherited scroll position");
	tabs->setCurrentIndex(1);
	check(scroll->verticalScrollBar()->value() == 100, "Controller scroll position not restored");
	controllers[2].connected = false;
	controllers[0].connected = true;
	refresh();
	check(tabs->tabData(tabs->currentIndex()).toInt() == 15 && tabs->count() == 2, "Hotplug changed selected controller");
	controllers[15].connected = false;
	refresh();
	check(tabs->count() == 1 && tabs->tabData(0).toInt() == 0 && buttons->item(14, 3)->text() == "0",
	      "Disconnected page retained another controller's values");
	dialog->hide();
	check(!timer->isActive(), "Hidden inspector still refreshes");
	const int reads = snapshot_reads;
	QEventLoop pause;
	QTimer::singleShot(80, &pause, &QEventLoop::quit);
	pause.exec();
	check(snapshot_reads == reads, "Hidden inspector still reads controller state");
	dialog->show();
	check(timer->isActive(), "Reopened inspector did not resume");
	dialog->close();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	check(main_window->findChildren<QDialog *>("shaderfilterControllerDialog").empty(), "Inspector survived close");
	controller_action->trigger(); // Leave open to verify frontend-exit cleanup as well.
}

int main(int argc, char **argv)
{
	qputenv("QT_QPA_PLATFORM", "minimal");
	QApplication app(argc, argv);
	QWidget window;
	main_window = &window;
	sf_tools_init();
	sf_tools_init();
	check(menu_count == 1, "Missing or duplicate controller Tools action");
	test_controller_dialog();
	check(event_callback, "Missing frontend exit callback");
	event_callback(OBS_FRONTEND_EVENT_EXIT, event_context);
	check(!event_callback && !controller_action && window.findChildren<QDialog *>().empty(), "Exit left UI/callbacks alive");
	sf_tools_shutdown();
	std::puts("Controller Tools dialog and exit cleanup tests passed");
}
