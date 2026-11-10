#include "UI/Rml/PollRenderer.hpp"
#include "UI/Rml/RmlRuntime.hpp"
#include "UI/Rml/RmlView.hpp"
#include "Discord/Entities.hpp"
#include "Platform/Image.hpp"
#include "Platform/Native.hpp"

#include <RmlUi/Core.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cmath>
#include <format>
#include <string>
#include <utility>
#include <vector>

using namespace Styx::UI::Rml;
namespace Core = Styx::Core;
namespace Discord = Styx::Discord;
namespace Platform = Styx::Platform;

namespace {

class TestViewFixture {
public:
    explicit TestViewFixture(int width = 800, int height = 600)
        : view("test-polls")
    {
        view.resize(width, height);
        view.show();
        REQUIRE(view.window());
        REQUIRE(view.context());
        REQUIRE(Runtime::instance().isInitialised());
        view.renderNow();

        const Styx::Platform::ImageBuffer dummy{ 1, 1, 4, { 255, 255, 255, 255 } };
        Runtime::instance().setFrame(frameKey, dummy);
    }

    ~TestViewFixture()
    {
        closeDocument();
        Runtime::instance().clearFrame(frameKey);
    }

    ::Rml::ElementDocument *loadRml(const std::string &rml)
    {
        closeDocument();
        const std::string fullRml = "<rml><head>"
            "<link type=\"text/rcss\" href=\"resources/ui/theme.rcss\"/>"
            "<link type=\"text/rcss\" href=\"resources/ui/chat.rcss\"/>"
            "<style>body { display: block; font-family: ui; font-size: 16px; }</style></head><body>" + rml + "</body></rml>";
        currentDoc = view.context()->LoadDocumentFromMemory(fullRml);
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

    static inline const std::string frameKey = "test_poll_frame";

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

PollRenderContext makeContext(const Platform::DateTime &now = {})
{
    PollRenderContext ctx;
    ctx.now = now;
    ctx.imageSource = [](const std::string &, Platform::Size) {
        return "frame:" + TestViewFixture::frameKey;
    };
    return ctx;
}

nlohmann::json makeQuestion(std::string text)
{
    return { { "text", std::move(text) } };
}

nlohmann::json makeAnswer(int id, std::string text, nlohmann::json emoji = nullptr)
{
    nlohmann::json media = { { "text", std::move(text) } };
    if (!emoji.is_null())
        media["emoji"] = std::move(emoji);
    return { { "answer_id", id }, { "poll_media", std::move(media) } };
}

nlohmann::json makeCount(int id, int count, bool meVoted)
{
    return { { "id", id }, { "count", count }, { "me_voted", meVoted } };
}

nlohmann::json makeField(std::string name, std::string value)
{
    return { { "name", std::move(name) }, { "value", std::move(value) } };
}

} // namespace

TEST_CASE("pollTimeLeft formats remaining countdown and reports closed polls", "[polls][rml]")
{
    const auto base = Platform::DateTime::fromUnixMillis(1'700'000'000'000LL);
    CHECK(pollTimeLeft(base.addSeconds(5 * 86400 + 3600), base) == "5d left");
    CHECK(pollTimeLeft(base.addSeconds(3 * 3600 + 59 * 60), base) == "3h left");
    CHECK(pollTimeLeft(base.addSeconds(12 * 60 + 30), base) == "12m left");
    CHECK(pollTimeLeft(base.addSeconds(30), base) == "Less than a minute left");
    CHECK(pollTimeLeft(base.addMillis(500), base) == "Less than a minute left");
    CHECK(pollTimeLeft(base, base) == "Poll closed");
    CHECK(pollTimeLeft(base.addSeconds(-10), base) == "Poll closed");
    CHECK(pollTimeLeft(Platform::DateTime{}, base).empty());
}

TEST_CASE("Open poll renders answers in order with percentage widths, voter highlights, and footer", "[polls][rml]")
{
    TestViewFixture fixture;
    const auto base = Platform::DateTime::fromUnixMillis(1'700'000'000'000LL);
    const auto expiry = base.addSeconds(5 * 86400);
    const auto ctx = makeContext(base);

    const nlohmann::json openPollJson = {
        { "question", makeQuestion("Favorite alien?") },
        { "answers", { makeAnswer(1, "Alien 1"), makeAnswer(2, "Alien 2"), makeAnswer(3, "Alien 3") } },
        { "expiry", expiry.iso8601() },
        { "allow_multiselect", false },
        { "results", {
            { "is_finalized", false },
            { "answer_counts", { makeCount(1, 3, false), makeCount(2, 1, true) } }
        } }
    };

    auto *doc = fixture.loadRml(renderPoll(Discord::Poll::fromJson(openPollJson), ctx));

    ::Rml::ElementList answers;
    doc->QuerySelectorAll(answers, ".poll-answer");
    REQUIRE(answers.size() == 3);
    CHECK(answers[0]->GetAttribute<::Rml::String>("data-answer-id", "") == "1");
    CHECK(answers[1]->GetAttribute<::Rml::String>("data-answer-id", "") == "2");
    CHECK(answers[2]->GetAttribute<::Rml::String>("data-answer-id", "") == "3");

    // Exercise layout with the real stylesheet, not just the serialized width property.
    const auto checkBar = [](::Rml::Element *answer, float fraction) {
        auto *track = requireElement(answer, ".poll-bar");
        auto *fill = requireElement(answer, ".poll-bar-fill");
        REQUIRE(track->GetOffsetWidth() > 0);
        REQUIRE(track->GetOffsetHeight() > 0);
        REQUIRE(fill->GetOffsetHeight() > 0);
        CHECK(std::abs(fill->GetOffsetWidth() - track->GetOffsetWidth() * fraction) < 1.5f);
    };
    checkBar(answers[0], 0.75f);
    checkBar(answers[1], 0.25f);
    checkBar(answers[2], 0.0f);
    auto *question = requireElement(doc, ".poll-question");
    auto *hint = requireElement(doc, ".poll-hint");
    CHECK(hint->GetAbsoluteOffset(::Rml::BoxArea::Border).y
          >= question->GetAbsoluteOffset(::Rml::BoxArea::Border).y + question->GetOffsetHeight());

    CHECK(requireElement(answers[0], ".poll-answer-votes")->GetInnerRML() == "3 votes · 75%");
    CHECK(requireElement(answers[1], ".poll-answer-votes")->GetInnerRML() == "1 vote · 25%");
    CHECK(requireElement(answers[2], ".poll-answer-votes")->GetInnerRML() == "0 votes · 0%");

    CHECK_FALSE(answers[0]->IsClassSet("poll-answer-mine"));
    CHECK(answers[1]->IsClassSet("poll-answer-mine"));
    CHECK_FALSE(answers[2]->IsClassSet("poll-answer-mine"));

    CHECK_FALSE(answers[0]->IsClassSet("poll-answer-winner"));
    CHECK_FALSE(answers[1]->IsClassSet("poll-answer-winner"));
    CHECK_FALSE(answers[2]->IsClassSet("poll-answer-winner"));

    const auto footerText = requireElement(doc, ".poll-footer")->GetInnerRML();
    CHECK(footerText.find("4 votes") != std::string::npos);
    CHECK(footerText.find("left") != std::string::npos);

    CHECK(requireElement(doc, ".poll-hint")->GetInnerRML() == "Select one answer");

    const nlohmann::json multiPollJson = {
        { "question", makeQuestion("Multiselect?") },
        { "answers", { makeAnswer(1, "Option 1") } },
        { "expiry", expiry.iso8601() },
        { "allow_multiselect", true }
    };
    auto *multiDoc = fixture.loadRml(renderPoll(Discord::Poll::fromJson(multiPollJson), ctx));
    CHECK(requireElement(multiDoc, ".poll-hint")->GetInnerRML() == "Select one or more answers");
}

TEST_CASE("Closed poll highlights winners and indicates pending count, zero votes gives no winners", "[polls][rml]")
{
    TestViewFixture fixture;
    const auto base = Platform::DateTime::fromUnixMillis(1'700'000'000'000LL);
    const auto closedCtx = makeContext(base.addSeconds(10));

    const nlohmann::json tiePollJson = {
        { "question", makeQuestion("Tie poll?") },
        { "answers", { makeAnswer(1, "Option A"), makeAnswer(2, "Option B"), makeAnswer(3, "Option C") } },
        { "expiry", base.iso8601() },
        { "allow_multiselect", false },
        { "results", {
            { "is_finalized", false },
            { "answer_counts", { makeCount(1, 2, false), makeCount(2, 2, false), makeCount(3, 0, false) } }
        } }
    };

    auto *tieDoc = fixture.loadRml(renderPoll(Discord::Poll::fromJson(tiePollJson), closedCtx));
    ::Rml::ElementList tieAnswers;
    tieDoc->QuerySelectorAll(tieAnswers, ".poll-answer");
    REQUIRE(tieAnswers.size() == 3);
    CHECK(tieAnswers[0]->IsClassSet("poll-answer-winner"));
    CHECK(tieAnswers[1]->IsClassSet("poll-answer-winner"));
    CHECK_FALSE(tieAnswers[2]->IsClassSet("poll-answer-winner"));

    const auto tieFooterText = requireElement(tieDoc, ".poll-footer")->GetInnerRML();
    CHECK(tieFooterText.find("Poll closed") != std::string::npos);
    CHECK(tieFooterText.find("Final count pending") != std::string::npos);

    // Discord's final tally is authoritative even if this machine's clock is behind.
    auto finalizedPollJson = tiePollJson;
    finalizedPollJson["expiry"] = base.addSeconds(3600).iso8601();
    finalizedPollJson["results"]["is_finalized"] = true;
    auto *finalizedDoc = fixture.loadRml(renderPoll(Discord::Poll::fromJson(finalizedPollJson), closedCtx));
    CHECK(requireElement(finalizedDoc, ".poll-answer")->IsClassSet("poll-answer-winner"));
    const auto finalizedFooter = requireElement(finalizedDoc, ".poll-footer")->GetInnerRML();
    CHECK(finalizedFooter.find("Poll closed") != std::string::npos);
    CHECK(finalizedFooter.find("Final count pending") == std::string::npos);

    const nlohmann::json zeroVotesPollJson = {
        { "question", makeQuestion("Zero votes?") },
        { "answers", { makeAnswer(1, "None A"), makeAnswer(2, "None B") } },
        { "expiry", base.iso8601() },
        { "allow_multiselect", false },
        { "results", {
            { "is_finalized", true },
            { "answer_counts", { makeCount(1, 0, false), makeCount(2, 0, false) } }
        } }
    };

    auto *zeroDoc = fixture.loadRml(renderPoll(Discord::Poll::fromJson(zeroVotesPollJson), closedCtx));
    ::Rml::ElementList zeroAnswers;
    zeroDoc->QuerySelectorAll(zeroAnswers, ".poll-answer");
    REQUIRE(zeroAnswers.size() == 2);
    CHECK_FALSE(zeroAnswers[0]->IsClassSet("poll-answer-winner"));
    CHECK_FALSE(zeroAnswers[1]->IsClassSet("poll-answer-winner"));
}

TEST_CASE("Poll with missing results suppresses progress bars and informs in footer", "[polls][rml]")
{
    TestViewFixture fixture;
    const auto base = Platform::DateTime::fromUnixMillis(1'700'000'000'000LL);
    const auto ctx = makeContext(base);

    const nlohmann::json noResultsJson = {
        { "question", makeQuestion("Pending poll?") },
        { "answers", { makeAnswer(1, "Answer 1") } },
        { "expiry", base.addSeconds(3600).iso8601() },
        { "allow_multiselect", false }
    };

    auto *doc = fixture.loadRml(renderPoll(Discord::Poll::fromJson(noResultsJson), ctx));
    CHECK(doc->QuerySelector(".poll-bar") == nullptr);
    CHECK(requireElement(doc, ".poll-footer")->GetInnerRML().find("Results not loaded") != std::string::npos);
}

TEST_CASE("Untrusted question and answer text cannot inject markup nodes", "[polls][security]")
{
    TestViewFixture fixture;
    const auto base = Platform::DateTime::fromUnixMillis(1'700'000'000'000LL);
    const auto ctx = makeContext(base);

    const nlohmann::json hostileJson = {
        { "question", makeQuestion("<b>Hostile Question</b>") },
        { "answers", { makeAnswer(1, "<b>Hostile Answer</b>") } },
        { "expiry", base.addSeconds(3600).iso8601() },
        { "allow_multiselect", false }
    };

    auto *doc = fixture.loadRml(renderPoll(Discord::Poll::fromJson(hostileJson), ctx));
    CHECK(doc->QuerySelector("b") == nullptr);
}

TEST_CASE("Custom emoji answers render an image emoji element", "[polls][rml]")
{
    TestViewFixture fixture;
    const auto base = Platform::DateTime::fromUnixMillis(1'700'000'000'000LL);
    const auto ctx = makeContext(base);

    const nlohmann::json customEmojiJson = {
        { "question", makeQuestion("Emoji poll?") },
        { "answers", {
            makeAnswer(1, "Custom", { { "id", "1120790948302033046" }, { "name", "meowlien" } })
        } },
        { "expiry", base.addSeconds(3600).iso8601() },
        { "allow_multiselect", false }
    };

    auto *doc = fixture.loadRml(renderPoll(Discord::Poll::fromJson(customEmojiJson), ctx));
    CHECK(requireElement(doc, ".poll-answer img.emoji") != nullptr);
}

TEST_CASE("renderPollResultNotice formats winners, handles decisive outcome ties, and escapes content", "[polls][rml]")
{
    TestViewFixture fixture;
    const auto ctx = makeContext();

    const nlohmann::json winnerEmbedJson = {
        { "type", "poll_result" },
        { "fields", {
            makeField("poll_question_text", "Favorite alien?"),
            makeField("total_votes", "101"),
            makeField("victor_answer_id", "1"),
            makeField("victor_answer_text", "Alien 1"),
            makeField("victor_answer_votes", "100")
        } }
    };

    const auto winnerRml = renderPollResultNotice(Discord::Embed::fromJson(winnerEmbedJson), ctx);
    auto *winnerDoc = fixture.loadRml(winnerRml);
    CHECK(requireElement(winnerDoc, "strong")->GetInnerRML() == "Favorite alien?");
    CHECK(requireElement(winnerDoc, ".poll-result-winner")->GetInnerRML() == "Alien 1");
    CHECK(winnerRml.find("won with 100 votes (99%)") != std::string::npos);

    const nlohmann::json noWinnerEmbedJson = {
        { "type", "poll_result" },
        { "fields", {
            makeField("poll_question_text", "Favorite alien?"),
            makeField("total_votes", "0")
        } }
    };

    const auto noWinnerRml = renderPollResultNotice(Discord::Embed::fromJson(noWinnerEmbedJson), ctx);
    auto *noWinnerDoc = fixture.loadRml(noWinnerRml);
    CHECK(noWinnerRml.find("There was no winner.") != std::string::npos);
    CHECK(noWinnerDoc->QuerySelector(".poll-result-winner") == nullptr);

    const nlohmann::json hostileEmbedJson = {
        { "type", "poll_result" },
        { "fields", {
            makeField("poll_question_text", "<b>Injected</b>"),
            makeField("total_votes", "0")
        } }
    };

    auto *hostileDoc = fixture.loadRml(renderPollResultNotice(Discord::Embed::fromJson(hostileEmbedJson), ctx));
    CHECK(hostileDoc->QuerySelector("b") == nullptr);
}

TEST_CASE("Poll state and choice helpers: pollIsClosed, pollOwnVotes, and togglePollChoice", "[polls][logic]")
{
    const auto base = Platform::DateTime::fromUnixMillis(1'700'000'000'000LL);
    const auto expiry = base.addSeconds(3600);

    const nlohmann::json pollJson = {
        { "question", makeQuestion("Colors?") },
        { "answers", { makeAnswer(10, "Red"), makeAnswer(20, "Green"), makeAnswer(30, "Blue") } },
        { "expiry", expiry.iso8601() },
        { "allow_multiselect", false },
        { "results", {
            { "is_finalized", false },
            { "answer_counts", {
                makeCount(20, 2, true),
                makeCount(10, 5, true),
                makeCount(20, 1, false),
                makeCount(99, 1, true)
            } }
        } }
    };
    const auto poll = Discord::Poll::fromJson(pollJson);

    CHECK_FALSE(pollIsClosed(poll, base));
    CHECK(pollIsClosed(poll, expiry));
    CHECK(pollIsClosed(poll, expiry.addSeconds(10)));

    auto finalizedPoll = poll;
    finalizedPoll.results->isFinalized = true;
    CHECK(pollIsClosed(finalizedPoll, base));

    const auto own = pollOwnVotes(poll);
    const std::vector<int> expectedOwn{ 10, 20 };
    CHECK(own == expectedOwn);

    std::vector<int> singleSelected;
    CHECK(togglePollChoice(singleSelected, poll, 10));
    CHECK(singleSelected == std::vector<int>{ 10 });
    CHECK(togglePollChoice(singleSelected, poll, 20));
    CHECK(singleSelected == std::vector<int>{ 20 });
    CHECK(togglePollChoice(singleSelected, poll, 20));
    CHECK(singleSelected.empty());
    CHECK_FALSE(togglePollChoice(singleSelected, poll, 99));
    CHECK(singleSelected.empty());

    auto multiPoll = poll;
    multiPoll.allowMultiselect = true;
    std::vector<int> multiSelected;
    CHECK(togglePollChoice(multiSelected, multiPoll, 10));
    CHECK(multiSelected == std::vector<int>{ 10 });
    CHECK(togglePollChoice(multiSelected, multiPoll, 30));
    const std::vector<int> expectedMulti{ 10, 30 };
    CHECK(multiSelected == expectedMulti);
    CHECK(togglePollChoice(multiSelected, multiPoll, 10));
    CHECK(multiSelected == std::vector<int>{ 30 });
    CHECK_FALSE(togglePollChoice(multiSelected, multiPoll, 42));
    CHECK(multiSelected == std::vector<int>{ 30 });
}

TEST_CASE("Interactive open poll renders native answer buttons, draft selection, and vote actions", "[polls][rml]")
{
    TestViewFixture fixture;
    const auto base = Platform::DateTime::fromUnixMillis(1'700'000'000'000LL);
    const auto expiry = base.addSeconds(3600);

    const nlohmann::json pollJson = {
        { "question", makeQuestion("Pick one") },
        { "answers", { makeAnswer(1, "Alpha"), makeAnswer(2, "Beta") } },
        { "expiry", expiry.iso8601() },
        { "allow_multiselect", false },
        { "results", {
            { "is_finalized", false },
            { "answer_counts", { makeCount(1, 1, false), makeCount(2, 0, false) } }
        } }
    };
    const auto poll = Discord::Poll::fromJson(pollJson);

    SECTION("Empty selection renders native buttons and disables Vote button") {
        auto ctx = makeContext(base);
        ctx.interactive = true;
        const std::vector<int> emptySelection;
        ctx.selectedAnswers = &emptySelection;

        auto *doc = fixture.loadRml(renderPoll(poll, ctx));

        ::Rml::ElementList answerButtons;
        doc->QuerySelectorAll(answerButtons, "button.poll-answer");
        REQUIRE(answerButtons.size() == 2);
        CHECK(answerButtons[0]->GetAttribute<::Rml::String>("data-action", "") == "poll-answer");
        CHECK(answerButtons[0]->GetAttribute<::Rml::String>("data-answer-id", "") == "1");
        CHECK_FALSE(answerButtons[0]->IsClassSet("poll-answer-selected"));
        CHECK_FALSE(answerButtons[1]->IsClassSet("poll-answer-selected"));

        auto *voteBtn = requireElement(doc, "button.poll-vote-submit");
        CHECK(voteBtn->GetAttribute<::Rml::String>("data-action", "") == "poll-vote");
        CHECK(voteBtn->HasAttribute("disabled"));
        CHECK(doc->QuerySelector("button.poll-vote-remove") == nullptr);
    }

    SECTION("Draft selection marks selected class and enables Vote button") {
        auto ctx = makeContext(base);
        ctx.interactive = true;
        const std::vector<int> selection{ 2 };
        ctx.selectedAnswers = &selection;

        auto *doc = fixture.loadRml(renderPoll(poll, ctx));

        ::Rml::ElementList answerButtons;
        doc->QuerySelectorAll(answerButtons, "button.poll-answer");
        REQUIRE(answerButtons.size() == 2);
        CHECK_FALSE(answerButtons[0]->IsClassSet("poll-answer-selected"));
        CHECK(answerButtons[1]->IsClassSet("poll-answer-selected"));

        auto *voteBtn = requireElement(doc, "button.poll-vote-submit");
        CHECK_FALSE(voteBtn->HasAttribute("disabled"));
    }
}

TEST_CASE("Confirmed votes preselect default draft and expose enabled Remove vote button", "[polls][rml]")
{
    TestViewFixture fixture;
    const auto base = Platform::DateTime::fromUnixMillis(1'700'000'000'000LL);
    const auto expiry = base.addSeconds(3600);

    const nlohmann::json pollJson = {
        { "question", makeQuestion("Ice cream?") },
        { "answers", { makeAnswer(1, "Vanilla"), makeAnswer(2, "Chocolate") } },
        { "expiry", expiry.iso8601() },
        { "allow_multiselect", false },
        { "results", {
            { "is_finalized", false },
            { "answer_counts", { makeCount(1, 0, false), makeCount(2, 1, true) } }
        } }
    };

    auto ctx = makeContext(base);
    ctx.interactive = true;
    ctx.selectedAnswers = nullptr;

    auto *doc = fixture.loadRml(renderPoll(Discord::Poll::fromJson(pollJson), ctx));

    ::Rml::ElementList answerButtons;
    doc->QuerySelectorAll(answerButtons, "button.poll-answer");
    REQUIRE(answerButtons.size() == 2);
    CHECK_FALSE(answerButtons[0]->IsClassSet("poll-answer-mine"));
    CHECK_FALSE(answerButtons[0]->IsClassSet("poll-answer-selected"));
    CHECK(answerButtons[1]->IsClassSet("poll-answer-mine"));
    CHECK(answerButtons[1]->IsClassSet("poll-answer-selected"));

    auto *voteBtn = requireElement(doc, "button.poll-vote-submit");
    CHECK_FALSE(voteBtn->HasAttribute("disabled"));

    auto *removeBtn = requireElement(doc, "button.poll-vote-remove");
    CHECK(removeBtn->GetAttribute<::Rml::String>("data-action", "") == "poll-remove-vote");
    CHECK_FALSE(removeBtn->HasAttribute("disabled"));
}

TEST_CASE("Pending poll disables all interactive controls and emits submission status", "[polls][rml]")
{
    TestViewFixture fixture;
    const auto base = Platform::DateTime::fromUnixMillis(1'700'000'000'000LL);
    const auto expiry = base.addSeconds(3600);

    const nlohmann::json pollJson = {
        { "question", makeQuestion("Voting in flight?") },
        { "answers", { makeAnswer(1, "Yes"), makeAnswer(2, "No") } },
        { "expiry", expiry.iso8601() },
        { "allow_multiselect", false },
        { "results", {
            { "is_finalized", false },
            { "answer_counts", { makeCount(1, 1, true), makeCount(2, 0, false) } }
        } }
    };

    auto ctx = makeContext(base);
    ctx.interactive = true;
    ctx.pending = true;
    ctx.canEnd = true;

    auto *doc = fixture.loadRml(renderPoll(Discord::Poll::fromJson(pollJson), ctx)
        + "<button id=\"after-poll\" style=\"tab-index: auto;\">After poll</button>");

    ::Rml::ElementList answerButtons;
    doc->QuerySelectorAll(answerButtons, "button.poll-answer");
    REQUIRE(answerButtons.size() == 2);
    for (auto *btn : answerButtons)
        CHECK(btn->HasAttribute("disabled"));

    auto *voteBtn = requireElement(doc, "button.poll-vote-submit");
    CHECK(voteBtn->HasAttribute("disabled"));

    auto *removeBtn = requireElement(doc, "button.poll-vote-remove");
    CHECK(removeBtn->HasAttribute("disabled"));

    auto *endBtn = requireElement(doc, "button.poll-end");
    CHECK(endBtn->HasAttribute("disabled"));
    CHECK(requireElement(doc, ".poll-vote-status")->IsVisible());

    // Generic button attributes need matching styles; pending controls must leave tab order.
    auto *after = doc->GetElementById("after-poll");
    after->Focus();
    doc->GetContext()->ProcessKeyDown(::Rml::Input::KI_TAB, 0);
    doc->GetContext()->ProcessKeyUp(::Rml::Input::KI_TAB, 0);
    CHECK(doc->GetContext()->GetFocusElement() == after);
}

TEST_CASE("Non-interactive or closed polls omit data-action controls, error markup is escaped", "[polls][rml]")
{
    TestViewFixture fixture;
    const auto base = Platform::DateTime::fromUnixMillis(1'700'000'000'000LL);
    const auto expiry = base.addSeconds(3600);

    const nlohmann::json openPollJson = {
        { "question", makeQuestion("Safe poll?") },
        { "answers", { makeAnswer(1, "Option 1"), makeAnswer(2, "Option 2") } },
        { "expiry", expiry.iso8601() },
        { "allow_multiselect", false }
    };

    SECTION("Non-interactive open poll has no data-action attributes or vote actions") {
        auto ctx = makeContext(base);
        ctx.interactive = false;

        auto *doc = fixture.loadRml(renderPoll(Discord::Poll::fromJson(openPollJson), ctx));
        CHECK(doc->QuerySelector("[data-action]") == nullptr);
        CHECK(doc->QuerySelector("button.poll-answer") == nullptr);
        CHECK(doc->QuerySelector(".poll-vote-actions") == nullptr);
    }

    SECTION("Closed poll ignores interactive flag and omits data-action controls") {
        auto closedCtx = makeContext(expiry.addSeconds(60));
        closedCtx.interactive = true;

        auto *doc = fixture.loadRml(renderPoll(Discord::Poll::fromJson(openPollJson), closedCtx));
        CHECK(doc->QuerySelector("[data-action]") == nullptr);
        CHECK(doc->QuerySelector("button.poll-answer") == nullptr);
        CHECK(doc->QuerySelector(".poll-vote-actions") == nullptr);
    }

    SECTION("Error string is escaped without injecting HTML tags") {
        auto ctx = makeContext(base);
        ctx.interactive = true;
        ctx.error = "Vote <b>failed</b> & rejected";

        auto *doc = fixture.loadRml(renderPoll(Discord::Poll::fromJson(openPollJson), ctx));
        CHECK(doc->QuerySelector("b") == nullptr);
        auto *errEl = requireElement(doc, ".poll-vote-error");
        CHECK(errEl->GetInnerRML().find("Vote &lt;b&gt;failed&lt;/b&gt; &amp; rejected") != std::string::npos);
    }
}

TEST_CASE("Unknown results keeps counts hidden and exposes Remove vote button", "[polls][rml]")
{
    TestViewFixture fixture;
    const auto base = Platform::DateTime::fromUnixMillis(1'700'000'000'000LL);
    const auto expiry = base.addSeconds(3600);

    const nlohmann::json unknownResultsJson = {
        { "question", makeQuestion("Mystery poll?") },
        { "answers", { makeAnswer(1, "Secret A"), makeAnswer(2, "Secret B") } },
        { "expiry", expiry.iso8601() },
        { "allow_multiselect", false }
    };

    auto ctx = makeContext(base);
    ctx.interactive = true;

    auto *doc = fixture.loadRml(renderPoll(Discord::Poll::fromJson(unknownResultsJson), ctx));
    CHECK(doc->QuerySelector(".poll-bar") == nullptr);
    CHECK(requireElement(doc, ".poll-footer")->GetInnerRML().find("Results not loaded") != std::string::npos);

    auto *removeBtn = requireElement(doc, "button.poll-vote-remove");
    CHECK(removeBtn->GetAttribute<::Rml::String>("data-action", "") == "poll-remove-vote");
    CHECK_FALSE(removeBtn->HasAttribute("disabled"));
}

TEST_CASE("Poll termination predicate: pollCanEnd", "[polls][logic]")
{
    const auto base = Platform::DateTime::fromUnixMillis(1'700'000'000'000LL);
    const auto expiry = base.addSeconds(3600);

    const nlohmann::json pollJson = {
        { "question", makeQuestion("Close early?") },
        { "answers", { makeAnswer(1, "Yes"), makeAnswer(2, "No") } },
        { "expiry", expiry.iso8601() },
        { "allow_multiselect", false },
        { "results", { { "is_finalized", false } } }
    };

    const Core::Snowflake selfId(12345);
    const Core::Snowflake otherId(67890);

    Discord::Message msg;
    msg.id = Core::Snowflake(1001);
    Discord::User author;
    author.id = selfId;
    author.username = "owner";
    msg.author = author;
    msg.poll = Discord::Poll::fromJson(pollJson);

    SECTION("Own open poll can be ended") {
        CHECK(pollCanEnd(msg, selfId, base));
        CHECK(pollCanVote(msg, selfId, base));
    }

    SECTION("Unknown identity cannot vote or end a poll") {
        msg.author->id = Core::Snowflake::Invalid;
        CHECK_FALSE(pollCanEnd(msg, Core::Snowflake::Invalid, base));
        CHECK_FALSE(pollCanVote(msg, Core::Snowflake::Invalid, base));
    }

    SECTION("Unknown author cannot end a poll") {
        msg.author = Discord::User{};
        CHECK_FALSE(pollCanEnd(msg, selfId, base));
    }

    SECTION("Other author is rejected") {
        auto otherAuthor = msg;
        otherAuthor.author->id = otherId;
        CHECK_FALSE(pollCanEnd(otherAuthor, selfId, base));
        CHECK(pollCanVote(otherAuthor, selfId, base));
    }

    SECTION("Forwarded message is rejected") {
        auto forwarded = msg;
        Discord::MessageReference ref;
        ref.type = Discord::MessageReferenceType::FORWARD;
        forwarded.messageReference = ref;
        CHECK_FALSE(pollCanEnd(forwarded, selfId, base));
    }

    SECTION("Missing poll is rejected") {
        auto noPoll = msg;
        noPoll.poll.undefine();
        CHECK_FALSE(pollCanEnd(noPoll, selfId, base));
    }

    SECTION("Elapsed expiry or finalized poll is rejected") {
        CHECK_FALSE(pollCanEnd(msg, selfId, expiry));

        auto finalized = msg;
        finalized.poll->results->isFinalized = true;
        CHECK_FALSE(pollCanEnd(finalized, selfId, base));
    }
}

TEST_CASE("End poll control requires an eligible interactive poll", "[polls][rml]")
{
    TestViewFixture fixture;
    const auto base = Platform::DateTime::fromUnixMillis(1'700'000'000'000LL);
    const auto expiry = base.addSeconds(3600);

    const nlohmann::json pollJson = {
        { "question", makeQuestion("Should we wrap up?") },
        { "answers", { makeAnswer(1, "Yes"), makeAnswer(2, "No") } },
        { "expiry", expiry.iso8601() },
        { "allow_multiselect", false },
        { "results", {
            { "is_finalized", false },
            { "answer_counts", { makeCount(1, 1, true), makeCount(2, 0, false) } }
        } }
    };
    const auto poll = Discord::Poll::fromJson(pollJson);

    SECTION("End poll button absent in read-only context even if canEnd is set") {
        auto ctx = makeContext(base);
        ctx.interactive = false;
        ctx.canEnd = true;

        auto *doc = fixture.loadRml(renderPoll(poll, ctx));
        CHECK(doc->QuerySelector("button.poll-end") == nullptr);
    }

    SECTION("End poll button absent when poll is closed even if canEnd is set") {
        auto ctx = makeContext(expiry.addSeconds(10));
        ctx.interactive = true;
        ctx.canEnd = true;

        auto *doc = fixture.loadRml(renderPoll(poll, ctx));
        CHECK(doc->QuerySelector("button.poll-end") == nullptr);
    }

    SECTION("End poll button absent when canEnd is false") {
        auto ctx = makeContext(base);
        ctx.interactive = true;
        ctx.canEnd = false;

        auto *doc = fixture.loadRml(renderPoll(poll, ctx));
        CHECK(doc->QuerySelector("button.poll-end") == nullptr);
    }

    SECTION("Enabled End poll button is visible, in-bounds, and keyboard focusable") {
        auto ctx = makeContext(base);
        ctx.interactive = true;
        ctx.canEnd = true;

        auto *doc = fixture.loadRml(renderPoll(poll, ctx));
        auto *endBtn = requireElement(doc, "button.poll-end");
        CHECK_FALSE(endBtn->HasAttribute("disabled"));
        CHECK(endBtn->IsVisible());

        auto *pollEl = requireElement(doc, ".poll");
        const auto endOffset = endBtn->GetAbsoluteOffset(::Rml::BoxArea::Border);
        const auto pollOffset = pollEl->GetAbsoluteOffset(::Rml::BoxArea::Border);
        CHECK(endBtn->GetOffsetWidth() > 0);
        CHECK(endBtn->GetOffsetHeight() > 0);
        CHECK(endOffset.x >= pollOffset.x);
        CHECK(endOffset.y >= pollOffset.y);
        CHECK(endOffset.x + endBtn->GetOffsetWidth() <= pollOffset.x + pollEl->GetOffsetWidth() + 1.0f);
        CHECK(endOffset.y + endBtn->GetOffsetHeight() <= pollOffset.y + pollEl->GetOffsetHeight() + 1.0f);


        endBtn->Focus();
        CHECK(doc->GetContext()->GetFocusElement() == endBtn);
    }

}
