#include "ImageViewerController.hpp"

#include "Shell.hpp"
#include "RmlView.hpp"
#include "Core/ImageManager.hpp"
#include "Platform/Desktop.hpp"
#include "Platform/Image.hpp"
#include "Platform/Trackable.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <filesystem>

namespace Styx::UI::Rml {

ImageViewerController::ImageViewerController(Shell &shell)
    : Controller(shell, nullptr)
{
}

ImageViewerController::~ImageViewerController()
{
    if (viewerDocument)
        viewerDocument->Close();
}

void ImageViewerController::ensureDocument()
{
    if (viewerDocument || !shell.view() || !shell.view()->context())
        return;

    viewerDocument = shell.view()->context()->LoadDocument(":/ui/image_viewer.rml");
    if (!viewerDocument)
        return;
    regionRoot = viewerDocument;

    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (!target)
            return;
        const auto &id = target->GetId();
        if (id == "viewer-btn-close") {
            event.StopPropagation();
            hide();
        } else if (id == "viewer-btn-browser") {
            event.StopPropagation();
            if (!currentUrl.empty())
                Platform::Desktop::openUrl(currentUrl);
        } else if (id == "viewer-btn-copy") {
            event.StopPropagation();
            if (!currentUrl.empty())
                Platform::Desktop::setClipboardText(currentUrl);
        } else if (id == "viewer-btn-save") {
            event.StopPropagation();
            if (currentUrl.empty() || !shell.images())
                return;
            const auto url = Core::ImageManager::fullQualityUrl(currentUrl);
            const auto account = shell.location().accountId;
            const auto name = std::filesystem::path(Platform::urlPath(url)).filename().string();
            Platform::Guard<Core::ImageManager> images(shell.images());
            Platform::Desktop::saveFile("Save Image", name, [images, url, account](std::vector<std::string> paths) {
                if (images && !paths.empty() && !paths.front().empty())
                    images->download(url, account, paths.front());
            });
        } else if (id == "viewer-nav-prev") {
            event.StopPropagation();
            prevImage();
        } else if (id == "viewer-nav-next") {
            event.StopPropagation();
            nextImage();
        } else if (id == "viewer-backdrop" || id == "viewer-viewport" || id == "image-viewer-root") {
            event.StopPropagation();
            hide();
        }
    }, viewerDocument);

    listen("mousedown", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (target && target->GetId() == "viewer-image" && event.GetParameter<int>("button", 0) == 0) {
            isDragging = true;
            lastMousePos = {static_cast<float>(event.GetParameter<int>("mouse_x", 0)),
                            static_cast<float>(event.GetParameter<int>("mouse_y", 0))};
            event.StopPropagation();
        }
    }, viewerDocument);

    listen("mousemove", [this](::Rml::Event &event) {
        if (!isDragging)
            return;
        const Platform::Point pos{static_cast<float>(event.GetParameter<int>("mouse_x", 0)),
                                  static_cast<float>(event.GetParameter<int>("mouse_y", 0))};
        panOffset.x += pos.x - lastMousePos.x;
        panOffset.y += pos.y - lastMousePos.y;
        lastMousePos = pos;
        updateTransform();
        event.StopPropagation();
    }, viewerDocument);
    listen("mouseup", [this](::Rml::Event &) { isDragging = false; }, viewerDocument);
    listen("mousescroll", [this](::Rml::Event &event) {
        const float delta = event.GetParameter<float>("wheel_delta_y", 0.0f);
        if (delta != 0.0f) {
            zoomLevel = std::clamp(zoomLevel * (delta < 0.0f ? 1.15f : 1.0f / 1.15f), 0.1f, 20.0f);
            updateTransform();
            event.StopPropagation();
        }
    }, viewerDocument);
    listen("keydown", [this](::Rml::Event &event) {
        const auto key = static_cast<::Rml::Input::KeyIdentifier>(event.GetParameter<int>("key_identifier", 0));
        if (key == ::Rml::Input::KI_ESCAPE) {
            event.StopPropagation();
            hide();
        } else if (key == ::Rml::Input::KI_LEFT) {
            event.StopPropagation();
            prevImage();
        } else if (key == ::Rml::Input::KI_RIGHT) {
            event.StopPropagation();
            nextImage();
        } else if (key == ::Rml::Input::KI_R) {
            event.StopPropagation();
            resetView();
            updateTransform();
        } else if (key == ::Rml::Input::KI_OEM_PLUS || key == ::Rml::Input::KI_ADD) {
            event.StopPropagation();
            zoomLevel = std::min(zoomLevel * 1.25f, 20.0f);
            updateTransform();
        } else if (key == ::Rml::Input::KI_OEM_MINUS || key == ::Rml::Input::KI_SUBTRACT) {
            event.StopPropagation();
            zoomLevel = std::max(zoomLevel / 1.25f, 0.1f);
            updateTransform();
        }
    }, viewerDocument);
}

void ImageViewerController::resetView()
{
    const float winW = shell.view() ? static_cast<float>(shell.view()->width()) : 1280.0f;
    const float winH = shell.view() ? static_cast<float>(shell.view()->height()) : 720.0f;
    panOffset = {};
    zoomLevel = 1.0f;
    if (!naturalImageSize.empty())
        zoomLevel = std::min({winW * 0.85f / naturalImageSize.width,
                              winH * 0.85f / naturalImageSize.height, 1.0f});
}

void ImageViewerController::updateTransform()
{
    if (!viewerDocument)
        return;
    auto *image = viewerDocument->GetElementById("viewer-image");
    if (!image)
        return;
    const float winW = shell.view() ? static_cast<float>(shell.view()->width()) : 1280.0f;
    const float winH = shell.view() ? static_cast<float>(shell.view()->height()) : 720.0f;
    const float width = (naturalImageSize.width > 0 ? naturalImageSize.width : 600) * zoomLevel;
    const float height = (naturalImageSize.height > 0 ? naturalImageSize.height : 400) * zoomLevel;
    image->SetProperty("width", std::to_string(width) + "px");
    image->SetProperty("height", std::to_string(height) + "px");
    image->SetProperty("left", std::to_string((winW - width) / 2.0f + panOffset.x) + "px");
    image->SetProperty("top", std::to_string((winH - height) / 2.0f + panOffset.y) + "px");
    requestRender();
}

void ImageViewerController::loadImage(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= galleryUrls.size() || !viewerDocument)
        return;
    currentIndex = index;
    currentUrl = galleryUrls[index];
    const auto generation = ++loadGeneration;
    if (auto *counter = viewerDocument->GetElementById("viewer-counter")) {
        counter->SetInnerRML(std::to_string(index + 1) + " of " + std::to_string(galleryUrls.size()));
        counter->SetClass("visible", galleryUrls.size() > 1);
    }
    if (auto *button = viewerDocument->GetElementById("viewer-nav-prev"))
        button->SetClass("visible", galleryUrls.size() > 1);
    if (auto *button = viewerDocument->GetElementById("viewer-nav-next"))
        button->SetClass("visible", galleryUrls.size() > 1);

    naturalImageSize = {800, 600};
    resetView();
    if (auto *image = viewerDocument->GetElementById("viewer-image"))
        image->SetAttribute("src", shell.imageSource(currentUrl, {1920, 1080}));
    updateTransform();

    if (auto *images = shell.images()) {
        Platform::Guard<ImageViewerController> self(this);
        images->fetch(Core::ImageManager::fullQualityUrl(currentUrl), shell.location().accountId, this,
                      [self, generation](const std::string &data) {
            if (!self || !self->visible || self->loadGeneration != generation)
                return;
            auto image = Platform::Image::decode(reinterpret_cast<const uint8_t *>(data.data()), data.size());
            if (image && !image->empty()) {
                self->naturalImageSize = {image->width, image->height};
                self->resetView();
                self->updateTransform();
            }
        });
    }
}

void ImageViewerController::nextImage()
{
    if (galleryUrls.size() > 1)
        loadImage((currentIndex + 1) % static_cast<int>(galleryUrls.size()));
}

void ImageViewerController::prevImage()
{
    if (galleryUrls.size() > 1)
        loadImage((currentIndex - 1 + static_cast<int>(galleryUrls.size())) % static_cast<int>(galleryUrls.size()));
}

void ImageViewerController::openImage(const std::string &url, const std::vector<std::string> &gallery)
{
    ensureDocument();
    if (!viewerDocument)
        return;
    galleryUrls = gallery;
    if (galleryUrls.empty() && !url.empty())
        galleryUrls.push_back(url);
    const auto found = std::find(galleryUrls.begin(), galleryUrls.end(), url);
    currentIndex = found == galleryUrls.end() ? 0 : static_cast<int>(found - galleryUrls.begin());
    visible = true;
    loadImage(currentIndex);
    viewerDocument->Show();
    viewerDocument->PullToFront();
    viewerDocument->SetClass("open", true);
    requestRender();
}

void ImageViewerController::hide()
{
    if (!viewerDocument || !visible)
        return;
    isDragging = false;
    ++loadGeneration;
    viewerDocument->SetClass("open", false);
    viewerDocument->Hide();
    visible = false;
    requestRender();
}

} // namespace Styx::UI::Rml
