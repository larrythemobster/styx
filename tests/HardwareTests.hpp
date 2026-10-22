#pragma once

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>

inline bool hardwareTestsAreAskedFor()
{
    const char *styx = std::getenv("STYX_HARDWARE_TESTS");
    if (styx && std::atoi(styx) > 0)
        return true;
    const char *acheron = std::getenv("ACHERON_HARDWARE_TESTS");
    return acheron && std::atoi(acheron) > 0;
}

#define SKIP_UNLESS_HARDWARE_IS_ASKED_FOR() \
    if (!hardwareTestsAreAskedFor())        \
    SKIP("needs this machine's own hardware, which STYX_HARDWARE_TESTS=1 asks for")
