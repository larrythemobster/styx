#pragma once

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>

inline bool hardwareTestsAreAskedFor()
{
    const char *value = std::getenv("STYX_HARDWARE_TESTS");
    return value && std::atoi(value) > 0;
}

#define SKIP_UNLESS_HARDWARE_IS_ASKED_FOR() \
    if (!hardwareTestsAreAskedFor())        \
    SKIP("needs this machine's own hardware, which STYX_HARDWARE_TESTS=1 asks for")
