#include "UI/Rml/ComponentRenderer.hpp"
#include "UI/Rml/RmlRuntime.hpp"
#include "UI/Rml/RmlView.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Enums.hpp"
#include "Platform/Image.hpp"
#include "Platform/Native.hpp"

#include <RmlUi/Core.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

using namespace Styx::UI::Rml;
using namespace Styx::Discord;
using Styx::Core::Snowflake;

namespace {

class TestViewFixture {
public:
    explicit TestViewFixture(int width = 800, int height = 600)
        : view("test-components")
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

    ::Rml::ElementDocument *loadComponents(const std::string &componentsRml)
    {
        closeDocument();
        const std::string fullRml = "<rml><head><style>body { font-family: ui; font-size: 16px; }</style></head><body>" + componentsRml + "</body></rml>";
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

    static inline const std::string frameKey = "test_component_frame";

private:
    View view;
    ::Rml::ElementDocument *currentDoc = nullptr;
};

Message parseMessage(nlohmann::json obj)
{
    if (!obj.contains("id"))
        obj["id"] = "1000";
    if (!obj.contains("channel_id"))
        obj["channel_id"] = "2000";
    if (!obj.contains("content"))
        obj["content"] = "";
    if (!obj.contains("timestamp"))
        obj["timestamp"] = "2026-01-01T00:00:00Z";
    if (!obj.contains("type"))
        obj["type"] = 0;
    if (!obj.contains("author"))
        obj["author"] = nlohmann::json{ { "id", "1" }, { "username", "author" }, { "discriminator", "0001" } };
    return Message::fromJson(obj);
}

ComponentRenderContext makeContext(
    std::vector<std::string> &requestedUrls,
    const std::unordered_set<std::string> *revealedSpoilers = nullptr)
{
    ComponentRenderContext ctx;
    ctx.revealedSpoilers = revealedSpoilers;

    ctx.markdown = [](const std::string &raw) {
        return Styx::Platform::htmlEscape(raw);
    };

    ctx.imageSource = [&requestedUrls](const std::string &url, Styx::Platform::Size) {
        requestedUrls.push_back(url);
        return "frame:" + TestViewFixture::frameKey;
    };

    ctx.defaultLabel = [](Snowflake id, const std::string &type) {
        return "Label-" + type + "-" + id.toString();
    };

    ctx.fileSize = [](int64_t bytes) {
        return std::to_string(bytes) + " B";
    };

    return ctx;
}

} // namespace

TEST_CASE("Untrusted labels and URLs cannot inject DOM nodes or live unsafe actions", "[components][security]")
{
    TestViewFixture fixture;
    std::vector<std::string> requestedUrls;
    auto ctx = makeContext(requestedUrls);

    const nlohmann::json json = {
        { "components", {
            {
                { "type", 1 },
                { "components", {
                    {
                        { "type", 2 },
                        { "style", 1 },
                        { "label", "<img id='injected-btn-img' src='x'/><b id='injected-b'>Hostile</b>" },
                        { "custom_id", "btn_hostile" }
                    },
                    {
                        { "type", 2 },
                        { "style", 5 },
                        { "label", "Bad Protocol" },
                        { "url", "javascript:alert(document.cookie)" }
                    },
                    {
                        { "type", 2 },
                        { "style", 5 },
                        { "label", "Local File" },
                        { "url", "file:///etc/passwd" }
                    }
                } }
            },
            {
                { "type", 1 },
                { "components", {
                    {
                        { "type", 3 },
                        { "custom_id", "select_hostile" },
                        { "options", {
                            {
                                { "label", "<div id='injected-opt'>Opt</div>" },
                                { "value", "val1" },
                                { "default", true },
                                { "description", "<span id='injected-desc'>Desc</span>" }
                            }
                        } }
                    }
                } }
            }
        } }
    };

    const Message message = parseMessage(json);
    const std::string rml = renderMessageComponents(message, ctx);
    auto *doc = fixture.loadComponents(rml);

    CHECK(doc->GetElementById("injected-btn-img") == nullptr);
    CHECK(doc->GetElementById("injected-b") == nullptr);
    CHECK(doc->GetElementById("injected-opt") == nullptr);
    CHECK(doc->GetElementById("injected-desc") == nullptr);

    CHECK(doc->QuerySelector("[data-action='link']") == nullptr);

    ::Rml::ElementList buttons;
    doc->QuerySelectorAll(buttons, "button.component-button");
    REQUIRE(buttons.size() == 3);
    REQUIRE(doc->QuerySelector(".component-select") != nullptr);
    for (auto *btn : buttons) {
        CHECK(btn->HasAttribute("disabled"));
        CHECK_FALSE(btn->HasAttribute("data-action"));
    }
}

TEST_CASE("Safe Discord proxy preferred over original and attachment:// resolves forwarded attachments", "[components][media]")
{
    TestViewFixture fixture;

    SECTION("Proxy URL is preferred over original when both are Discord asset URLs") {
        std::vector<std::string> requestedUrls;
        auto ctx = makeContext(requestedUrls);

        const nlohmann::json json = {
            { "flags", static_cast<int>(MessageFlag::IS_COMPONENTS_V2) },
            { "components", {
                {
                    { "type", 9 },
                    { "components", { { { "type", 10 }, { "content", "Caption" } } } },
                    { "accessory", {
                        { "type", 11 },
                        { "media", {
                            { "url", "https://cdn.discordapp.com/attachments/10/20/original.png" },
                            { "proxy_url", "https://media.discordapp.net/attachments/10/20/proxy.png" }
                        } }
                    } }
                }
            } }
        };

        const Message message = parseMessage(json);
        const std::string rml = renderMessageComponents(message, ctx);
        auto *doc = fixture.loadComponents(rml);

        REQUIRE(requestedUrls.size() == 1);
        CHECK(requestedUrls.front() == "https://media.discordapp.net/attachments/10/20/proxy.png");
        CHECK(doc->QuerySelector("img") != nullptr);
    }

    SECTION("attachment:// resolves against visible forwarded message attachments") {
        std::vector<std::string> requestedUrls;
        auto ctx = makeContext(requestedUrls);

        const nlohmann::json json = {
            { "message_reference", { { "type", 1 }, { "message_id", "9001" }, { "channel_id", "2000" } } },
            { "message_snapshots", {
                {
                    { "message", {
                        { "id", "9001" },
                        { "flags", static_cast<int>(MessageFlag::IS_COMPONENTS_V2) },
                        { "attachments", {
                            {
                                { "id", "777" },
                                { "filename", "preview.jpg" },
                                { "size", 1024 },
                                { "url", "https://cdn.discordapp.com/attachments/1/777/preview.jpg" },
                                { "proxy_url", "https://media.discordapp.net/attachments/1/777/preview.jpg" }
                            }
                        } },
                        { "components", {
                            {
                                { "type", 9 },
                                { "components", { { { "type", 10 }, { "content", "Forwarded Caption" } } } },
                                { "accessory", {
                                    { "type", 11 },
                                    { "media", { { "url", "attachment://preview.jpg" } } }
                                } }
                            }
                        } }
                    } }
                }
            } }
        };

        const Message message = parseMessage(json);
        REQUIRE(message.isForwarded());
        REQUIRE(message.contentMessage().attachments.hasValue());
        REQUIRE_FALSE(message.contentMessage().attachments->empty());

        const std::string rml = renderMessageComponents(message, ctx);
        auto *doc = fixture.loadComponents(rml);

        REQUIRE(requestedUrls.size() == 1);
        CHECK(requestedUrls.front() == "https://media.discordapp.net/attachments/1/777/preview.jpg");
        CHECK(doc->QuerySelector("img") != nullptr);
    }
}

TEST_CASE("Media resolver boundaries: exact query/hash filenames, fallback direct CDN, scheme and lookalike guards", "[components][media]")
{
    TestViewFixture fixture;

    SECTION("Literal '?' and '#' in attachment filename resolve exactly") {
        std::vector<std::string> requestedUrls;
        auto ctx = makeContext(requestedUrls);

        const nlohmann::json json = {
            { "flags", static_cast<int>(MessageFlag::IS_COMPONENTS_V2) },
            { "attachments", {
                {
                    { "id", "888" },
                    { "filename", "photo?query#hash.png" },
                    { "size", 2048 },
                    { "url", "https://cdn.discordapp.com/attachments/1/888/photo%3Fquery%23hash.png" },
                    { "proxy_url", "https://media.discordapp.net/attachments/1/888/photo%3Fquery%23hash.png" }
                }
            } },
            { "components", {
                {
                    { "type", 9 },
                    { "components", { { { "type", 10 }, { "content", "Literal query/hash test" } } } },
                    { "accessory", {
                        { "type", 11 },
                        { "media", { { "url", "attachment://photo?query#hash.png" } } }
                    } }
                }
            } }
        };

        const Message message = parseMessage(json);
        const std::string rml = renderMessageComponents(message, ctx);
        auto *doc = fixture.loadComponents(rml);

        REQUIRE(requestedUrls.size() == 1);
        CHECK(requestedUrls.front() == "https://media.discordapp.net/attachments/1/888/photo%3Fquery%23hash.png");
        CHECK(doc->QuerySelector("img") != nullptr);
    }

    SECTION("Safe direct CDN URL remains usable if attachment_id not found, while unsafe schemes and lookalikes stay blocked") {
        std::vector<std::string> requestedUrls;
        auto ctx = makeContext(requestedUrls);

        const nlohmann::json json = {
            { "flags", static_cast<int>(MessageFlag::IS_COMPONENTS_V2) },
            { "components", {
                {
                    { "type", 12 },
                    { "items", {
                        { { "media", {
                            { "attachment_id", "999999999" },
                            { "url", "https://cdn.discordapp.com/attachments/1/2/direct_fallback.png" }
                        } } },
                        { { "media", {
                            { "attachment_id", "999999998" },
                            { "url", "https://discordapp.net.evil.invalid/leak.png" }
                        } } },
                        { { "media", {
                            { "attachment_id", "999999997" },
                            { "url", "http://insecure-cdn.discordapp.com/leak.png" }
                        } } }
                    } }
                }
            } }
        };

        const Message message = parseMessage(json);
        const std::string rml = renderMessageComponents(message, ctx);
        auto *doc = fixture.loadComponents(rml);

        REQUIRE(requestedUrls.size() == 1);
        CHECK(requestedUrls.front() == "https://cdn.discordapp.com/attachments/1/2/direct_fallback.png");
        CHECK(doc->QuerySelector(".component-unavailable") != nullptr);
    }

    SECTION("Mixed-case HTTPS and Unicode path allowed for valid Discord asset") {
        std::vector<std::string> requestedUrls;
        auto ctx = makeContext(requestedUrls);

        const nlohmann::json json = {
            { "flags", static_cast<int>(MessageFlag::IS_COMPONENTS_V2) },
            { "components", {
                {
                    { "type", 9 },
                    { "components", { { { "type", 10 }, { "content", "Unicode path" } } } },
                    { "accessory", {
                        { "type", 11 },
                        { "media", { { "url", "HtTpS://cdn.discordapp.com/attachments/1/2/café_image.png" } } }
                    } }
                }
            } }
        };

        const Message message = parseMessage(json);
        const std::string rml = renderMessageComponents(message, ctx);
        auto *doc = fixture.loadComponents(rml);

        REQUIRE(requestedUrls.size() == 1);
        CHECK(requestedUrls.front() == "HtTpS://cdn.discordapp.com/attachments/1/2/café_image.png");
        CHECK(doc->QuerySelector("img") != nullptr);
    }
}

TEST_CASE("Unresolved, local, and lookalike schemes cannot reach imageSource", "[components][security]")
{
    TestViewFixture fixture;
    std::vector<std::string> requestedUrls;
    auto ctx = makeContext(requestedUrls);

    const nlohmann::json json = {
        { "flags", static_cast<int>(MessageFlag::IS_COMPONENTS_V2) },
        { "components", {
            {
                { "type", 12 },
                { "items", {
                    { { "media", { { "url", "https://images-ext-1.discordapp.net/external/abc123/safe_image.png" } } } },
                    { { "media", { { "url", "https://discordapp.net.evil.invalid/leak.png" } } } },
                    { { "media", { { "url", "https://evil-tracking-site.com/logger.png" } } } },
                    { { "media", { { "url", "file:///etc/shadow" } } } },
                    { { "media", { { "url", "frame:forbidden_token" } } } },
                    { { "media", { { "url", "attachment://missing_attachment.png" } } } }
                } }
            }
        } }
    };

    const Message message = parseMessage(json);
    const std::string rml = renderMessageComponents(message, ctx);
    auto *doc = fixture.loadComponents(rml);

    REQUIRE(requestedUrls.size() == 1);
    CHECK(requestedUrls.front() == "https://images-ext-1.discordapp.net/external/abc123/safe_image.png");
    CHECK(doc->QuerySelector(".component-unavailable") != nullptr);
}

TEST_CASE("Spoilers hide descendant media callbacks and descriptions until correct path revealed", "[components][spoilers]")
{
    TestViewFixture fixture;

    // Component 0: Container with section/accessory thumbnail spoiler and gallery-item spoiler
    // Component 1: Independent sibling container spoiler
    const nlohmann::json json = {
        { "flags", static_cast<int>(MessageFlag::IS_COMPONENTS_V2) },
        { "components", {
            {
                { "type", 17 },
                { "components", {
                    {
                        { "type", 9 },
                        { "components", { { { "type", 10 }, { "content", "Section caption" } } } },
                        { "accessory", {
                            { "type", 11 },
                            { "spoiler", true },
                            { "description", "SecretThumbnailDesc" },
                            { "media", { { "url", "https://cdn.discordapp.com/attachments/10/20/secret_thumb.png" } } }
                        } }
                    },
                    {
                        { "type", 12 },
                        { "items", {
                            {
                                { "spoiler", true },
                                { "description", "SecretGalleryDesc" },
                                { "media", { { "url", "https://cdn.discordapp.com/attachments/10/20/secret_gallery.png" } } }
                            }
                        } }
                    }
                } }
            },
            {
                { "type", 17 },
                { "spoiler", true },
                { "components", {
                    { { "type", 10 }, { "content", "SiblingSecretContent" } },
                    { { "type", 12 }, { "items", {
                        { { "media", { { "url", "https://cdn.discordapp.com/attachments/10/20/sibling_secret.png" } } } }
                    } } }
                } }
            }
        } }
    };

    const Message message = parseMessage(json);

    SECTION("All hidden: no media callbacks, descriptions and sibling content absent from DOM") {
        std::vector<std::string> requestedUrls;
        auto ctx = makeContext(requestedUrls, nullptr);

        const std::string rml = renderMessageComponents(message, ctx);
        auto *doc = fixture.loadComponents(rml);

        CHECK(requestedUrls.empty());
        CHECK(doc->GetInnerRML().find("SecretThumbnailDesc") == std::string::npos);
        CHECK(doc->GetInnerRML().find("SecretGalleryDesc") == std::string::npos);
        CHECK(doc->GetInnerRML().find("SiblingSecretContent") == std::string::npos);

        CHECK(doc->QuerySelector("[data-component-path='0.0.a']") != nullptr);
        CHECK(doc->QuerySelector("[data-component-path='0.1.i0']") != nullptr);
        CHECK(doc->QuerySelector("[data-component-path='1']") != nullptr);
    }

    SECTION("Wrong path and container-only reveal must not request child media") {
        std::vector<std::string> requestedUrls;
        const std::unordered_set<std::string> revealedPaths = { "0", "0.0", "random.path" };
        auto ctx = makeContext(requestedUrls, &revealedPaths);

        const std::string rml = renderMessageComponents(message, ctx);
        auto *doc = fixture.loadComponents(rml);

        CHECK(requestedUrls.empty());
        CHECK(doc->QuerySelector("[data-component-path='0.0.a']") != nullptr);
        CHECK(doc->QuerySelector("[data-component-path='0.1.i0']") != nullptr);
        CHECK(doc->QuerySelector("[data-component-path='1']") != nullptr);
    }

    SECTION("Exact child path reveals only that media while other spoilers remain hidden") {
        std::vector<std::string> requestedUrls;
        const std::unordered_set<std::string> revealedPaths = { "0", "0.0.a" };
        auto ctx = makeContext(requestedUrls, &revealedPaths);

        const std::string rml = renderMessageComponents(message, ctx);
        auto *doc = fixture.loadComponents(rml);

        REQUIRE(requestedUrls.size() == 1);
        CHECK(requestedUrls.front() == "https://cdn.discordapp.com/attachments/10/20/secret_thumb.png");
        CHECK(doc->QuerySelector("[data-component-path='0.0.a']") == nullptr);

        CHECK(doc->QuerySelector("[data-component-path='0.1.i0']") != nullptr);
        CHECK(doc->QuerySelector("[data-component-path='1']") != nullptr);
        CHECK(doc->GetInnerRML().find("SiblingSecretContent") == std::string::npos);
    }

    SECTION("Revealing second child media item does not reveal independent sibling container") {
        std::vector<std::string> requestedUrls;
        const std::unordered_set<std::string> revealedPaths = { "0", "0.0.a", "0.1.i0" };
        auto ctx = makeContext(requestedUrls, &revealedPaths);

        const std::string rml = renderMessageComponents(message, ctx);
        auto *doc = fixture.loadComponents(rml);

        REQUIRE(requestedUrls.size() == 2);
        CHECK(requestedUrls[0] == "https://cdn.discordapp.com/attachments/10/20/secret_thumb.png");
        CHECK(requestedUrls[1] == "https://cdn.discordapp.com/attachments/10/20/secret_gallery.png");

        CHECK(doc->QuerySelector("[data-component-path='0.0.a']") == nullptr);
        CHECK(doc->QuerySelector("[data-component-path='0.1.i0']") == nullptr);

        CHECK(doc->QuerySelector("[data-component-path='1']") != nullptr);
        CHECK(doc->GetInnerRML().find("SiblingSecretContent") == std::string::npos);
    }
}

TEST_CASE("Read-only select summaries retain every selected default", "[components][select]")
{
    TestViewFixture fixture;
    std::vector<std::string> requestedUrls;
    auto ctx = makeContext(requestedUrls);

    const nlohmann::json json = {
        { "components", {
            {
                { "type", 1 },
                { "components", {
                    {
                        { "type", 3 },
                        { "custom_id", "multi_string_select" },
                        { "max_values", 2 },
                        { "options", {
                            {
                                { "label", "Option Alpha" },
                                { "value", "val_a" },
                                { "default", true },
                                { "emoji", { { "id", "333333333333333333" }, { "name", "custom_party" } } }
                            },
                            {
                                { "label", "Option selected Beta" },
                                { "value", "val selected beta" },
                                { "default", true }
                            },
                            {
                                { "label", "Option Gamma" },
                                { "value", "val_c" },
                                { "default", false }
                            }
                        } }
                    }
                } }
            },
            {
                { "type", 1 },
                { "components", {
                    {
                        { "type", 5 },
                        { "custom_id", "multi_user_select" },
                        { "max_values", 2 },
                        { "default_values", {
                            { { "id", "111111111111111111" }, { "type", "user" } },
                            { { "id", "222222222222222222" }, { "type", "user" } }
                        } }
                    }
                } }
            }
        } }
    };

    const Message message = parseMessage(json);
    const std::string rml = renderMessageComponents(message, ctx);
    auto *doc = fixture.loadComponents(rml);

    ::Rml::ElementList selectElements;
    doc->QuerySelectorAll(selectElements, ".component-select");
    REQUIRE(selectElements.size() == 2);

    for (auto *select : selectElements) {
        CHECK(select->GetAttribute<::Rml::String>("aria-disabled", "") == "true");
        CHECK_FALSE(select->HasAttribute("data-action"));
    }
    auto *selectedString = selectElements[0]->QuerySelector(".component-select-value");
    REQUIRE(selectedString != nullptr);
    const auto selectedText = selectedString->GetInnerRML();
    CHECK(selectedText.find("Option Alpha") != std::string::npos);
    CHECK(selectedText.find("Option selected Beta") != std::string::npos);
    CHECK(selectedString->QuerySelector("img.emoji") != nullptr);

    auto *selectedUser = selectElements[1]->QuerySelector(".component-select-value");
    REQUIRE(selectedUser != nullptr);
    const auto selectedUsers = selectedUser->GetInnerRML();
    CHECK(selectedUsers.find("Label-user-111111111111111111") != std::string::npos);
    CHECK(selectedUsers.find("Label-user-222222222222222222") != std::string::npos);
}

TEST_CASE("Malformed unknown component node preserves supported siblings", "[components]")
{
    TestViewFixture fixture;
    std::vector<std::string> requestedUrls;
    auto ctx = makeContext(requestedUrls);

    const nlohmann::json json = {
        { "components", {
            {
                { "type", 1 },
                { "components", {
                    { { "type", 2 }, { "style", 1 }, { "label", "Button Alpha" }, { "custom_id", "btn_alpha" } },
                    { { "type", 99 }, { "label", "Unknown Widget" } },
                    { { "type", 2 }, { "style", 2 }, { "label", "Button Beta" }, { "custom_id", "btn_beta" } }
                } }
            }
        } }
    };

    const Message message = parseMessage(json);
    const std::string rml = renderMessageComponents(message, ctx);
    auto *doc = fixture.loadComponents(rml);

    CHECK(doc->QuerySelector(".component-unsupported") != nullptr);

    const std::string inner = doc->GetInnerRML();
    CHECK(inner.find("Button Alpha") != std::string::npos);
    CHECK(inner.find("Button Beta") != std::string::npos);
}

TEST_CASE("V2-only components ignored without IS_COMPONENTS_V2 flag while legacy actions render", "[components][v2]")
{
    TestViewFixture fixture;
    std::vector<std::string> requestedUrls;
    auto ctx = makeContext(requestedUrls);

    nlohmann::json json = {
        { "flags", 0 },
        { "components", {
            {
                { "type", 1 },
                { "components", {
                    { { "type", 2 }, { "style", 1 }, { "label", "Legacy Action" }, { "custom_id", "btn_legacy" } }
                } }
            },
            {
                { "type", 17 },
                { "components", {
                    { { "type", 10 }, { "content", "Exclusive V2 Content" } }
                } }
            }
        } }
    };

    const Message msgLegacy = parseMessage(json);
    const std::string rmlLegacy = renderMessageComponents(msgLegacy, ctx);
    auto *docLegacy = fixture.loadComponents(rmlLegacy);

    CHECK(docLegacy->GetInnerRML().find("Legacy Action") != std::string::npos);
    CHECK(docLegacy->GetInnerRML().find("Exclusive V2 Content") == std::string::npos);
    CHECK(docLegacy->QuerySelector(".component-container") == nullptr);

    json["flags"] = static_cast<int>(MessageFlag::IS_COMPONENTS_V2);
    const Message msgV2 = parseMessage(json);
    const std::string rmlV2 = renderMessageComponents(msgV2, ctx);
    auto *docV2 = fixture.loadComponents(rmlV2);

    CHECK(docV2->GetInnerRML().find("Legacy Action") != std::string::npos);
    CHECK(docV2->GetInnerRML().find("Exclusive V2 Content") != std::string::npos);
    CHECK(docV2->QuerySelector(".component-container") != nullptr);
}

TEST_CASE("Disabled bot/premium buttons and wire-disabled links are inert, safe link uses confirmation action", "[components][buttons]")
{
    TestViewFixture fixture;
    std::vector<std::string> requestedUrls;
    auto ctx = makeContext(requestedUrls);

    const nlohmann::json json = {
        { "components", {
            {
                { "type", 1 },
                { "components", {
                    { { "type", 2 }, { "style", 1 }, { "label", "Bot Primary" }, { "custom_id", "btn_bot" } },
                    { { "type", 2 }, { "style", 6 }, { "sku_id", "123456789012345678" } },
                    { { "type", 2 }, { "style", 5 }, { "label", "Disabled Link" }, { "url", "https://example.com/disabled" }, { "disabled", true } },
                    { { "type", 2 }, { "style", 5 }, { "label", "Safe Link" }, { "url", "https://example.com/safe" }, { "disabled", false } }
                } }
            }
        } }
    };

    const Message message = parseMessage(json);
    const std::string rml = renderMessageComponents(message, ctx);
    auto *doc = fixture.loadComponents(rml);

    ::Rml::ElementList buttons;
    doc->QuerySelectorAll(buttons, "button.component-button");
    REQUIRE(buttons.size() == 3);
    for (auto *btn : buttons) {
        CHECK(btn->HasAttribute("disabled"));
        CHECK_FALSE(btn->HasAttribute("data-action"));
    }

    auto *safeLink = doc->QuerySelector("a[data-action='link']");
    REQUIRE(safeLink != nullptr);
    CHECK(safeLink->GetTagName() == "a");
    CHECK(safeLink->GetAttribute<::Rml::String>("data-href", "") == "https://example.com/safe");
    CHECK_FALSE(safeLink->HasAttribute("disabled"));

    auto *disabledLinkActive = doc->QuerySelector("a[data-action='link'][data-href='https://example.com/disabled']");
    CHECK(disabledLinkActive == nullptr);
}
