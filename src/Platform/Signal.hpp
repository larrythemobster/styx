#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

#include "Dispatcher.hpp"

namespace Styx::Platform {

enum class ConnectionType {
    Direct,
    Queued
};
struct ConnectionState {
    std::atomic<bool> connected{true};
    bool autoDisconnect{true};
    std::function<void()> disconnectFn;

    void disconnect()
    {
        if (connected.exchange(false)) {
            if (disconnectFn) {
                auto fn = std::move(disconnectFn);
                fn();
            }
        }
    }
};

class Connection {
public:
    Connection() = default;

    explicit Connection(std::shared_ptr<ConnectionState> state)
        : m_state(std::move(state))
    {
    }

    ~Connection()
    {
        if (m_state && m_state->autoDisconnect) {
            disconnect();
        }
    }

    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    Connection(Connection &&other) noexcept
        : m_state(std::move(other.m_state))
    {
    }

    Connection &operator=(Connection &&other) noexcept
    {
        if (this != &other) {
            disconnect();
            m_state = std::move(other.m_state);
        }
        return *this;
    }

    void disconnect();
    bool isConnected() const;
    void setAutoDisconnect(bool autoDisconnect)
    {
        if (m_state) {
            m_state->autoDisconnect = autoDisconnect;
        }
    }

private:
    std::shared_ptr<ConnectionState> m_state;
};

inline void Connection::disconnect()
{
    if (m_state) {
        m_state->disconnect();
        m_state.reset();
    }
}

inline bool Connection::isConnected() const
{
    return m_state && m_state->connected.load();
}

template <typename... Args>
class Signal {
public:
    using Slot = std::function<void(Args...)>;

    Signal()
        : m_lifecycle(std::make_shared<SignalLifecycle>())
    {
    }

    ~Signal()
    {
        disconnectAll();
    }

    Signal(const Signal &) = delete;
    Signal &operator=(const Signal &) = delete;

    Connection connect(Slot slot, ConnectionType type = ConnectionType::Direct)
    {
        return connect(nullptr, std::move(slot), type);
    }

    Connection connect(const void *owner, Slot slot, ConnectionType type = ConnectionType::Direct)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        uint64_t id = m_nextSlotId++;

        auto entry = std::make_shared<SlotEntry>();
        entry->id = id;
        entry->owner = owner;
        entry->slot = std::move(slot);
        entry->type = type;
        entry->active = true;

        m_slots.push_back(entry);

        auto connState = std::make_shared<ConnectionState>();
        connState->autoDisconnect = (owner == nullptr);
        std::weak_ptr<SignalLifecycle> weakLife = m_lifecycle;
        std::weak_ptr<SlotEntry> weakEntry = entry;
        connState->disconnectFn = [this, weakLife, weakEntry]() {
            if (auto life = weakLife.lock()) {
                if (auto e = weakEntry.lock()) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    e->active = false;
                    if (m_emitDepth == 0) {
                        pruneInactive();
                    }
                }
            }
        };

        return Connection(std::move(connState));
    }

    void disconnect(const void *owner)
    {
        if (!owner) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto &entry : m_slots) {
            if (entry->owner == owner) {
                entry->active = false;
            }
        }
        if (m_emitDepth == 0) {
            pruneInactive();
        }
    }

    void disconnectAll()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto &entry : m_slots) {
            entry->active = false;
        }
        if (m_emitDepth == 0) {
            m_slots.clear();
        }
    }

    void publish(Args... args)
    {
        std::vector<std::shared_ptr<SlotEntry>> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_emitDepth;
            snapshot = m_slots;
        }

        for (const auto &entry : snapshot) {
            if (entry && entry->active && entry->slot) {
                if (entry->type == ConnectionType::Direct) {
                    entry->slot(args...);
                } else {
                    // Queued connection: forward to main dispatcher
                    auto tupleArgs = std::make_tuple(args...);
                    auto slotFn = entry->slot;
                    std::weak_ptr<SlotEntry> weakEntry = entry;
                    Dispatcher::main().post([weakEntry, slotFn = std::move(slotFn), tupleArgs = std::move(tupleArgs)]() mutable {
                        if (auto e = weakEntry.lock()) {
                            if (e->active && slotFn) {
                                std::apply(slotFn, tupleArgs);
                            }
                        }
                    });
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            --m_emitDepth;
            if (m_emitDepth == 0) {
                pruneInactive();
            }
        }
    }

    void emitQueued(Args... args)
    {
        auto tupleArgs = std::make_tuple(args...);
        std::weak_ptr<SignalLifecycle> weakLife = m_lifecycle;
        Dispatcher::main().post([this, weakLife, tupleArgs = std::move(tupleArgs)]() mutable {
            if (auto life = weakLife.lock()) {
                std::apply([this](auto &&... unpacked) {
                    publish(std::forward<decltype(unpacked)>(unpacked)...);
                }, tupleArgs);
            }
        });
    }

    void operator()(Args... args)
    {
        publish(std::forward<Args>(args)...);
    }

    size_t connectionCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t count = 0;
        for (const auto &entry : m_slots) {
            if (entry && entry->active) {
                ++count;
            }
        }
        return count;
    }

private:
    struct SlotEntry {
        uint64_t id{0};
        const void *owner{nullptr};
        Slot slot;
        ConnectionType type{ConnectionType::Direct};
        bool active{true};
    };

    struct SignalLifecycle {};

    void pruneInactive()
    {
        m_slots.erase(
            std::remove_if(m_slots.begin(), m_slots.end(), [](const std::shared_ptr<SlotEntry> &e) {
                return !e || !e->active;
            }),
            m_slots.end());
    }

    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<SlotEntry>> m_slots;
    uint64_t m_nextSlotId{1};
    int m_emitDepth{0};
    std::shared_ptr<SignalLifecycle> m_lifecycle;
};

} // namespace Styx::Platform
