#include "shader-controller-dialog.hpp"
#include "shader-gamepad.h"

#include <obs-module.h>
#include <QTabBar>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QScrollBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

const char *button_names[SF_GAMEPAD_BUTTON_COUNT] = {
	"A / Cross",
	"B / Circle",
	"X / Square",
	"Y / Triangle",
	"ShaderFilter.Controller.LeftBumper",
	"ShaderFilter.Controller.RightBumper",
	"Back",
	"Start",
	"Guide",
	"ShaderFilter.Controller.LeftStick",
	"ShaderFilter.Controller.RightStick",
	"ShaderFilter.Controller.DpadUp",
	"ShaderFilter.Controller.DpadRight",
	"ShaderFilter.Controller.DpadDown",
	"ShaderFilter.Controller.DpadLeft",
};
const char *axis_names[SF_GAMEPAD_AXIS_COUNT] = {
	"ShaderFilter.Controller.LeftX",  "ShaderFilter.Controller.LeftY",       "ShaderFilter.Controller.RightX",
	"ShaderFilter.Controller.RightY", "ShaderFilter.Controller.LeftTrigger", "ShaderFilter.Controller.RightTrigger",
};

void set_cell(QTableWidget *table, int row, int column, const QString &value)
{
	auto *item = table->item(row, column);
	if (item->text() != value)
		item->setText(value);
}

QTableWidget *make_table(QWidget *parent, int rows, const QStringList &headers, const char *const *names)
{
	auto *table = new QTableWidget(rows, headers.size(), parent);
	table->setHorizontalHeaderLabels(headers);
	table->verticalHeader()->hide();
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setAlternatingRowColors(true);
	table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	for (int row = 0; row < rows; ++row) {
		for (int column = 0; column < headers.size(); ++column) {
			auto *item = new QTableWidgetItem;
			if (column != 1)
				item->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
			table->setItem(row, column, item);
		}
		set_cell(table, row, 0, QString::number(row));
		set_cell(table, row, 1, text(names[row]));
	}
	table->setFixedHeight(table->horizontalHeader()->sizeHint().height() + table->verticalHeader()->length() +
			      2 * table->frameWidth() + 4);
	return table;
}

class ControllerDialog final : public QDialog {
public:
	explicit ControllerDialog(QWidget *parent) : QDialog(parent)
	{
		setObjectName("shaderfilterControllerDialog");
		setAttribute(Qt::WA_DeleteOnClose);
		setWindowTitle(text("ShaderFilter.Controller.Tools"));
		resize(760, 640);
		auto *layout = new QVBoxLayout(this);
		auto *description = new QLabel(text("ShaderFilter.Controller.Description"), this);
		description->setWordWrap(true);
		layout->addWidget(description);
		controllers = new QTabBar(this);
		controllers->setObjectName("controllerIndex");
		controllers->setAccessibleName(text("ShaderFilter.Controller.Index"));
		controllers->setExpanding(false);
		controllers->setUsesScrollButtons(true);
		layout->addWidget(controllers);
		scroll = new QScrollArea(this);
		scroll->setObjectName("controllerScrollArea");
		scroll->setWidgetResizable(true);
		auto *page = new QWidget(scroll);
		page->setMinimumWidth(640);
		auto *page_layout = new QVBoxLayout(page);
		scroll->setWidget(page);
		layout->addWidget(scroll, 1);
		status = new QLabel(this);
		status->setObjectName("controllerStatus");
		status->setWordWrap(true);
		page_layout->addWidget(status);
		last_button = new QLabel(this);
		last_button->setObjectName("controllerLastButton");
		last_button->setTextInteractionFlags(Qt::TextSelectableByMouse);
		page_layout->addWidget(last_button);
		buttons = make_table(this, SF_GAMEPAD_BUTTON_COUNT,
				     {text("ShaderFilter.Controller.ButtonIndex"), text("ShaderFilter.Controller.Button"),
				      text("ShaderFilter.Controller.State"), text("ShaderFilter.Controller.Presses")},
				     button_names);
		buttons->setObjectName("controllerButtons");
		buttons->setAccessibleName(text("ShaderFilter.Controller.Buttons"));
		page_layout->addWidget(buttons);
		sticks = new QLabel(this);
		sticks->setObjectName("controllerSticks");
		sticks->setWordWrap(true);
		page_layout->addWidget(sticks);
		axes = make_table(this, SF_GAMEPAD_AXIS_COUNT,
				  {text("ShaderFilter.Controller.AxisIndex"), text("ShaderFilter.Controller.Axis"),
				   text("ShaderFilter.Controller.Value"), text("ShaderFilter.Controller.Cumulative")},
				  axis_names);
		axes->setObjectName("controllerAxes");
		axes->setAccessibleName(text("ShaderFilter.Controller.Axes"));
		page_layout->addWidget(axes);
		auto *hint = new QLabel(text("ShaderFilter.Controller.Hint"), this);
		hint->setWordWrap(true);
		page_layout->addWidget(hint);
		page_layout->addStretch();
		auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
		layout->addWidget(close);
		QObject::connect(close, &QDialogButtonBox::rejected, this, &QDialog::close);
		refresh_timer = new QTimer(this);
		refresh_timer->setObjectName("controllerRefreshTimer");
		refresh_timer->setInterval(50); // Display at 20 Hz; shared input still polls at 125 Hz.
		QObject::connect(refresh_timer, &QTimer::timeout, this, [this] { refresh(); });
		QObject::connect(controllers, &QTabBar::currentChanged, this, [this] { refresh(); });
	}

protected:
	void showEvent(QShowEvent *event) override
	{
		QDialog::showEvent(event);
		refresh();
		refresh_timer->start();
	}
	void hideEvent(QHideEvent *event) override
	{
		refresh_timer->stop();
		QDialog::hideEvent(event);
	}

private:
	void refresh()
	{
		sf_gamepad_snapshot snapshot;
		sf_gamepad_get_snapshot(&snapshot);
		const int selected = controllers->currentIndex() < 0 ? -1
								     : controllers->tabData(controllers->currentIndex()).toInt();
		// Preserve slot IDs and selection across hotplug; list only mapped controllers.
		{
			QSignalBlocker block(controllers);
			int index = 0;
			for (int controller = 0; controller < SF_GAMEPAD_COUNT; ++controller) {
				if (!snapshot.available || !snapshot.info[controller][0])
					continue;
				if (index >= controllers->count() || controllers->tabData(index).toInt() != controller) {
					controllers->insertTab(index, text("ShaderFilter.Controller.Slot").arg(controller));
					controllers->setTabData(index, controller);
				}
				++index;
			}
			while (controllers->count() > index)
				controllers->removeTab(index);
			int retained = -1;
			for (int tab = 0; tab < index; ++tab) {
				if (controllers->tabData(tab).toInt() == selected)
					retained = tab;
			}
			controllers->setCurrentIndex(retained >= 0 ? retained : (index ? 0 : -1));
		}
		const int controller = controllers->currentIndex() < 0 ? -1
								       : controllers->tabData(controllers->currentIndex()).toInt();
		if (controller != displayed_controller) {
			if (displayed_controller >= 0)
				scroll_positions[displayed_controller] = scroll->verticalScrollBar()->value();
			scroll->verticalScrollBar()->setValue(controller >= 0 ? scroll_positions[controller] : 0);
			displayed_controller = controller;
		}
		sf_gamepad_controller_state state;
		sf_gamepad_get_controller_state(controller, &state);
		const bool connected = snapshot.available && controller >= 0 && state.connected;
		controllers->setEnabled(controllers->count() > 0);
		status->setText(!snapshot.available ? text("ShaderFilter.Controller.Unavailable")
				: connected         ? text("ShaderFilter.Controller.Connected").arg(controller)
						    : text("ShaderFilter.Controller.None"));
		last_button->setText(connected && state.last_button >= 0 && state.last_button < SF_GAMEPAD_BUTTON_COUNT
					     ? text("ShaderFilter.Controller.Last")
						       .arg(state.last_button)
						       .arg(text(button_names[state.last_button]))
					     : text("ShaderFilter.Controller.LastNone"));
		for (int button = 0; button < SF_GAMEPAD_BUTTON_COUNT; ++button) {
			set_cell(buttons, button, 2,
				 connected ? text(state.buttons[button] ? "ShaderFilter.Controller.Pressed"
									: "ShaderFilter.Controller.Released")
					   : QString::fromUtf8("—"));
			set_cell(buttons, button, 3,
				 connected ? QString::number(static_cast<qulonglong>(state.presses[button]))
					   : QString::fromUtf8("—"));
		}
		for (int axis = 0; axis < SF_GAMEPAD_AXIS_COUNT; ++axis) {
			set_cell(axes, axis, 2, connected ? QString::number(state.axes[axis], 'f', 6) : QString::fromUtf8("—"));
			set_cell(axes, axis, 3, connected ? QString::number(state.axes_c[axis], 'g', 17) : QString::fromUtf8("—"));
		}
		sticks->setText(connected ? text("ShaderFilter.Controller.Sticks")
						    .arg(state.axes[0], 0, 'f', 6)
						    .arg(state.axes[1], 0, 'f', 6)
						    .arg(state.axes[2], 0, 'f', 6)
						    .arg(state.axes[3], 0, 'f', 6)
					  : QString());
	}
	QTabBar *controllers;
	QScrollArea *scroll;
	int displayed_controller = -1;
	int scroll_positions[SF_GAMEPAD_COUNT] = {};
	QLabel *status;
	QLabel *last_button;
	QLabel *sticks;
	QTableWidget *buttons;
	QTableWidget *axes;
	QTimer *refresh_timer;
};
} // namespace

QDialog *sf_create_controller_dialog(QWidget *parent)
{
	return new ControllerDialog(parent);
}
