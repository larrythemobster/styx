#include "UI/Rml/GroupDmCreateForm.hpp"
#include "UI/Rml/RmlRuntime.hpp"
#include "UI/Rml/RmlView.hpp"
#include "Core/Snowflake.hpp"
#include "Platform/Native.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Input.h>
#include <catch2/catch_test_macros.hpp>
#include <SDL3/SDL.h>

#include <format>
#include <string>
#include <utility>
#include <vector>

using namespace Styx::UI::Rml;
namespace Core = Styx::Core;
namespace Platform = Styx::Platform;

namespace {

class TestViewFixture {
public:
    explicit TestViewFixture(int width = 800, int height = 600)
        : view("test-group-dm-create-form")
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
    }

    ~TestViewFixture()
    {
        closeDocument();
    }

    ::Rml::ElementDocument *loadDocument(const std::string &path = "ui/create_group_dm.rml")
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

    void requestRender() { view.requestRender(); }

    ::Rml::Context *context() { return view.context(); }

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

std::vector<GroupDmFriend> makeTestFriends()
{
    return {
        { Core::Snowflake(101), "Alice Smith", "alicesmith", "icon:user-plus" },
        { Core::Snowflake(102), "Bob Jones", "bobjones", "icon:user-plus" },
        { Core::Snowflake(103), "Charlie Brown", "charlie", "icon:user-plus" },
        { Core::Snowflake(104), "Diana Prince", "wonderdiana", "icon:user-plus" },
    };
}

} // namespace

TEST_CASE("Group DM create form capacity and deselection", "[group_dm][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    GroupDmCreateForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);
    form.reset(makeTestFriends(), 2, true);
    fixture.update();

    auto *friend101 = requireElement(doc, "#group-dm-friend-101");
    auto *friend102 = requireElement(doc, "#group-dm-friend-102");
    auto *friend103 = requireElement(doc, "#group-dm-friend-103");

    friend101->DispatchEvent("click", {});
    fixture.update();
    CHECK(form.selected().size() == 1);
    CHECK(form.selected().front() == Core::Snowflake(101));

    friend102 = requireElement(doc, "#group-dm-friend-102");
    friend102->DispatchEvent("click", {});
    fixture.update();
    CHECK(form.selected().size() == 2);

    friend103 = requireElement(doc, "#group-dm-friend-103");
    CHECK(friend103->HasAttribute("disabled"));

    friend103->DispatchEvent("click", {});
    fixture.update();
    CHECK(form.selected().size() == 2);

    friend101 = requireElement(doc, "#group-dm-friend-101");
    friend101->DispatchEvent("click", {});
    fixture.update();
    CHECK(form.selected().size() == 1);
    CHECK(form.selected().front() == Core::Snowflake(102));

    friend103 = requireElement(doc, "#group-dm-friend-103");
    REQUIRE_FALSE(friend103->HasAttribute("disabled"));
}

TEST_CASE("Group DM create form preserves selection across filtering", "[group_dm][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    GroupDmCreateForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);
    form.reset(makeTestFriends(), 3, true);
    fixture.update();

    auto *friend101 = requireElement(doc, "#group-dm-friend-101");
    friend101->DispatchEvent("click", {});
    fixture.update();
    REQUIRE(form.selected().size() == 1);

    auto *search = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("group-dm-search"));
    REQUIRE(search != nullptr);
    search->Focus();
    search->SetValue("Bob");
    search->DispatchEvent("change", {});
    fixture.update();

    CHECK(doc->GetElementById("group-dm-friend-101") == nullptr);
    CHECK(form.selected().size() == 1);
    CHECK(form.selected().front() == Core::Snowflake(101));

    auto *friend102 = requireElement(doc, "#group-dm-friend-102");
    friend102->DispatchEvent("click", {});
    fixture.update();
    CHECK(form.selected().size() == 2);
    CHECK(form.selected()[0] == Core::Snowflake(101));
    CHECK(form.selected()[1] == Core::Snowflake(102));

    search = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("group-dm-search"));
    REQUIRE(search != nullptr);
    search->SetValue("");
    search->DispatchEvent("change", {});
    fixture.update();

}

TEST_CASE("Group DM create form candidate pruning preserves remaining selection", "[group_dm][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    GroupDmCreateForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);
    form.reset(makeTestFriends(), 3, true);
    fixture.update();

    requireElement(doc, "#group-dm-friend-101")->DispatchEvent("click", {});
    requireElement(doc, "#group-dm-friend-102")->DispatchEvent("click", {});
    fixture.update();
    REQUIRE(form.selected().size() == 2);

    std::vector<GroupDmFriend> updatedFriends = {
        { Core::Snowflake(102), "Bob Jones", "bobjones", "icon:user-plus" },
        { Core::Snowflake(103), "Charlie Brown", "charlie", "icon:user-plus" },
        { Core::Snowflake(104), "Diana Prince", "wonderdiana", "icon:user-plus" },
    };
    form.setCandidates(std::move(updatedFriends));
    fixture.update();

    CHECK(form.selected().size() == 1);
    CHECK(form.selected().front() == Core::Snowflake(102));
    CHECK(doc->GetElementById("group-dm-friend-101") == nullptr);
    auto *friend102 = requireElement(doc, "#group-dm-friend-102");
    REQUIRE_FALSE(friend102->HasAttribute("disabled"));
}

TEST_CASE("Group DM create form empty allowed vs conversion mode", "[group_dm][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    GroupDmCreateForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);

    bool submitFired = false;
    auto conn = form.submitRequested.connect([&] { submitFired = true; });

    form.reset(makeTestFriends(), 9, true);
    fixture.update();

    auto *submitBtn = requireElement(doc, "#group-dm-submit");
    REQUIRE_FALSE(submitBtn->HasAttribute("disabled"));
    submitBtn->DispatchEvent("click", {});
    fixture.update();
    CHECK(submitFired);

    submitFired = false;
    form.reset(makeTestFriends(), 8, false);
    fixture.update();

    submitBtn = requireElement(doc, "#group-dm-submit");
    CHECK(submitBtn->HasAttribute("disabled"));
    submitBtn->DispatchEvent("click", {});
    fixture.update();
    CHECK_FALSE(submitFired);

    auto *friend101 = requireElement(doc, "#group-dm-friend-101");
    friend101->DispatchEvent("click", {});
    fixture.update();

    submitBtn = requireElement(doc, "#group-dm-submit");
    REQUIRE_FALSE(submitBtn->HasAttribute("disabled"));
    submitBtn->DispatchEvent("click", {});
    fixture.update();
    CHECK(submitFired);
}

TEST_CASE("Group DM create form pending blocks mutation but allows cancel", "[group_dm][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    GroupDmCreateForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);
    form.reset(makeTestFriends(), 3, true);
    fixture.update();

    auto *friend101 = requireElement(doc, "#group-dm-friend-101");
    friend101->DispatchEvent("click", {});
    fixture.update();
    REQUIRE(form.selected().size() == 1);

    bool submitFired = false;
    bool cancelFired = false;
    auto connSubmit = form.submitRequested.connect([&] { submitFired = true; });
    auto connCancel = form.cancelRequested.connect([&] { cancelFired = true; });

    form.setPending(true);
    fixture.update();

    auto *submitBtn = requireElement(doc, "#group-dm-submit");
    submitBtn->DispatchEvent("click", {});
    fixture.update();
    CHECK_FALSE(submitFired);

    auto *friend102 = requireElement(doc, "#group-dm-friend-102");
    friend102->DispatchEvent("click", {});
    fixture.update();
    CHECK(form.selected().size() == 1);
    CHECK(form.selected().front() == Core::Snowflake(101));

    friend101 = requireElement(doc, "#group-dm-friend-101");
    friend101->DispatchEvent("click", {});
    fixture.update();
    CHECK(form.selected().size() == 1);

    std::vector<GroupDmFriend> replacement = {
        { Core::Snowflake(102), "Bob Jones", "bobjones", "icon:user-plus" },
    };
    form.setCandidates(replacement);
    fixture.update();
    CHECK(form.selected().size() == 1);
    CHECK(form.selected().front() == Core::Snowflake(101));

    auto *cancelBtn = requireElement(doc, "#group-dm-cancel");
    cancelBtn->DispatchEvent("click", {});
    fixture.update();
    CHECK(cancelFired);
}

TEST_CASE("Group DM create form escapes markup in friend labels", "[group_dm][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    GroupDmCreateForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);

    std::vector<GroupDmFriend> xssFriends = {
        { Core::Snowflake(201), "<script id=\"xss\">alert(1)</script>", "hack<b id=\"bold\">name</b>", "icon:user-plus" },
    };
    form.reset(std::move(xssFriends), 3, true);
    form.setError("<img id='error-injected'/>");
    fixture.update();

    CHECK(doc->QuerySelector("#xss") == nullptr);
    CHECK(doc->QuerySelector("#bold") == nullptr);
    CHECK(doc->QuerySelector("#error-injected") == nullptr);
}

TEST_CASE("Group DM search retains its caret through candidate and selection changes", "[group_dm][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();
    GroupDmCreateForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);
    form.reset(makeTestFriends(), 3, true);
    form.focusSearch();
    fixture.context()->ProcessTextInput("Bo");
    fixture.update();

    auto *search = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("group-dm-search"));
    REQUIRE(search);
    search->SetSelectionRange(1, 1);
    form.setCandidates(makeTestFriends());
    requireElement(doc, "#group-dm-friend-102")->DispatchEvent("click", {});
    fixture.update();

    int start = -1;
    int end = -1;
    search->GetSelection(&start, &end, nullptr);
    CHECK(start == 1);
    CHECK(end == 1);
    CHECK(fixture.context()->GetFocusElement() == search);
    CHECK(search->GetValue() == "Bo");
    fixture.context()->ProcessTextInput("x");
    fixture.update();
    CHECK(search->GetValue() == "Bxo");
    REQUIRE(form.selected().size() == 1);
    CHECK(form.selected().front() == Core::Snowflake(102));

    int submissions = 0;
    int cancellations = 0;
    auto submitConnection = form.submitRequested.connect([&] { ++submissions; });
    auto cancelConnection = form.cancelRequested.connect([&] { ++cancellations; });
    fixture.context()->ProcessKeyDown(::Rml::Input::KI_RETURN, 0);
    fixture.context()->ProcessKeyUp(::Rml::Input::KI_RETURN, 0);
    CHECK(submissions == 1);
    form.setPending(true);
    fixture.context()->ProcessKeyDown(::Rml::Input::KI_RETURN, 0);
    fixture.context()->ProcessKeyUp(::Rml::Input::KI_RETURN, 0);
    CHECK(submissions == 1);
    fixture.context()->ProcessKeyDown(::Rml::Input::KI_ESCAPE, 0);
    fixture.context()->ProcessKeyUp(::Rml::Input::KI_ESCAPE, 0);
    CHECK(cancellations == 1);
}
