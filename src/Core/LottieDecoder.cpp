#include "LottieDecoder.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#ifdef STYX_HAVE_RLOTTIE
#include <rlottie.h>
#endif

namespace Styx {
namespace Core {
namespace Lottie {

bool looksLikeLottie(const std::string &data)
{
    const auto firstInk = std::find_if(data.cbegin(), data.cend(), [](char c) { return !std::isspace(static_cast<unsigned char>(c)); });
    return firstInk != data.cend() && *firstInk == '{';
}

#ifdef STYX_HAVE_RLOTTIE

namespace {
constexpr double MaxRenderedFps = 30.0;
} // namespace

struct Animation::Impl
{
    std::unique_ptr<rlottie::Animation> animation;
    int sourceFrames = 0;
    int sourceFramesPerRendered = 1;
    double sourceFps = 0;
};

bool isSupported()
{
    return true;
}

std::unique_ptr<Animation> Animation::fromJson(const std::string &json)
{
    const std::string modelCacheKey;
    const std::string externalResourceDir;
    auto animation = rlottie::Animation::loadFromData(json, modelCacheKey, externalResourceDir, false);
    if (!animation || animation->totalFrame() == 0 || animation->frameRate() <= 0)
        return nullptr;

    auto impl = std::make_unique<Impl>();
    impl->sourceFrames = int(animation->totalFrame());
    impl->sourceFps = animation->frameRate();
    impl->sourceFramesPerRendered = std::max(1, int(std::lround(impl->sourceFps / MaxRenderedFps)));
    impl->animation = std::move(animation);
    return std::unique_ptr<Animation>(new Animation(std::move(impl)));
}

int Animation::frameCount() const
{
    return (impl->sourceFrames + impl->sourceFramesPerRendered - 1) / impl->sourceFramesPerRendered;
}

int Animation::frameDelayMs() const
{
    return int(std::lround(impl->sourceFramesPerRendered * 1000.0 / impl->sourceFps));
}

Platform::ImageBuffer Animation::render(int frame, const Platform::Size &size) const
{
    if (size.width <= 0 || size.height <= 0)
        return {};
    std::vector<uint32_t> argb(static_cast<size_t>(size.width) * size.height);
    rlottie::Surface surface(argb.data(), size.width, size.height, size.width * sizeof(uint32_t));
    impl->animation->renderSync(size_t(frame * impl->sourceFramesPerRendered), surface);
    Platform::ImageBuffer image;
    image.width = size.width;
    image.height = size.height;
    image.pixels.resize(argb.size() * 4);
    for (size_t i = 0; i < argb.size(); ++i) {
        uint32_t pixel = argb[i];
        unsigned alpha = pixel >> 24;
        auto unpremultiply = [alpha](unsigned value) {
            return static_cast<uint8_t>(alpha ? std::min(255u, (value * 255u + alpha / 2) / alpha) : 0);
        };
        image.pixels[i * 4] = unpremultiply((pixel >> 16) & 255);
        image.pixels[i * 4 + 1] = unpremultiply((pixel >> 8) & 255);
        image.pixels[i * 4 + 2] = unpremultiply(pixel & 255);
        image.pixels[i * 4 + 3] = static_cast<uint8_t>(alpha);
    }
    return image;
}

#else

struct Animation::Impl
{
};

bool isSupported()
{
    return false;
}

std::unique_ptr<Animation> Animation::fromJson(const std::string &)
{
    return nullptr;
}

int Animation::frameCount() const
{
    return 0;
}

int Animation::frameDelayMs() const
{
    return 0;
}

Platform::ImageBuffer Animation::render(int, const Platform::Size &) const
{
    return {};
}

#endif

Animation::Animation(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}

Animation::~Animation() = default;

} // namespace Lottie
} // namespace Core
} // namespace Styx
