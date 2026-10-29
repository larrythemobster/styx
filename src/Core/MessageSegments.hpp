#pragma once

#include <deque>
#include <vector>

#include "Snowflake.hpp"

namespace Styx {
namespace Core {

class MessageSegments
{
public:
    struct Run
    {
        std::deque<Snowflake> ids;
        bool isTail = false;
    };

    struct Position
    {
        const Run *run = nullptr; // null when the id is unknown
        int index = -1;
    };

    [[nodiscard]] Position find(Snowflake id) const;
    [[nodiscard]] const Run *tail() const;

    void merge(const std::vector<Snowflake> &sortedIds, Snowflake anchorId, bool reachesPresent);

    void setTail(const std::vector<Snowflake> &sortedIds);

    void remove(Snowflake id);

private:
    struct Location
    {
        int run = -1;
        int index = -1;
    };
    [[nodiscard]] Location locate(Snowflake id) const;

    std::vector<Run> runs; // ordered by first id, non-overlapping
};

} // namespace Core
} // namespace Styx
