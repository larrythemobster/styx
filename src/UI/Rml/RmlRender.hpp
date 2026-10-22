#pragma once

#include "RmlUi_Renderer_GL3.h"

#include <QOpenGLContext>
#include <QPointer>

#include <vector>

namespace Styx {
namespace UI {
namespace Rml {

// RmlUi's GL3 renderer with texture sources resolved through Runtime::loadImage
// (Qt resources, icons, emoji, remote images, video frames) instead of TGA files.
class RenderInterface final : public RenderInterface_GL3
{
public:
    ::Rml::TextureHandle LoadTexture(::Rml::Vector2i &dimensions, const ::Rml::String &source) override;
    void ReleaseGeometry(::Rml::CompiledGeometryHandle handle) override;
    void ReleaseTexture(::Rml::TextureHandle handle) override;

    // DOM callbacks can run without GL current. Drain their releases in paintGL or shutdown.
    void flushReleasedResources();

private:
    QPointer<QOpenGLContext> ownerContext = QOpenGLContext::currentContext();
    std::vector<::Rml::CompiledGeometryHandle> releasedGeometry;
    std::vector<::Rml::TextureHandle> releasedTextures;

};
} // namespace Rml
} // namespace UI
} // namespace Styx
