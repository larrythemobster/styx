#include "UI/Rml/GroupDmDetailsForm.hpp"
#include "UI/Rml/RmlRuntime.hpp"
#include "UI/Rml/RmlView.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Input.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <SDL3/SDL.h>

#include <format>
#include <string>
#include <utility>

using namespace Styx::UI::Rml;

namespace {

class TestViewFixture
{
public:
    explicit TestViewFixture(int width = 800, int height = 600)
        : view("test-group-dm-details-form")
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
        Styx::Platform::ImageBuffer preview;
        preview.width = 64;
        preview.height = 64;
        preview.pixels.assign(64 * 64 * 4, 255);
        Runtime::instance().setFrame("group-details-test-icon", preview);
    }

    ~TestViewFixture()
    {
        closeDocument();
        Runtime::instance().clearFrame("group-details-test-icon");
    }

    ::Rml::ElementDocument *loadDocument(const std::string &path = "ui/group_dm_details.rml")
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

    void requestRender()
    {
        view.requestRender();
    }

    ::Rml::Context *context()
    {
        return view.context();
    }

    std::string previewSource() const { return "frame:group-details-test-icon"; }

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

void setInputValue(::Rml::ElementDocument *doc, const std::string &value)
{
    auto *inputEl = requireElement(doc, "#gdm-details-name");
    auto *ctl = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(inputEl);
    REQUIRE(ctl);
    ctl->SetValue(value);
    ctl->DispatchEvent("change", {{"value", ::Rml::Variant(value)}});
}

} // namespace

TEST_CASE("Group DM details edit JSON semantics revert and replacement no-op", "[group_dm_details][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    GroupDmDetailsForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);

    form.reset({ "Original Group", "hash123", "icon:camera" });
    fixture.update();

    CHECK_FALSE(form.isDirty());

    setInputValue(doc, "Renamed Group");
    fixture.update();
    CHECK(form.isDirty());

    auto res = form.edit();
    REQUIRE(res.success());
    CHECK(res.value->name.hasValue());
    CHECK(res.value->name.get() == "Renamed Group");
    CHECK(res.value->icon.isUndefined());
    const nlohmann::json expectedName = nlohmann::json::parse(R"({"name": "Renamed Group"})");
    CHECK(nlohmann::json::parse(res.value->toJson().toBytes()) == expectedName);

    setInputValue(doc, "Original Group");
    fixture.update();
    CHECK_FALSE(form.isDirty());

    form.removeIcon();
    fixture.update();
    CHECK(form.isDirty());

    res = form.edit();
    REQUIRE(res.success());
    CHECK(res.value->name.isUndefined());
    CHECK(res.value->icon.isNull());
    const nlohmann::json expectedIconNull = nlohmann::json::parse(R"({"icon": null})");
    CHECK(nlohmann::json::parse(res.value->toJson().toBytes()) == expectedIconNull);

    form.reset({ "Original Group", "hash123", "icon:camera" });
    fixture.update();
    CHECK_FALSE(form.isDirty());

    form.setIcon("data:image/png;base64,AAAA", fixture.previewSource(), 64, 64);
    fixture.update();
    CHECK(form.isDirty());

    res = form.edit();
    REQUIRE(res.success());
    CHECK(res.value->icon.hasValue());
    CHECK(res.value->icon.get() == "data:image/png;base64,AAAA");

    form.reset({ "Empty Icon Group", "", "" });
    fixture.update();
    CHECK_FALSE(form.isDirty());

    form.setIcon("data:image/png;base64,BBBB", fixture.previewSource(), 64, 64);
    fixture.update();
    CHECK(form.isDirty());

    form.removeIcon();
    fixture.update();
    CHECK_FALSE(form.isDirty());
}

TEST_CASE("Group DM details clean-field live rebase preserving dirty fields and caret", "[group_dm_details][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    GroupDmDetailsForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);

    form.reset({ "Original Name", "hash1", "icon:camera" });
    fixture.update();

    setInputValue(doc, "Dirty Name");
    fixture.update();
    CHECK(form.isDirty());

    auto *nameInput = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(requireElement(doc, "#gdm-details-name"));
    REQUIRE(nameInput);
    form.focusName();
    nameInput->SetSelectionRange(2, 2);
    form.updateBaseline({ "Server Name", "hash2", "icon:camera" });
    fixture.update();

    int selectionStart = -1;
    int selectionEnd = -1;
    nameInput->GetSelection(&selectionStart, &selectionEnd, nullptr);
    CHECK(selectionStart == 2);
    CHECK(selectionEnd == 2);
    CHECK(fixture.context()->GetFocusElement() == nameInput);
    CHECK(nameInput->GetValue() == "Dirty Name");
    CHECK(form.isDirty());

    setInputValue(doc, "Server Name");
    fixture.update();
    CHECK_FALSE(form.isDirty());

    form.setIcon("data:image/png;base64,CCCC", fixture.previewSource(), 64, 64);
    fixture.update();
    CHECK(form.isDirty());

    form.updateBaseline({ "Server Name 2", "hash2", "icon:camera" });
    fixture.update();

    CHECK(nameInput->GetValue() == "Server Name 2");
    CHECK(form.isDirty());

    auto res = form.edit();
    REQUIRE(res.success());
    CHECK(res.value->name.isUndefined());
    CHECK(res.value->icon.hasValue());
    CHECK(res.value->icon.get() == "data:image/png;base64,CCCC");

    form.reset({ "Normalized Name", "saved-hash", "icon:camera" });
    fixture.update();
    CHECK_FALSE(form.isDirty());
    CHECK(nameInput->GetValue() == "Normalized Name");
    setInputValue(doc, "Second Edit");
    res = form.edit();
    REQUIRE(res.success());
    CHECK(res.value->name.get() == "Second Edit");
    CHECK(res.value->icon.isUndefined());
}

TEST_CASE("Group DM details Unicode 100 boundary invalid changed names and exact bytes", "[group_dm_details][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    GroupDmDetailsForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);

    form.reset({ "", "", "" });
    fixture.update();

    auto *saveBtn = requireElement(doc, "#gdm-details-save");

    setInputValue(doc, "   ");
    fixture.update();
    CHECK(form.isDirty());
    CHECK_FALSE(form.edit().success());
    CHECK(saveBtn->HasAttribute("disabled"));

    setInputValue(doc, std::string("\xff\xfe", 2));
    fixture.update();
    CHECK(form.isDirty());
    CHECK_FALSE(form.edit().success());
    CHECK(saveBtn->HasAttribute("disabled"));

    const std::string character = "\xe7\x95\x8c";
    std::string valid100;
    for (int i = 0; i < 100; ++i)
        valid100 += character;
    setInputValue(doc, valid100);
    fixture.update();
    CHECK(form.isDirty());
    auto res = form.edit();
    REQUIRE(res.success());
    CHECK(res.value->name.get() == valid100);
    CHECK_FALSE(saveBtn->HasAttribute("disabled"));

    const std::string invalid101 = valid100 + character;
    setInputValue(doc, invalid101);
    fixture.update();
    CHECK(form.isDirty());
    CHECK_FALSE(form.edit().success());
    CHECK(saveBtn->HasAttribute("disabled"));

    const std::string exactTyped = "  Exact Typed Bytes  ";
    setInputValue(doc, exactTyped);
    fixture.update();
    res = form.edit();
    REQUIRE(res.success());
    CHECK(res.value->name.get() == exactTyped);

    form.reset({ "", "", "" });
    form.setIcon("data:image/png;base64,DDDD", fixture.previewSource(), 64, 64);
    fixture.update();
    CHECK(form.isDirty());
    res = form.edit();
    REQUIRE(res.success());
    CHECK(res.value->name.isUndefined());
    CHECK(res.value->icon.hasValue());
}

TEST_CASE("Group DM details pending synthetic rejection retry typing and escape", "[group_dm_details][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    GroupDmDetailsForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);

    form.reset({ "Pending Test", "h1", "icon:camera" });
    form.setPending(true);
    fixture.update();

    auto *nameInput = requireElement(doc, "#gdm-details-name");
    auto *chooseBtn = requireElement(doc, "#gdm-details-choose");
    auto *removeBtn = requireElement(doc, "#gdm-details-remove");
    auto *resetBtn = requireElement(doc, "#gdm-details-reset");
    auto *saveBtn = requireElement(doc, "#gdm-details-save");

    CHECK(nameInput->HasAttribute("disabled"));
    CHECK(chooseBtn->HasAttribute("disabled"));
    CHECK(removeBtn->HasAttribute("disabled"));
    CHECK(resetBtn->HasAttribute("disabled"));
    CHECK(saveBtn->HasAttribute("disabled"));

    int submits = 0;
    int picks = 0;
    int removes = 0;
    int resets = 0;
    int closes = 0;

    auto subConn = form.submitRequested.connect([&] { ++submits; });
    auto pickConn = form.pickIconRequested.connect([&] { ++picks; });
    auto remConn = form.removeIconRequested.connect([&] { ++removes; });
    auto resConn = form.resetRequested.connect([&] { ++resets; });
    auto clsConn = form.closeRequested.connect([&] { ++closes; });

    saveBtn->DispatchEvent("click", {});
    chooseBtn->DispatchEvent("click", {});
    removeBtn->DispatchEvent("click", {});
    resetBtn->DispatchEvent("click", {});
    fixture.update();

    CHECK(submits == 0);
    CHECK(picks == 0);
    CHECK(removes == 0);
    CHECK(resets == 0);

    form.setIcon("data:image/png;base64,FAIL", "frame:fail", 64, 64);
    CHECK_FALSE(form.isDirty());
    form.removeIcon();
    CHECK_FALSE(form.isDirty());

    fixture.context()->ProcessKeyDown(::Rml::Input::KI_ESCAPE, 0);
    fixture.context()->ProcessKeyUp(::Rml::Input::KI_ESCAPE, 0);
    CHECK(closes == 1);

    auto *cancelBtn = requireElement(doc, "#gdm-details-cancel");
    cancelBtn->DispatchEvent("click", {});
    CHECK(closes == 2);

    form.setPending(false);
    fixture.update();
    form.focusName();
    fixture.context()->ProcessTextInput("Retried");

    auto *ctl = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("gdm-details-name"));
    REQUIRE(ctl);
    CHECK(ctl->GetValue().find("Retried") != std::string::npos);
}

TEST_CASE("Group DM details escapes saved names icons and server errors", "[group_dm_details][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    GroupDmDetailsForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);

    form.reset({ "<script id=\"xss-name\">bad</script>", "h", "icon:camera\"/><img id=\"xss-icon\"/>" });
    form.setError("<b id=\"xss-err\">error</b>");
    fixture.update();

    CHECK(doc->QuerySelector("#xss-name") == nullptr);
    CHECK(doc->QuerySelector("#xss-icon") == nullptr);
    CHECK(doc->QuerySelector("#xss-err") == nullptr);
}
