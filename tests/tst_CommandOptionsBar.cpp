#include "UI/Rml/CommandOptionsBar.hpp"
#include "UI/Rml/RmlRuntime.hpp"
#include "UI/Rml/RmlView.hpp"
#include "Discord/ApplicationCommands.hpp"
#include "UI/Chat/SelectCandidates.hpp"
#include "UI/Chat/SlashCommands.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace Styx::UI::Rml;
using namespace Styx::Discord;
using Styx::UI::SelectCandidate;
using Styx::UI::SlashCommandEntry;

namespace {

class TestViewFixture {
public:
    explicit TestViewFixture(int width = 800, int height = 600)
        : view("test-command-options")
    {
        view.resize(width, height);
        view.show();
        REQUIRE(view.window());
        REQUIRE(view.context());
        REQUIRE(Runtime::instance().isInitialised());
        view.renderNow();
    }

    ~TestViewFixture()
    {
        closeDocument();
    }

    ::Rml::ElementDocument *loadHostDocument()
    {
        closeDocument();
        const std::string fullRml = "<rml><head><style>body { font-family: ui; font-size: 16px; }</style></head><body><div id=\"host\"></div></body></rml>";
        currentDoc = view.context()->LoadDocumentFromMemory(fullRml);
        REQUIRE(currentDoc);
        currentDoc->Show();
        view.context()->Update();
        return currentDoc;
    }

    void update() { view.context()->Update(); }

    void closeDocument()
    {
        if (currentDoc) {
            currentDoc->Close();
            view.context()->Update();
            currentDoc = nullptr;
        }
    }

private:
    View view;
    ::Rml::ElementDocument *currentDoc = nullptr;
};

} // namespace

TEST_CASE("Command options bar renders controls, retains drafts across updates and handles pending state", "[commands][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadHostDocument();
    auto *host = doc->GetElementById("host");
    REQUIRE(host != nullptr);

    CommandOptionsBar bar;
    bar.setHost(host);
    CHECK_FALSE(bar.isOpen());

    SlashCommandEntry cmd;
    cmd.displayName = "search";
    cmd.description = "Search command";

    ApplicationCommandOption opt0;
    opt0.type = ApplicationCommandOptionType::STRING;
    opt0.name = "query";
    opt0.description = "Search query";
    opt0.required = true;

    ApplicationCommandOption opt1;
    opt1.type = ApplicationCommandOptionType::INTEGER;
    opt1.name = "limit";
    opt1.description = "Result limit";
    opt1.required = false;

    ApplicationCommandOption opt2;
    opt2.type = ApplicationCommandOptionType::BOOLEAN;
    opt2.name = "include_archived";
    opt2.description = "Include archived";
    opt2.required = false;

    ApplicationCommandOption opt3;
    opt3.type = ApplicationCommandOptionType::STRING;
    opt3.name = "sort";
    opt3.description = "Sort order";
    opt3.required = false;
    opt3.choices = {
        { "Recent", "recent" },
        { "Relevant", "relevant" }
    };

    ApplicationCommandOption opt4;
    opt4.type = ApplicationCommandOptionType::USER;
    opt4.name = "author";
    opt4.description = "Filter by author";
    opt4.required = false;

    ApplicationCommandOption opt5;
    opt5.type = ApplicationCommandOptionType::ATTACHMENT;
    opt5.name = "filter_file";
    opt5.description = "Filter list file";
    opt5.required = false;

    cmd.options = { opt0, opt1, opt2, opt3, opt4, opt5 };

    std::vector<std::vector<SelectCandidate>> candidates(6);
    candidates[4] = {
        { "111", "Alice", "user" },
        { "222", "Bob", "user" }
    };

    bar.show(cmd, candidates);
    CHECK(bar.isOpen());

    // 1. Render produces 5 controls with ids command-option-0..4; attachment has none and shows unsupported text.
    for (int i = 0; i <= 4; ++i) {
        auto *ctrl = host->QuerySelector("#command-option-" + std::to_string(i));
        REQUIRE(ctrl != nullptr);
    }
    CHECK(host->QuerySelector("#command-option-5") == nullptr);

    auto *unsupported = host->QuerySelector(".command-option-unsupported");
    REQUIRE(unsupported != nullptr);
    CHECK(unsupported->GetInnerRML() == "Files aren't supported yet");

    auto *label0 = host->QuerySelector(".command-option-label");
    REQUIRE(label0 != nullptr);
    CHECK(label0->GetInnerRML().ends_with(" *"));

    auto *userSelect = rmlui_dynamic_cast<::Rml::ElementFormControlSelect *>(host->QuerySelector("#command-option-4"));
    REQUIRE(userSelect != nullptr);
    REQUIRE(userSelect->GetNumOptions() == 3);
    CHECK(userSelect->GetOption(0)->GetAttribute<::Rml::String>("value", "") == "");
    CHECK(userSelect->GetOption(1)->GetAttribute<::Rml::String>("value", "") == "111");
    CHECK(userSelect->GetOption(2)->GetAttribute<::Rml::String>("value", "") == "222");

    // 2. Drafts survive re-render: set value via SetValue, call setErrors
    auto *input0 = rmlui_dynamic_cast<::Rml::ElementFormControl *>(host->QuerySelector("#command-option-0"));
    REQUIRE(input0 != nullptr);
    input0->SetValue("typed query");

    bar.setErrors({ "Too short" }, "general");

    auto *input0After = rmlui_dynamic_cast<::Rml::ElementFormControl *>(host->QuerySelector("#command-option-0"));
    REQUIRE(input0After != nullptr);
    CHECK(input0After->GetValue() == "typed query");

    auto *optErr = host->QuerySelector(".command-option-error");
    REQUIRE(optErr != nullptr);
    CHECK(optErr->GetInnerRML() == "Too short");

    auto *cmdErr = host->QuerySelector(".command-options-error");
    REQUIRE(cmdErr != nullptr);
    CHECK(cmdErr->GetInnerRML() == "general");

    const auto drafts = bar.values();
    REQUIRE(drafts.size() == 6);
    CHECK(drafts[0] == "typed query");
    CHECK(drafts[1].empty());
    CHECK(drafts[2].empty());
    CHECK(drafts[3].empty());
    CHECK(drafts[4].empty());
    CHECK(drafts[5].empty());

    // setHost() preserves drafts across host teardown/rebuild
    input0After->SetValue("retained query");
    bar.setHost(nullptr);
    bar.setHost(host);
    CHECK(bar.values()[0] == "retained query");
    auto *input0Reloaded = rmlui_dynamic_cast<::Rml::ElementFormControl *>(host->QuerySelector("#command-option-0"));
    REQUIRE(input0Reloaded != nullptr);
    CHECK(input0Reloaded->GetValue() == "retained query");

    // 3. setPending(true): every control has disabled attribute, status says Sending…; hide(): display none and empty
    bar.setPending(true);
    for (int i = 0; i <= 4; ++i) {
        auto *ctrl = host->QuerySelector("#command-option-" + std::to_string(i));
        REQUIRE(ctrl != nullptr);
        CHECK(ctrl->HasAttribute("disabled"));
    }

    auto *status = host->QuerySelector(".command-options-status");
    REQUIRE(status != nullptr);
    CHECK(status->GetInnerRML() == "Sending…");

    bar.hide();
    CHECK_FALSE(bar.isOpen());
    fixture.update();
    CHECK_FALSE(host->IsVisible());
    CHECK(host->GetNumChildren() == 0);
    CHECK(host->GetInnerRML().empty());
}

TEST_CASE("Command options bar escapes option names and user drafts", "[commands][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadHostDocument();
    auto *host = doc->GetElementById("host");
    REQUIRE(host != nullptr);

    CommandOptionsBar bar;
    bar.setHost(host);

    SlashCommandEntry cmd;
    cmd.displayName = "run";
    ApplicationCommandOption opt;
    opt.type = ApplicationCommandOptionType::STRING;
    opt.name = "option<b>&\"";
    opt.description = "desc<b>&\"";
    cmd.options = { opt };

    bar.show(cmd, {});

    auto *input = rmlui_dynamic_cast<::Rml::ElementFormControl *>(host->QuerySelector("#command-option-0"));
    REQUIRE(input != nullptr);
    input->SetValue("draft<b>&\"");

    bar.setErrors({}, "");

    CHECK(host->QuerySelector("b") == nullptr);
    CHECK(bar.values()[0] == "draft<b>&\"");

    auto *inputAfter = rmlui_dynamic_cast<::Rml::ElementFormControl *>(host->QuerySelector("#command-option-0"));
    REQUIRE(inputAfter != nullptr);
    CHECK(inputAfter->GetValue() == "draft<b>&\"");

    auto *label = host->QuerySelector(".command-option-label");
    REQUIRE(label != nullptr);
    CHECK(label->GetAttribute<::Rml::String>("title", "") == "desc<b>&\"");
}

TEST_CASE("Command options bar manages autocomplete suggestions and selection", "[commands][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadHostDocument();
    auto *host = doc->GetElementById("host");
    REQUIRE(host != nullptr);

    CommandOptionsBar bar;
    bar.setHost(host);

    SlashCommandEntry cmd;
    cmd.displayName = "search";

    ApplicationCommandOption queryOpt;
    queryOpt.type = ApplicationCommandOptionType::STRING;
    queryOpt.name = "query";

    ApplicationCommandOption flagOpt;
    flagOpt.type = ApplicationCommandOptionType::BOOLEAN;
    flagOpt.name = "flag";

    cmd.options = { queryOpt, flagOpt };
    bar.show(cmd, {});

    SECTION("setSuggestions on STRING option renders rows with data attributes and escapes names") {
        bar.setSuggestions(0, {
            { "Apple <b>bold</b>", "appl" },
            { "Banana", "bana" }
        });

        REQUIRE(host->QuerySelector(".command-suggestions") != nullptr);
        CHECK(host->QuerySelector("b") == nullptr);

        ::Rml::ElementList items;
        host->QuerySelectorAll(items, ".command-suggestion");
        REQUIRE(items.size() == 2);

        CHECK(items[0]->GetAttribute<::Rml::String>("data-action", "") == "command-suggestion");
        CHECK(items[0]->GetAttribute<::Rml::String>("data-option", "") == "0");
        CHECK(items[0]->GetAttribute<::Rml::String>("data-choice", "") == "0");
        CHECK(items[0]->GetInnerRML() == "Apple &lt;b&gt;bold&lt;/b&gt;");

        CHECK(items[1]->GetAttribute<::Rml::String>("data-action", "") == "command-suggestion");
        CHECK(items[1]->GetAttribute<::Rml::String>("data-option", "") == "0");
        CHECK(items[1]->GetAttribute<::Rml::String>("data-choice", "") == "1");
        CHECK(items[1]->GetInnerRML() == "Banana");
    }

    SECTION("chooseSuggestion with an integer value puts 42 in values()[i] and removes command-suggestions; a string value goes in as is") {
        bar.setSuggestions(0, {
            { "Choice number", 42 },
            { "Choice text", "custom string" }
        });
        REQUIRE(host->QuerySelector(".command-suggestions") != nullptr);

        CHECK_FALSE(bar.chooseSuggestion(1, 0));
        CHECK_FALSE(bar.chooseSuggestion(0, 5));

        CHECK(bar.chooseSuggestion(0, 0));
        CHECK(bar.values()[0] == "42");
        CHECK(host->QuerySelector(".command-suggestions") == nullptr);
        CHECK_FALSE(bar.chooseSuggestion(0, 0));

        bar.setSuggestions(0, {
            { "Choice text", "custom string" }
        });
        REQUIRE(host->QuerySelector(".command-suggestions") != nullptr);

        CHECK(bar.chooseSuggestion(0, 0));
        CHECK(bar.values()[0] == "custom string");
        CHECK(host->QuerySelector(".command-suggestions") == nullptr);
    }

    SECTION("setSuggestions for a BOOLEAN option index renders nothing and chooseSuggestion returns false") {
        bar.setSuggestions(1, { { "Yes", "yes" } });
        CHECK(host->QuerySelector(".command-suggestions") == nullptr);
        CHECK_FALSE(bar.chooseSuggestion(1, 0));
    }

    SECTION("show() of another command clears suggestions") {
        bar.setSuggestions(0, { { "Apple", "appl" } });
        REQUIRE(host->QuerySelector(".command-suggestions") != nullptr);

        SlashCommandEntry otherCmd;
        otherCmd.displayName = "other";
        otherCmd.options = { queryOpt };
        bar.show(otherCmd, {});

        CHECK(host->QuerySelector(".command-suggestions") == nullptr);
        CHECK_FALSE(bar.chooseSuggestion(0, 0));
    }

    SECTION("Suggestions keep focus and the caret in the field, and picking one refocuses it") {
        auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(host->GetElementById("command-option-0"));
        REQUIRE(input != nullptr);
        input->SetValue("abcd");
        input->Focus();
        input->SetSelectionRange(2, 2);
        bar.setSuggestions(0, { { "Apple", "appl" } });

        auto *rebuilt = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(host->GetElementById("command-option-0"));
        REQUIRE(rebuilt != nullptr);
        CHECK(doc->GetContext()->GetFocusElement() == rebuilt);
        int start = -1;
        int end = -1;
        rebuilt->GetSelection(&start, &end, nullptr);
        CHECK(start == 2);
        CHECK(end == 2);

        host->Focus();
        REQUIRE(bar.chooseSuggestion(0, 0));
        CHECK(doc->GetContext()->GetFocusElement() == host->GetElementById("command-option-0"));
    }
}
