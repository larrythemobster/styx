#pragma once

#include <QtWidgets>

namespace Styx {
namespace UI {

class SettingsWindow : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsWindow(QWidget *parent = nullptr);

signals:
    void channelListModeChanged(bool classic);
    void channelListIndentChanged();
    void closeToTrayChanged();
    void animateEmojiChanged(bool enabled);
    void animateStickersChanged(bool enabled);
    void gifSettingsChanged();
    void animationCacheLimitChanged(int megabytes);
    void animationSizeLimitChanged(int megabytes);

private:
    void setupUi();

    QListWidget *categoryList;
    QStackedWidget *pages;
};

} // namespace UI
} // namespace Styx
