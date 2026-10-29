#pragma once

#include "Platform/Native.hpp"
#include <optional>
#include <string_view>
#include <vector>

namespace Styx {
namespace Core {
namespace Theme {

enum class Token {
    // backgrounds
    WindowBg,
    BaseBg,
    AlternateBaseBg,
    ButtonBg,
    TooltipBg,
    // text
    WindowText,
    PrimaryText,
    ButtonText,
    TooltipText,
    PlaceholderText,
    DisabledText,
    BrightText,
    // accents
    Highlight,
    HighlightedText,
    LinkText,
    Divider,
    // extended chat elements
    ChatError,
    EmbedDefault,
    MentionText,
    MentionBg,
    // presence
    StatusOnline,
    StatusIdle,
    StatusDnd,
    StatusOffline,
    StatusStreaming,
};

enum class PaletteRole { Window, Base, AlternateBase, Button, ToolTipBase, WindowText,
    Text, ButtonText, ToolTipText, PlaceholderText, BrightText, Highlight,
    HighlightedText, Link, Mid };

struct TokenDescriptor
{
    Token token;
    const char *id;
    const char *label;
    const char *group;
    Platform::Color defaultColor;
    std::optional<PaletteRole> role;
    bool supportsAlpha = false;
};

const std::vector<TokenDescriptor> &registry();
const TokenDescriptor *findById(std::string_view id);
const TokenDescriptor &descriptor(Token token);


} // namespace Theme
} // namespace Core
} // namespace Styx
