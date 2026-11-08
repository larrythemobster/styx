#pragma once

#include "Controller.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/SearchCompletion.hpp"
#include "Core/SearchQuery.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/MessageSearch.hpp"
#include "Platform/Signal.hpp"
#include "Platform/Trackable.hpp"

#include <string>
#include <vector>

namespace Rml {
class ElementDocument;
class ElementFormControlInput;
}

namespace Styx {
namespace UI {
namespace Rml {

// The message search popover, opened from the chat header for a server or a DM.
class SearchController final : public Controller
{
public:
    explicit SearchController(Shell &shell);
    ~SearchController() override;

    // Searches the server guildId, or the DM channelId when guildId is invalid. scopeName is shown
    // in the header ("Server name" or "@friend"). The box keeps its text when reopened for the same scope.
    void open(Core::ClientInstance *instance, Core::Snowflake guildId, Core::Snowflake channelId, const std::string &scopeName);
    void close();
    [[nodiscard]] bool isOpen() const { return visible; }

    // channel, message
    Platform::Signal<Core::Snowflake, Core::Snowflake> resultActivated;

private:
    void ensureDocument();
    void setupEventListeners();
    // Reads the box, resolves names and loads the first page.
    void submit();
    void loadPage(int page);
    void render();
    // Forgets the last search: query, results and every status.
    void clearResults();
    [[nodiscard]] bool canGoBack() const;
    [[nodiscard]] bool canGoNext() const;
    [[nodiscard]] std::vector<Core::SearchName> people() const;
    [[nodiscard]] std::vector<Core::SearchName> channels() const;
    [[nodiscard]] std::string resultRml(const Discord::Message &message) const;
    // Recomputes suggestions from the box text and redraws them; hides them for an empty list.
    void updateSuggestions();
    // Puts suggestion index's replacement into the box, keeps focus and recomputes suggestions.
    void acceptSuggestion(int index);
    void hideSuggestions();
    void renderSuggestions();
    [[nodiscard]] std::string inputText() const;
    [[nodiscard]] ::Rml::ElementFormControlInput *input() const;

    static constexpr int PAGE_SIZE = 25;

    ::Rml::ElementDocument *searchDocument = nullptr;
    bool visible = false;

    Platform::Guard<Core::ClientInstance> clientInstance;
    Core::Snowflake guildId;
    Core::Snowflake channelId;

    Discord::MessageSearchQuery query;
    std::vector<std::string> unknownNames;
    bool searched = false;
    bool loading = false;
    bool indexNotReady = false;
    std::string error;
    int page = 0;
    int totalResults = 0;
    std::vector<Discord::Message> results;
    int loadGeneration = 0;

    std::vector<Core::SearchSuggestion> suggestions;
    // Highlighted row, or -1 when none is (Enter then searches instead of picking).
    int selectedSuggestion = -1;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
