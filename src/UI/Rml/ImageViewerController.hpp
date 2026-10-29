#pragma once

#include "Controller.hpp"
#include "Platform/Native.hpp"

#include <string>
#include <vector>

namespace Rml {
class ElementDocument;
}

namespace Styx::UI::Rml {

class ImageViewerController final : public Controller
{
public:
    explicit ImageViewerController(Shell &shell);
    ~ImageViewerController() override;

    void openImage(const std::string &url, const std::vector<std::string> &gallery = {});
    void hide();
    [[nodiscard]] bool isOpen() const { return visible; }

private:
    void ensureDocument();
    void loadImage(int index);
    void resetView();
    void updateTransform();
    void nextImage();
    void prevImage();

    ::Rml::ElementDocument *viewerDocument = nullptr;
    std::vector<std::string> galleryUrls;
    int currentIndex = 0;
    std::string currentUrl;
    Platform::Size naturalImageSize;
    Platform::Point panOffset;
    Platform::Point lastMousePos;
    float zoomLevel = 1.0f;
    unsigned long long loadGeneration = 0;
    bool isDragging = false;
    bool visible = false;
};

} // namespace Styx::UI::Rml
