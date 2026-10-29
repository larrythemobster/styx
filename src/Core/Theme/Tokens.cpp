#include "Tokens.hpp"

namespace Styx {
namespace Core {
namespace Theme {

const std::vector<TokenDescriptor> &registry()
{
    static const std::vector<TokenDescriptor> reg = {
        // backgrounds
        { Token::WindowBg, "window.bg", "Window background", "Backgrounds", Platform::Color{24, 22, 34, 255, true},
          PaletteRole::Window, false },
        { Token::BaseBg, "base.bg", "Base", "Backgrounds", Platform::Color{30, 28, 44, 255, true},
          PaletteRole::Base, false },
        { Token::AlternateBaseBg, "base.alt", "Alternate base", "Backgrounds", Platform::Color{36, 34, 54, 255, true},
          PaletteRole::AlternateBase, false },
        { Token::ButtonBg, "button.bg", "Button", "Backgrounds", Platform::Color{40, 38, 60, 255, true},
          PaletteRole::Button, false },
        { Token::TooltipBg, "tooltip.bg", "Tooltip background", "Backgrounds", Platform::Color{40, 38, 60, 255, true},
          PaletteRole::ToolTipBase, false },

        // text
        { Token::WindowText, "text.window", "Window text", "Text", Platform::Color{210, 208, 225, 255, true},
          PaletteRole::WindowText, false },
        { Token::PrimaryText, "text.primary", "Primary text", "Text", Platform::Color{200, 198, 220, 255, true},
          PaletteRole::Text, false },
        { Token::ButtonText, "text.button", "Button text", "Text", Platform::Color{210, 208, 225, 255, true},
          PaletteRole::ButtonText, false },
        { Token::TooltipText, "text.tooltip", "Tooltip text", "Text", Platform::Color{220, 218, 240, 255, true},
          PaletteRole::ToolTipText, false },
        { Token::PlaceholderText, "text.placeholder", "Placeholder text", "Text",
          Platform::Color{126, 124, 153, 255, true}, PaletteRole::PlaceholderText, false },
        { Token::DisabledText, "text.disabled", "Disabled text", "Text", Platform::Color{126, 124, 153, 255, true},
          std::nullopt, false },
        { Token::BrightText, "text.bright", "Bright text", "Text", Platform::Color{189, 146, 236, 255, true},
          PaletteRole::BrightText, false },

        // accents
        { Token::Highlight, "accent.highlight", "Highlight", "Accents",
          Platform::Color{124, 92, 192, 255, true}, PaletteRole::Highlight, false },
        { Token::HighlightedText, "accent.highlightText", "Highlighted text", "Accents",
          Platform::Color{245, 244, 255, 255, true}, PaletteRole::HighlightedText, false },
        { Token::LinkText, "accent.link", "Links", "Accents", Platform::Color{157, 180, 255, 255, true}, PaletteRole::Link,
          false },
        { Token::Divider, "accent.divider", "Dividers", "Accents", Platform::Color{58, 55, 88, 255, true},
          PaletteRole::Mid, false },

        // extended chat elements
        { Token::ChatError, "chat.error", "Failed message", "Chat", Platform::Color{220, 50, 50, 255, true},
          std::nullopt, false },
        { Token::EmbedDefault, "chat.embedDefault", "Default embed color", "Chat",
          Platform::Color{88, 101, 242, 255, true}, std::nullopt, true },
        { Token::MentionText, "chat.mentionText", "Mention text", "Chat", Platform::Color{201, 205, 251, 255, true},
          std::nullopt, false },
        { Token::MentionBg, "chat.mentionBg", "Mention background", "Chat",
          Platform::Color{88, 101, 242, 77, true}, std::nullopt, true },

        // presence
        { Token::StatusOnline, "status.online", "Online", "Presence", Platform::Color{35, 165, 90, 255, true},
          std::nullopt, false },
        { Token::StatusIdle, "status.idle", "Idle", "Presence", Platform::Color{240, 178, 50, 255, true},
          std::nullopt, false },
        { Token::StatusDnd, "status.dnd", "Do not disturb", "Presence", Platform::Color{242, 63, 67, 255, true},
          std::nullopt, false },
        { Token::StatusOffline, "status.offline", "Offline", "Presence", Platform::Color{128, 132, 142, 255, true},
          std::nullopt, false },
        { Token::StatusStreaming, "status.streaming", "Streaming", "Presence", Platform::Color{89, 54, 149, 255, true},
          std::nullopt, false },
    };
    return reg;
}

const TokenDescriptor *findById(std::string_view id)
{
    for (const TokenDescriptor &d : registry()) {
        if (id == d.id)
            return &d;
    }
    return nullptr;
}

const TokenDescriptor &descriptor(Token token)
{
    for (const TokenDescriptor &d : registry()) {
        if (d.token == token)
            return d;
    }
    return registry().front(); // just in case
}

} // namespace Theme
} // namespace Core
} // namespace Styx
