#include "UI/Rml/GroupDmMembersForm.hpp"
#include "UI/Rml/RmlRuntime.hpp"
#include "UI/Rml/RmlView.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Input.h>
#include <catch2/catch_test_macros.hpp>
#include <SDL3/SDL.h>

#include <format>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace Styx::UI::Rml;
namespace Core = Styx::Core;

namespace {

class TestViewFixture
{
public:
    explicit TestViewFixture(int width = 800, int height = 600)
        : view("test-group-dm-members-form")
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

    ::Rml::ElementDocument *loadDocument(const std::string &path = "ui/group_dm_members.rml")
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

GroupDmMemberPerson makePerson(Core::Snowflake id, std::string name, std::string user = {}, bool isSelf = false, bool isOwner = false)
{
    return GroupDmMemberPerson{
        id,
        std::move(name),
        std::move(user),
        "icon:user-plus",
        isSelf,
        isOwner
    };
}

} // namespace

TEST_CASE("Group DM members form owner vs nonowner and self removal", "[group_dm_members][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    GroupDmMembersForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);

    const Core::Snowflake selfId(101);
    const Core::Snowflake memberId(102);

    GroupDmMembersSnapshot ownerSnapshot{
        .members = {
            makePerson(selfId, "Self User", "selfuser", true, true),
            makePerson(memberId, "Other User", "otheruser", false, false),
        },
        .friends = {},
        .canRemove = true,
    };

    form.reset(std::move(ownerSnapshot));
    fixture.update();

    CHECK(doc->QuerySelector(std::format("#gdm-members-remove-{}", selfId.toString()).c_str()) == nullptr);

    auto *removeBtn = requireElement(doc, std::format("#gdm-members-remove-{}", memberId.toString()).c_str());

    std::vector<Core::Snowflake> removedIds;
    auto removeConn = form.removeRequested.connect([&](Core::Snowflake id) {
        removedIds.push_back(id);
    });

    removeBtn->DispatchEvent("click", {});
    fixture.update();
    REQUIRE(removedIds.size() == 1);
    CHECK(removedIds.front() == memberId);

    // Synthetic remove click on self must be rejected
    removeBtn->SetAttribute("data-user-id", selfId.toString());
    removeBtn->DispatchEvent("click", {});
    fixture.update();
    CHECK(removedIds.size() == 1);

    // Switch to non-owner
    GroupDmMembersSnapshot nonOwnerSnapshot{
        .members = {
            makePerson(selfId, "Self User", "selfuser", true, false),
            makePerson(memberId, "Other User", "otheruser", false, true),
        },
        .friends = {},
        .canRemove = false,
    };

    form.update(std::move(nonOwnerSnapshot));
    fixture.update();

    CHECK(doc->QuerySelector(std::format("#gdm-members-remove-{}", selfId.toString()).c_str()) == nullptr);
    CHECK(doc->QuerySelector(std::format("#gdm-members-remove-{}", memberId.toString()).c_str()) == nullptr);
}

TEST_CASE("Group DM members form capacity 9 to 10 and back", "[group_dm_members][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    GroupDmMembersForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);

    std::vector<GroupDmMemberPerson> nineMembers;
    nineMembers.reserve(9);
    nineMembers.push_back(makePerson(Core::Snowflake(100), "Owner", "owner", true, true));
    for (std::uint64_t i = 1; i <= 8; ++i)
        nineMembers.push_back(makePerson(Core::Snowflake(100 + i), std::format("Member {}", i), {}, false, false));

    const Core::Snowflake friend1(201);
    const Core::Snowflake friend2(202);

    GroupDmMembersSnapshot snapshot9{
        .members = nineMembers,
        .friends = {
            makePerson(friend1, "Friend One", "friend1"),
            makePerson(friend2, "Friend Two", "friend2"),
        },
        .canRemove = true,
    };

    form.reset(snapshot9);
    fixture.update();

    auto *countEl = requireElement(doc, "#gdm-members-count");
    CHECK(countEl->GetInnerRML() == "9 / 10");

    auto *addBtn1 = requireElement(doc, std::format("#gdm-members-add-{}", friend1.toString()).c_str());
    CHECK_FALSE(addBtn1->HasAttribute("disabled"));

    std::vector<Core::Snowflake> addedIds;
    auto addConn = form.addRequested.connect([&](Core::Snowflake id) {
        addedIds.push_back(id);
    });

    addBtn1->DispatchEvent("click", {});
    fixture.update();
    REQUIRE(addedIds.size() == 1);
    CHECK(addedIds.front() == friend1);

    // Increase to 10 members (capacity reached)
    auto tenMembers = nineMembers;
    tenMembers.push_back(makePerson(friend1, "Friend One", "friend1", false, false));

    GroupDmMembersSnapshot snapshot10{
        .members = std::move(tenMembers),
        .friends = { makePerson(friend2, "Friend Two", "friend2") },
        .canRemove = true,
    };

    form.update(std::move(snapshot10));
    fixture.update();

    CHECK(countEl->GetInnerRML() == "10 / 10");
    auto *addBtn2 = requireElement(doc, std::format("#gdm-members-add-{}", friend2.toString()).c_str());
    CHECK(addBtn2->HasAttribute("disabled"));

    addBtn2->DispatchEvent("click", {});
    fixture.update();
    CHECK(addedIds.size() == 1);

    // Return back to 9 members
    form.update(std::move(snapshot9));
    fixture.update();

    CHECK(countEl->GetInnerRML() == "9 / 10");
    auto *addBtn2Restored = requireElement(doc, std::format("#gdm-members-add-{}", friend2.toString()).c_str());
    CHECK_FALSE(addBtn2Restored->HasAttribute("disabled"));

    addBtn2Restored->DispatchEvent("click", {});
    fixture.update();
    REQUIRE(addedIds.size() == 2);
    CHECK(addedIds.back() == friend2);
}

TEST_CASE("Group DM members search filters friends and retains caret during updates", "[group_dm_members][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    GroupDmMembersForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);

    const Core::Snowflake selfId(101);
    const Core::Snowflake friendAlice(201);
    const Core::Snowflake friendBob(202);

    GroupDmMembersSnapshot snapshot{
        .members = { makePerson(selfId, "Self User", "self", true, true) },
        .friends = {
            makePerson(friendAlice, "Alice Smith", "alicesmith"),
            makePerson(friendBob, "Bob Jones", "bobjones"),
        },
        .canRemove = true,
    };

    form.reset(std::move(snapshot));
    form.focusSearch();
    fixture.context()->ProcessTextInput("Bo");
    fixture.update();

    auto *search = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("gdm-members-search"));
    REQUIRE(search);
    CHECK(search->GetValue() == "Bo");

    CHECK(doc->QuerySelector(std::format("#gdm-members-add-{}", friendAlice.toString()).c_str()) == nullptr);
    CHECK(doc->QuerySelector(std::format("#gdm-members-add-{}", friendBob.toString()).c_str()) != nullptr);

    search->SetSelectionRange(1, 1);

    // Live update occurs
    const Core::Snowflake newMember(102);
    GroupDmMembersSnapshot liveSnapshot{
        .members = {
            makePerson(selfId, "Self User", "self", true, true),
            makePerson(newMember, "New Member", "newmember", false, false),
        },
        .friends = {
            makePerson(friendAlice, "Alice Smith", "alicesmith"),
            makePerson(friendBob, "Bob Jones", "bobjones"),
        },
        .canRemove = true,
    };

    form.update(std::move(liveSnapshot));
    fixture.update();

    int start = -1;
    int end = -1;
    search->GetSelection(&start, &end, nullptr);
    CHECK(start == 1);
    CHECK(end == 1);
    CHECK(fixture.context()->GetFocusElement() == search);
    CHECK(search->GetValue() == "Bo");

    // Friends list remains filtered
    CHECK(doc->QuerySelector(std::format("#gdm-members-add-{}", friendAlice.toString()).c_str()) == nullptr);
    CHECK(doc->QuerySelector(std::format("#gdm-members-add-{}", friendBob.toString()).c_str()) != nullptr);
}

TEST_CASE("Group DM members pending state and escape close", "[group_dm_members][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    GroupDmMembersForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);

    const Core::Snowflake selfId(101);
    const Core::Snowflake memberId(102);
    const Core::Snowflake friendId(201);

    GroupDmMembersSnapshot snapshot{
        .members = {
            makePerson(selfId, "Self", "self", true, true),
            makePerson(memberId, "Member", "member", false, false),
        },
        .friends = { makePerson(friendId, "Friend", "friend") },
        .canRemove = true,
    };

    form.reset(std::move(snapshot));
    form.setPending(true);
    fixture.update();

    auto *search = requireElement(doc, "#gdm-members-search");
    CHECK(search->HasAttribute("disabled"));

    auto *addBtn = requireElement(doc, std::format("#gdm-members-add-{}", friendId.toString()).c_str());
    CHECK(addBtn->HasAttribute("disabled"));

    auto *removeBtn = requireElement(doc, std::format("#gdm-members-remove-{}", memberId.toString()).c_str());
    CHECK(removeBtn->HasAttribute("disabled"));

    int adds = 0;
    int removes = 0;
    int closes = 0;
    auto addConn = form.addRequested.connect([&](Core::Snowflake) { ++adds; });
    auto removeConn = form.removeRequested.connect([&](Core::Snowflake) { ++removes; });
    auto closeConn = form.closeRequested.connect([&] { ++closes; });

    addBtn->DispatchEvent("click", {});
    removeBtn->DispatchEvent("click", {});
    fixture.update();
    CHECK(adds == 0);
    CHECK(removes == 0);

    GroupDmMembersSnapshot changedOwner{
        .members = {
            makePerson(selfId, "Self", "self", true, false),
            makePerson(memberId, "Member", "member", false, true),
        },
        .friends = { makePerson(friendId, "Friend", "friend") },
        .canRemove = false,
    };
    form.update(std::move(changedOwner));
    fixture.update();
    CHECK(doc->GetElementById("gdm-members-remove-102") == nullptr);
    auto *pendingAdd = requireElement(doc, "#gdm-members-add-201");
    CHECK(pendingAdd->HasAttribute("disabled"));
    pendingAdd->RemoveAttribute("disabled");
    pendingAdd->DispatchEvent("click", {});
    CHECK(adds == 0);

    // Escape closes even while pending
    fixture.context()->ProcessKeyDown(::Rml::Input::KI_ESCAPE, 0);
    fixture.context()->ProcessKeyUp(::Rml::Input::KI_ESCAPE, 0);
    CHECK(closes == 1);

    // Done button works while pending
    auto *doneBtn = requireElement(doc, "#gdm-members-done");
    doneBtn->DispatchEvent("click", {});
    fixture.update();
    CHECK(closes == 2);

    form.setPending(false);
    form.focusSearch();
    fixture.context()->ProcessTextInput("retry");
    auto *retrySearch = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("gdm-members-search"));
    REQUIRE(retrySearch);
    CHECK(retrySearch->GetValue() == "retry");
}

TEST_CASE("Group DM members stale synthetic action and unknown id rejection", "[group_dm_members][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    GroupDmMembersForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);

    const Core::Snowflake selfId(101);
    const Core::Snowflake memberId(102);
    const Core::Snowflake friendId(201);

    GroupDmMembersSnapshot snapshot{
        .members = {
            makePerson(selfId, "Self", "self", true, true),
            makePerson(memberId, "Member", "member", false, false),
        },
        .friends = { makePerson(friendId, "Friend", "friend") },
        .canRemove = true,
    };

    form.reset(std::move(snapshot));
    fixture.update();

    int adds = 0;
    int removes = 0;
    auto addConn = form.addRequested.connect([&](Core::Snowflake) { ++adds; });
    auto removeConn = form.removeRequested.connect([&](Core::Snowflake) { ++removes; });

    auto *addBtn = requireElement(doc, std::format("#gdm-members-add-{}", friendId.toString()).c_str());

    // Unknown friend ID
    addBtn->SetAttribute("data-user-id", "999");
    addBtn->DispatchEvent("click", {});
    fixture.update();
    CHECK(adds == 0);

    // Already member ID
    addBtn->SetAttribute("data-user-id", memberId.toString());
    addBtn->DispatchEvent("click", {});
    fixture.update();
    CHECK(adds == 0);

    // ID 0
    addBtn->SetAttribute("data-user-id", "0");
    addBtn->DispatchEvent("click", {});
    fixture.update();
    CHECK(adds == 0);

    auto *removeBtn = requireElement(doc, std::format("#gdm-members-remove-{}", memberId.toString()).c_str());

    // Unknown member ID
    removeBtn->SetAttribute("data-user-id", "999");
    removeBtn->DispatchEvent("click", {});
    fixture.update();
    CHECK(removes == 0);
}

TEST_CASE("Group DM members escaping all user and server content", "[group_dm_members][rml]")
{
    TestViewFixture fixture;
    auto *doc = fixture.loadDocument();

    GroupDmMembersForm form([&] { fixture.requestRender(); });
    form.setDocument(doc);

    GroupDmMembersSnapshot xssSnapshot{
        .members = {
            GroupDmMemberPerson{
                Core::Snowflake(101),
                "<script id=\"xss-name\">alert(1)</script>",
                "hack<b id=\"xss-user\">user</b>",
                "icon:user-plus\"/><img id=\"xss-avatar\"/>",
                true,
                true
            }
        },
        .friends = {
            GroupDmMemberPerson{
                Core::Snowflake(201),
                "<span id=\"xss-friend\">bad</span>",
                "friend<em id=\"xss-friend-user\">em</em>",
                "icon:user-plus",
                false,
                false
            }
        },
        .canRemove = true,
    };

    form.reset(std::move(xssSnapshot));
    form.setError("<div id=\"xss-error\">error</div>");
    fixture.update();

    CHECK(doc->QuerySelector("#xss-name") == nullptr);
    CHECK(doc->QuerySelector("#xss-user") == nullptr);
    CHECK(doc->QuerySelector("#xss-avatar") == nullptr);
    CHECK(doc->QuerySelector("#xss-friend") == nullptr);
    CHECK(doc->QuerySelector("#xss-friend-user") == nullptr);
    CHECK(doc->QuerySelector("#xss-error") == nullptr);
}
