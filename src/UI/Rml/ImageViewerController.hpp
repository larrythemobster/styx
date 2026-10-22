#pragma once

#include "Controller.hpp"
#include <QList>
#include <QPointer>
#include <QPointF>
#include <QSizeF>
#include <QUrl>

namespace Rml {
class ElementDocument;
}

namespace Styx {
namespace UI {
class Shell;
namespace Rml {

class ImageViewerController final : public Controller
{
    Q_OBJECT
public:
    explicit ImageViewerController(Shell &shell, QObject *parent = nullptr);
    ~ImageViewerController() override;

    void openImage(const QUrl &url, const QList<QUrl> &gallery = {});
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
    QList<QUrl> galleryUrls;
    int currentIndex = 0;
    QUrl currentUrl;
    QSizeF naturalImageSize;

    float zoomLevel = 1.0f;
    QPointF panOffset;
    QPointF lastMousePos;
    bool isDragging = false;
    bool visible = false;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
