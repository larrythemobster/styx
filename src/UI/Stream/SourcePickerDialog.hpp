#pragma once

#include <QDialog>
#include <QList>

#include "Core/Stream/ScreenCapture.hpp"
#include "Core/Stream/StreamQuality.hpp"

class QCheckBox;
class QComboBox;
class QListWidget;
class QPushButton;
class QTabWidget;

namespace Acheron {
namespace UI {

// Pick what to Go Live with: a screen or a window (with live thumbnails), the quality
// preset, and whether to share audio.
class SourcePickerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SourcePickerDialog(QWidget *parent = nullptr);

    [[nodiscard]] Core::Stream::CaptureSource selectedSource() const;
    [[nodiscard]] Core::Stream::StreamQuality selectedQuality() const;
    [[nodiscard]] bool includeAudio() const;

private:
    QListWidget *makeList();
    void populate();
    void updateGoButton();

    QTabWidget *tabs = nullptr;
    QListWidget *screenList = nullptr;
    QListWidget *windowList = nullptr;
    QComboBox *qualityCombo = nullptr;
    QCheckBox *audioCheck = nullptr;
    QPushButton *goButton = nullptr;
    QList<Core::Stream::CaptureSource> sources;
};

} // namespace UI
} // namespace Acheron
