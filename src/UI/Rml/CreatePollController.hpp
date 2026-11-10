#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"
#include "Platform/Trackable.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace Rml {
class ElementDocument;
}

namespace Styx {

namespace Core {
class ClientInstance;
struct PickerEmoji;
} // namespace Core

namespace UI {
namespace Rml {

class ExpressionPickerController;
class PollCreateForm;

class CreatePollController final : public Controller
{
public:
    explicit CreatePollController(Shell &shell);
    ~CreatePollController() override;

    void open();
    void close();
    [[nodiscard]] bool isOpen() const { return visible; }

    static bool canCreate(Core::ClientInstance *instance, Core::Snowflake channelId);

private:
    void ensureDocument();
    void submit();
    void requestEmoji(std::size_t index);
    void onEmojiPicked(const Core::PickerEmoji &emoji);
    void onPickerClosed();

    ::Rml::ElementDocument *document = nullptr;
    std::unique_ptr<PollCreateForm> form;
    std::unique_ptr<ExpressionPickerController> picker;

    bool visible = false;
    bool pending = false;
    bool suppressFocusRestoration = false;
    std::uint64_t generation = 0;
    std::uint64_t pickerGeneration = 0;
    std::optional<std::size_t> pickerTargetIndex;
    std::string nonce;

    Platform::Guard<Core::ClientInstance> originInstance;
    Core::Snowflake originAccountId;
    Core::Snowflake originChannelId;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
