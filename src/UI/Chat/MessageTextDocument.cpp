#include "MessageTextDocument.hpp"

#include <QTextOption>

#include "Core/ImageManager.hpp"
#include "Core/Theme/Icons.hpp"
#include "Core/Theme/Manager.hpp"

namespace Styx {
namespace UI {

MessageTextDocument::MessageTextDocument(Core::ImageManager *images, Core::Snowflake accountId, QObject *parent)
    : QTextDocument(parent), emoji(images, this)
{
    emoji.setAccountId(accountId);
    connect(images, &Core::ImageManager::imageFetched, this, &MessageTextDocument::imageLoaded);
}

QString MessageTextDocument::richTextStyleSheet()
{
    using Core::Theme::Manager;
    using Core::Theme::Token;
    const QColor link = Manager::instance().color(Token::LinkText);
    const QColor mentionText = Manager::instance().color(Token::MentionText);
    const QColor mentionBg = Manager::instance().color(Token::MentionBg);
    const QString mentionBgRgba = QStringLiteral("rgba(%1, %2, %3, %4)")
                                          .arg(mentionBg.red())
                                          .arg(mentionBg.green())
                                          .arg(mentionBg.blue())
                                          .arg(QString::number(mentionBg.alphaF(), 'f', 3));
    const QFont codeFont = Manager::instance().font(Core::Theme::FontRole::Code);
    QString code = QStringLiteral("code { font-family: '%1';").arg(codeFont.family());
    if (codeFont.pointSizeF() > 0)
        code += QStringLiteral(" font-size: %1pt;").arg(codeFont.pointSizeF());
    code += QStringLiteral(" }");

    return QStringLiteral("a { color: %1; } "
                          ".mention { color: %2; background-color: %3; text-decoration: none; } ")
                   .arg(link.name(QColor::HexRgb))
                   .arg(mentionText.name(QColor::HexRgb))
                   .arg(mentionBgRgba) +
           code;
}

void MessageTextDocument::setupDocument(QTextDocument &doc, const QString &htmlContent, const QFont &font, int textWidth)
{
    QString wrapped = QString("<div style=\"white-space: pre-wrap;\">%1</div>")
                              .arg(htmlContent);

    doc.setDefaultFont(font);
    doc.setDefaultStyleSheet(richTextStyleSheet());

    if (htmlContent.contains(QLatin1String("styx-icon:view-thread"))) {
        const QColor threadColor = Core::Theme::Manager::instance().color(Core::Theme::Token::LinkText);
        doc.addResource(QTextDocument::ImageResource,
                        QUrl(QStringLiteral("styx-icon:view-thread")),
                        Core::Theme::Icons::pixmap(Core::Theme::Icons::Name::Spool, 14, threadColor, 2.0));
    }

    if (htmlContent.contains(QLatin1String("styx-icon:forwarded"))) {
        const QColor mutedColor = Core::Theme::Manager::instance().color(Core::Theme::Token::PlaceholderText);
        doc.addResource(QTextDocument::ImageResource,
                        QUrl(QStringLiteral("styx-icon:forwarded")),
                        Core::Theme::Icons::pixmap(Core::Theme::Icons::Name::Forward, 12, mutedColor, 2.0));
    }

    if (htmlContent.contains(QLatin1String("styx-icon:mention-"))) {
        using namespace Core::Theme::Icons;
        static const QList<QPair<QString, QString>> mentionIcons = {
            { "hash", Name::Hash },
            { "chevron", Name::ChevronRight },
            { "message", Name::MessageCircle },
            { "post", Name::FileText },
            { "forum", Name::MessagesSquare },
            { "thread", Name::Spool },
            { "voice", Name::VolumeOn },
            { "announcement", Name::Megaphone },
            { "locked", Name::Lock },
        };
        const QColor mentionColor = Core::Theme::Manager::instance().color(Core::Theme::Token::MentionText);
        for (const auto &icon : mentionIcons) {
            QString url = "styx-icon:mention-" + icon.first;
            if (htmlContent.contains(url))
                doc.addResource(QTextDocument::ImageResource, QUrl(url), pixmap(icon.second, 14, mentionColor, 2.0));
        }
    }

    doc.setHtml(wrapped);
    doc.setTextWidth(textWidth);
    doc.setDocumentMargin(0);
    QTextOption opt = doc.defaultTextOption();
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    doc.setDefaultTextOption(opt);
}

void MessageTextDocument::setMessageHtml(const QString &html, const QFont &font, int textWidth)
{
    setupDocument(*this, html, font, textWidth);
    emoji.install(*this);
}
} // namespace UI
} // namespace Styx
