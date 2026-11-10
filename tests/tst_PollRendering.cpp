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
#include <format>
#include <string>
#include <utility>
#include <vector>

using namespace Styx::UI::Rml;
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
        const std::string fullRml = "<rml><head><style>body { font-family: ui; font-size: 16px; }</style></head><body>" + rml + "</body></rml>";
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

    // RmlUi prints percentages without the sign, so compare the unit and the number.
    const auto fillPercent = [](::Rml::Element *answer) {
        const ::Rml::Property *width = requireElement(answer, ".poll-bar-fill")->GetProperty("width");
        REQUIRE(width != nullptr);
        REQUIRE(width->unit == ::Rml::Unit::PERCENT);
        return width->Get<float>();
    };
    CHECK(fillPercent(answers[0]) == 75.0f);
    CHECK(fillPercent(answers[1]) == 25.0f);
    CHECK(fillPercent(answers[2]) == 0.0f);

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
