#include "Paths.hpp"

#include <system_error>

namespace Styx::Platform::Paths {

void ensureDirExists(const std::filesystem::path &path)
{
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
}

} // namespace Styx::Platform::Paths
