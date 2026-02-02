#include "Dock.hpp"
#include "AudioTapManager.hpp"
#include "ShaderDispatcher.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QSlider>
#include <QGroupBox>
#include <QFormLayout>



static inline bool source_has_audio(obs_source_t* s)
{
	if (!s) return false;
	uint32_t flags = obs_source_get_output_flags(s);
	return (flags & OBS_SOURCE_AUDIO) != 0;
}

AudioLinkDock::AudioLinkDock(QWidget* parent)
	: QDockWidget(parent)
{
	setWindowTitle(tr("AudioLink"));
	setMinimumSize(QSize(360, 120));
	setAllowedAreas(Qt::AllDockWidgetAreas);

	buildUi();
	populateSources();
	if (sourceList_->count() > 0) {
		sourceList_->setCurrentIndex(0);
		onSourceChanged(0);
	}
}

void AudioLinkDock::onSourceChanged(int index)
{
    if (index < 0)
        return;

    // Extract UUID stored as item data
    QString uuid = sourceList_->itemData(index).toString();
    if (uuid.isEmpty())
        return;

    // Update the audio tap manager
    GetAudioTapManager()->SetActiveSource(uuid.toStdString());
}

void AudioLinkDock::buildUi()
{
	content_ = new QWidget(this);
	auto* v = new QVBoxLayout(content_);
	v->setContentsMargins(8, 8, 8, 8);
	v->setSpacing(8);

	auto* row = new QWidget(content_);
	auto* h = new QHBoxLayout(row);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(8);

	auto* label = new QLabel(tr("Audio source:"), row);
	sourceList_ = new QComboBox(row);
	sourceList_->setSizeAdjustPolicy(QComboBox::AdjustToContents);

	refreshBtn_ = new QPushButton(tr("Refresh"), row);
	connect(refreshBtn_, &QPushButton::clicked, this, &AudioLinkDock::refreshSources);

	connect(sourceList_, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, &AudioLinkDock::onSourceChanged);

	h->addWidget(label);
	h->addWidget(sourceList_, 1);
	h->addWidget(refreshBtn_);
	v->addWidget(row);



	setWidget(content_);
}

void AudioLinkDock::populateSources()
{
    // Remember the currently selected UUID (if any)
    QString prevUuid;
    int currentIndex = sourceList_->currentIndex();
    if (currentIndex >= 0)
        prevUuid = sourceList_->itemData(currentIndex).toString();

    sourceList_->clear();

    // Re-enumerate sources
    obs_enum_sources([](void* param, obs_source_t* src) -> bool {
        auto* combo = static_cast<QComboBox*>(param);
        if (source_has_audio(src)) {
            const char* name = obs_source_get_name(src);
            const char* uuid = obs_source_get_uuid(src);
            combo->addItem(
                QString::fromUtf8(name ? name : "(unnamed)"),
                QVariant(QString::fromUtf8(uuid ? uuid : ""))
            );
        }
        return true;
    }, sourceList_);

    // Try to restore previous selection if still present
    if (!prevUuid.isEmpty()) {
        int newIndex = sourceList_->findData(prevUuid);
        if (newIndex >= 0)
            sourceList_->setCurrentIndex(newIndex);
    }


}


void AudioLinkDock::refreshSources()
{
	populateSources();
}
