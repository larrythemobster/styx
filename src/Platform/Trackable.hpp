#pragma once

#include "Signal.hpp"
#include <memory>
#include <utility>
#include <vector>

namespace Styx::Platform {

class Trackable {
public:
    Trackable() = default;
    virtual ~Trackable() {
        lifetime_.reset();
        destroyed.publish();
        connections_.clear();
    }
    Trackable(const Trackable &) = delete;
    Trackable &operator=(const Trackable &) = delete;
    std::weak_ptr<void> lifetime() const { return lifetime_; }
    void track(Connection connection) {
        connection.setAutoDisconnect(true);
        connections_.push_back(std::move(connection));
    }
    Signal<> destroyed;
private:
    std::shared_ptr<void> lifetime_ = std::make_shared<int>(0);
    std::vector<Connection> connections_;
};

template<class T> class Guard {
public:
    Guard() = default;
    Guard(std::nullptr_t) {}
    Guard(T *pointer) { reset(pointer); }
    Guard &operator=(T *pointer) { reset(pointer); return *this; }
    void reset(T *pointer = nullptr) {
        pointer_ = pointer;
        lifetime_ = pointer ? pointer->lifetime() : std::weak_ptr<void>{};
    }
    T *get() const { return lifetime_.expired() ? nullptr : pointer_; }
    T *operator->() const { return get(); }
    T &operator*() const { return *get(); }
    explicit operator bool() const { return get() != nullptr; }
private:
    T *pointer_ = nullptr;
    std::weak_ptr<void> lifetime_;
};

}
