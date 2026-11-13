#include "Badge.hpp"

namespace Styx::Platform::Badge {

// No dock badge on this platform yet.
void setCount(void *, int) {}
bool supported() { return false; }

} // namespace Styx::Platform::Badge
