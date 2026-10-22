// Ported from ouwou/Acheron video-streams commit 71b4907 (GPL-3.0-or-later).
// PipeWire stream ownership is deliberately represented by LinuxGrabber so AVFrame references
// remain valid until the consumer releases them.
#include "Core/Video/LinuxCapture.hpp"

namespace Styx::Core::Video::PortalCapture {
// The portal implementation is selected only when the host provides both sdbus-c++ and
// PipeWire.  Keeping this translation unit present gives non-Linux builds the same API while
// Do not offer a portal source until the session protocol and PipeWire stream consumer are
// present together. Returning true here would send callers into a guaranteed failed open.
bool usable() {
    return false;
}
CaptureSource adopt(std::uint32_t node, FrameSize size) { return {"pipewire:" + std::to_string(node), CaptureSource::Kind::Screen, "Desktop portal", {0, 0, size.width, size.height}, 0}; }
FrameSize sizeOf(const CaptureSource &source) { return {source.geometry.width, source.geometry.height}; }
std::unique_ptr<LinuxGrabber> open(const std::string &, int, bool) { return nullptr; }
void release(const std::string &) {}
} // namespace Styx::Core::Video::PortalCapture
