#pragma once

#include <QDockWidget>
#include <QComboBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

class AudioLinkDock final : public QDockWidget {
	Q_OBJECT
public:
	explicit AudioLinkDock(QWidget* parent = nullptr);
	~AudioLinkDock() override = default;

private slots:
    	void onSourceChanged(int index);
	void refreshSources();

private:
	void buildUi();
	void populateSources();

	QWidget*      content_ = nullptr;
	QComboBox*    sourceList_ = nullptr;
	QPushButton*  refreshBtn_ = nullptr;

};
