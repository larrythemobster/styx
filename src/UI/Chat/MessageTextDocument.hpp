#pragma once

#include <QTextDocument>

#include "Core/Snowflake.hpp"
#include "UI/Chat/EmojiTextObject.hpp"

namespace Styx {
namespace Core {
class ImageManager;
}
namespace UI {

class MessageTextDocument : public QTextDocument
{
    Q_OBJECT
public:
    explicit MessageTextDocument(Core::ImageManager *images, Core::Snowflake accountId, QObject *parent = nullptr);

    static QString richTextStyleSheet();
    static void setupDocument(QTextDocument &doc, const QString &htmlContent, const QFont &font, int textWidth);

    void setMessageHtml(const QString &html, const QFont &font, int textWidth);
signals:
    void imageLoaded();

private:
    EmojiTextObject emoji;
};

} // namespace UI
} // namespace Styx
