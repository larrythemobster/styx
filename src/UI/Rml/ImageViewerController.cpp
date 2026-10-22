#include "ImageViewerController.hpp"

#include "Shell.hpp"
#include "RmlView.hpp"
#include "RmlRuntime.hpp"

#include "Core/ImageManager.hpp"

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QPixmap>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>

namespace Styx {
namespace UI {
namespace Rml {

ImageViewerController::ImageViewerController(Shell &shell, QObject *parent)
    : Controller(shell, nullptr, parent)
{
}

ImageViewerController::~ImageViewerController()
{
    if (viewerDocument) {
        viewerDocument->Close();
        viewerDocument = nullptr;
    }
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

        if (target->GetId() == "viewer-btn-close") {
            event.StopPropagation();
            hide();
            return;
        }

        if (target->GetId() == "viewer-btn-browser") {
            event.StopPropagation();
            if (currentUrl.isValid())
                QDesktopServices::openUrl(currentUrl);
            return;
        }

        if (target->GetId() == "viewer-btn-copy") {
            event.StopPropagation();
            if (currentUrl.isValid())
                QGuiApplication::clipboard()->setText(currentUrl.toString());
            return;
        }

        if (target->GetId() == "viewer-nav-prev") {
            event.StopPropagation();
            prevImage();
            return;
        }

        if (target->GetId() == "viewer-nav-next") {
            event.StopPropagation();
            nextImage();
            return;
        }

        // Clicking backdrop or viewport outside image closes
        if (target->GetId() == "viewer-backdrop" || target->GetId() == "viewer-viewport" || target->GetId() == "image-viewer-root") {
            event.StopPropagation();
            hide();
            return;
        }
    }, viewerDocument);

    listen("mousedown", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (target && target->GetId() == "viewer-image") {
            if (event.GetParameter<int>("button", 0) == 0) { // left button
                isDragging = true;
                lastMousePos = QPointF(static_cast<float>(event.GetParameter<int>("mouse_x", 0)),
                                       static_cast<float>(event.GetParameter<int>("mouse_y", 0)));
                event.StopPropagation();
            }
        }
    }, viewerDocument);

    listen("mousemove", [this](::Rml::Event &event) {
        if (isDragging) {
            QPointF pos(static_cast<float>(event.GetParameter<int>("mouse_x", 0)),
                        static_cast<float>(event.GetParameter<int>("mouse_y", 0)));
            panOffset += (pos - lastMousePos);
            lastMousePos = pos;
            updateTransform();
            event.StopPropagation();
        }
    }, viewerDocument);

    listen("mouseup", [this](::Rml::Event &) {
        isDragging = false;
    }, viewerDocument);

    listen("mousescroll", [this](::Rml::Event &event) {
        float wheelDelta = event.GetParameter<float>("wheel_delta_y", 0.0f);
        if (wheelDelta != 0.0f) {
            float factor = wheelDelta < 0.0f ? 1.15f : (1.0f / 1.15f);
            zoomLevel = std::clamp(zoomLevel * factor, 0.1f, 20.0f);
            updateTransform();
            event.StopPropagation();
        }
    }, viewerDocument);

    listen("keydown", [this](::Rml::Event &event) {
        const auto key = static_cast<::Rml::Input::KeyIdentifier>(
            event.GetParameter<int>("key_identifier", 0));
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
    float winW = shell.view() ? static_cast<float>(shell.view()->width()) : 1280.0f;
    float winH = shell.view() ? static_cast<float>(shell.view()->height()) : 720.0f;

    panOffset = QPointF(0.0f, 0.0f);
    zoomLevel = 1.0f;

    if (naturalImageSize.isValid() && naturalImageSize.width() > 0 && naturalImageSize.height() > 0) {
        float maxW = winW * 0.85f;
        float maxH = winH * 0.85f;
        float scaleX = maxW / static_cast<float>(naturalImageSize.width());
        float scaleY = maxH / static_cast<float>(naturalImageSize.height());
        zoomLevel = std::min({ scaleX, scaleY, 1.0f });
    }
}

void ImageViewerController::updateTransform()
{
    if (!viewerDocument)
        return;

    auto *imgEl = viewerDocument->GetElementById("viewer-image");
    if (!imgEl)
        return;

    float winW = shell.view() ? static_cast<float>(shell.view()->width()) : 1280.0f;
    float winH = shell.view() ? static_cast<float>(shell.view()->height()) : 720.0f;

    float imgW = naturalImageSize.isValid() && naturalImageSize.width() > 0 ? static_cast<float>(naturalImageSize.width()) : 600.0f;
    float imgH = naturalImageSize.isValid() && naturalImageSize.height() > 0 ? static_cast<float>(naturalImageSize.height()) : 400.0f;

    float currentW = imgW * zoomLevel;
    float currentH = imgH * zoomLevel;

    float left = (winW - currentW) / 2.0f + static_cast<float>(panOffset.x());
    float top = (winH - currentH) / 2.0f + static_cast<float>(panOffset.y());

    imgEl->SetProperty("width", QStringLiteral("%1px").arg(currentW).toStdString());
    imgEl->SetProperty("height", QStringLiteral("%1px").arg(currentH).toStdString());
    imgEl->SetProperty("left", QStringLiteral("%1px").arg(left).toStdString());
    imgEl->SetProperty("top", QStringLiteral("%1px").arg(top).toStdString());

    requestRender();
}

void ImageViewerController::loadImage(int index)
{
    if (galleryUrls.isEmpty() || index < 0 || index >= galleryUrls.size() || !viewerDocument)
        return;

    currentIndex = index;
    currentUrl = galleryUrls[currentIndex];

    // Counter
    if (auto *counter = viewerDocument->GetElementById("viewer-counter")) {
        counter->SetInnerRML(QStringLiteral("%1 of %2").arg(currentIndex + 1).arg(galleryUrls.size()).toStdString());
        counter->SetClass("visible", galleryUrls.size() > 1);
    }

    // Nav arrows
    if (auto *prevBtn = viewerDocument->GetElementById("viewer-nav-prev"))
        prevBtn->SetClass("visible", galleryUrls.size() > 1);
    if (auto *nextBtn = viewerDocument->GetElementById("viewer-nav-next"))
        nextBtn->SetClass("visible", galleryUrls.size() > 1);

    // Default dimensions while loading
    naturalImageSize = QSizeF(800, 600);
    resetView();

    // Assign image source
    if (auto *imgEl = viewerDocument->GetElementById("viewer-image")) {
        QString src = shell.imageSource(currentUrl, QSize(1920, 1080));
        imgEl->SetAttribute("src", src.toStdString());
    }

    updateTransform();

    // Fetch full quality to determine true dimensions
    if (auto *images = shell.images()) {
        QUrl fullUrl = Core::ImageManager::fullQualityUrl(currentUrl);
        Core::Snowflake accId = shell.location().accountId;
        QPointer<ImageViewerController> self(this);
        int expectedIndex = currentIndex;

        images->fetch(fullUrl, accId, this, [self, expectedIndex](const QByteArray &data) {
            if (!self || !self->visible || self->currentIndex != expectedIndex)
                return;
            QPixmap pixmap;
            if (pixmap.loadFromData(data) && !pixmap.isNull()) {
                self->naturalImageSize = pixmap.size();
                self->resetView();
                self->updateTransform();
            }
        });
    }
}

void ImageViewerController::nextImage()
{
    if (galleryUrls.size() <= 1)
        return;
    int next = (currentIndex + 1) % galleryUrls.size();
    loadImage(next);
}

void ImageViewerController::prevImage()
{
    if (galleryUrls.size() <= 1)
        return;
    int prev = (currentIndex - 1 + galleryUrls.size()) % galleryUrls.size();
    loadImage(prev);
}

void ImageViewerController::openImage(const QUrl &url, const QList<QUrl> &gallery)
{
    ensureDocument();
    if (!viewerDocument)
        return;

    galleryUrls = gallery;
    if (galleryUrls.isEmpty() && url.isValid())
        galleryUrls.append(url);

    currentIndex = galleryUrls.indexOf(url);
    if (currentIndex < 0)
        currentIndex = 0;

    loadImage(currentIndex);

    viewerDocument->Show();
    viewerDocument->PullToFront();
    viewerDocument->SetClass("open", true);
    visible = true;
    requestRender();
}

void ImageViewerController::hide()
{
    if (!viewerDocument || !visible)
        return;

    isDragging = false;
    viewerDocument->SetClass("open", false);
    viewerDocument->Hide();
    visible = false;
    requestRender();
}

} // namespace Rml
} // namespace UI
} // namespace Styx
