#pragma once

#include <QtCore/QTimer>
#include <QtWidgets/QApplication>

namespace Styx {
class App : public QApplication
{
    Q_OBJECT
public:
    explicit App(int &argc, char **argv);

public slots:
    void pumpDispatcher();

private:
    QTimer m_dispatcherTimer;
};
} // namespace Styx
