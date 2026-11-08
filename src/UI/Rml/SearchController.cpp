#include "SearchController.hpp"

#include "Shell.hpp"
#include "RmlView.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/JsonUtils.hpp"
#include "Core/NotificationText.hpp"
#include "Core/SearchQuery.hpp"
#include "Core/SearchSyntax.hpp"
#include "Core/UserManager.hpp"
#include "Discord/Entities.hpp"
#include "Platform/Native.hpp"
#include "Platform/Utf8.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

namespace Styx {
namespace UI {
namespace Rml {

namespace {

int localUtcOffsetMinutes()
{
    const std::string text = Platform::DateTime::now().format("%z");
    if (text.size() != 5 || (text[0] != '+' && text[0] != '-'))
        return 0;

    for (size_t i = 1; i < 5; ++i) {
        if (text[i] < '0' || text[i] > '9')
            return 0;
    }

    const int hours = (text[1] - '0') * 10 + (text[2] - '0');
    const int minutes = (text[3] - '0') * 10 + (text[4] - '0');
    const int total = hours * 60 + minutes;
    return text[0] == '-' ? -total : total;
}

std::vector<std::string> extractNames(const Discord::User &user, std::string_view nick = {})
{
    std::vector<std::string> names;
    if (!nick.empty())
        names.emplace_back(nick);
    if (user.globalName.hasValue() && !user.globalName.get().empty())
        names.push_back(user.globalName.get());
    if (user.username.hasValue() && !user.username.get().empty())
        names.push_back(user.username.get());
    return names;
}

} // namespace

SearchController::SearchController(Shell &shell)
    : Controller(shell, nullptr)
{
}

SearchController::~SearchController()
{
    if (searchDocument) {
        searchDocument->Close();
        searchDocument = nullptr;
    }
}

void SearchController::ensureDocument()
{
    if (searchDocument)
        return;

    auto *view = shell.view();
    if (!view || !view->context())
        return;

    searchDocument = view->context()->LoadDocument("ui/search.rml");
    if (!searchDocument)
        return;

    regionRoot = searchDocument;
    setupEventListeners();
}

void SearchController::open(Core::ClientInstance *instance, Core::Snowflake guildId, Core::Snowflake channelId, const std::string &scopeName)
{
    ensureDocument();
    if (!searchDocument)
        return;

    if (this->guildId != guildId || this->channelId != channelId) {
        if (auto *inputEl = input())
            inputEl->SetValue("");
        clearResults();
    }

    clientInstance = instance;
    this->guildId = guildId;
    this->channelId = channelId;

    if (auto *scopeEl = searchDocument->GetElementById("search-scope"))
        scopeEl->SetInnerRML(Platform::htmlEscape(scopeName));

    ++loadGeneration;
    visible = true;
    searchDocument->Show(::Rml::ModalFlag::None);

    render();

    if (auto *inputEl = input())
        inputEl->Focus();

    updateSuggestions();

    requestRender();
}

void SearchController::close()
{
    if (!visible)
        return;

    visible = false;
    ++loadGeneration;

    hideSuggestions();

    if (searchDocument)
        searchDocument->Hide();
    requestRender();
}

void SearchController::setupEventListeners()
{
    if (!searchDocument)
        return;

    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (!target)
            return;

        auto *actionEl = closest(target, "data-action");
        if (!actionEl)
            return;

        const std::string action = attribute(actionEl, "data-action");

        if (action == "close") {
            close();
        } else if (action == "prev-page") {
            if (canGoBack())
                loadPage(page - 1);
        } else if (action == "next-page") {
            if (canGoNext())
                loadPage(page + 1);
        } else if (action == "open-result") {
            const std::string chStr = attribute(actionEl, "data-channel-id");
            const std::string msgStr = attribute(actionEl, "data-message-id");
            const Core::Snowflake chId = Core::JsonUtils::detail::snowflake(chStr);
            const Core::Snowflake msgId = Core::JsonUtils::detail::snowflake(msgStr);
            if (chId.isValid() && msgId.isValid()) {
                resultActivated.publish(chId, msgId);
                close();
            }
        } else if (action == "pick-suggestion") {
            const std::string idxStr = attribute(actionEl, "data-index");
            int idx = -1;
            const auto [ptr, ec] = std::from_chars(idxStr.data(), idxStr.data() + idxStr.size(), idx);
            if (ec == std::errc{} && ptr == idxStr.data() + idxStr.size())
                acceptSuggestion(idx);
        }
    });

    if (auto *inputEl = input()) {
        listen("change", [this](::Rml::Event &) {
            updateSuggestions();
        }, inputEl);

        listen("keydown", [this](::Rml::Event &event) {
            const int key = event.GetParameter<int>("key_identifier", 0);
            if (key == ::Rml::Input::KI_DOWN) {
                if (!suggestions.empty()) {
                    if (selectedSuggestion < 0 || selectedSuggestion + 1 >= static_cast<int>(suggestions.size()))
                        selectedSuggestion = 0;
                    else
                        ++selectedSuggestion;
                    renderSuggestions();
                    event.StopPropagation();
                    return;
                }
            } else if (key == ::Rml::Input::KI_UP) {
                if (!suggestions.empty()) {
                    if (selectedSuggestion <= 0 || selectedSuggestion >= static_cast<int>(suggestions.size()))
                        selectedSuggestion = static_cast<int>(suggestions.size()) - 1;
                    else
                        --selectedSuggestion;
                    renderSuggestions();
                    event.StopPropagation();
                    return;
                }
            } else if (key == ::Rml::Input::KI_TAB) {
                if (!suggestions.empty()) {
                    acceptSuggestion(selectedSuggestion >= 0 ? selectedSuggestion : 0);
                    event.StopPropagation();
                    return;
                }
            } else if (key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_NUMPADENTER) {
                event.StopPropagation();
                if (selectedSuggestion >= 0)
                    acceptSuggestion(selectedSuggestion);
                else {
                    hideSuggestions();
                    submit();
                }
                return;
            } else if (key == ::Rml::Input::KI_ESCAPE) {
                event.StopPropagation();
                if (!suggestions.empty())
                    hideSuggestions();
                else
                    close();
                return;
            }
        }, inputEl);
    }
}

void SearchController::submit()
{
    if (!searchDocument)
        return;

    const std::string text = std::string(Platform::trim(inputText()));
    if (text.empty()) {
        clearResults();
        render();
        return;
    }

    const Core::SearchSyntax syntax = Core::parseSearch(text, localUtcOffsetMinutes());
    Core::ResolvedSearch resolved = Core::resolveSearch(syntax, people(), channels());
    query = std::move(resolved.query);
    unknownNames = std::move(resolved.unknownNames);
    searched = true;

    if (!unknownNames.empty()) {
        results.clear();
        totalResults = 0;
        loading = false;
        indexNotReady = false;
        error.clear();
        render();
        return;
    }

    loadPage(0);
}

void SearchController::loadPage(int p)
{
    if (!clientInstance)
        return;

    page = p;
    query.offset = p * PAGE_SIZE;
    loading = true;
    indexNotReady = false;
    error.clear();
    render();

    const int gen = ++loadGeneration;
    clientInstance->searchMessages(
        guildId,
        channelId,
        query,
        [this, guard = lifetime(), gen](const Core::Result<Discord::MessageSearchResult> &res) {
            if (guard.expired() || gen != loadGeneration)
                return;

            loading = false;
            if (!res.success()) {
                error = "Search failed: " + res.error;
            } else if (res.value->indexNotReady) {
                indexNotReady = true;
                results.clear();
                totalResults = 0;
            } else {
                results = res.value->messages;
                totalResults = res.value->totalResults;
            }
            render();
            requestRender();
        });
}

std::vector<Core::SearchName> SearchController::people() const
{
    if (!clientInstance || !clientInstance->users())
        return {};

    std::vector<Core::SearchName> list;

    if (guildId.isValid()) {
        const auto members = clientInstance->users()->getKnownMembers(guildId);
        for (const auto &member : members) {
            if (!member.user.hasValue())
                continue;

            const auto &user = member.user.get();
            std::string_view nick;
            if (member.nick.hasValue() && !member.nick.get().empty())
                nick = member.nick.get();

            list.push_back(Core::SearchName{ user.id.get(), extractNames(user, nick) });
        }
    } else {
        if (auto ch = clientInstance->getChannel(channelId)) {
            if (ch->recipients.hasValue()) {
                for (const auto &user : ch->recipients.get())
                    list.push_back(Core::SearchName{ user.id.get(), extractNames(user) });
            }
        }

        if (auto selfUser = clientInstance->users()->getUser(clientInstance->accountId()))
            list.push_back(Core::SearchName{ selfUser->id.get(), extractNames(*selfUser) });
    }

    return list;
}

std::vector<Core::SearchName> SearchController::channels() const
{
    if (!clientInstance || !guildId.isValid())
        return {};

    std::vector<Core::SearchName> list;
    const auto guildChannels = clientInstance->getGuildChannels(guildId);
    for (const auto &ch : guildChannels) {
        if (ch.name.hasValue() && !ch.name.get().empty())
            list.push_back(Core::SearchName{ ch.id.get(), { ch.name.get() } });
    }
    return list;
}

std::string SearchController::resultRml(const Discord::Message &message) const
{
    const std::string chIdStr = message.channelId.hasValue() ? message.channelId.get().toString() : std::string{};
    const std::string msgIdStr = message.id.hasValue() ? message.id.get().toString() : std::string{};

    std::string authorStr;
    if (message.author.hasValue() && clientInstance && clientInstance->users())
        authorStr = clientInstance->users()->getAuthorDisplayName(message.author.get(), guildId);

    std::string channelSpan;
    if (guildId.isValid()) {
        std::string chName;
        if (clientInstance && message.channelId.hasValue()) {
            if (auto ch = clientInstance->getChannel(message.channelId.get())) {
                if (ch->name.hasValue())
                    chName = ch->name.get();
            }
        }
        channelSpan = std::format(R"(<span class="search-result-channel">#{}</span>)", Platform::htmlEscape(chName));
    }

    std::string timeStr;
    if (message.timestamp.hasValue() && message.timestamp.get().valid())
        timeStr = message.timestamp.get().format("%m/%d/%Y %I:%M %p");

    const int attachments = message.attachments.hasValue() ? static_cast<int>(message.attachments.get().size()) : 0;
    const int stickers = message.stickerItems.hasValue() ? static_cast<int>(message.stickerItems.get().size()) : 0;
    const Core::MentionNames mentions = clientInstance ? clientInstance->mentionNames(guildId) : Core::MentionNames{};
    const std::string content = message.content.hasValue() ? message.content.get() : std::string{};
    const std::string text = Core::notificationText(content, attachments, stickers, mentions);

    return std::format(
        R"(<div class="search-result" data-action="open-result" data-channel-id="{}" data-message-id="{}"><div class="search-result-header"><span class="search-result-author">{}</span>{}<span class="search-result-time">{}</span></div><div class="search-result-text">{}</div></div>)",
        chIdStr,
        msgIdStr,
        Platform::htmlEscape(authorStr),
        channelSpan,
        Platform::htmlEscape(timeStr),
        Platform::htmlEscape(text));
}

void SearchController::render()
{
    if (!searchDocument)
        return;

    std::string statusText;
    bool showStatus = false;

    if (!unknownNames.empty()) {
        showStatus = true;
        std::string lines;
        for (size_t i = 0; i < unknownNames.size(); ++i) {
            if (i > 0)
                lines += "<br/>";
            lines += std::format("No member or channel named “{}”.", Platform::htmlEscape(unknownNames[i]));
        }
        statusText = lines;
    } else if (!error.empty()) {
        showStatus = true;
        statusText = Platform::htmlEscape(error);
    } else if (indexNotReady) {
        showStatus = true;
        statusText = "Discord is still indexing these messages. Try again in a moment.";
    } else if (loading && results.empty()) {
        showStatus = true;
        statusText = "Searching…";
    } else if (searched && !loading && totalResults == 0) {
        showStatus = true;
        statusText = "No results";
    }

    if (auto *statusEl = searchDocument->GetElementById("search-status")) {
        if (showStatus) {
            statusEl->SetInnerRML(statusText);
            statusEl->SetProperty("display", "block");
        } else {
            statusEl->SetInnerRML("");
            statusEl->SetProperty("display", "none");
        }
    }

    if (auto *countEl = searchDocument->GetElementById("search-count")) {
        if (searched && !results.empty()) {
            const std::string text = totalResults == 1 ? "1 result" : std::format("{} results", totalResults);
            countEl->SetInnerRML(Platform::htmlEscape(text));
            countEl->SetProperty("display", "block");
        } else {
            countEl->SetInnerRML("");
            countEl->SetProperty("display", "none");
        }
    }

    if (auto *resultsEl = searchDocument->GetElementById("search-results")) {
        std::string rows;
        if (!results.empty()) {
            for (const auto &msg : results)
                rows += resultRml(msg);
        }
        resultsEl->SetInnerRML(rows);
    }

    if (auto *pagerEl = searchDocument->GetElementById("search-pager")) {
        if (totalResults > PAGE_SIZE) {
            pagerEl->SetProperty("display", "flex");

            const int totalPages = std::min(static_cast<int>(std::ceil(static_cast<double>(totalResults) / PAGE_SIZE)), 400);
            if (auto *pageEl = searchDocument->GetElementById("search-page"))
                pageEl->SetInnerRML(Platform::htmlEscape(std::format("Page {} of {}", page + 1, totalPages)));

            const bool canPrev = canGoBack();
            if (auto *prevBtn = searchDocument->GetElementById("search-prev")) {
                if (canPrev)
                    prevBtn->RemoveAttribute("disabled");
                else
                    prevBtn->SetAttribute("disabled", "");
            }

            const bool canNext = canGoNext();
            if (auto *nextBtn = searchDocument->GetElementById("search-next")) {
                if (canNext)
                    nextBtn->RemoveAttribute("disabled");
                else
                    nextBtn->SetAttribute("disabled", "");
            }
        } else {
            pagerEl->SetProperty("display", "none");
        }
    }
}

void SearchController::clearResults()
{
    query = {};
    unknownNames.clear();
    searched = false;
    loading = false;
    indexNotReady = false;
    error.clear();
    page = 0;
    totalResults = 0;
    results.clear();
}

bool SearchController::canGoBack() const
{
    return page > 0 && !loading;
}

bool SearchController::canGoNext() const
{
    // Discord rejects offsets past 9975.
    const int nextOffset = (page + 1) * PAGE_SIZE;
    return nextOffset < totalResults && nextOffset <= 9975 && !loading;
}

void SearchController::updateSuggestions()
{
    suggestions = Core::searchSuggestions(inputText(), people(), channels());
    selectedSuggestion = -1;
    renderSuggestions();
}

void SearchController::acceptSuggestion(int index)
{
    auto *inputEl = input();
    if (index < 0 || index >= static_cast<int>(suggestions.size()) || !inputEl)
        return;

    const std::string &replacement = suggestions[index].replacement;
    inputEl->SetValue(replacement);
    const int len = static_cast<int>(replacement.size());
    inputEl->SetSelectionRange(len, len);
    inputEl->Focus();

    updateSuggestions();
}

void SearchController::hideSuggestions()
{
    suggestions.clear();
    selectedSuggestion = -1;
    renderSuggestions();
}

void SearchController::renderSuggestions()
{
    if (!searchDocument)
        return;

    auto *suggestionsEl = searchDocument->GetElementById("search-suggestions");
    if (!suggestionsEl)
        return;

    if (suggestions.empty()) {
        suggestionsEl->SetInnerRML("");
        suggestionsEl->SetProperty("display", "none");
        requestRender();
        return;
    }

    std::string rows;
    for (size_t i = 0; i < suggestions.size(); ++i) {
        const auto &s = suggestions[i];
        const bool isSelected = (static_cast<int>(i) == selectedSuggestion);
        const std::string_view cls = isSelected ? "search-suggestion selected" : "search-suggestion";
        std::string detailSpan;
        if (!s.detail.empty())
            detailSpan = std::format(R"(<span class="search-suggestion-detail">{}</span>)", Platform::htmlEscape(s.detail));

        rows += std::format(
            R"(<div class="{}" data-action="pick-suggestion" data-index="{}"><span class="search-suggestion-label">{}</span>{}</div>)",
            cls,
            i,
            Platform::htmlEscape(s.label),
            detailSpan);
    }

    suggestionsEl->SetInnerRML(rows);
    suggestionsEl->SetProperty("display", "flex");
    requestRender();
}

::Rml::ElementFormControlInput *SearchController::input() const
{
    if (!searchDocument)
        return nullptr;
    return rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(searchDocument->GetElementById("search-input"));
}

std::string SearchController::inputText() const
{
    auto *inputEl = input();
    return inputEl ? inputEl->GetValue() : std::string();
}

} // namespace Rml
} // namespace UI
} // namespace Styx
