#pragma once

#include "Core/Snowflake.hpp"
#include "Platform/Image.hpp"
#include "Platform/Trackable.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Styx::Core { class ImageManager; }
namespace Styx::UI::Rml {
class RenderInterface;
class Runtime : public Platform::Trackable {
public:
    static Runtime &instance();
    ~Runtime() override;
    bool initialise(std::string *error);
    void shutdown();
    bool isInitialised() const { return initialised; }
    RenderInterface *renderer() const { return render.get(); }
    std::string cursorName() const;
    void setImageManager(Core::ImageManager *manager);
    std::string imageSource(const std::string &url, Platform::Size logicalSize, Core::Snowflake accountId);
    static std::string emojiSource(const std::string &emoji);
    static std::string iconSource(const std::string &name) { return "icon:" + name; }
    void setFrame(const std::string &key, const Platform::ImageBuffer &frame);
    void clearFrame(const std::string &key);
    Platform::ImageBuffer loadImage(const std::string &source);
    bool releaseChangedTextures();
    Platform::Signal<> texturesChanged;
private:
    Runtime();
    void loadFonts();
    void onImageFetched(const std::string &url, Platform::Size size);
    struct ImageRequest { std::string url; Platform::Size size; Core::Snowflake accountId; };
    class System;
    class Files;
    class EmojiRenderer;
    bool initialised = false;
    std::unique_ptr<System> system;
    std::unique_ptr<Files> files;
    std::unique_ptr<RenderInterface> render;
    std::unique_ptr<EmojiRenderer> emoji;
    Platform::Guard<Core::ImageManager> images;
    Platform::Connection imageConnection;
    std::unordered_map<std::string, int> imageIds;
    std::unordered_map<int, ImageRequest> imageRequests;
    std::unordered_map<std::string, Platform::ImageBuffer> frames;
    std::unordered_set<std::string> changedTextures;
    bool textureSizesChanged = false;
    std::vector<std::string> fontData;
};
std::string richText(const std::string &plain);
std::string escape(const std::string &plain);
}
