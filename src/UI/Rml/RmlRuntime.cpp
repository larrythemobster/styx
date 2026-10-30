#include "RmlRuntime.hpp"
#include "RmlRender.hpp"
#include "RmlView.hpp"
#include "Core/EmojiSegmenter.hpp"
#include "Core/ImageManager.hpp"
#include "Core/Logging.hpp"
#include "Platform/Desktop.hpp"
#include "Platform/Paths.hpp"
#include "Platform/Utf8.hpp"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Styx::UI::Rml {
namespace {
void *currentGLContext()
{
#ifdef _WIN32
    return wglGetCurrentContext();
#else
    return SDL_GL_GetCurrentContext();
#endif
}
std::filesystem::path resourcePath(std::string name)
{
    if (name.starts_with(":/")) name.erase(0, 2);
    if (name.starts_with("resources/")) name.erase(0, 10);
    if (name == "styx.svg") name = "app/styx.svg";
    auto path = Platform::utf8Path(name);
    if (path.is_absolute()) return path;
    if (std::filesystem::exists(path)) return path;
    if (auto exact = Platform::Resources::path(name); std::filesystem::exists(exact)) return exact;
    return Platform::Resources::path("ui/" + name);
}
std::string imageKey(const std::string &url, Platform::Size size)
{
    return std::format("{}|{}x{}", url, size.width, size.height);
}
Platform::ImageBuffer blank(Platform::Size size)
{
    if (size.empty()) size = {1, 1};
    return {size.width, size.height, 4, std::vector<uint8_t>(static_cast<size_t>(size.width) * size.height * 4)};
}
}
class Runtime::System final : public ::Rml::SystemInterface {
public:
    double GetElapsedTime() override { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
    bool LogMessage(::Rml::Log::Type type, const ::Rml::String &message) override
    {
        auto level = type == ::Rml::Log::LT_ERROR || type == ::Rml::Log::LT_ASSERT ? Core::LogLevel::Error
            : type == ::Rml::Log::LT_WARNING ? Core::LogLevel::Warning : Core::LogLevel::Debug;
        Core::log(level, LogUI) << "RmlUi:" << message;
        return true;
    }
    void SetMouseCursor(const ::Rml::String &name) override { cursor = name; }
    void SetClipboardText(const ::Rml::String &text) override { SDL_SetClipboardText(text.c_str()); }
    void GetClipboardText(::Rml::String &text) override { if (auto *value = SDL_GetClipboardText()) { text = value; SDL_free(value); } }
    std::string cursor;
private:
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};
class Runtime::Files final : public ::Rml::FileInterface {
public:
    ::Rml::FileHandle Open(const ::Rml::String &name) override
    {
        try {
            auto file = std::make_unique<std::ifstream>(resourcePath(name), std::ios::binary);
            if (!*file) return 0;
            return reinterpret_cast<::Rml::FileHandle>(file.release());
        } catch (const std::exception &error) {
            Core::log(Core::LogLevel::Warning, LogUI) << name << error.what();
            return 0;
        }
    }
    void Close(::Rml::FileHandle handle) override { delete reinterpret_cast<std::ifstream *>(handle); }
    size_t Read(void *buffer, size_t size, ::Rml::FileHandle handle) override
    {
        auto &file = *reinterpret_cast<std::ifstream *>(handle);
        file.read(static_cast<char *>(buffer), static_cast<std::streamsize>(size));
        return static_cast<size_t>(file.gcount());
    }
    bool Seek(::Rml::FileHandle handle, long offset, int origin) override
    {
        auto &file = *reinterpret_cast<std::ifstream *>(handle);
        file.clear();
        auto direction = origin == SEEK_SET ? std::ios::beg : origin == SEEK_CUR ? std::ios::cur : std::ios::end;
        file.seekg(offset, direction);
        return bool(file);
    }
    size_t Tell(::Rml::FileHandle handle) override
    {
        auto &file = *reinterpret_cast<std::ifstream *>(handle);
        if (file.eof()) file.clear();
        const auto position = file.tellg();
        return position >= 0 ? static_cast<size_t>(position) : 0;
    }
};
class Runtime::EmojiRenderer {
public:
    EmojiRenderer()
    {
        if (FT_Init_FreeType(&library)) throw std::runtime_error("Cannot initialise emoji font rasterizer");
        try {
            data = Platform::Resources::read("TwemojiCOLRv0.ttf");
            if (FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte *>(data.data()), static_cast<FT_Long>(data.size()), 0, &face))
                throw std::runtime_error("Cannot load Twemoji font");
            FT_Set_Pixel_Sizes(face, 0, 52);
            font = hb_ft_font_create_referenced(face);
        } catch (...) { if (face) FT_Done_Face(face); FT_Done_FreeType(library); throw; }
    }
    ~EmojiRenderer() { if (font) hb_font_destroy(font); if (face) FT_Done_Face(face); if (library) FT_Done_FreeType(library); }
    Platform::ImageBuffer render(const std::string &text)
    {
        auto *buffer = hb_buffer_create();
        hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
        hb_buffer_guess_segment_properties(buffer);
        hb_shape(font, buffer, nullptr, 0);
        unsigned count = 0;
        auto *infos = hb_buffer_get_glyph_infos(buffer, &count);
        auto *positions = hb_buffer_get_glyph_positions(buffer, &count);
        int advance = 0;
        for (unsigned i = 0; i < count; ++i) advance += positions[i].x_advance;
        auto result = blank({std::max(64, advance / 64 + 8), 72});
        int x = (result.width * 64 - advance) / 2;
        int y = 58 * 64;
        for (unsigned i = 0; i < count; ++i) {
            if (!FT_Load_Glyph(face, infos[i].codepoint, FT_LOAD_RENDER | FT_LOAD_COLOR)) {
                const auto &bitmap = face->glyph->bitmap;
                int left = (x + positions[i].x_offset) / 64 + face->glyph->bitmap_left;
                int top = (y - positions[i].y_offset) / 64 - face->glyph->bitmap_top;
                for (unsigned row = 0; row < bitmap.rows; ++row) {
                    int dy = top + static_cast<int>(row);
                    if (dy < 0 || dy >= result.height) continue;
                    const auto *source = bitmap.buffer + (bitmap.pitch >= 0 ? row : bitmap.rows - 1 - row) * std::abs(bitmap.pitch);
                    for (unsigned col = 0; col < bitmap.width; ++col) {
                        int dx = left + static_cast<int>(col);
                        if (dx < 0 || dx >= result.width) continue;
                        auto *pixel = result.pixels.data() + (static_cast<size_t>(dy) * result.width + dx) * 4;
                        uint8_t a = 0, r = 255, g = 255, b = 255;
                        if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
                            a = source[col * 4 + 3];
                            if (a) { b = static_cast<uint8_t>(std::min(255, source[col * 4] * 255 / a)); g = static_cast<uint8_t>(std::min(255, source[col * 4 + 1] * 255 / a)); r = static_cast<uint8_t>(std::min(255, source[col * 4 + 2] * 255 / a)); }
                        } else if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) a = source[col];
                        const unsigned alpha = a + (pixel[3] * (255 - a) + 127) / 255;
                        if (alpha) {
                            auto blend = [&](unsigned color, unsigned old) { return uint8_t((color * a + (old * pixel[3] * (255 - a) + 127) / 255 + alpha / 2) / alpha); };
                            pixel[0] = blend(r, pixel[0]); pixel[1] = blend(g, pixel[1]); pixel[2] = blend(b, pixel[2]); pixel[3] = static_cast<uint8_t>(alpha);
                        }
                    }
                }
            }
            x += positions[i].x_advance;
            y -= positions[i].y_advance;
        }
        hb_buffer_destroy(buffer);
        return result;
    }
private:
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    hb_font_t *font = nullptr;
    std::string data;
};

RenderInterface::RenderInterface() : ownerContext(currentGLContext()) {}
::Rml::TextureHandle RenderInterface::LoadTexture(::Rml::Vector2i &dimensions, const ::Rml::String &source)
{
    Platform::ImageBuffer image;
    try { image = Runtime::instance().loadImage(source); }
    catch (const std::exception &error) {
        Core::log(Core::LogLevel::Warning, LogUI) << "Could not load texture:" << source << error.what();
        return 0;
    }
    if (image.empty()) return 0;
    dimensions = {image.width, image.height};
    for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
        const unsigned alpha = image.pixels[i + 3];
        for (size_t c = 0; c < 3; ++c) image.pixels[i + c] = static_cast<uint8_t>((image.pixels[i + c] * alpha + 127) / 255);
    }
    return GenerateTexture({reinterpret_cast<const ::Rml::byte *>(image.pixels.data()), image.pixels.size()}, dimensions);
}
::Rml::CompiledGeometryHandle RenderInterface::CompileGeometry(::Rml::Span<const ::Rml::Vertex> vertices, ::Rml::Span<const int> indices)
{
    RenderContextScope current;
    return RenderInterface_GL3::CompileGeometry(vertices, indices);
}
::Rml::TextureHandle RenderInterface::GenerateTexture(::Rml::Span<const ::Rml::byte> data, ::Rml::Vector2i dimensions)
{
    RenderContextScope current;
    return RenderInterface_GL3::GenerateTexture(data, dimensions);
}
void RenderInterface::ReleaseGeometry(::Rml::CompiledGeometryHandle handle)
{
    if (ownerContext && currentGLContext() == ownerContext) RenderInterface_GL3::ReleaseGeometry(handle);
    else releasedGeometry.push_back(handle);
}
void RenderInterface::ReleaseTexture(::Rml::TextureHandle handle)
{
    if (ownerContext && currentGLContext() == ownerContext) RenderInterface_GL3::ReleaseTexture(handle);
    else releasedTextures.push_back(handle);
}
void RenderInterface::flushReleasedResources()
{
    if (!ownerContext || currentGLContext() != ownerContext) throw std::logic_error("Renderer releases require its owning GL context");
    for (auto handle : releasedGeometry) RenderInterface_GL3::ReleaseGeometry(handle);
    releasedGeometry.clear();
    for (auto handle : releasedTextures) RenderInterface_GL3::ReleaseTexture(handle);
    releasedTextures.clear();
}
Runtime::Runtime() = default;
Runtime::~Runtime() = default;
Runtime &Runtime::instance() { static Runtime value; return value; }
std::string Runtime::cursorName() const { return system ? system->cursor : std::string{}; }
bool Runtime::initialise(std::string *error)
{
    if (initialised) return true;
    ::Rml::String message;
    if (!RmlGL3::Initialize(&message)) { if (error) *error = message; return false; }
    system = std::make_unique<System>();
    files = std::make_unique<Files>();
    render = std::make_unique<RenderInterface>();
    if (!*render) { if (error) *error = "RmlUi GL3 shader compilation failed"; shutdown(); return false; }
    ::Rml::SetSystemInterface(system.get());
    ::Rml::SetFileInterface(files.get());
    ::Rml::SetRenderInterface(render.get());
    if (!::Rml::Initialise()) { if (error) *error = "RmlUi initialisation failed"; shutdown(); return false; }
    initialised = true;
    try { loadFonts(); emoji = std::make_unique<EmojiRenderer>(); }
    catch (const std::exception &failure) { if (error) *error = failure.what(); shutdown(); return false; }
    return true;
}
void Runtime::shutdown()
{
    if (initialised) { ::Rml::Shutdown(); initialised = false; }
    if (render) { render->flushReleasedResources(); render.reset(); RmlGL3::Shutdown(); }
    emoji.reset();
    fontData.clear();
    files.reset();
    system.reset();
    changedTextures.clear();
    frames.clear();
}
void Runtime::loadFonts()
{
    using S = ::Rml::Style::FontStyle;
    using W = ::Rml::Style::FontWeight;
    struct Font { std::filesystem::path path; const char *family; S style; W weight; bool fallback; };
    std::vector<Font> fonts;
#ifdef _WIN32
    const auto root = Platform::utf8Path(SDL_getenv("WINDIR") ? SDL_getenv("WINDIR") : "C:/Windows") / "Fonts";
    fonts = {{root / "segoeui.ttf", "ui", S::Normal, W::Normal, false}, {root / "segoeuii.ttf", "ui", S::Italic, W::Normal, false},
        {root / "seguisb.ttf", "ui", S::Normal, W(600), false}, {root / "segoeuib.ttf", "ui", S::Normal, W::Bold, false},
        {root / "segoeuiz.ttf", "ui", S::Italic, W::Bold, false}, {root / "consola.ttf", "mono", S::Normal, W::Normal, false},
        {root / "consolab.ttf", "mono", S::Normal, W::Bold, false}, {root / "seguisym.ttf", "symbols", S::Normal, W::Normal, true},
        {root / "msyh.ttc", "cjk", S::Normal, W::Normal, true}};
#elif defined(__APPLE__)
    fonts = {{"/System/Library/Fonts/Supplemental/Arial.ttf", "ui", S::Normal, W::Normal, false},
        {"/System/Library/Fonts/Supplemental/Arial Bold.ttf", "ui", S::Normal, W::Bold, false},
        {"/System/Library/Fonts/Menlo.ttc", "mono", S::Normal, W::Normal, false},
        {"/System/Library/Fonts/PingFang.ttc", "cjk", S::Normal, W::Normal, true}};
#else
    fonts = {{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "ui", S::Normal, W::Normal, false},
        {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "ui", S::Normal, W::Bold, false},
        {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf", "ui", S::Italic, W::Normal, false},
        {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "mono", S::Normal, W::Normal, false},
        {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "cjk", S::Normal, W::Normal, true}};
#endif
    bool loaded = false;
    for (const auto &font : fonts) {
        if (!std::filesystem::exists(font.path)) continue;
        fontData.push_back(Platform::Resources::readFile(font.path));
        const auto &data = fontData.back();
        bool ok = ::Rml::LoadFontFace({reinterpret_cast<const ::Rml::byte *>(data.data()), data.size()}, font.family, font.style, font.weight, font.fallback);
        loaded |= ok && std::string_view(font.family) == "ui";
    }
    if (!loaded) throw std::runtime_error("No usable system UI font found");
}
void Runtime::setImageManager(Core::ImageManager *manager)
{
    if (images.get() == manager) return;
    imageConnection.disconnect();
    images = manager;
    if (manager) imageConnection = manager->imageFetched.connect([this](const std::string &url, const Platform::Size &size, const Platform::ImageBuffer &) { onImageFetched(url, size); });
}
std::string Runtime::imageSource(const std::string &url, Platform::Size size, Core::Snowflake accountId)
{
    if (url.empty()) return {};
    auto key = imageKey(url, size) + "|" + accountId.toString();
    auto [it, inserted] = imageIds.try_emplace(key, static_cast<int>(imageIds.size()) + 1);
    if (inserted) imageRequests.emplace(it->second, ImageRequest{url, size, accountId});
    return "img:" + std::to_string(it->second);
}
void Runtime::onImageFetched(const std::string &url, Platform::Size size)
{
    if (!initialised) return;
    for (const auto &[id, request] : imageRequests) if (request.url == url && request.size == size) changedTextures.insert("img:" + std::to_string(id));
    texturesChanged.publish();
}
void Runtime::setFrame(const std::string &key, const Platform::ImageBuffer &frame)
{
    auto it = frames.find(key);
    if (it == frames.end() || it->second.width != frame.width || it->second.height != frame.height) textureSizesChanged = true;
    frames.insert_or_assign(key, frame);
    changedTextures.insert("frame:" + key);
    texturesChanged.publish();
}
void Runtime::clearFrame(const std::string &key) { frames.erase(key); changedTextures.insert("frame:" + key); texturesChanged.publish(); }
bool Runtime::releaseChangedTextures()
{
    for (const auto &source : changedTextures) ::Rml::ReleaseTexture(source);
    changedTextures.clear();
    return std::exchange(textureSizesChanged, false);
}
std::string Runtime::emojiSource(const std::string &text)
{
    std::string result = "emoji:";
    for (auto cp : Platform::codepoints(text)) {
        if (result.size() > 6) result += '-';
        result += std::format("{:x}", static_cast<uint32_t>(cp));
    }
    return result;
}
Platform::ImageBuffer Runtime::loadImage(const std::string &source)
{
    if (source.starts_with("img:")) {
        int id = 0;
        auto [end, ec] = std::from_chars(source.data() + 4, source.data() + source.size(), id);
        auto it = imageRequests.find(id);
        if (ec != std::errc{} || it == imageRequests.end() || !images) return {};
        auto &request = it->second;
        if (!images->isCached(request.url, request.size)) { images->get(request.url, request.size, request.accountId); return blank(request.size); }
        return images->get(request.url, request.size, request.accountId);
    }
    if (source.starts_with("frame:")) {
        auto it = frames.find(source.substr(6));
        return it != frames.end() ? it->second : blank({1, 1});
    }
    if (source.starts_with("icon:")) {
        const auto name = source.substr(5);
        if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos) return {};
        auto svg = Platform::Resources::read("icons/lucide/" + name + ".svg");
        svg = Platform::replace(svg, "currentColor", "#ffffff");
        auto image = Platform::Image::decode(reinterpret_cast<const uint8_t *>(svg.data()), svg.size());
        return image ? Platform::Image::scale(*image, 48, 48) : Platform::ImageBuffer{};
    }
    if (source.starts_with("emoji:")) {
        std::vector<char32_t> points;
        for (const auto &part : Platform::split(std::string_view(source).substr(6), '-')) {
            uint32_t cp = 0;
            auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), cp, 16);
            if (error != std::errc{} || end != part.data() + part.size() || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return {};
            points.push_back(static_cast<char32_t>(cp));
        }
        return emoji ? emoji->render(Platform::fromCodepoints(points)) : Platform::ImageBuffer{};
    }
    auto decoded = Platform::Image::decodeFile(resourcePath(source));
    return decoded.value_or(Platform::ImageBuffer{});
}
std::string escape(const std::string &plain) { return Platform::replace(Platform::htmlEscape(plain), "\n", "<br/>"); }
std::string richText(const std::string &plain)
{
    const auto sequences = Core::extractEmojiSequences(plain);
    if (sequences.empty()) return escape(plain);
    std::string out;
    size_t offset = 0;
    for (const auto &sequence : sequences) {
        const auto at = plain.find(sequence, offset);
        if (at == std::string::npos) continue;
        out += escape(plain.substr(offset, at - offset));
        out += "<img class=\"emoji\" src=\"" + Runtime::emojiSource(sequence) + "\"/>";
        offset = at + sequence.size();
    }
    out += escape(plain.substr(offset));
    return out;
}
}
