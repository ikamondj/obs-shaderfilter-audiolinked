#include "shader-tools.h"
#include "shader-controller-dialog.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QPointer>

namespace {
QPointer<QAction> controller_action;
QPointer<QDialog> controller_dialog;
bool frontend_registered = false;

void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT)
		sf_tools_shutdown();
}

void show_controller_dialog()
{
	if (!controller_dialog)
		controller_dialog = sf_create_controller_dialog(static_cast<QWidget *>(obs_frontend_get_main_window()));
	controller_dialog->show();
	controller_dialog->raise();
	controller_dialog->activateWindow();
}
} // namespace

void sf_tools_init(void)
{
	if (controller_action || !qobject_cast<QApplication *>(QCoreApplication::instance()))
		return;
	controller_action =
		static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("ShaderFilter.Controller.Tools")));
	if (!controller_action) {
		blog(LOG_WARNING, "[obs-shaderfilter] Could not add controller lookup Tools action");
		return;
	}
	QObject::connect(controller_action, &QAction::triggered, controller_action, show_controller_dialog);
	obs_frontend_add_event_callback(frontend_event, nullptr);
	frontend_registered = true;
}

void sf_tools_shutdown(void)
{
	// EXIT is the last opportunity to call the frontend API; unload is idempotent.
	if (frontend_registered) {
		obs_frontend_remove_event_callback(frontend_event, nullptr);
		frontend_registered = false;
	}
	delete controller_dialog.data();
	delete controller_action.data();
}
