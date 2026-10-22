#include "App.hpp"
#include "Platform/Dispatcher.hpp"

#include <algorithm>

namespace Styx {
App::App(int &argc, char **argv)
    : QApplication(argc, argv)
{
    auto &dispatcher = Platform::Dispatcher::main();

    m_dispatcherTimer.setSingleShot(true);
    connect(&m_dispatcherTimer, &QTimer::timeout, this, &App::pumpDispatcher);

    dispatcher.setWakeHook([this]() {
        QMetaObject::invokeMethod(this, &App::pumpDispatcher, Qt::QueuedConnection);
    });

    pumpDispatcher();
}

void App::pumpDispatcher()
{
    auto &d = Platform::Dispatcher::main();
    d.runPending();
    auto next = d.nextTimeout();
    if (next) {
        m_dispatcherTimer.start(std::max(0, static_cast<int>(next->count())));
    } else {
        m_dispatcherTimer.stop();
    }
}
} // namespace Styx
