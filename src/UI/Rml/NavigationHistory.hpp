#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace Styx::UI {

template<typename Location>
class NavigationHistory {
public:
    explicit NavigationHistory(std::size_t capacity = 100) : capacity_(capacity ? capacity : 1) {}

    bool record(const Location &location)
    {
        if (!entries_.empty() && entries_[current_] == location) return false;
        if (!entries_.empty()) entries_.erase(entries_.begin() + current_ + 1, entries_.end());
        entries_.push_back(location);
        if (entries_.size() > capacity_) entries_.erase(entries_.begin());
        current_ = entries_.size() - 1;
        return true;
    }

    template<typename Predicate>
    std::optional<std::size_t> find(int direction, Predicate valid) const
    {
        if (entries_.empty()) return std::nullopt;
        auto index = static_cast<std::ptrdiff_t>(current_) + direction;
        while (index >= 0 && index < static_cast<std::ptrdiff_t>(entries_.size())) {
            if (valid(entries_[static_cast<std::size_t>(index)])) return static_cast<std::size_t>(index);
            index += direction;
        }
        return std::nullopt;
    }

    const Location &at(std::size_t index) const { return entries_[index]; }
    void select(std::size_t index) { current_ = index; }

private:
    std::vector<Location> entries_;
    std::size_t current_ = 0;
    std::size_t capacity_;
};

} // namespace Styx::UI
