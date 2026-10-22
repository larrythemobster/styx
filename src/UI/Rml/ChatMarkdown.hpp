#pragma once

#include <QRegularExpression>
#include <QString>
#include <QList>
#include <QUrl>

#include "Core/Markdown/Parser.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/UserManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Entities.hpp"
#include "RmlRuntime.hpp"
#include "Shell.hpp"

namespace Acheron {
namespace UI {
namespace Rml {

struct RenderedMarkdown
{
    QString rml;
    bool mentionsUser = false;
};

class ChatMarkdown
{
public:
    static RenderedMarkdown toRml(const QString &source, Shell &shell, Core::Snowflake currentUserId, Core::Snowflake guildId, Core::Snowflake channelId)
    {
        RenderedMarkdown result;
        if (source.isEmpty())
            return result;

        // Check if raw source mentions the user
        if (currentUserId.isValid()) {
            QString userMentionStr = QStringLiteral("<@%1>").arg(quint64(currentUserId));
            QString userNickMentionStr = QStringLiteral("<@!%1>").arg(quint64(currentUserId));
            if (source.contains(userMentionStr) || source.contains(userNickMentionStr))
                result.mentionsUser = true;
        }

        // Pre-process multiline constructs: Code blocks, blockquotes, headings, lists, spoilers
        // To safely handle markdown, let's process lines and blocks
        QString processed = source;

        // 1. Code blocks: ```(lang)?\n?(content)```
        QHash<QString, QString> codeBlockPlaceholders;
        int codeBlockCounter = 0;
        static const QRegularExpression codeBlockRegex(QStringLiteral(R"(```(?:([a-zA-Z0-9_+-]+)?\n)?([\s\S]*?)```)"));
        auto cbMatch = codeBlockRegex.match(processed);
        while (cbMatch.hasMatch()) {
            QString lang = cbMatch.captured(1).toHtmlEscaped();
            QString code = cbMatch.captured(2).toHtmlEscaped();
            QString placeholder = QStringLiteral("ACHERONCODEBLOCK%1TAG").arg(++codeBlockCounter);
            QString rmlCode = QStringLiteral("<pre class=\"code-block\"><code>%1</code></pre>").arg(code);
            codeBlockPlaceholders.insert(placeholder, rmlCode);
            processed.replace(cbMatch.capturedStart(0), cbMatch.capturedLength(0), placeholder);
            cbMatch = codeBlockRegex.match(processed);
        }

        // 2. Spoilers: ||content||
        QHash<QString, QString> spoilerPlaceholders;
        int spoilerCounter = 0;
        static const QRegularExpression spoilerRegex(QStringLiteral(R"(\|\|([\s\S]+?)\|\|)"));
        auto spMatch = spoilerRegex.match(processed);
        while (spMatch.hasMatch()) {
            QString inner = spMatch.captured(1);
            QString placeholder = QStringLiteral("ACHERONSPOILER%1TAG").arg(++spoilerCounter);
            // We will render inner markdown when resolving placeholders
            spoilerPlaceholders.insert(placeholder, inner);
            processed.replace(spMatch.capturedStart(0), spMatch.capturedLength(0), placeholder);
            spMatch = spoilerRegex.match(processed);
        }

        // Use Core::Markdown::Parser for the remaining AST
        Core::Markdown::Parser parser;
        Core::Markdown::ParseState parseState;
        parseState.customState[QStringLiteral("channelId")] = quint64(channelId);
        QList<Core::Markdown::AstNode> ast = parser.parse(processed, parseState);
        bool emojiOnly = Core::Markdown::Parser::isEmojiOnly(ast);

        // Render AST nodes to RML
        QString rml = renderNodes(ast, shell, emojiOnly, result.mentionsUser, currentUserId, guildId);

        // Restore spoilers
        for (auto it = spoilerPlaceholders.cbegin(); it != spoilerPlaceholders.cend(); ++it) {
            RenderedMarkdown innerParsed = toRml(it.value(), shell, currentUserId, guildId, channelId);
            if (innerParsed.mentionsUser)
                result.mentionsUser = true;
            QString spoilerHtml = QStringLiteral("<span class=\"spoiler blurred\" data-action=\"reveal-text-spoiler\">%1</span>")
                                      .arg(innerParsed.rml);
            rml.replace(it.key(), spoilerHtml);
        }

        // Restore code blocks
        for (auto it = codeBlockPlaceholders.cbegin(); it != codeBlockPlaceholders.cend(); ++it) {
            rml.replace(it.key(), it.value());
        }
        while (rml.endsWith(QStringLiteral("<br/>"))) {
            rml.chop(5);
        }

        result.rml = rml;
        return result;
    }

private:
    static QString renderNodes(const QList<Core::Markdown::AstNode> &nodes, Shell &shell, bool emojiOnly, bool &mentionsUser, Core::Snowflake currentUserId, Core::Snowflake guildId)
    {
        QString out;
        for (const auto &node : nodes) {
            if (node.type == QStringLiteral("text")) {
                // Unicode emojis in text to RML
                out += richText(node.content);
            } else if (node.type == QStringLiteral("user")) {
                Core::Snowflake userId(node.content.toULongLong());
                Core::ClientInstance *instance = shell.currentInstance();
                QString name;
                if (instance && instance->users())
                    name = instance->users()->getDisplayName(userId, guildId);
                if (name.isEmpty())
                    name = QString::number(quint64(userId));
                bool isMe = (userId == currentUserId);
                if (isMe)
                    mentionsUser = true;
                out += QStringLiteral("<span class=\"mention mention-user%1\" data-action=\"user-mention\" data-user-id=\"%2\">@%3</span>")
                    .arg(isMe ? QStringLiteral(" mention-me") : QString(),
                         QString::number(quint64(userId)),
                         name.toHtmlEscaped());
            } else if (node.type == QStringLiteral("channel")) {
                Core::Snowflake chId(node.content.toULongLong());
                Core::ClientInstance *instance = shell.currentInstance();
                QString name;
                if (instance) {
                    if (auto ch = instance->getChannel(chId)) {
                        if (ch->name.hasValue())
                            name = *ch->name;
                    }
                }
                if (name.isEmpty())
                    name = node.content;
                out += QStringLiteral("<a href=\"acheron://channel/%1\" class=\"mention mention-channel\" data-action=\"channel-mention\" data-channel-id=\"%1\">#%2</a>")
                    .arg(node.content.toHtmlEscaped(), name.toHtmlEscaped());
            } else if (node.type == QStringLiteral("channelLink")) {
                QString href = node.attributes.value(QStringLiteral("href")).toString();
                out += QStringLiteral("<a href=\"%1\" class=\"mention mention-channel\" data-action=\"channel-link\" data-href=\"%1\">%2</a>")
                    .arg(href.toHtmlEscaped(), href.toHtmlEscaped());
            } else if (node.type == QStringLiteral("customEmoji")) {
                Core::Snowflake emojiId(node.content.toULongLong());
                QString name = node.attributes.value(QStringLiteral("name")).toString();
                bool animated = node.attributes.value(QStringLiteral("animated")).toBool();
                int px = emojiOnly ? 48 : 22;
                QUrl url = Discord::Cdn::emoji(emojiId, px, animated);
                QString src = shell.imageSource(url, QSize(px, px));
                out += QStringLiteral("<img class=\"emoji%1\" src=\"%2\" title=\":%3:\" alt=\":%3:\" />")
                    .arg(emojiOnly ? QStringLiteral(" jumbo") : QString(),
                         src.toHtmlEscaped(),
                         name.toHtmlEscaped());
            } else if (node.type == QStringLiteral("strong")) {
                out += QStringLiteral("<strong>%1</strong>").arg(renderNodes(node.children, shell, false, mentionsUser, currentUserId, guildId));
            } else if (node.type == QStringLiteral("em")) {
                out += QStringLiteral("<em>%1</em>").arg(renderNodes(node.children, shell, false, mentionsUser, currentUserId, guildId));
            } else if (node.type == QStringLiteral("u")) {
                out += QStringLiteral("<u>%1</u>").arg(renderNodes(node.children, shell, false, mentionsUser, currentUserId, guildId));
            } else if (node.type == QStringLiteral("strike")) {
                out += QStringLiteral("<s>%1</s>").arg(renderNodes(node.children, shell, false, mentionsUser, currentUserId, guildId));
            } else if (node.type == QStringLiteral("inlineCode")) {
                out += QStringLiteral("<code class=\"inline-code\">%1</code>").arg(escape(node.content));
            } else if (node.type == QStringLiteral("url") || node.type == QStringLiteral("autolink")) {
                QString href = node.attributes.value(QStringLiteral("href")).toString();
                if (href.isEmpty())
                    href = node.content;
                out += QStringLiteral("<a href=\"%1\" class=\"chat-link\" data-action=\"link\" data-href=\"%1\">%2</a>")
                    .arg(href.toHtmlEscaped(), node.content.toHtmlEscaped());
            } else if (node.type == QStringLiteral("link")) {
                QString href = node.attributes.value(QStringLiteral("href")).toString();
                QString text = renderNodes(node.children, shell, false, mentionsUser, currentUserId, guildId);
                if (text.isEmpty())
                    text = href.toHtmlEscaped();
                out += QStringLiteral("<a href=\"%1\" class=\"chat-link\" data-action=\"link\" data-href=\"%1\">%2</a>")
                    .arg(href.toHtmlEscaped(), text);
            } else if (node.type == QStringLiteral("br") || node.type == QStringLiteral("newline")) {
                out += QStringLiteral("<br/>");
            } else {
                if (!node.children.isEmpty()) {
                    out += renderNodes(node.children, shell, emojiOnly, mentionsUser, currentUserId, guildId);
                } else if (!node.content.isEmpty()) {
                    out += escape(node.content);
                }
            }
        }
        return out;
    }
};

} // namespace Rml
} // namespace UI
} // namespace Acheron