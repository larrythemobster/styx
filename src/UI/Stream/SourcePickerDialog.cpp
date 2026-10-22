#include "SourcePickerDialog.hpp"

#include "Core/Stream/LoopbackCapture.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace Styx {
namespace UI {

namespace {

constexpr int SOURCE_INDEX_ROLE = Qt::UserRole + 1;
const QSize THUMBNAIL_SIZE(240, 135);

} // namespace

SourcePickerDialog::SourcePickerDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Share your screen"));
    resize(820, 560);

    auto *layout = new QVBoxLayout(this);

    tabs = new QTabWidget(this);
    screenList = makeList();
    windowList = makeList();
    tabs->addTab(screenList, tr("Screens"));
    tabs->addTab(windowList, tr("Applications"));
    layout->addWidget(tabs, 1);

    auto *options = new QHBoxLayout;
    options->addWidget(new QLabel(tr("Quality:"), this));
    qualityCombo = new QComboBox(this);
    QSettings settings;
    const QString savedQuality = settings.value(QStringLiteral("stream/quality"), QStringLiteral("720p30")).toString();
    for (const auto &preset : Core::Stream::StreamQuality::presets()) {
        qualityCombo->addItem(preset.label(), preset.id);
        if (preset.id == savedQuality)
            qualityCombo->setCurrentIndex(qualityCombo->count() - 1);
    }
    qualityCombo->setToolTip(tr("Discord allows 720p 30 fps without Nitro; higher presets may be refused."));
    options->addWidget(qualityCombo);
    options->addSpacing(16);

    audioCheck = new QCheckBox(tr("Share audio"), this);
    const bool audioSupported = Core::Stream::LoopbackCapture::isSupported();
    audioCheck->setEnabled(audioSupported);
    audioCheck->setChecked(audioSupported && settings.value(QStringLiteral("stream/audio"), true).toBool());
    audioCheck->setToolTip(audioSupported ? tr("A window shares its app's sound; a screen shares everything except this call.")
                                          : tr("Sharing audio needs Windows 10 version 2004 or later."));
    options->addWidget(audioCheck);
    options->addStretch();
    layout->addLayout(options);

    auto *buttons = new QDialogButtonBox(this);
    goButton = buttons->addButton(tr("Go Live"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        QSettings s;
        s.setValue(QStringLiteral("stream/quality"), qualityCombo->currentData().toString());
        s.setValue(QStringLiteral("stream/audio"), audioCheck->isChecked());
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(tabs, &QTabWidget::currentChanged, this, &SourcePickerDialog::updateGoButton);
    populate();
}

QListWidget *SourcePickerDialog::makeList()
{
    auto *list = new QListWidget(this);
    list->setViewMode(QListView::IconMode);
    list->setIconSize(THUMBNAIL_SIZE);
    list->setGridSize(THUMBNAIL_SIZE + QSize(24, 48));
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setWordWrap(true);
    list->setUniformItemSizes(true);
    connect(list, &QListWidget::itemSelectionChanged, this, &SourcePickerDialog::updateGoButton);
    connect(list, &QListWidget::itemDoubleClicked, this, [this] {
        if (goButton && goButton->isEnabled())
            goButton->click();
    });
    return list;
}

void SourcePickerDialog::populate()
{
    sources = Core::Stream::enumerateCaptureSources();
    const qint64 ownPid = QCoreApplication::applicationPid();

    QPixmap placeholder(THUMBNAIL_SIZE);
    placeholder.fill(QColor(0x20, 0x22, 0x25));

    for (int i = 0; i < sources.size(); ++i) {
        const auto &source = sources[i];
        if (source.kind == Core::Stream::CaptureSource::Kind::Window && source.processId == static_cast<quint32>(ownPid))
            continue; // sharing ourselves only shows the stream preview recursively
        QListWidget *list = source.kind == Core::Stream::CaptureSource::Kind::Monitor ? screenList : windowList;
        auto *item = new QListWidgetItem(QIcon(placeholder), source.title, list);
        item->setData(SOURCE_INDEX_ROLE, i);
        item->setToolTip(source.title);
        if (source.primary)
            list->setCurrentItem(item);
    }
    if (!screenList->currentItem() && screenList->count() > 0)
        screenList->setCurrentRow(0);

    // thumbnails off the UI thread: PrintWindow on a dozen windows takes a moment
    const auto snapshot = sources;
    auto *watcher = new QFutureWatcher<QList<QImage>>(this);
    connect(watcher, &QFutureWatcher<QList<QImage>>::finished, this, [this, watcher] {
        const QList<QImage> images = watcher->result();
        watcher->deleteLater();
        for (QListWidget *list : { screenList, windowList }) {
            for (int row = 0; row < list->count(); ++row) {
                QListWidgetItem *item = list->item(row);
                const int index = item->data(SOURCE_INDEX_ROLE).toInt();
                if (index < images.size() && !images[index].isNull())
                    item->setIcon(QIcon(QPixmap::fromImage(images[index])));
            }
        }
    });
    watcher->setFuture(QtConcurrent::run([snapshot] {
        QList<QImage> images;
        images.reserve(snapshot.size());
        for (const auto &source : snapshot)
            images.append(Core::Stream::captureThumbnail(source, THUMBNAIL_SIZE));
        return images;
    }));

    updateGoButton();
}

void SourcePickerDialog::updateGoButton()
{
    if (!goButton || !tabs)
        return;
    auto *list = qobject_cast<QListWidget *>(tabs->currentWidget());
    goButton->setEnabled(list && list->currentItem() && list->currentItem()->isSelected());
}

Core::Stream::CaptureSource SourcePickerDialog::selectedSource() const
{
    auto *list = qobject_cast<QListWidget *>(tabs->currentWidget());
    if (!list || !list->currentItem())
        return {};
    const int index = list->currentItem()->data(SOURCE_INDEX_ROLE).toInt();
    return index >= 0 && index < sources.size() ? sources[index] : Core::Stream::CaptureSource{};
}

Core::Stream::StreamQuality SourcePickerDialog::selectedQuality() const
{
    return Core::Stream::StreamQuality::byId(qualityCombo->currentData().toString());
}

bool SourcePickerDialog::includeAudio() const
{
    return audioCheck->isChecked();
}

} // namespace UI
} // namespace Styx
