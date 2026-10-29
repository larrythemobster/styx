#pragma once

#include "Platform/Image.hpp"
#include "Platform/Native.hpp"
#include <string>

#include <memory>

namespace Styx {
namespace Core {
namespace Lottie {

[[nodiscard]] bool isSupported();

[[nodiscard]] bool looksLikeLottie(const std::string &data);

class Animation
{
public:
    ~Animation();

    [[nodiscard]] static std::unique_ptr<Animation> fromJson(const std::string &json);

    [[nodiscard]] int frameCount() const;
    [[nodiscard]] int frameDelayMs() const;
    [[nodiscard]] Platform::ImageBuffer render(int frame, const Platform::Size &size) const;

private:
    struct Impl;
    explicit Animation(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl;
};

} // namespace Lottie
} // namespace Core
} // namespace Styx
