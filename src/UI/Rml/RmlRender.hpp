#pragma once
#include "RmlUi_Renderer_GL3.h"
#include <vector>

namespace Styx::UI::Rml {
class RenderInterface final : public RenderInterface_GL3 {
public:
    RenderInterface();
    ::Rml::TextureHandle LoadTexture(::Rml::Vector2i &dimensions, const ::Rml::String &source) override;
    ::Rml::CompiledGeometryHandle CompileGeometry(::Rml::Span<const ::Rml::Vertex> vertices, ::Rml::Span<const int> indices) override;
    ::Rml::TextureHandle GenerateTexture(::Rml::Span<const ::Rml::byte> data, ::Rml::Vector2i dimensions) override;
    void ReleaseGeometry(::Rml::CompiledGeometryHandle handle) override;
    void ReleaseTexture(::Rml::TextureHandle handle) override;
    void flushReleasedResources();
private:
    void *ownerContext = nullptr;
    std::vector<::Rml::CompiledGeometryHandle> releasedGeometry;
    std::vector<::Rml::TextureHandle> releasedTextures;
};
}
