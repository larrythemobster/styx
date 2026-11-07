#pragma once

#include "Dispatcher.hpp"
#include <atomic>
#include <memory>
#include <utility>

namespace Styx::Platform {

class Timer {
public:
    Timer() = default;
    ~Timer() { stop(); }
    Timer(const Timer &) = delete;
    Timer &operator=(const Timer &) = delete;
    void start(std::chrono::milliseconds interval, bool repeating, std::function<void()> callback) {
        stop();
        active_ = std::make_shared<std::atomic<bool>>(true);
        auto active = active_;
        id_ = Dispatcher::main().setTimer(interval, repeating, [active, repeating, callback = std::move(callback)] {
            if (!active->load()) return;
            if (!repeating) active->store(false);
            callback();
        });
    }
    void stop() {
        if (active_) active_->store(false);
        if (id_) Dispatcher::main().cancelTimer(std::exchange(id_, 0));
        active_.reset();
    }
    bool active() const { return active_ && active_->load(); }
private:
    TimerId id_ = 0;
    std::shared_ptr<std::atomic<bool>> active_;
};

}
