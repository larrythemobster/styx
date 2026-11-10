#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "Core/Snowflake.hpp"
#include "Core/PollVotes.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Enums.hpp"
#include "Discord/Events.hpp"
#include "Platform/Paths.hpp"
#include "Platform/Settings.hpp"
#include "Platform/Sqlite.hpp"
#include "Storage/DatabaseManager.hpp"
#include "Storage/MessageRepository.hpp"

using Styx::Core::Snowflake;
using namespace Styx::Discord;
using namespace Styx::Storage;

namespace {

#ifdef _WIN32
struct EnvVarBackup
{
    std::wstring name;
    std::optional<std::wstring> value;
};
#else
struct EnvVarBackup
{
    std::string name;
    std::optional<std::string> value;
};
#endif

class ScopedEnv
{
public:
    explicit ScopedEnv(const std::filesystem::path &sandboxPath)
    {
#ifdef _WIN32
        const wchar_t *vars[] = { L"APPDATA", L"HOME", L"XDG_CONFIG_HOME", L"XDG_DATA_HOME" };
        for (const wchar_t *var : vars) {
            const wchar_t *val = _wgetenv(var);
            m_backups.push_back({ var, val ? std::optional<std::wstring>(val) : std::nullopt });
        }
        try {
            const std::wstring widePath = sandboxPath.wstring();
            for (const auto &b : m_backups) {
                if (_wputenv_s(b.name.c_str(), widePath.c_str()) != 0)
                    throw std::runtime_error("Failed setting environment variable in sandbox");
            }
        } catch (...) {
            restore();
            throw;
        }
#else
        const char *vars[] = { "APPDATA", "HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME" };
        for (const char *var : vars) {
            const char *val = std::getenv(var);
            m_backups.push_back({ var, val ? std::optional<std::string>(val) : std::nullopt });
        }
        try {
            const std::string pathStr = sandboxPath.string();
            for (const auto &b : m_backups) {
                if (setenv(b.name.c_str(), pathStr.c_str(), 1) != 0)
                    throw std::runtime_error("Failed setting environment variable in sandbox");
            }
        } catch (...) {
            restore();
            throw;
        }
#endif
    }

    ScopedEnv(const ScopedEnv &) = delete;
    ScopedEnv &operator=(const ScopedEnv &) = delete;

    ~ScopedEnv() noexcept
    {
        restore();
    }

private:
    void restore() noexcept
    {
        for (const auto &b : m_backups) {
#ifdef _WIN32
            if (b.value.has_value())
                _wputenv_s(b.name.c_str(), b.value->c_str());
            else
                _wputenv_s(b.name.c_str(), L"");
#else
            if (b.value.has_value())
                setenv(b.name.c_str(), b.value->c_str(), 1);
            else
                unsetenv(b.name.c_str());
#endif
        }
    }

    std::vector<EnvVarBackup> m_backups;
};

static std::atomic<uint64_t> s_fixtureCounter{ 0 };

class TempDir
{
public:
    TempDir()
    {
        const auto base = std::filesystem::temp_directory_path();
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = base / ("styx_cache_test_" + std::to_string(now) + "_" + std::to_string(++s_fixtureCounter));
        std::error_code ec;
        if (std::filesystem::create_directory(m_path, ec) && !ec)
            m_owned = true;
    }

    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    ~TempDir()
    {
        if (m_owned) {
            std::error_code ec;
            std::filesystem::remove_all(m_path, ec);
        }
    }

    const std::filesystem::path &path() const { return m_path; }
    bool isOwned() const { return m_owned; }

private:
    std::filesystem::path m_path;
    bool m_owned = false;
};

struct SandboxFixture
{
    TempDir tempDir;
    ScopedEnv env;

    SandboxFixture() : tempDir(), env(tempDir.path())
    {
        REQUIRE(tempDir.isOwned());
        Styx::Platform::Settings::instance().setFilePath(Styx::Platform::Paths::configDir() / "settings.json");
        Styx::Platform::Settings::instance().clear();
        Styx::Platform::Settings::instance().setBool("general/in_memory_cache", false);
    }

    ~SandboxFixture()
    {
        DatabaseManager::instance().shutdown();
        Styx::Platform::Settings::instance().clear();
    }

    std::string openCache(Snowflake accountId)
    {
        const std::string name = DatabaseManager::instance().openCacheDatabase(accountId);
        REQUIRE(name == DatabaseManager::getCacheConnectionName(accountId));
        return name;
    }
};

static nlohmann::json makeMessageJson(
    const std::string &id, const std::string &channelId, const std::string &authorId,
    const std::string &username, const std::string &content, const std::string &componentsJson = "",
    const std::string &stickersJson = "", int64_t flags = 0, const std::string &snapshotJson = "")
{
    nlohmann::json obj;
    obj["id"] = id;
    obj["channel_id"] = channelId;
    obj["author"] = { { "id", authorId }, { "username", username } };
    obj["content"] = content;
    obj["timestamp"] = "2026-11-09T12:00:00.000000+00:00";
    obj["type"] = 0;
    obj["flags"] = flags;
    if (!componentsJson.empty())
        obj["components"] = nlohmann::json::parse(componentsJson);
    if (!stickersJson.empty())
        obj["sticker_items"] = nlohmann::json::parse(stickersJson);
    if (!snapshotJson.empty())
        obj["message_snapshots"] = nlohmann::json::array({ { { "message", nlohmann::json::parse(snapshotJson) } } });
    return obj;
}

static Message makeSyntheticMessage(
    const std::string &id, const std::string &channelId, const std::string &authorId,
    const std::string &username, const std::string &content, const std::string &componentsJson = "",
    const std::string &stickersJson = "", int64_t flags = 0, const std::string &snapshotJson = "")
{
    return Message::fromJson(makeMessageJson(
        id, channelId, authorId, username, content, componentsJson, stickersJson, flags, snapshotJson));
}

static nlohmann::json makeAttachmentJson(const std::string &id, const std::string &filename)
{
    return {
        { "id", id },
        { "filename", filename },
        { "size", 1024 },
        { "url", "https://cdn.example.com/" + filename },
        { "proxy_url", "https://media.example.com/" + filename }
    };
}

static int64_t countRows(Styx::Platform::Database &db, const std::string &sql)
{
    auto q = db.prepare(sql);
    REQUIRE(q.step());
    return q.getInt64(0);
}

} // namespace

TEST_CASE("Message cache: nested V2 component tree survives save, close, reopen, getMessage and range retrieval", "[message_cache]")
{
    SandboxFixture fixture;
    const Snowflake accountId(10001);
    fixture.openCache(accountId);
    MessageRepository repo(accountId);

    const std::string mainComponents =
        R"([{"type":17,"id":10,"custom_unknown_field":"survives_raw 🐱","components":[{"type":10,"id":11,"content":"Nested container text"},{"type":999,"id":15,"unknown_meta":"preserved"}]}])";
    const std::string replyComponents =
        R"([{"type":1,"id":20,"components":[{"type":2,"id":21,"style":1,"custom_id":"reply_btn","label":"Reply Button"}]}])";
    const std::string replyStickers = R"([{"id":"501","name":"reply_sticker","format_type":1}])";

    auto mainObj = makeMessageJson("100", "200", "9001", "main_user", "",
        mainComponents, "", static_cast<int64_t>(MessageFlag::IS_COMPONENTS_V2));
    auto replyObj = makeMessageJson("101", "200", "9002", "reply_user", "Legacy reply content",
        replyComponents, replyStickers, 0);
    mainObj["referenced_message"] = replyObj;

    repo.saveMessages({ Message::fromJson(mainObj) });

    DatabaseManager::instance().closeCacheDatabase(accountId);
    fixture.openCache(accountId);
    MessageRepository reopenedRepo(accountId);

    const auto loadedOpt = reopenedRepo.getMessage(100);
    REQUIRE(loadedOpt.has_value());
    const auto &loaded = *loadedOpt;

    REQUIRE(loaded.id.get() == Snowflake(100));
    REQUIRE(loaded.content.get().empty());
    REQUIRE(loaded.flags.get().testFlag(MessageFlag::IS_COMPONENTS_V2));
    REQUIRE(loaded.stickerItems.isUndefined());
    REQUIRE_FALSE(loaded.stickerItems.hasValue());
    REQUIRE(loaded.components.hasValue());
    REQUIRE(loaded.components->front().type.get() == MessageComponentType::CONTAINER);
    REQUIRE(loaded.components->front().components.hasValue());
    REQUIRE(loaded.components->front().components->size() == 2);
    REQUIRE(loaded.components->front().components->front().type.get() == MessageComponentType::TEXT_DISPLAY);
    REQUIRE(loaded.components->front().components->front().content.valueOr("") == "Nested container text");
    REQUIRE(loaded.components->front().components->back().type.get() == static_cast<MessageComponentType>(999));
    REQUIRE(loaded.components->front().components->back().id.get() == 15);
    REQUIRE(nlohmann::json::parse(loaded.componentsJson) == nlohmann::json::parse(mainComponents));

    REQUIRE(loaded.referencedMessage != nullptr);
    const auto &ref = *loaded.referencedMessage;
    REQUIRE(ref.id.get() == Snowflake(101));
    REQUIRE(ref.content.get() == "Legacy reply content");
    REQUIRE_FALSE(ref.flags.get().testFlag(MessageFlag::IS_COMPONENTS_V2));
    REQUIRE(ref.components.hasValue());
    REQUIRE(ref.components->front().type.get() == MessageComponentType::ACTION_ROW);
    REQUIRE(ref.components->front().components->front().customId.valueOr("") == "reply_btn");
    REQUIRE(nlohmann::json::parse(ref.componentsJson) == nlohmann::json::parse(replyComponents));
    REQUIRE(ref.stickerItems.hasValue());
    REQUIRE(ref.stickerItems->front().id.get() == Snowflake(501));

    const auto range = reopenedRepo.getMessagesInRange(200, 50, 150);
    REQUIRE(range.size() == 1);
    const auto &rangeMsg = range.front();
    REQUIRE(rangeMsg.id.get() == Snowflake(100));
    REQUIRE(rangeMsg.content.get().empty());
    REQUIRE(rangeMsg.stickerItems.isUndefined());
    REQUIRE(rangeMsg.components->front().components->back().type.get() == static_cast<MessageComponentType>(999));
    REQUIRE(nlohmann::json::parse(rangeMsg.componentsJson) == nlohmann::json::parse(mainComponents));
    REQUIRE(rangeMsg.referencedMessage != nullptr);
    REQUIRE(rangeMsg.referencedMessage->stickerItems.hasValue());
    REQUIRE(rangeMsg.referencedMessage->stickerItems->front().id.get() == Snowflake(501));
    REQUIRE(nlohmann::json::parse(rangeMsg.referencedMessage->componentsJson) == nlohmann::json::parse(replyComponents));
}

TEST_CASE("Message cache: merged partial updates preserve omitted components, replace, and clear to empty array", "[message_cache]")
{
    SandboxFixture fixture;
    const Snowflake accountId(10002);
    fixture.openCache(accountId);
    MessageRepository repo(accountId);

    const std::string initialComponents =
        R"([{"type":1,"id":10,"components":[{"type":2,"id":11,"label":"Save","style":1,"custom_id":"btn_save"}]}])";
    repo.saveMessages({
        makeSyntheticMessage("200", "50", "9010", "edit_user", "Original content", initialComponents),
        makeSyntheticMessage("201", "50", "9010", "edit_user", "No components message")
    });

    auto baselineOpt = repo.getMessage(200);
    REQUIRE(baselineOpt.has_value());
    auto baseline = *baselineOpt;
    REQUIRE(baseline.components.hasValue());
    REQUIRE(nlohmann::json::parse(baseline.componentsJson) == nlohmann::json::parse(initialComponents));

    nlohmann::json updateOmit;
    updateOmit["id"] = "200";
    updateOmit["channel_id"] = "50";
    updateOmit["content"] = "Edited content";
    baseline.applyUpdate(Message::fromJson(updateOmit));
    repo.updateMessageContent(baseline);

    auto reloadedOmit = repo.getMessage(200);
    REQUIRE(reloadedOmit.has_value());
    REQUIRE(reloadedOmit->content.get() == "Edited content");
    REQUIRE(reloadedOmit->components.hasValue());
    REQUIRE(reloadedOmit->components->front().components->front().id.get() == 11);
    REQUIRE(nlohmann::json::parse(reloadedOmit->componentsJson) == nlohmann::json::parse(initialComponents));

    const std::string replComponents = R"([{"type":1,"id":12,"components":[{"type":2,"id":13,"style":2,"label":"Updated","custom_id":"updated"}]}])";
    nlohmann::json updateRepl;
    updateRepl["id"] = "200";
    updateRepl["channel_id"] = "50";
    updateRepl["components"] = nlohmann::json::parse(replComponents);
    baseline.applyUpdate(Message::fromJson(updateRepl));
    repo.updateMessageContent(baseline);

    auto reloadedRepl = repo.getMessage(200);
    REQUIRE(reloadedRepl.has_value());
    REQUIRE(reloadedRepl->components.hasValue());
    REQUIRE(reloadedRepl->components->front().type.get() == MessageComponentType::ACTION_ROW);
    REQUIRE(reloadedRepl->components->front().id.get() == 12);
    REQUIRE(reloadedRepl->components->front().components->front().customId.get() == "updated");
    REQUIRE(nlohmann::json::parse(reloadedRepl->componentsJson) == nlohmann::json::parse(replComponents));

    nlohmann::json updateClear;
    updateClear["id"] = "200";
    updateClear["channel_id"] = "50";
    updateClear["components"] = nlohmann::json::array();
    baseline.applyUpdate(Message::fromJson(updateClear));
    repo.updateMessageContent(baseline);

    auto reloadedClear = repo.getMessage(200);
    REQUIRE(reloadedClear.has_value());
    REQUIRE(reloadedClear->components.hasValue());
    REQUIRE_FALSE(reloadedClear->components.isUndefined());
    REQUIRE(reloadedClear->components->empty());
    REQUIRE(reloadedClear->componentsJson == "[]");

    auto reloadedMissing = repo.getMessage(201);
    REQUIRE(reloadedMissing.has_value());
    REQUIRE_FALSE(reloadedMissing->components.hasValue());
    REQUIRE(reloadedMissing->components.isUndefined());
    REQUIRE(reloadedMissing->componentsJson.empty());
}

TEST_CASE("Message cache: migration idempotently alters historical messages table without components", "[message_cache]")
{
    SandboxFixture fixture;
    const Snowflake accountId(10003);
    const auto dbPath = Styx::Platform::Paths::dataDir() / ("cache_" + accountId.toString() + ".sqlite");
    Styx::Platform::Paths::ensureDirExists(Styx::Platform::Paths::dataDir());

    {
        Styx::Platform::Database oldDb;
        REQUIRE(oldDb.open(dbPath));
        oldDb.execute(
            "CREATE TABLE messages ("
            "id INTEGER NOT NULL, author_id INTEGER NOT NULL, channel_id INTEGER NOT NULL, "
            "content TEXT NOT NULL, timestamp TEXT NOT NULL, edited_timestamp TEXT, "
            "type INTEGER NOT NULL, flags INTEGER NOT NULL, embeds TEXT, reactions TEXT, "
            "stickers TEXT, deleted INTEGER NOT NULL, referenced_message_id INTEGER, "
            "reference_type INTEGER, reference_channel_id INTEGER, reference_guild_id INTEGER, "
            "snapshot TEXT, context_only INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(id));\n"
            "CREATE TABLE users (id INTEGER NOT NULL, username TEXT NOT NULL, "
            "global_name TEXT, avatar TEXT, bot INTEGER, PRIMARY KEY(id));\n"
            "INSERT INTO users (id, username) VALUES (9030, 'historical_user');\n"
            "INSERT INTO messages (id, author_id, channel_id, content, timestamp, type, flags, deleted) "
            "VALUES (300, 9030, 70, 'Historical unmigrated message', '2026-01-01T00:00:00.000000+00:00', 0, 0, 0);"
        );
        oldDb.close();
    }

    fixture.openCache(accountId);
    MessageRepository repo(accountId);

    auto historicalOpt = repo.getMessage(300);
    REQUIRE(historicalOpt.has_value());
    REQUIRE(historicalOpt->id.get() == Snowflake(300));
    REQUIRE(historicalOpt->content.get() == "Historical unmigrated message");
    REQUIRE(historicalOpt->author->username.valueOr("") == "historical_user");
    REQUIRE_FALSE(historicalOpt->components.hasValue());
    REQUIRE(historicalOpt->components.isUndefined());
    REQUIRE(historicalOpt->componentsJson.empty());

    const std::string newComponents =
        R"([{"type":1,"id":50,"components":[{"type":2,"id":51,"label":"Click","style":1,"custom_id":"btn_click"}]}])";
    repo.saveMessages({
        makeSyntheticMessage("301", "70", "9031", "migrated_user", "Post migration message", newComponents)
    });

    auto loadedNew = repo.getMessage(301);
    REQUIRE(loadedNew.has_value());
    REQUIRE(loadedNew->id.get() == Snowflake(301));
    REQUIRE(loadedNew->components.hasValue());
    REQUIRE(nlohmann::json::parse(loadedNew->componentsJson) == nlohmann::json::parse(newComponents));

    DatabaseManager::instance().closeCacheDatabase(accountId);
    fixture.openCache(accountId);
    MessageRepository reopenedRepo(accountId);

    auto verifyOld = reopenedRepo.getMessage(300);
    REQUIRE(verifyOld.has_value());
    REQUIRE(verifyOld->id.get() == Snowflake(300));
    REQUIRE(verifyOld->content.get() == "Historical unmigrated message");

    auto verifyNew = reopenedRepo.getMessage(301);
    REQUIRE(verifyNew.has_value());
    REQUIRE(verifyNew->id.get() == Snowflake(301));
    REQUIRE(verifyNew->components.hasValue());
    REQUIRE(nlohmann::json::parse(verifyNew->componentsJson) == nlohmann::json::parse(newComponents));
}

TEST_CASE("Message cache: referenced message INSERT OR IGNORE preserves newer tree and respects context-only visibility", "[message_cache]")
{
    SandboxFixture fixture;
    const Snowflake accountId(10004);
    fixture.openCache(accountId);
    MessageRepository repo(accountId);

    const std::string fullComponents =
        R"([{"type":17,"id":70,"components":[{"type":10,"id":71,"content":"Full original container"}]}])";
    repo.saveMessages({
        makeSyntheticMessage("400", "80", "9040", "full_user", "",
            fullComponents, "", static_cast<int64_t>(MessageFlag::IS_COMPONENTS_V2))
    });

    auto staleRefObj = makeMessageJson("400", "80", "9040", "full_user", "Nonempty stale legacy context", "");
    auto main401Obj = makeMessageJson("401", "80", "9041", "reply_user", "Reply message 401");
    main401Obj["referenced_message"] = staleRefObj;
    repo.saveMessages({ Message::fromJson(main401Obj) });

    auto check400 = repo.getMessage(400);
    REQUIRE(check400.has_value());
    REQUIRE(check400->id.get() == Snowflake(400));
    REQUIRE(check400->content.get().empty());
    REQUIRE(check400->flags.get().testFlag(MessageFlag::IS_COMPONENTS_V2));
    REQUIRE(check400->components.hasValue());
    REQUIRE(nlohmann::json::parse(check400->componentsJson) == nlohmann::json::parse(fullComponents));

    const std::string refComponents =
        R"([{"type":1,"id":80,"components":[{"type":2,"id":81,"label":"RefBtn","style":1,"custom_id":"ref_btn"}]}])";
    auto ref402Obj = makeMessageJson("402", "80", "9042", "context_user", "Context only content", refComponents);
    auto main403Obj = makeMessageJson("403", "80", "9043", "other_user", "Main message 403");
    main403Obj["referenced_message"] = ref402Obj;
    repo.saveMessages({ Message::fromJson(main403Obj) });

    auto single402 = repo.getMessage(402);
    REQUIRE(single402.has_value());
    REQUIRE(single402->id.get() == Snowflake(402));
    REQUIRE(single402->components.hasValue());

    auto rangeInitial = repo.getMessagesInRange(80, 400, 405);
    bool found402InInitial = false;
    for (const auto &m : rangeInitial) {
        if (m.id.get() == Snowflake(402))
            found402InInitial = true;
    }
    REQUIRE_FALSE(found402InInitial);

    repo.saveMessages({
        makeSyntheticMessage("402", "80", "9042", "context_user", "Promoted full content", refComponents)
    });

    auto rangePromoted = repo.getMessagesInRange(80, 400, 405);
    bool found402Promoted = false;
    for (const auto &m : rangePromoted) {
        if (m.id.get() == Snowflake(402))
            found402Promoted = true;
    }
    REQUIRE(found402Promoted);

    auto checkPromoted = repo.getMessage(402);
    REQUIRE(checkPromoted.has_value());
    REQUIRE(checkPromoted->id.get() == Snowflake(402));
    REQUIRE(checkPromoted->content.get() == "Promoted full content");
    REQUIRE(nlohmann::json::parse(checkPromoted->componentsJson) == nlohmann::json::parse(refComponents));
}

TEST_CASE("Message cache: forwarded snapshot components survive disk reload for top-level and referenced messages", "[message_cache]")
{
    SandboxFixture fixture;
    const Snowflake accountId(10005);
    fixture.openCache(accountId);
    MessageRepository repo(accountId);

    const std::string topSnapshotComponents =
        R"([{"type":17,"id":90,"components":[{"type":10,"id":91,"content":"Forwarded top snapshot text"}]}])";
    const auto topSnapObj = makeMessageJson("501", "90", "9050", "snap_author", "",
        topSnapshotComponents, "", static_cast<int64_t>(MessageFlag::IS_COMPONENTS_V2));

    auto topFwdObj = makeMessageJson("500", "90", "9051", "forwarder", "",
        "", "", static_cast<int64_t>(MessageFlag::HAS_SNAPSHOT), topSnapObj.dump());
    topFwdObj["message_reference"] = { { "type", 1 }, { "channel_id", "90" }, { "message_id", "501" } };

    const std::string refSnapshotComponents =
        R"([{"type":1,"id":95,"components":[{"type":2,"id":96,"style":1,"custom_id":"snap_btn","label":"Snapshot Button"}]}])";
    const auto refSnapObj = makeMessageJson("505", "90", "9052", "ref_snap_author", "",
        refSnapshotComponents, "", static_cast<int64_t>(MessageFlag::IS_COMPONENTS_V2));

    auto refFwdObj = makeMessageJson("504", "90", "9053", "ref_fwd_author", "",
        "", "", static_cast<int64_t>(MessageFlag::HAS_SNAPSHOT), refSnapObj.dump());
    refFwdObj["message_reference"] = { { "type", 1 }, { "channel_id", "90" }, { "message_id", "505" } };

    auto topReplyObj = makeMessageJson("506", "90", "9054", "reply_author", "Reply to forward");
    topReplyObj["referenced_message"] = refFwdObj;

    {
        auto &db = DatabaseManager::instance().getDatabase(DatabaseManager::getCacheConnectionName(accountId));
        repo.saveMessages({ Message::fromJson(topFwdObj), Message::fromJson(topReplyObj) }, db);
    }

    DatabaseManager::instance().closeCacheDatabase(accountId);
    fixture.openCache(accountId);
    MessageRepository reopenedRepo(accountId);

    auto loadedTop = reopenedRepo.getMessage(500);
    REQUIRE(loadedTop.has_value());
    REQUIRE(loadedTop->id.get() == Snowflake(500));
    REQUIRE(loadedTop->isForwarded());
    REQUIRE(loadedTop->snapshotMessage != nullptr);
    REQUIRE(loadedTop->snapshotMessage->flags.get().testFlag(MessageFlag::IS_COMPONENTS_V2));
    REQUIRE(loadedTop->contentMessage().components.hasValue());
    REQUIRE(loadedTop->contentMessage().components->front().type.get() == MessageComponentType::CONTAINER);
    REQUIRE(loadedTop->contentMessage().components->front().components.hasValue());
    REQUIRE(loadedTop->contentMessage().components->front().components->front().content.valueOr("") == "Forwarded top snapshot text");
    REQUIRE(nlohmann::json::parse(loadedTop->snapshotMessage->componentsJson) == nlohmann::json::parse(topSnapshotComponents));

    auto loadedReply = reopenedRepo.getMessage(506);
    REQUIRE(loadedReply.has_value());
    REQUIRE(loadedReply->id.get() == Snowflake(506));
    REQUIRE(loadedReply->referencedMessage != nullptr);
    const auto &ref = *loadedReply->referencedMessage;
    REQUIRE(ref.id.get() == Snowflake(504));
    REQUIRE(ref.isForwarded());
    REQUIRE(ref.snapshotMessage != nullptr);
    REQUIRE(ref.snapshotMessage->flags.get().testFlag(MessageFlag::IS_COMPONENTS_V2));
    REQUIRE(ref.contentMessage().components.hasValue());
    REQUIRE(ref.contentMessage().components->front().type.get() == MessageComponentType::ACTION_ROW);
    REQUIRE(ref.contentMessage().components->front().components.hasValue());
    REQUIRE(ref.contentMessage().components->front().components->front().customId.valueOr("") == "snap_btn");
    REQUIRE(nlohmann::json::parse(ref.snapshotMessage->componentsJson) == nlohmann::json::parse(refSnapshotComponents));
}

TEST_CASE("Message cache: mixed public and private save yields public reload and no private main, attachment, or reference row", "[message_cache]")
{
    SandboxFixture fixture;
    const Snowflake accountId(10006);
    fixture.openCache(accountId);
    MessageRepository repo(accountId);

    auto pubObj = makeMessageJson("601", "200", "9061", "user_pub", "Public outer text");
    pubObj["attachments"] = nlohmann::json::array({ makeAttachmentJson("701", "pub.png") });

    auto privRefObj = makeMessageJson("603", "200", "9063", "user_priv_ref", "Secret ref text",
        "", "", static_cast<int64_t>(MessageFlag::EPHEMERAL));
    privRefObj["attachments"] = nlohmann::json::array({ makeAttachmentJson("703", "priv_ref.png") });
    pubObj["referenced_message"] = privRefObj;

    auto privMainObj = makeMessageJson("602", "200", "9062", "user_priv_main", "Secret main text",
        "", "", static_cast<int64_t>(MessageFlag::EPHEMERAL));
    privMainObj["attachments"] = nlohmann::json::array({ makeAttachmentJson("702", "priv_main.png") });

    repo.saveMessages({ Message::fromJson(pubObj), Message::fromJson(privMainObj) });

    DatabaseManager::instance().closeCacheDatabase(accountId);
    fixture.openCache(accountId);
    MessageRepository reopenedRepo(accountId);

    const auto loadedPub = reopenedRepo.getMessage(601);
    REQUIRE(loadedPub.has_value());
    REQUIRE(loadedPub->id.get() == Snowflake(601));
    REQUIRE(loadedPub->content.get() == "Public outer text");
    REQUIRE(loadedPub->attachments.hasValue());
    REQUIRE(loadedPub->attachments->size() == 1);
    REQUIRE(loadedPub->attachments->front().id.get() == Snowflake(701));
    REQUIRE(loadedPub->referencedMessage == nullptr);

    REQUIRE_FALSE(reopenedRepo.getMessage(602).has_value());
    REQUIRE_FALSE(reopenedRepo.getMessage(603).has_value());

    {
        auto &db = DatabaseManager::instance().getDatabase(DatabaseManager::getCacheConnectionName(accountId));
        REQUIRE(countRows(db, "SELECT count(*) FROM messages WHERE id IN (602, 603)") == 0);
        REQUIRE(countRows(db, "SELECT count(*) FROM attachments WHERE message_id IN (602, 603)") == 0);
    }
}

TEST_CASE("Message cache: updating existing public row with private message purges old row and attachments without persisting payload", "[message_cache]")
{
    SandboxFixture fixture;
    const Snowflake accountId(10007);
    fixture.openCache(accountId);
    MessageRepository repo(accountId);

    auto pubObj = makeMessageJson("610", "200", "9070", "user_update", "Original public content");
    pubObj["attachments"] = nlohmann::json::array({ makeAttachmentJson("710", "old.png") });
    repo.saveMessages({ Message::fromJson(pubObj) });

    const auto loadedBefore = repo.getMessage(610);
    REQUIRE(loadedBefore.has_value());
    REQUIRE(loadedBefore->content.get() == "Original public content");
    REQUIRE(loadedBefore->attachments.hasValue());
    REQUIRE(loadedBefore->attachments->size() == 1);

    auto privUpdateObj = makeMessageJson("610", "200", "9070", "user_update", "Secret replaced text",
        "", "", static_cast<int64_t>(MessageFlag::EPHEMERAL));
    repo.updateMessageContent(Message::fromJson(privUpdateObj));

    REQUIRE_FALSE(repo.getMessage(610).has_value());

    {
        auto &db = DatabaseManager::instance().getDatabase(DatabaseManager::getCacheConnectionName(accountId));
        REQUIRE(countRows(db, "SELECT count(*) FROM messages WHERE id = 610") == 0);
        REQUIRE(countRows(db, "SELECT count(*) FROM attachments WHERE message_id = 610") == 0);
        REQUIRE(countRows(db, "SELECT count(*) FROM messages WHERE content LIKE '%Secret replaced text%'") == 0);
    }

    auto pubObj2 = makeMessageJson("611", "200", "9071", "user_save_purge", "Initial public text");
    pubObj2["attachments"] = nlohmann::json::array({ makeAttachmentJson("711", "old2.png") });
    repo.saveMessages({ Message::fromJson(pubObj2) });
    REQUIRE(repo.getMessage(611).has_value());

    auto privSaveObj = makeMessageJson("611", "200", "9071", "user_save_purge", "Secret save text",
        "", "", static_cast<int64_t>(MessageFlag::EPHEMERAL));
    repo.saveMessages({ Message::fromJson(privSaveObj) });

    REQUIRE_FALSE(repo.getMessage(611).has_value());

    {
        auto &db = DatabaseManager::instance().getDatabase(DatabaseManager::getCacheConnectionName(accountId));
        REQUIRE(countRows(db, "SELECT count(*) FROM messages WHERE id = 611") == 0);
        REQUIRE(countRows(db, "SELECT count(*) FROM attachments WHERE message_id = 611") == 0);
        REQUIRE(countRows(db, "SELECT count(*) FROM messages WHERE content LIKE '%Secret save text%'") == 0);
    }
}

TEST_CASE("Message cache: private snapshot JSON does not persist while outer public text does", "[message_cache]")
{
    SandboxFixture fixture;
    const Snowflake accountId(10008);
    fixture.openCache(accountId);
    MessageRepository repo(accountId);

    const auto privateSnapshotObj = makeMessageJson("621", "200", "9081", "snap_author", "Secret snapshot text",
        "", "", static_cast<int64_t>(MessageFlag::EPHEMERAL));

    auto forwardObj = makeMessageJson("620", "200", "9080", "fwd_author", "Public outer forwarder text",
        "", "", static_cast<int64_t>(MessageFlag::HAS_SNAPSHOT), privateSnapshotObj.dump());
    forwardObj["message_reference"] = { { "type", 1 }, { "channel_id", "200" }, { "message_id", "621" } };

    repo.saveMessages({ Message::fromJson(forwardObj) });

    DatabaseManager::instance().closeCacheDatabase(accountId);
    fixture.openCache(accountId);
    MessageRepository reopenedRepo(accountId);

    const auto loaded = reopenedRepo.getMessage(620);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->id.get() == Snowflake(620));
    REQUIRE(loaded->content.get() == "Public outer forwarder text");
    REQUIRE(loaded->snapshotMessage == nullptr);
    REQUIRE(loaded->snapshotJson.empty());

    {
        auto &db = DatabaseManager::instance().getDatabase(DatabaseManager::getCacheConnectionName(accountId));
        REQUIRE(countRows(db, "SELECT count(*) FROM messages WHERE snapshot IS NOT NULL AND id = 620") == 0);
        REQUIRE(countRows(db, "SELECT count(*) FROM messages WHERE content LIKE '%Secret snapshot text%'") == 0);
    }
}

TEST_CASE("Message cache: legacy private messages and attachments are purged on reopen while ordinary rows survive", "[message_cache]")
{
    SandboxFixture fixture;
    const Snowflake accountId(10009);
    fixture.openCache(accountId);

    {
        auto &db = DatabaseManager::instance().getDatabase(DatabaseManager::getCacheConnectionName(accountId));
        db.execute(
            "INSERT INTO users (id, username) VALUES (9090, 'legacy_user');\n"
            "INSERT INTO messages (id, author_id, channel_id, content, timestamp, type, flags, deleted) "
            "VALUES (630, 9090, 200, 'Ordinary surviving message', '2026-11-09T12:00:00.000000+00:00', 0, 0, 0);\n"
            "INSERT INTO attachments (id, message_id, filename, size, url, proxy_url) "
            "VALUES (730, 630, 'surviving.png', 1024, 'https://cdn.example.com/surviving.png', 'https://media.example.com/surviving.png');\n"
            "INSERT INTO messages (id, author_id, channel_id, content, timestamp, type, flags, deleted) "
            "VALUES (631, 9090, 200, 'Legacy private message', '2026-11-09T12:00:00.000000+00:00', 0, 64, 0);\n"
            "INSERT INTO attachments (id, message_id, filename, size, url, proxy_url) "
            "VALUES (731, 631, 'legacy.png', 2048, 'https://cdn.example.com/legacy.png', 'https://media.example.com/legacy.png');\n"
            "INSERT INTO messages (id, author_id, channel_id, content, timestamp, type, flags, deleted) "
            "VALUES (632, 9090, 200, 'Legacy private with extra flags', '2026-11-09T12:00:00.000000+00:00', 0, 65, 0);\n"
            "INSERT INTO attachments (id, message_id, filename, size, url, proxy_url) "
            "VALUES (732, 632, 'legacy2.png', 4096, 'https://cdn.example.com/legacy2.png', 'https://media.example.com/legacy2.png');"
        );
    }

    DatabaseManager::instance().closeCacheDatabase(accountId);
    fixture.openCache(accountId);
    MessageRepository reopenedRepo(accountId);

    const auto ordinaryMsg = reopenedRepo.getMessage(630);
    REQUIRE(ordinaryMsg.has_value());
    REQUIRE(ordinaryMsg->id.get() == Snowflake(630));
    REQUIRE(ordinaryMsg->content.get() == "Ordinary surviving message");
    REQUIRE(ordinaryMsg->attachments.hasValue());
    REQUIRE(ordinaryMsg->attachments->size() == 1);
    REQUIRE(ordinaryMsg->attachments->front().id.get() == Snowflake(730));

    REQUIRE_FALSE(reopenedRepo.getMessage(631).has_value());
    REQUIRE_FALSE(reopenedRepo.getMessage(632).has_value());

    {
        auto &db = DatabaseManager::instance().getDatabase(DatabaseManager::getCacheConnectionName(accountId));
        REQUIRE(countRows(db, "SELECT count(*) FROM messages WHERE (flags & 64) != 0") == 0);
        REQUIRE(countRows(db, "SELECT count(*) FROM attachments WHERE message_id IN (631, 632)") == 0);
    }

    DatabaseManager::instance().closeCacheDatabase(accountId);
    fixture.openCache(accountId);
    MessageRepository secondReopenRepo(accountId);

    const auto ordinaryAgain = secondReopenRepo.getMessage(630);
    REQUIRE(ordinaryAgain.has_value());
    REQUIRE(ordinaryAgain->id.get() == Snowflake(630));
    REQUIRE(ordinaryAgain->content.get() == "Ordinary surviving message");
    REQUIRE(ordinaryAgain->attachments.hasValue());
    REQUIRE(ordinaryAgain->attachments->size() == 1);
    REQUIRE(ordinaryAgain->attachments->front().id.get() == Snowflake(730));

    {
        auto &db = DatabaseManager::instance().getDatabase(DatabaseManager::getCacheConnectionName(accountId));
        REQUIRE(countRows(db, "SELECT count(*) FROM messages WHERE (flags & 64) != 0") == 0);
        REQUIRE(countRows(db, "SELECT count(*) FROM attachments WHERE message_id IN (631, 632)") == 0);
    }
}

TEST_CASE("Message cache: poll round-trip preserves poll data and referenced message poll", "[message_cache]")
{
    SandboxFixture fixture;
    const Snowflake accountId(10010);
    fixture.openCache(accountId);
    MessageRepository repo(accountId);

    const std::string mainPoll =
        R"({"question":{"text":"Aliens?"},"answers":[{"answer_id":1,"poll_media":{"text":"Alien"}},{"answer_id":2,"poll_media":{"text":"Alien 2","emoji":{"id":null,"name":"👽"}}}],"expiry":"2024-05-02T10:00:02.039342+00:00","allow_multiselect":true,"layout_type":1,"results":{"answer_counts":[{"id":1,"count":1,"me_voted":false}],"is_finalized":false}})";
    const std::string replyPoll =
        R"({"question":{"text":"Pizza or tacos?"},"answers":[{"answer_id":1,"poll_media":{"text":"Pizza"}},{"answer_id":2,"poll_media":{"text":"Tacos"}}],"expiry":"2024-05-03T10:00:02.039342+00:00","allow_multiselect":false,"layout_type":1,"results":{"answer_counts":[],"is_finalized":true}})";

    auto mainObj = makeMessageJson("700", "200", "9001", "poll_user", "");
    mainObj["poll"] = nlohmann::json::parse(mainPoll);

    auto replyObj = makeMessageJson("701", "200", "9002", "reply_user", "Replying to poll");
    replyObj["poll"] = nlohmann::json::parse(replyPoll);
    mainObj["referenced_message"] = replyObj;

    repo.saveMessages({ Message::fromJson(mainObj) });

    DatabaseManager::instance().closeCacheDatabase(accountId);
    fixture.openCache(accountId);
    MessageRepository reopenedRepo(accountId);

    const auto loadedOpt = reopenedRepo.getMessage(700);
    REQUIRE(loadedOpt.has_value());
    const auto &loaded = *loadedOpt;

    REQUIRE(loaded.id.get() == Snowflake(700));
    REQUIRE(loaded.poll.hasValue());
    REQUIRE(loaded.poll->question.hasValue());
    REQUIRE(loaded.poll->question->text.valueOr("") == "Aliens?");
    REQUIRE(loaded.poll->results.hasValue());
    REQUIRE_FALSE(loaded.poll->results->isFinalized.get());
    REQUIRE(loaded.poll->results->answerCounts.hasValue());
    REQUIRE(loaded.poll->results->answerCounts->size() == 1);
    REQUIRE(loaded.poll->results->answerCounts->front().count.get() == 1);
    REQUIRE(nlohmann::json::parse(loaded.pollJson) == nlohmann::json::parse(mainPoll));

    REQUIRE(loaded.referencedMessage != nullptr);
    const auto &ref = *loaded.referencedMessage;
    REQUIRE(ref.id.get() == Snowflake(701));
    REQUIRE(ref.poll.hasValue());
    REQUIRE(ref.poll->question.hasValue());
    REQUIRE(ref.poll->question->text.valueOr("") == "Pizza or tacos?");
    REQUIRE(ref.poll->results.hasValue());
    REQUIRE(ref.poll->results->isFinalized.get());
    REQUIRE(nlohmann::json::parse(ref.pollJson) == nlohmann::json::parse(replyPoll));
}

TEST_CASE("Message cache: poll vote update survives repo persistence and reload", "[message_cache]")
{
    SandboxFixture fixture;
    const Snowflake accountId(10011);
    fixture.openCache(accountId);
    MessageRepository repo(accountId);

    const std::string pollText =
        R"({"question":{"text":"Game?"},"answers":[{"answer_id":7,"poll_media":{"text":"Doom"}},{"answer_id":42,"poll_media":{"text":"Quake"}}],"expiry":"2024-05-02T10:00:02.039342+00:00","allow_multiselect":true,"layout_type":1,"custom_poll_field":"poll_opaque","results":{"answer_counts":[{"id":7,"count":1,"me_voted":false,"custom_count_field":"count_opaque"}],"is_finalized":false,"custom_results_field":777}})";

    auto msgObj = makeMessageJson("710", "200", "9001", "poll_user", "");
    msgObj["poll"] = nlohmann::json::parse(pollText);
    repo.saveMessages({ Message::fromJson(msgObj) });

    auto loadedOpt = repo.getMessage(Snowflake(710));
    REQUIRE(loadedOpt.has_value());
    auto loadedMsg = *loadedOpt;

    MessagePollVote voteEvent;
    voteEvent.userId = Snowflake(500);
    voteEvent.channelId = Snowflake(200);
    voteEvent.messageId = Snowflake(710);
    voteEvent.answerId = 7;

    REQUIRE(Styx::Core::applyPollVote(loadedMsg, voteEvent, true, Snowflake(500)));

    repo.updateMessageContent(loadedMsg);

    DatabaseManager::instance().closeCacheDatabase(accountId);
    fixture.openCache(accountId);
    MessageRepository reopenedRepo(accountId);

    const auto reloadedOpt = reopenedRepo.getMessage(Snowflake(710));
    REQUIRE(reloadedOpt.has_value());
    const auto &reloaded = *reloadedOpt;

    REQUIRE(reloaded.poll.hasValue());
    REQUIRE(reloaded.poll->results.hasValue());
    REQUIRE(reloaded.poll->results->answerCounts.hasValue());
    REQUIRE(reloaded.poll->results->answerCounts->size() == 1);
    const auto &ac = reloaded.poll->results->answerCounts->front();
    REQUIRE(ac.id.get() == 7);
    REQUIRE(ac.count.get() == 2);
    REQUIRE(ac.meVoted.get());

    const auto doc = nlohmann::json::parse(reloaded.pollJson);
    REQUIRE(doc["custom_poll_field"] == "poll_opaque");
    REQUIRE(doc["results"]["custom_results_field"] == 777);
    REQUIRE(doc["results"]["answer_counts"][0]["custom_count_field"] == "count_opaque");
}
