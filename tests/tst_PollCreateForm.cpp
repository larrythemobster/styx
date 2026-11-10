#include "UI/Rml/PollCreateForm.hpp"
#include "UI/Rml/RmlRuntime.hpp"
#include "UI/Rml/RmlView.hpp"
#include "Discord/PollCreate.hpp"
#include "Platform/Image.hpp"
#include "Platform/Native.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <catch2/catch_test_macros.hpp>
#include <SDL3/SDL.h>

#include <format>
#include <string>

using namespace Styx::UI::Rml;
namespace Core = Styx::Core;
namespace Discord = Styx::Discord;
namespace Platform = Styx::Platform;

namespace {

class TestViewFixture {
public:
    explicit TestViewFixture(int width = 800, int height = 600)
        : view("test-poll-create-form")
    {
        view.resize(width, height);
        view.show();
        REQUIRE(view.window());
        REQUIRE(view.context());
        REQUIRE(Runtime::instance().isInitialised());
        REQUIRE(SDL_SyncWindow(view.window()));
        View::processEvents();
        REQUIRE(view.context()->GetDimensions().x == width);
        REQUIRE(view.context()->GetDimensions().y == height);
        view.renderNow();

        const Platform::ImageBuffer dummy{ 1, 1, 4, { 255, 255, 255, 255 } };
        Runtime::instance().setFrame(frameKey, dummy);
    }

    ~TestViewFixture()
    {
        closeDocument();
        Runtime::instance().clearFrame(frameKey);
    }

    ::Rml::ElementDocument *loadDocument(const std::string &path = "ui/create_poll.rml")
    {
        closeDocument();
        currentDoc = view.context()->LoadDocument(path);
        REQUIRE(currentDoc);
        currentDoc->Show();
        view.context()->Update();
        return currentDoc;
    }

    void closeDocument()
    {
        if (currentDoc) {
            currentDoc->Close();
            view.context()->Update();
            currentDoc = nullptr;
        }
    }

    void update()
    {
        view.context()->Update();
    }

    void render()
    {
        view.renderNow();
    }

    void requestRender() { view.requestRender(); }

    ::Rml::Context *context() { return view.context(); }

    static inline const std::string frameKey = "test_poll_form_frame";

private:
    View view;
    ::Rml::ElementDocument *currentDoc = nullptr;
};

::Rml::Element *requireElement(::Rml::Element *parent, const char *selector)
{
    auto *el = parent->QuerySelector(selector);
    if (!el)
        FAIL(std::format("Element not found: {}", selector));
    return el;
}

PollCreateForm::ImageSource makeImageSource()
{
    return [](const std::string &, Platform::Size) {
        return "frame:" + TestViewFixture::frameKey;
    };
}

} // namespace

TEST_CASE("Poll create form preserves text inputs across adding and removing answers", "[polls][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    PollCreateForm form(makeImageSource(), [&] { fixture.requestRender(); });
    form.setDocument(doc);
    fixture.update();

    auto *question = rmlui_dynamic_cast<::Rml::ElementFormControlTextArea *>(doc->GetElementById("create-poll-question"));
    REQUIRE(question != nullptr);
    question->Focus();
    fixture.context()->ProcessTextInput("Favorite beverage?");
    fixture.update();

    auto *input0 = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("create-poll-answer-0"));
    REQUIRE(input0 != nullptr);
    input0->Focus();
    fixture.context()->ProcessTextInput("Coffee");
    fixture.update();

    auto *input1 = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("create-poll-answer-1"));
    REQUIRE(input1 != nullptr);
    input1->Focus();
    fixture.context()->ProcessTextInput("Tea");
    fixture.update();

    CHECK(form.draft().question == "Favorite beverage?");
    CHECK(form.draft().answers.size() == 2);
    CHECK(form.draft().answers[0].text == "Coffee");
    CHECK(form.draft().answers[1].text == "Tea");

    auto *addBtn = requireElement(doc, "#create-poll-add");
    addBtn->DispatchEvent("click", {});
    fixture.update();

    CHECK(form.draft().answers.size() == 3);
    CHECK(form.draft().question == "Favorite beverage?");
    CHECK(form.draft().answers[0].text == "Coffee");
    CHECK(form.draft().answers[1].text == "Tea");
    CHECK(form.draft().answers[2].text.empty());

    auto *input2 = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("create-poll-answer-2"));
    REQUIRE(input2 != nullptr);
    CHECK(fixture.context()->GetFocusElement() == input2);

    fixture.context()->ProcessTextInput("Water");
    fixture.update();
    CHECK(form.draft().answers[2].text == "Water");

    auto *removeBtn0 = requireElement(doc, ".create-poll-answer-row[data-index='0'] .create-poll-remove-answer");
    removeBtn0->DispatchEvent("click", {});
    fixture.update();

    CHECK(form.draft().answers.size() == 2);
    CHECK(form.draft().question == "Favorite beverage?");
    CHECK(form.draft().answers[0].text == "Tea");
    CHECK(form.draft().answers[1].text == "Water");

    auto *focused = fixture.context()->GetFocusElement();
    CHECK(focused == doc->GetElementById("create-poll-answer-0"));
}

TEST_CASE("Poll create form enforces minimum two and maximum ten answer rows", "[polls][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    PollCreateForm form(makeImageSource(), {});
    form.setDocument(doc);
    fixture.update();

    CHECK(form.draft().answers.size() == 2);

    auto *remove0 = requireElement(doc, ".create-poll-answer-row[data-index='0'] .create-poll-remove-answer");
    auto *remove1 = requireElement(doc, ".create-poll-answer-row[data-index='1'] .create-poll-remove-answer");
    CHECK(remove0->HasAttribute("disabled"));
    CHECK(remove1->HasAttribute("disabled"));

    remove0->DispatchEvent("click", {});
    fixture.update();
    CHECK(form.draft().answers.size() == 2);

    auto *addBtn = requireElement(doc, "#create-poll-add");
    for (int i = 2; i < 10; ++i) {
        CHECK_FALSE(addBtn->HasAttribute("disabled"));
        addBtn->DispatchEvent("click", {});
        fixture.update();
    }

    CHECK(form.draft().answers.size() == 10);
    CHECK(addBtn->HasAttribute("disabled"));

    addBtn->DispatchEvent("click", {});
    fixture.update();
    CHECK(form.draft().answers.size() == 10);

    for (int i = 9; i >= 2; --i) {
        auto *lastRowRemove = requireElement(doc, std::format(".create-poll-answer-row[data-index='{}'] .create-poll-remove-answer", i).c_str());
        CHECK_FALSE(lastRowRemove->HasAttribute("disabled"));
        lastRowRemove->DispatchEvent("click", {});
        fixture.update();
    }

    CHECK(form.draft().answers.size() == 2);
    CHECK_FALSE(addBtn->HasAttribute("disabled"));
    CHECK(requireElement(doc, ".create-poll-answer-row[data-index='0'] .create-poll-remove-answer")->HasAttribute("disabled"));
    CHECK(requireElement(doc, ".create-poll-answer-row[data-index='1'] .create-poll-remove-answer")->HasAttribute("disabled"));
}

TEST_CASE("Input focus and caret position survive text typing and emoji updates", "[polls][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    PollCreateForm form(makeImageSource(), {});
    form.setDocument(doc);
    fixture.update();

    auto *input0 = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("create-poll-answer-0"));
    REQUIRE(input0 != nullptr);
    input0->Focus();
    fixture.context()->ProcessTextInput("Alpha");
    fixture.update();

    input0->SetSelectionRange(2, 2);
    fixture.update();

    int start = -1;
    int end = -1;
    input0->GetSelection(&start, &end, nullptr);
    CHECK(start == 2);
    CHECK(end == 2);
    CHECK(fixture.context()->GetFocusElement() == input0);

    form.setEmoji(1, Discord::Emoji::unicode("🎉"));
    fixture.update();

    CHECK(fixture.context()->GetFocusElement() == doc->GetElementById("create-poll-answer-0"));
    auto *stillInput0 = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("create-poll-answer-0"));
    REQUIRE(stillInput0 != nullptr);
    stillInput0->GetSelection(&start, &end, nullptr);
    CHECK(start == 2);
    CHECK(end == 2);
}

TEST_CASE("Selected custom and unicode emoji persist in draft and clear reliably", "[polls][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    PollCreateForm form(makeImageSource(), {});
    form.setDocument(doc);
    fixture.update();

    auto *clear0 = requireElement(doc, ".create-poll-answer-row[data-index='0'] .create-poll-clear-emoji");
    CHECK(clear0->HasAttribute("disabled"));

    form.setEmoji(0, Discord::Emoji::custom(Core::Snowflake(777), "sparkles", false));
    fixture.update();

    REQUIRE(form.draft().answers[0].emoji.has_value());
    CHECK(form.draft().answers[0].emoji->id.get() == Core::Snowflake(777));
    CHECK_FALSE(clear0->HasAttribute("disabled"));

    clear0->DispatchEvent("click", {});
    fixture.update();

    REQUIRE_FALSE(form.draft().answers[0].emoji.has_value());
    CHECK(clear0->HasAttribute("disabled"));

    form.setEmoji(0, Discord::Emoji::unicode("🔥"));
    fixture.update();

    REQUIRE(form.draft().answers[0].emoji.has_value());
    CHECK(form.draft().answers[0].emoji->name.valueOr("") == "🔥");
    CHECK_FALSE(clear0->HasAttribute("disabled"));
}

TEST_CASE("Duration and multiselect synchronize with native select and checkbox", "[polls][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    PollCreateForm form(makeImageSource(), {});
    form.setDocument(doc);
    fixture.update();

    auto *durationSelect = rmlui_dynamic_cast<::Rml::ElementFormControlSelect *>(doc->GetElementById("create-poll-duration"));
    REQUIRE(durationSelect != nullptr);
    durationSelect->SetValue("168");
    durationSelect->DispatchEvent("change", {});
    fixture.update();

    CHECK(form.draft().durationHours == 168);

    auto *multiCheckbox = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("create-poll-multiselect"));
    REQUIRE(multiCheckbox != nullptr);
    multiCheckbox->SetAttribute("checked", "");
    multiCheckbox->DispatchEvent("change", {});
    fixture.update();

    CHECK(form.draft().allowMultiselect);
}

TEST_CASE("Pending poll disables submissions while preserving cancellation and Escape", "[polls][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    PollCreateForm form(makeImageSource(), {});
    form.setDocument(doc);
    fixture.update();

    auto *q = rmlui_dynamic_cast<::Rml::ElementFormControlTextArea *>(doc->GetElementById("create-poll-question"));
    REQUIRE(q != nullptr);
    q->Focus();
    fixture.context()->ProcessTextInput("Valid question?");

    auto *a0 = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("create-poll-answer-0"));
    REQUIRE(a0 != nullptr);
    a0->Focus();
    fixture.context()->ProcessTextInput("One");

    auto *a1 = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("create-poll-answer-1"));
    REQUIRE(a1 != nullptr);
    a1->Focus();
    fixture.context()->ProcessTextInput("Two");
    fixture.update();

    auto *submitBtn = requireElement(doc, "#create-poll-submit");
    CHECK_FALSE(submitBtn->HasAttribute("disabled"));

    int submits = 0;
    int cancels = 0;
    auto subConn = form.submitRequested.connect([&] { ++submits; });
    auto cancelConn = form.cancelRequested.connect([&] { ++cancels; });

    submitBtn->DispatchEvent("click", {});
    fixture.update();
    CHECK(submits == 1);

    form.setPending(true);
    fixture.update();

    CHECK(submitBtn->HasAttribute("disabled"));
    CHECK(requireElement(doc, "#create-poll-add")->HasAttribute("disabled"));

    submitBtn->DispatchEvent("click", {});
    fixture.update();
    CHECK(submits == 1);

    auto *cancelBtn = requireElement(doc, "#create-poll-cancel");
    CHECK_FALSE(cancelBtn->HasAttribute("disabled"));
    cancelBtn->DispatchEvent("click", {});
    fixture.update();
    CHECK(cancels == 1);

    fixture.context()->ProcessKeyDown(::Rml::Input::KI_ESCAPE, 0);
    fixture.context()->ProcessKeyUp(::Rml::Input::KI_ESCAPE, 0);
    fixture.update();
    CHECK(cancels == 2);
}

TEST_CASE("HTML injection is escaped in dialog error display", "[polls][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    PollCreateForm form(makeImageSource(), {});
    form.setDocument(doc);
    fixture.update();

    form.setError("<b id=\"injected-attack-tag\">boom</b>");
    fixture.update();

    CHECK(doc->GetElementById("injected-attack-tag") == nullptr);
    auto *errorEl = requireElement(doc, "#create-poll-error");
    CHECK(errorEl->IsVisible());
}

TEST_CASE("Card and footer remain in viewport bounds at 430px with scrollable fields", "[polls][rml]")
{
    TestViewFixture fixture(430, 430);
    auto *doc = fixture.loadDocument();

    PollCreateForm form(makeImageSource(), {});
    form.setDocument(doc);
    fixture.update();

    auto *addBtn = requireElement(doc, "#create-poll-add");
    for (int i = 2; i < 10; ++i) {
        addBtn->DispatchEvent("click", {});
        fixture.update();
    }
    CHECK(form.draft().answers.size() == 10);

    fixture.render();
    fixture.update();

    auto *card = requireElement(doc, "#create-poll-card");
    CHECK(card->GetAbsoluteTop() >= 0.0f);
    CHECK(card->GetAbsoluteTop() + card->GetClientHeight() <= 430.0f);
    CHECK(card->GetAbsoluteLeft() >= 0.0f);
    CHECK(card->GetAbsoluteLeft() + card->GetClientWidth() <= 430.0f);

    auto *footer = requireElement(doc, ".create-poll-footer");
    CHECK(footer->GetAbsoluteTop() + footer->GetClientHeight() <= card->GetAbsoluteTop() + card->GetClientHeight());

    auto *fields = requireElement(doc, "#create-poll-fields");
    CHECK(fields->GetScrollHeight() > fields->GetClientHeight());
}
