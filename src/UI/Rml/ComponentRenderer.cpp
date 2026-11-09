#include "UI/Rml/ComponentRenderer.hpp"

#include "Discord/CdnUrls.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Enums.hpp"
#include "Platform/Native.hpp"
#include "UI/Rml/RmlRuntime.hpp"

#include <cctype>
#include <charconv>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Styx::UI::Rml {
namespace {

template<bool Optional, bool Nullable>
const std::string &stringOrEmpty(const Core::JsonUtils::JsonObject::Field<std::string, Optional, Nullable> &field)
{
    static const std::string empty;
    return field.hasValue() ? field.get() : empty;
}

bool hasControlOrSpace(std::string_view text)
{
    for (char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc <= 32 || uc == 127 || c == '\\')
            return true;
    }
    return false;
}

bool startsWithCi(std::string_view text, std::string_view prefix)
{
    if (text.size() < prefix.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

bool isSafeExternalUrl(std::string_view url)
{
    if (url.empty() || hasControlOrSpace(url))
        return false;

    if (!startsWithCi(url, "http://") && !startsWithCi(url, "https://"))
        return false;

    return !Platform::urlHost(url).empty();
}

bool isSafeMediaUrl(const std::string &url)
{
    if (url.empty() || hasControlOrSpace(url))
        return false;

    return Discord::Cdn::isDiscordAssetUrl(url);
}

bool checkAudioOrVideo(std::string_view contentType, std::string_view url)
{
    if (!contentType.empty())
        return contentType.starts_with("video/") || contentType.starts_with("audio/");

    url = url.substr(0, url.find_first_of("?#"));
    const auto dot = url.rfind('.');
    if (dot != std::string_view::npos) {
        const auto ext = url.substr(dot);

        if (ext.size() >= 4 && ext.size() <= 5) {
            char lower[6] = {0};
            for (size_t i = 0; i < ext.size(); ++i)
                lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[i])));
            const std::string_view le(lower, ext.size());
            if (le == ".mp4" || le == ".webm" || le == ".mov" ||
                le == ".m4v" || le == ".mp3" || le == ".ogg" ||
                le == ".wav" || le == ".flac")
                return true;
        }
    }
    return false;
}

struct ResolvedMedia {
    const std::string *safeFetchUrl = nullptr;
    const std::string *openUrl = nullptr;
    std::string_view filename;
    std::optional<int64_t> fileSize;
    int width = 0;
    int height = 0;
    std::string_view contentType;
    bool isAudioOrVideo = false;
    bool valid = false;
};

ResolvedMedia resolveMedia(const Discord::ComponentMedia &media,
                           const Discord::Message &visible,
                           const Discord::MessageComponent &comp)
{
    ResolvedMedia result;

    const std::string &rawUrl = media.url.get();
    const bool isAttachmentScheme = rawUrl.starts_with("attachment://");
    const bool hasAttachmentId = media.attachmentId.hasValue() && media.attachmentId.get().isValid();

    const Discord::Attachment *found = nullptr;
    if (visible.attachments.hasValue()) {
        const auto &atts = visible.attachments.get();
        if (hasAttachmentId) {
            const auto targetId = media.attachmentId.get();
            for (const auto &att : atts) {
                if (att.id.get() == targetId) {
                    found = &att;
                    break;
                }
            }
        }
        if (!found && isAttachmentScheme) {
            const std::string_view targetFilename = std::string_view(rawUrl).substr(13);
            for (const auto &att : atts) {
                if (att.filename.get() == targetFilename) {
                    found = &att;
                    break;
                }
            }
        }
    }

    if (isAttachmentScheme && !found) {
        result.valid = false;
        return result;
    }

    const std::string *original = isAttachmentScheme ? &found->url.get() : &rawUrl;
    const bool originalSafe = isSafeMediaUrl(*original);
    if (media.proxyUrl.hasValue()) {
        if (isSafeMediaUrl(media.proxyUrl.get()))
            result.safeFetchUrl = &media.proxyUrl.get();
    } else if (found && found->proxyUrl.hasValue() && isSafeMediaUrl(found->proxyUrl.get())) {
        result.safeFetchUrl = &found->proxyUrl.get();
    } else if (originalSafe) {
        result.safeFetchUrl = original;
    }

    if (!result.safeFetchUrl)
        return result;
    result.openUrl = originalSafe ? original : result.safeFetchUrl;

    result.valid = true;

    if (media.width.hasValue() && media.width.get() > 0)
        result.width = media.width.get();
    else if (found && found->width.hasValue() && found->width.get() > 0)
        result.width = found->width.get();

    if (media.height.hasValue() && media.height.get() > 0)
        result.height = media.height.get();
    else if (found && found->height.hasValue() && found->height.get() > 0)
        result.height = found->height.get();

    if (media.contentType.hasValue())
        result.contentType = media.contentType.get();
    else if (found && found->contentType.hasValue())
        result.contentType = found->contentType.get();

    if (comp.name.hasValue() && !comp.name.get().empty())
        result.filename = comp.name.get();
    else if (found)
        result.filename = found->filename.get();
    else if (isAttachmentScheme)
        result.filename = std::string_view(rawUrl).substr(13);

    if (comp.size.hasValue())
        result.fileSize = comp.size.get();
    else if (found)
        result.fileSize = found->size.get();

    result.isAudioOrVideo = checkAudioOrVideo(result.contentType, *result.safeFetchUrl);
    return result;
}

std::string renderFileCard(std::string_view downloadUrl,
                           std::string_view filename,
                           std::optional<int64_t> fileSizeBytes,
                           std::string_view contentType,
                           const ComponentRenderContext &context)
{
    if (downloadUrl.empty())
        return "<div class=\"component-unavailable\">File unavailable</div>";

    const bool isAudio = contentType.starts_with("audio/");
    const bool isVideo = contentType.starts_with("video/");
    const std::string iconSrc = isVideo ? "icon:image-play" : isAudio ? "icon:file-audio" : "icon:file-text";

    const std::string formattedSize = fileSizeBytes ? context.fileSize(*fileSizeBytes) : std::string{};

    return std::format(
        "<div class=\"attachment-file-card\" data-action=\"link\" data-href=\"{0}\">"
        "<img class=\"attachment-file-icon\" src=\"{1}\" />"
        "<div class=\"attachment-file-info\">"
        "<span class=\"attachment-filename\">{2}</span>"
        "<span class=\"attachment-filesize\">{3}</span>"
        "</div>"
        "</div>",
        Platform::htmlEscape(downloadUrl),
        iconSrc,
        Platform::htmlEscape(filename),
        Platform::htmlEscape(formattedSize));
}

bool isSpoilerHidden(bool isSpoiler, const std::string &path, const ComponentRenderContext &context)
{
    if (!isSpoiler)
        return false;
    if (!context.revealedSpoilers)
        return true;
    return !context.revealedSpoilers->contains(path);
}

std::string renderSpoilerButton(const std::string &path)
{
    return std::format(
        "<button class=\"component-spoiler\" data-action=\"reveal-component-spoiler\" data-component-path=\"{0}\">"
        "Spoiler — click to reveal</button>",
        Platform::htmlEscape(path));
}

std::string renderButtonEmoji(const Discord::Emoji &emoji, const ComponentRenderContext &context)
{
    if (emoji.id.hasValue() && emoji.id.get().isValid()) {
        const std::string url = Discord::Cdn::emoji(emoji.id.get(), 20, emoji.animated.valueOr(false));
        const std::string src = context.imageSource(url, {16, 16});
        const std::string &name = stringOrEmpty(emoji.name);
        return std::format("<img class=\"emoji\" src=\"{0}\" alt=\":{1}:\" />",
                           Platform::htmlEscape(src),
                           Platform::htmlEscape(name));
    }
    if (emoji.name.hasValue() && !emoji.name.get().empty())
        return richText(emoji.name.get());
    return {};
}

std::string buildButtonContent(const Discord::MessageComponent &comp, const ComponentRenderContext &context)
{
    std::string content;
    if (comp.emoji.hasValue())
        content += renderButtonEmoji(comp.emoji.get(), context);

    if (comp.label.hasValue() && !comp.label.get().empty()) {
        if (!content.empty())
            content += ' ';
        content += richText(comp.label.get());
    }
    return content;
}

std::string buttonStyleClass(int style)
{
    switch (style) {
    case 1: return "component-primary";
    case 2: return "component-secondary";
    case 3: return "component-success";
    case 4: return "component-danger";
    case 5: return "component-link";
    case 6: return "component-premium";
    default: return "component-secondary";
    }
}

std::string renderButton(const Discord::MessageComponent &comp, const std::string &path, const ComponentRenderContext &context)
{
    const int style = comp.style.valueOr(1);
    const bool wireDisabled = comp.disabled.valueOr(false);
    const std::string &url = stringOrEmpty(comp.url);
    const bool safeUrl = isSafeExternalUrl(url);
    std::string inner = buildButtonContent(comp, context);

    if (style == 6 && inner.empty())
        inner = "<img class=\"component-link-icon\" src=\"icon:gem\" /> Premium";

    if (style == 5 && !wireDisabled && safeUrl) {
        return std::format(
            "<a class=\"component-button component-link chat-link\" data-action=\"link\" data-href=\"{0}\">{1}"
            "<img class=\"component-link-icon\" src=\"icon:link\" /></a>",
            Platform::htmlEscape(url),
            inner);
    }

    if (style == 5) {
        return std::format(
            "<button class=\"component-button component-link\" disabled title=\"Read-only\">{0}</button>",
            inner);
    }

    // Premium buttons start a purchase, which Styx doesn't offer.
    const bool clickable = context.interactive && style != 6 && !wireDisabled && !stringOrEmpty(comp.customId).empty();
    if (clickable && context.pendingPath == path) {
        return std::format(
            "<button class=\"component-button {0} component-pending\" disabled title=\"Sending\">{1}</button>",
            buttonStyleClass(style),
            inner);
    }
    if (clickable) {
        return std::format(
            "<button class=\"component-button component-active {0}\" data-action=\"component-button\" data-component-path=\"{1}\">{2}</button>",
            buttonStyleClass(style),
            Platform::htmlEscape(path),
            inner);
    }

    return std::format(
        "<button class=\"component-button {0}\" disabled title=\"Read-only\">{1}</button>",
        buttonStyleClass(style),
        inner);
}

std::string renderSelect(const Discord::MessageComponent &comp, const ComponentRenderContext &context)
{
    std::string selected;
    auto appendSelection = [&](std::string display) {
        if (!selected.empty())
            selected += ", ";
        selected += display;
    };
    if (comp.type.get() == Discord::MessageComponentType::STRING_SELECT && comp.options.hasValue()) {
        for (const auto &option : comp.options.get()) {
            if (!option.isDefault.valueOr(false))
                continue;
            std::string display;
            if (option.emoji.hasValue())
                display = renderButtonEmoji(option.emoji.get(), context) + " ";
            display += richText(option.label.get());
            if (option.description.hasValue() && !option.description->empty())
                display += " — " + richText(option.description.get());
            appendSelection(std::move(display));
        }
    } else if (comp.type.get() != Discord::MessageComponentType::STRING_SELECT && comp.defaultValues.hasValue()) {
        for (const auto &value : comp.defaultValues.get()) {
            auto label = context.defaultLabel(value.id.get(), value.type.get());
            if (label.empty())
                label = value.id->toString();
            appendSelection(richText(label));
        }
    }
    if (selected.empty()) {
        const auto &placeholder = stringOrEmpty(comp.placeholder);
        selected = richText(placeholder.empty() ? "Select an option" : placeholder);
    }
    return std::format(
        "<div class=\"component-select\" role=\"combobox\" aria-disabled=\"true\" aria-expanded=\"false\" title=\"Read-only\">"
        "<div class=\"component-select-value\">{}</div><img class=\"component-select-arrow\" src=\"icon:arrow-down\" /></div>",
        selected);
}

std::string renderComponent(const Discord::MessageComponent &comp,
                            const std::string &path,
                            const Discord::Message &visible,
                            const ComponentRenderContext &context,
                            bool isV2,
                            int depth);

std::string renderChildren(const Discord::MessageComponent &comp,
                           const std::string &path,
                           const Discord::Message &visible,
                           const ComponentRenderContext &context,
                           bool isV2,
                           int depth)
{
    if (!comp.components.hasValue())
        return {};

    std::string inner;
    const auto &children = comp.components.get();
    for (size_t j = 0; j < children.size(); ++j) {
        const std::string childPath = path + "." + std::to_string(j);
        inner += renderComponent(children[j], childPath, visible, context, isV2, depth + 1);
    }
    return inner;
}

std::string renderThumbnail(const Discord::MessageComponent &comp,
                            const std::string &path,
                            const Discord::Message &visible,
                            const ComponentRenderContext &context)
{
    if (isSpoilerHidden(comp.spoiler.valueOr(false), path, context))
        return std::format("<div class=\"component-thumbnail\">{0}</div>", renderSpoilerButton(path));

    if (!comp.media.hasValue())
        return "<div class=\"component-thumbnail\"><div class=\"component-unavailable\">Media unavailable</div></div>";

    const ResolvedMedia res = resolveMedia(comp.media.get(), visible, comp);
    if (!res.valid)
        return "<div class=\"component-thumbnail\"><div class=\"component-unavailable\">Media unavailable</div></div>";

    const int naturalW = res.width > 0 ? res.width : 80;
    const int naturalH = res.height > 0 ? res.height : 80;
    Platform::Size fitted = Platform::fitWithin({naturalW, naturalH}, {80, 80});
    if (fitted.empty())
        fitted = {80, 80};

    const std::string src = context.imageSource(*res.safeFetchUrl, fitted);
    const std::string_view desc = stringOrEmpty(comp.description);

    return std::format(
        "<div class=\"component-thumbnail\">"
        "<img class=\"component-media\" src=\"{0}\" data-action=\"open-image\" data-url=\"{1}\" "
        "alt=\"{2}\" title=\"{2}\" style=\"width:{3}dp; height:auto;\" />"
        "</div>",
        Platform::htmlEscape(src),
        Platform::htmlEscape(*res.openUrl),
        Platform::htmlEscape(desc),
        fitted.width);
}

std::string renderMediaGallery(const Discord::MessageComponent &comp,
                               const std::string &path,
                               const Discord::Message &visible,
                               const ComponentRenderContext &context)
{
    if (!comp.items.hasValue() || comp.items->empty())
        return {};

    std::string itemsHtml;
    const auto &items = comp.items.get();
    for (size_t k = 0; k < items.size(); ++k) {
        const auto &item = items[k];
        const std::string itemPath = path + ".i" + std::to_string(k);

        itemsHtml += "<div class=\"component-gallery-item\">";
        if (isSpoilerHidden(item.spoiler.valueOr(false), itemPath, context)) {
            itemsHtml += renderSpoilerButton(itemPath);
        } else {
            const ResolvedMedia res = resolveMedia(item.media.get(), visible, comp);
            if (!res.valid) {
                itemsHtml += "<div class=\"component-unavailable\">Media unavailable</div>";
            } else if (res.isAudioOrVideo) {
                const std::string_view fname = !res.filename.empty() ? res.filename : "Media";
                itemsHtml += renderFileCard(*res.safeFetchUrl, fname, res.fileSize, res.contentType, context);
            } else {
                const int naturalW = res.width > 0 ? res.width : 280;
                const int naturalH = res.height > 0 ? res.height : 210;
                Platform::Size fitted = Platform::fitWithin({naturalW, naturalH}, {280, 210});
                if (fitted.empty())
                    fitted = {280, 210};

                const std::string src = context.imageSource(*res.safeFetchUrl, fitted);
                const std::string_view desc = stringOrEmpty(item.description);

                itemsHtml += std::format(
                    "<img class=\"component-media\" src=\"{0}\" data-action=\"open-image\" data-url=\"{1}\" "
                    "alt=\"{2}\" title=\"{2}\" style=\"width:{3}dp; height:auto;\" />",
                    Platform::htmlEscape(src),
                    Platform::htmlEscape(*res.openUrl),
                    Platform::htmlEscape(desc),
                    fitted.width);
            }

            if (item.description.hasValue() && !item.description.get().empty()) {
                itemsHtml += std::format(
                    "<div class=\"component-media-caption\">{0}</div>",
                    Platform::htmlEscape(item.description.get()));
            }
        }
        itemsHtml += "</div>";
    }

    return std::format("<div class=\"component-gallery\">{0}</div>", itemsHtml);
}

std::string renderFile(const Discord::MessageComponent &comp,
                       const std::string &path,
                       const Discord::Message &visible,
                       const ComponentRenderContext &context)
{
    if (isSpoilerHidden(comp.spoiler.valueOr(false), path, context))
        return std::format("<div class=\"component-file\">{0}</div>", renderSpoilerButton(path));

    if (!comp.file.hasValue())
        return "<div class=\"component-file\"><div class=\"component-unavailable\">File unavailable</div></div>";

    const ResolvedMedia res = resolveMedia(comp.file.get(), visible, comp);
    if (!res.valid)
        return "<div class=\"component-file\"><div class=\"component-unavailable\">File unavailable</div></div>";

    std::string_view filename = res.filename;
    if (filename.empty())
        filename = "attachment";

    return std::format(
        "<div class=\"component-file\">{0}</div>",
        renderFileCard(*res.safeFetchUrl, filename, res.fileSize, res.contentType, context));
}

std::string renderSeparator(const Discord::MessageComponent &comp)
{
    std::string classes = "component-separator";
    if (comp.spacing.valueOr(1) == 2)
        classes += " component-separator-large";
    if (comp.divider.hasValue() && !comp.divider.get())
        classes += " component-no-divider";

    return std::format("<div class=\"{0}\"></div>", classes);
}

std::string renderContainer(const Discord::MessageComponent &comp,
                            const std::string &path,
                            const Discord::Message &visible,
                            const ComponentRenderContext &context,
                            bool isV2,
                            int depth)
{
    std::string styleAttr;
    if (comp.accentColor.hasValue())
        styleAttr = std::format(" style=\"border-left-width: 4dp; border-left-color: #{:06x};\"", comp.accentColor.get() & 0xFFFFFF);

    if (isSpoilerHidden(comp.spoiler.valueOr(false), path, context))
        return std::format("<div class=\"component-container\"{0}>{1}</div>", styleAttr, renderSpoilerButton(path));

    const std::string innerHtml = renderChildren(comp, path, visible, context, isV2, depth);
    return std::format("<div class=\"component-container\"{0}>{1}</div>", styleAttr, innerHtml);
}

std::string renderSection(const Discord::MessageComponent &comp,
                          const std::string &path,
                          const Discord::Message &visible,
                          const ComponentRenderContext &context,
                          bool isV2,
                          int depth)
{
    const std::string contentHtml = renderChildren(comp, path, visible, context, isV2, depth);

    std::string accessoryHtml;
    if (comp.accessory) {
        const std::string accessoryPath = path + ".a";
        accessoryHtml = std::format(
            "<div class=\"component-accessory {0}\">{1}</div>",
            comp.accessory->type.get() == Discord::MessageComponentType::THUMBNAIL
                ? "component-accessory-thumbnail" : "component-accessory-button",
            renderComponent(*comp.accessory, accessoryPath, visible, context, isV2, depth + 1));
    }

    return std::format(
        "<div class=\"component-section\"><div class=\"component-section-content\">{0}</div>{1}</div>",
        contentHtml,
        accessoryHtml);
}

std::string renderComponent(const Discord::MessageComponent &comp,
                            const std::string &path,
                            const Discord::Message &visible,
                            const ComponentRenderContext &context,
                            bool isV2,
                            int depth)
{
    if (depth > 16)
        return "<div class=\"component-unsupported\">Unsupported component</div>";

    const auto type = comp.type.valueOr(Discord::MessageComponentType::UNKNOWN);

    const bool isV2Type = (type == Discord::MessageComponentType::SECTION ||
                           type == Discord::MessageComponentType::TEXT_DISPLAY ||
                           type == Discord::MessageComponentType::THUMBNAIL ||
                           type == Discord::MessageComponentType::MEDIA_GALLERY ||
                           type == Discord::MessageComponentType::FILE ||
                           type == Discord::MessageComponentType::SEPARATOR ||
                           type == Discord::MessageComponentType::CONTAINER);

    if (isV2Type && !isV2)
        return {};

    switch (type) {
    case Discord::MessageComponentType::ACTION_ROW:
        return std::format("<div class=\"component-row\">{0}</div>",
                           renderChildren(comp, path, visible, context, isV2, depth));

    case Discord::MessageComponentType::BUTTON:
        return renderButton(comp, path, context);

    case Discord::MessageComponentType::STRING_SELECT:
    case Discord::MessageComponentType::USER_SELECT:
    case Discord::MessageComponentType::ROLE_SELECT:
    case Discord::MessageComponentType::MENTIONABLE_SELECT:
    case Discord::MessageComponentType::CHANNEL_SELECT:
        return renderSelect(comp, context);

    case Discord::MessageComponentType::SECTION:
        return renderSection(comp, path, visible, context, isV2, depth);

    case Discord::MessageComponentType::TEXT_DISPLAY:
        return std::format("<div class=\"component-text message-body\">{0}</div>",
                           context.markdown(stringOrEmpty(comp.content)));

    case Discord::MessageComponentType::THUMBNAIL:
        return renderThumbnail(comp, path, visible, context);

    case Discord::MessageComponentType::MEDIA_GALLERY:
        return renderMediaGallery(comp, path, visible, context);

    case Discord::MessageComponentType::FILE:
        return renderFile(comp, path, visible, context);

    case Discord::MessageComponentType::SEPARATOR:
        return renderSeparator(comp);

    case Discord::MessageComponentType::CONTAINER:
        return renderContainer(comp, path, visible, context, isV2, depth);

    default:
        return "<div class=\"component-unsupported\">Unsupported component</div>";
    }
}

} // namespace

std::string renderMessageComponents(const Discord::Message &message,
                                    const ComponentRenderContext &context)
{
    const auto &visible = message.contentMessage();
    if (!visible.components.hasValue() || visible.components->empty())
        return {};

    const bool isV2 = visible.flags.hasValue() && visible.flags->testFlag(Discord::MessageFlag::IS_COMPONENTS_V2);

    std::string innerHtml;
    const auto &comps = visible.components.get();
    for (size_t i = 0; i < comps.size(); ++i) {
        const std::string path = std::to_string(i);
        innerHtml += renderComponent(comps[i], path, visible, context, isV2, 0);
    }

    if (innerHtml.empty())
        return {};

    return "<div class=\"message-components\">" + innerHtml + "</div>";
}

const Discord::MessageComponent *componentAtPath(const Discord::Message &message, std::string_view path)
{
    const auto &visible = message.contentMessage();
    if (!visible.components.hasValue())
        return nullptr;

    const Discord::MessageComponent *current = nullptr;
    while (!path.empty()) {
        const size_t dot = path.find('.');
        const std::string_view segment = path.substr(0, dot);
        path = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);

        if (current && segment == "a") {
            current = current->accessory.get();
        } else {
            const auto &list = current ? current->components : visible.components;
            size_t index = 0;
            const auto [end, ec] = std::from_chars(segment.data(), segment.data() + segment.size(), index);
            if (ec != std::errc{} || end != segment.data() + segment.size() || !list.hasValue() || index >= list->size())
                return nullptr;
            current = &list.get()[index];
        }
        if (!current)
            return nullptr;
    }
    return current;
}

} // namespace Styx::UI::Rml
