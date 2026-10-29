#include "FrecencyTracker.hpp"

#include <algorithm>
#include <cmath>

#include <chrono>

namespace Styx {
namespace Core {

namespace {

constexpr double MS_PER_DAY = 86400000.0;

uint64_t lastUse(const FrecencyTracker::Entry &entry)
{
    return entry.recentUses.empty() ? 0 : entry.recentUses.back();
}

int32_t computeFrecency(FrecencyTracker::Formula formula, const FrecencyTracker::Entry &entry,
                        uint32_t maxTotalUse)
{
    switch (formula) {
    case FrecencyTracker::Formula::Chat:
        return static_cast<int32_t>(
                std::ceil(entry.totalUses * (entry.score / entry.recentUses.size())));
    case FrecencyTracker::Formula::Reaction: {
        if (maxTotalUse == 0)
            return 0;
        const double ratio = static_cast<double>(entry.totalUses) / maxTotalUse;
        return static_cast<int32_t>(std::trunc(1000.0 * (ratio * 0.2 + (entry.score / 1000.0) * 0.8)));
    }
    }
    return 0;
}

} // namespace

int FrecencyTracker::defaultComputeWeight(int ageInDays)
{
    if (ageInDays <= 3)
        return 100;
    if (ageInDays <= 15)
        return 70;
    if (ageInDays <= 30)
        return 50;
    if (ageInDays <= 45)
        return 30;
    if (ageInDays <= 80)
        return 10;
    return 1;
}

FrecencyTracker::FrecencyTracker(Options options)
    : options(options)
{
}

FrecencyTracker::Options FrecencyTracker::chatEmojiOptions()
{
    return { 42, 10, Formula::Chat };
}

FrecencyTracker::Options FrecencyTracker::reactionEmojiOptions()
{
    return { 42, 10, Formula::Reaction };
}

int FrecencyTracker::ageInDays(uint64_t nowMs, uint64_t tMs)
{
    const double diff = (static_cast<double>(nowMs) - static_cast<double>(tMs)) / MS_PER_DAY;
    return static_cast<int>(std::trunc(diff));
}

void FrecencyTracker::markDirty()
{
    dirty = true;
}

void FrecencyTracker::track(const std::string &key, std::optional<uint64_t> timestampMs,
                            uint32_t usesSinceLastTrack)
{
    const uint64_t timestamp = timestampMs.value_or(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));
    auto it = history.find(key);
    if (it == history.end()) {
        Entry entry;
        entry.totalUses = usesSinceLastTrack;
        entry.recentUses.push_back(timestamp);
        history.emplace(key, std::move(entry));
    } else {
        Entry &entry = it->second;
        entry.frecency = -1;
        entry.totalUses += usesSinceLastTrack;
        entry.recentUses.push_back(timestamp);
        if (timestampMs)
            std::sort(entry.recentUses.begin(), entry.recentUses.end());
        while (static_cast<int>(entry.recentUses.size()) > options.maxSamples)
            entry.recentUses.erase(entry.recentUses.begin());
    }

    markDirty();
}

void FrecencyTracker::overwriteHistory(const std::unordered_map<std::string, Entry> &newHistory)
{
    history = newHistory;
    for (auto &[key, entry] : history) {
        entry.recentUses.erase(std::remove(entry.recentUses.begin(), entry.recentUses.end(), 0),
                               entry.recentUses.end());
        std::sort(entry.recentUses.begin(), entry.recentUses.end());
        entry.frecency = -1;
    }
    markDirty();
}

void FrecencyTracker::compute(uint64_t nowMs)
{
    uint32_t maxTotalUse = 0;
    for (const auto &[key, entry] : history)
        maxTotalUse = std::max(maxTotalUse, entry.totalUses);

    for (auto it = history.begin(); it != history.end();) {
        Entry &entry = it->second;
        if (entry.frecency != -1) {
            ++it;
            continue;
        }

        entry.score = 0;
        const int samples = std::min<int>(entry.recentUses.size(), options.maxSamples);
        for (int i = 0; i < samples; ++i)
            entry.score += defaultComputeWeight(ageInDays(nowMs, entry.recentUses.at(i)));

        // Every weight is positive, so a zero score means the entry has no samples left at all.
        if (entry.score <= 0) {
            it = history.erase(it);
            continue;
        }

        entry.frecency = computeFrecency(options.formula, entry, maxTotalUse);
        ++it;
    }

    std::vector<std::pair<std::string, int32_t>> ranked;
    ranked.reserve(history.size());
    for (const auto &[key, entry] : history)
        ranked.emplace_back(key, entry.frecency);

    // Discord sorts by -frecency only; the key tiebreak keeps our order deterministic.
    std::sort(ranked.begin(), ranked.end(),
              [](const std::pair<std::string, int32_t> &a, const std::pair<std::string, int32_t> &b) {
                  if (a.second != b.second)
                      return a.second > b.second;
                  return a.first < b.first;
              });

    const int count = std::min<int>(ranked.size(), std::max(options.numFrequentlyItems, 0));
    frequentlyKeys.clear();
    frequentlyKeys.reserve(count);
    for (int i = 0; i < count; ++i)
        frequentlyKeys.push_back(ranked.at(i).first);

    dirty = false;
}

void FrecencyTracker::refresh(uint64_t nowMs)
{
    if (dirty)
        compute(nowMs);
}

const std::vector<std::string> &FrecencyTracker::frequently(uint64_t nowMs)
{
    refresh(nowMs);
    return frequentlyKeys;
}

std::optional<double> FrecencyTracker::score(const std::string &key) const
{
    const auto it = history.find(key);
    if (it == history.end())
        return std::nullopt;
    return it->second.score;
}

std::optional<int32_t> FrecencyTracker::frecency(const std::string &key) const
{
    const auto it = history.find(key);
    if (it == history.end())
        return std::nullopt;
    return it->second.frecency;
}

void FrecencyTracker::reset()
{
    history.clear();
    frequentlyKeys.clear();
    dirty = false;
}

std::unordered_map<std::string, FrecencyTracker::Entry> FrecencyTracker::capForPersistence(const std::unordered_map<std::string, Entry> &source, int limit)
{
    const int keep = std::max(limit, 0);

    std::vector<std::pair<std::string, Entry>> entries;
    entries.reserve(source.size());
    for (const auto &[key, entry] : source)
        entries.emplace_back(key, entry);

    if (entries.size() > static_cast<size_t>(keep)) {
        std::sort(entries.begin(), entries.end(),
                  [](const std::pair<std::string, Entry> &a, const std::pair<std::string, Entry> &b) {
                      const uint64_t lhs = lastUse(a.second);
                      const uint64_t rhs = lastUse(b.second);
                      if (lhs != rhs)
                          return lhs > rhs;
                      return a.first < b.first;
                  });
        entries.erase(entries.begin() + keep, entries.end());
    }

    std::unordered_map<std::string, Entry> capped;
    capped.reserve(entries.size());
    for (const auto &pair : entries) {
        Entry entry = pair.second;
        entry.score = std::round(entry.score);
        capped.emplace(pair.first, std::move(entry));
    }

    return capped;
}

} // namespace Core
} // namespace Styx
