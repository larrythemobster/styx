#include <catch2/catch_test_macros.hpp>

#include "Platform/Dispatcher.hpp"
#include "Platform/Http.hpp"
#include "Platform/Image.hpp"
#include "Platform/Paths.hpp"
#include "Platform/SecureStore.hpp"
#include "Platform/Settings.hpp"
#include "Platform/Signal.hpp"
#include "Platform/Sqlite.hpp"
#include "Platform/Utf8.hpp"
#include "Platform/WorkerLoop.hpp"
#include "Platform/ThreadPool.hpp"

#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>
#include <atomic>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <objbase.h>
using native_sock_t = SOCKET;
constexpr native_sock_t kInvalidSock = INVALID_SOCKET;
inline int closeSock(native_sock_t s) { return ::closesocket(s); }
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using native_sock_t = int;
constexpr native_sock_t kInvalidSock = -1;
inline int closeSock(native_sock_t s) { return ::close(s); }
#endif

namespace {

class LoopbackHttpServer {
public:
    LoopbackHttpServer()
    {
#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
        m_server = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_server == kInvalidSock)
            return;

        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bindAddr.sin_port = 0;

        if (::bind(m_server, reinterpret_cast<sockaddr *>(&bindAddr), sizeof(bindAddr)) != 0 ||
            ::listen(m_server, 5) != 0) {
            closeSock(m_server);
            m_server = kInvalidSock;
            return;
        }

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(m_server, reinterpret_cast<sockaddr *>(&bound), &len) == 0) {
            m_port = ntohs(bound.sin_port);
        }

        m_thread = std::thread(&LoopbackHttpServer::run, this);
    }

    ~LoopbackHttpServer()
    {
        m_running = false;
        if (m_server != kInvalidSock) {
#ifdef _WIN32
            ::shutdown(m_server, SD_BOTH);
#else
            ::shutdown(m_server, SHUT_RDWR);
#endif
            closeSock(m_server);
            m_server = kInvalidSock;
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    uint16_t port() const { return m_port; }
    bool valid() const { return m_port != 0 && m_server != kInvalidSock; }

private:
    void run()
    {
        while (m_running.load()) {
            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            native_sock_t client = ::accept(m_server, reinterpret_cast<sockaddr *>(&clientAddr), &clientLen);
            if (client == kInvalidSock) {
                break;
            }

            char buffer[4096]{};
            int n = ::recv(client, buffer, sizeof(buffer) - 1, 0);
            if (n > 0) {
                std::string req(buffer, n);
                if (req.rfind("GET /test", 0) == 0) {
                    std::string body = "Hello Platform!";
                    std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                                       "\r\nX-Custom-Header: StyxPlatform\r\nConnection: close\r\n\r\n" + body;
                    ::send(client, resp.data(), static_cast<int>(resp.size()), 0);
                } else if (req.rfind("POST /echo", 0) == 0) {
                    size_t bodyPos = req.find("\r\n\r\n");
                    std::string body = (bodyPos != std::string::npos) ? req.substr(bodyPos + 4) : "";
                    std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                                       "\r\nConnection: close\r\n\r\n" + body;
                    ::send(client, resp.data(), static_cast<int>(resp.size()), 0);
                } else {
                    std::string resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    ::send(client, resp.data(), static_cast<int>(resp.size()), 0);
                }
            }
            closeSock(client);
        }
    }

    native_sock_t m_server{kInvalidSock};
    uint16_t m_port{0};
    std::atomic<bool> m_running{true};
    std::thread m_thread;
};

} // namespace
using namespace Styx::Platform;
using namespace std::chrono_literals;

TEST_CASE("Dispatcher: task ordering, cross-thread post, timers", "[dispatcher]")
{
    Dispatcher dispatcher;

    SECTION("Task ordering")
    {
        std::vector<int> executionOrder;
        dispatcher.post([&]() { executionOrder.push_back(1); });
        dispatcher.post([&]() { executionOrder.push_back(2); });
        dispatcher.post([&]() { executionOrder.push_back(3); });

        REQUIRE(dispatcher.hasPendingTasks());
        dispatcher.runPending();
        REQUIRE_FALSE(dispatcher.hasPendingTasks());

        std::vector<int> expected = { 1, 2, 3 };
        REQUIRE(executionOrder == expected);
    }

    SECTION("Wake hook")
    {
        bool wakeCalled = false;
        dispatcher.setWakeHook([&]() { wakeCalled = true; });
        dispatcher.post([]() {});
        REQUIRE(wakeCalled);
        dispatcher.drainTasks();
    }

    SECTION("Cross-thread post")
    {
        std::atomic<bool> workerFinished{false};
        std::atomic<bool> taskExecuted{false};

        std::thread t([&]() {
            dispatcher.post([&]() { taskExecuted.store(true); });
            workerFinished.store(true);
        });

        t.join();
        REQUIRE(workerFinished.load());

        dispatcher.runPending();
        REQUIRE(taskExecuted.load());
    }

    SECTION("Timers: single-shot, repeating, cancellation")
    {
        int singleShotCount = 0;
        int repeatingCount = 0;

        TimerId singleId = dispatcher.setTimer(10ms, false, [&]() { singleShotCount++; });
        TimerId repeatId = dispatcher.setTimer(10ms, true, [&]() { repeatingCount++; });

        // Immediately running shouldn't fire yet
        dispatcher.runPending();
        REQUIRE(singleShotCount == 0);
        REQUIRE(repeatingCount == 0);

        auto timeout = dispatcher.nextTimeout();
        REQUIRE(timeout.has_value());

        // Sleep to let timers expire
        std::this_thread::sleep_for(25ms);
        dispatcher.runPending();

        REQUIRE(singleShotCount == 1);
        REQUIRE(repeatingCount >= 1);

        // Cancel repeating timer
        dispatcher.cancelTimer(repeatId);
        int savedRepeat = repeatingCount;

        std::this_thread::sleep_for(25ms);
        dispatcher.runPending();

        REQUIRE(singleShotCount == 1); // Single-shot never fires again
        REQUIRE(repeatingCount == savedRepeat); // Cancelled repeating timer stops
    }
}

TEST_CASE("Signal: connections, disconnect during emit, queued delivery", "[signal]")
{
    SECTION("Basic emit and multiple connections")
    {
        Signal<int, std::string> sig;
        int sum = 0;
        std::string lastStr;

        auto c1 = sig.connect([&](int n, std::string s) {
            sum += n;
            lastStr = s;
        });

        auto c2 = sig.connect([&](int n, std::string) {
            sum += n * 2;
        });

        REQUIRE(sig.connectionCount() == 2);
        sig.emit(5, "hello");

        REQUIRE(sum == 15);
        REQUIRE(lastStr == "hello");
    }

    SECTION("Owner-based disconnect")
    {
        Signal<int> sig;
        int countA = 0;
        int countB = 0;
        int ownerA = 10;
        int ownerB = 20;

        sig.connect(&ownerA, [&](int) { countA++; });
        sig.connect(&ownerB, [&](int) { countB++; });

        sig.emit(1);
        REQUIRE(countA == 1);
        REQUIRE(countB == 1);

        sig.disconnect(&ownerA);
        sig.emit(1);
        REQUIRE(countA == 1);
        REQUIRE(countB == 2);
    }

    SECTION("RAII Connection disconnect")
    {
        Signal<int> sig;
        int count = 0;
        {
            Connection c = sig.connect([&](int) { count++; });
            REQUIRE(c.isConnected());
            sig.emit(1);
            REQUIRE(count == 1);
        } // c destructs here

        sig.emit(1);
        REQUIRE(count == 1);
    }

    SECTION("Safe disconnect during emit")
    {
        Signal<int> sig;
        std::vector<int> received;
        Connection c2;

        auto c1 = sig.connect([&](int val) {
            received.push_back(val * 10);
            c2.disconnect(); // Disconnect c2 while emit is running
        });

        c2 = sig.connect([&](int val) {
            received.push_back(val * 100);
        });

        auto c3 = sig.connect([&](int val) {
            received.push_back(val * 1000);
        });

        sig.emit(1);

        // c1 should have run (10), c2 disconnected before its turn so not run, c3 should have run (1000)
        std::vector<int> expected = { 10, 1000 };
        REQUIRE(received == expected);
    }

    SECTION("Queued connection via Dispatcher")
    {
        Signal<int> sig;
        int result = 0;

        Connection c = sig.connect([&](int val) { result = val; }, ConnectionType::Queued);

        sig.emit(42);
        REQUIRE(result == 0); // Not executed directly

        Dispatcher::main().runPending();
        REQUIRE(result == 42); // Executed via Dispatcher pump
    }
}

TEST_CASE("Settings: typed store, atomic save, change notification", "[settings]")
{
    std::filesystem::path testDir = Paths::cacheDir() / "test_settings_dir";
    std::filesystem::path testFile = testDir / "test_settings.json";

    std::error_code ec;
    std::filesystem::remove(testFile, ec);

    Settings settings(testFile);

    SECTION("Typed values and defaults")
    {
        REQUIRE(settings.getBool("missing_bool", true) == true);
        REQUIRE(settings.getInt("missing_int", 99) == 99);
        REQUIRE(settings.getString("missing_str", "default") == "default");

        settings.setBool("general/in_memory_cache", true);
        settings.setInt("chat/gif_max_playing", 6);
        settings.setString("general/font_engine", "freetype");

        REQUIRE(settings.getBool("general/in_memory_cache") == true);
        REQUIRE(settings.getInt("chat/gif_max_playing") == 6);
        REQUIRE(settings.getString("general/font_engine") == "freetype");
    }

    SECTION("Atomic save and load round-trip")
    {
        settings.setBool("test_key_bool", true);
        settings.setInt("test_key_int", 1234);
        settings.setString("test_key_str", "Styx Rocks");

        REQUIRE(settings.save());
        REQUIRE(std::filesystem::exists(testFile));

        Settings reloaded(testFile);
        REQUIRE(reloaded.getBool("test_key_bool") == true);
        REQUIRE(reloaded.getInt("test_key_int") == 1234);
        REQUIRE(reloaded.getString("test_key_str") == "Styx Rocks");
    }

    SECTION("Change notifications")
    {
        std::string changedKey;
        nlohmann::json changedVal;

        auto conn = settings.changed.connect([&](const std::string &k, const nlohmann::json &v) {
            changedKey = k;
            changedVal = v;
        });

        settings.setInt("notify_test", 777);
        REQUIRE(changedKey == "notify_test");
        REQUIRE(changedVal == 777);
    }

    std::filesystem::remove(testFile, ec);
    std::filesystem::remove(testDir, ec);
}

TEST_CASE("Sqlite: CRUD, prepared statements, and transaction rollback", "[sqlite]")
{
    Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(db.isOpen());

    SECTION("Schema creation and CRUD")
    {
        db.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, balance REAL, avatar BLOB);");

        // Insert using prepared statement with typed binds
        Statement insertStmt = db.prepare("INSERT INTO users (id, name, balance) VALUES (?, ?, ?);");
        insertStmt.bind(1, static_cast<int64_t>(1001));
        insertStmt.bind(2, "Alice");
        insertStmt.bind(3, 125.50);
        insertStmt.execute();

        // Insert using named parameters
        Statement insertNamed = db.prepare("INSERT INTO users (id, name, balance) VALUES (:id, :name, :bal);");
        insertNamed.bind(":id", static_cast<int64_t>(1002));
        insertNamed.bind(":name", "Bob");
        insertNamed.bind(":bal", 50.25);
        insertNamed.execute();

        REQUIRE(db.changes() == 1);

        // Query rows
        Statement query = db.prepare("SELECT id, name, balance FROM users ORDER BY id ASC;");
        REQUIRE(query.step());
        REQUIRE(query.getInt64(0) == 1001);
        REQUIRE(query.getText(1) == "Alice");
        REQUIRE(query.getDouble(2) == 125.50);

        REQUIRE(query.step());
        REQUIRE(query.getInt64(0) == 1002);
        REQUIRE(query.getText(1) == "Bob");

        REQUIRE_FALSE(query.step()); // Done
    }

    SECTION("Transaction commit")
    {
        db.execute("CREATE TABLE accounts (id INTEGER PRIMARY KEY, name TEXT);");
        {
            Transaction tx(db);
            Statement stmt = db.prepare("INSERT INTO accounts (id, name) VALUES (1, 'Committed');");
            stmt.execute();
            tx.commit();
        }

        Statement check = db.prepare("SELECT count(*) FROM accounts WHERE id = 1;");
        REQUIRE(check.step());
        REQUIRE(check.getInt(0) == 1);
    }

    SECTION("Transaction rollback on scope exit")
    {
        db.execute("CREATE TABLE accounts (id INTEGER PRIMARY KEY, name TEXT);");
        {
            Transaction tx(db);
            Statement stmt = db.prepare("INSERT INTO accounts (id, name) VALUES (2, 'RolledBack');");
            stmt.execute();
            // Exit without commit -> auto-rollback
        }

        Statement check = db.prepare("SELECT count(*) FROM accounts WHERE id = 2;");
        REQUIRE(check.step());
        REQUIRE(check.getInt(0) == 0);
    }
}

TEST_CASE("SecureStore: write, read, delete throwaway credential", "[securestore]")
{
    const std::string throwawayService = "StyxUnitTest";
    const std::string throwawayKey = "throwaway_account_test_key_99999";
    const std::string testSecret = "SecretPasswordToken!#@123";

    // Clean up in case left over
    SecureStore::deletePassword(throwawayService, throwawayKey);

    REQUIRE(SecureStore::writePassword(throwawayService, throwawayKey, testSecret));

    auto loaded = SecureStore::readPassword(throwawayService, throwawayKey);
    REQUIRE(loaded.has_value());
    REQUIRE(*loaded == testSecret);

    REQUIRE(SecureStore::deletePassword(throwawayService, throwawayKey));

    auto afterDelete = SecureStore::readPassword(throwawayService, throwawayKey);
    REQUIRE_FALSE(afterDelete.has_value());
}

TEST_CASE("Image: decode resource PNG and scale", "[image]")
{
    std::filesystem::path placeholderPath = "resources/placeholder.png";
    if (!std::filesystem::exists(placeholderPath)) {
        placeholderPath = "../resources/placeholder.png";
    }

    if (std::filesystem::exists(placeholderPath)) {
        auto img = Image::decodeFile(placeholderPath);
        REQUIRE(img.has_value());
        REQUIRE_FALSE(img->empty());
        REQUIRE(img->width > 0);
        REQUIRE(img->height > 0);
        REQUIRE(img->channels == 4);
        REQUIRE(img->pixels.size() == static_cast<size_t>(img->width) * img->height * 4);

        // Test scaling
        int targetW = 32;
        int targetH = 32;
        ImageBuffer scaled = Image::scale(*img, targetW, targetH);
        REQUIRE(scaled.width == targetW);
        REQUIRE(scaled.height == targetH);
        REQUIRE(scaled.channels == 4);
        REQUIRE(scaled.pixels.size() == static_cast<size_t>(targetW) * targetH * 4);
    }
}

TEST_CASE("Http: GET, POST and cancellation", "[http]")
{
    SECTION("Loopback GET request")
    {
        LoopbackHttpServer server;
        REQUIRE(server.valid());

        bool callbackFired = false;
        HttpResponse result;

        HttpRequest req;
        req.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/test";
        req.timeout = 5000ms;


        HttpClient::instance().request(req, [&](const HttpResponse &resp) {

            callbackFired = true;
            result = resp;
        });


        for (int i = 0; i < 100 && !callbackFired; ++i) {
            std::this_thread::sleep_for(10ms);
            Dispatcher::main().runPending();
        }

        REQUIRE(callbackFired);
        REQUIRE(result.ok());
        REQUIRE(result.statusCode == 200);
        REQUIRE(result.body == "Hello Platform!");
        REQUIRE(result.getHeader("x-custom-header") == "StyxPlatform");
    }

    SECTION("Loopback POST request")
    {
        LoopbackHttpServer server;
        REQUIRE(server.valid());

        bool callbackFired = false;
        HttpResponse result;

        std::string postUrl = "http://127.0.0.1:" + std::to_string(server.port()) + "/echo";
        HttpClient::instance().post(postUrl, "EchoThisContent", "text/plain", [&](const HttpResponse &resp) {
            callbackFired = true;
            result = resp;
        });

        for (int i = 0; i < 100 && !callbackFired; ++i) {
            std::this_thread::sleep_for(10ms);
            Dispatcher::main().runPending();
        }

        REQUIRE(callbackFired);
        REQUIRE(result.ok());
        REQUIRE(result.statusCode == 200);
        REQUIRE(result.body == "EchoThisContent");
    }

    SECTION("Cancellation")
    {
        bool callbackFired = false;
        bool wasCancelled = false;

        HttpRequest req;
        req.url = "http://127.0.0.1:65432/will-not-respond";
        req.timeout = 5000ms;

        uint64_t reqId = HttpClient::instance().request(req, [&](const HttpResponse &resp) {
            callbackFired = true;
            wasCancelled = resp.cancelled;
        });

        // Cancel immediately
        HttpClient::instance().cancel(reqId);

        for (int i = 0; i < 50 && !callbackFired; ++i) {
            std::this_thread::sleep_for(10ms);
            Dispatcher::main().runPending();
        }

        if (callbackFired) {
            REQUIRE(wasCancelled);
        }
    }
}

TEST_CASE("Utf8: helpers and string conversions", "[utf8]")
{
    SECTION("Trimming")
    {
        REQUIRE(trim("  hello world \t\r\n") == "hello world");
        REQUIRE(trimLeft("   left") == "left");
        REQUIRE(trimRight("right   ") == "right");
        REQUIRE(trim("   ").empty());
    }

    SECTION("Splitting")
    {
        auto parts = split("foo/bar/baz", '/');
        REQUIRE(parts.size() == 3);
        REQUIRE(parts[0] == "foo");
        REQUIRE(parts[1] == "bar");
        REQUIRE(parts[2] == "baz");

        auto partsSkip = split("foo//bar", '/', true);
        REQUIRE(partsSkip.size() == 2);
        REQUIRE(partsSkip[0] == "foo");
        REQUIRE(partsSkip[1] == "bar");

        auto strParts = split("a::b::c", "::");
        REQUIRE(strParts.size() == 3);
        REQUIRE(strParts[0] == "a");
        REQUIRE(strParts[1] == "b");
        REQUIRE(strParts[2] == "c");
    }

    SECTION("Case folding")
    {
        REQUIRE(toLowerAscii("Styx_PLATFORM") == "styx_platform");
        REQUIRE(toUpperAscii("styx_platform") == "STYX_PLATFORM");
        REQUIRE(equalsIgnoreCaseAscii("Styx", "STYX"));
        REQUIRE_FALSE(equalsIgnoreCaseAscii("Styx", "Other"));
    }

    SECTION("Prefix / suffix / replace")
    {
        REQUIRE(startsWith("account_123_token", "account_"));
        REQUIRE(endsWith("account_123_token", "_token"));
        REQUIRE(replace("hello world world", "world", "styx") == "hello styx styx");
    }

    SECTION("UTF-8 <-> UTF-16 round trip")
    {
        std::string original = "Hello World! Styx \u2764 \U0001F600";
        std::wstring wstr = toUtf16(original);
        REQUIRE_FALSE(wstr.empty());
        std::string roundTrip = fromUtf16(wstr);
        REQUIRE(roundTrip == original);
    }
}

TEST_CASE("Storage: Message row and schema round trip", "[storage]")
{
    Database db;
    REQUIRE(db.open(":memory:"));

    db.execute(R"(
        CREATE TABLE "messages" (
            "id" INTEGER NOT NULL,
            "author_id" INTEGER NOT NULL,
            "channel_id" INTEGER NOT NULL,
            "content" TEXT NOT NULL,
            "timestamp" TEXT NOT NULL,
            "edited_timestamp" TEXT,
            "type" INTEGER NOT NULL,
            "flags" INTEGER NOT NULL,
            "embeds" TEXT,
            "reactions" TEXT,
            "stickers" TEXT,
            "deleted" INTEGER NOT NULL,
            "referenced_message_id" INTEGER,
            "reference_type" INTEGER,
            "reference_channel_id" INTEGER,
            "reference_guild_id" INTEGER,
            "snapshot" TEXT,
            "context_only" INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY("id")
        );
    )");

    auto insert = db.prepare(R"(
        INSERT INTO messages
        (id, author_id, channel_id, content, timestamp, edited_timestamp, type, flags, embeds, reactions, stickers, deleted,
         referenced_message_id, reference_type, reference_channel_id, reference_guild_id, snapshot, context_only)
        VALUES (:id, :author_id, :channel_id, :content, :timestamp, :edited_timestamp, :type, :flags, :embeds, :reactions, :stickers, :deleted,
                :referenced_message_id, :reference_type, :reference_channel_id, :reference_guild_id, :snapshot, :context_only)
    )");

    insert.bind(":id", int64_t(123456789012345LL));
    insert.bind(":author_id", int64_t(98765432109876LL));
    insert.bind(":channel_id", int64_t(11223344556677LL));
    insert.bind(":content", "Hello Styx Platform!");
    insert.bind(":timestamp", "2026-10-22T10:00:00.000Z");
    insert.bindNull(":edited_timestamp");
    insert.bind(":type", 0);
    insert.bind(":flags", 0);
    insert.bind(":embeds", R"([{"title":"Test Embed"}])");
    insert.bindNull(":reactions");
    insert.bindNull(":stickers");
    insert.bind(":deleted", 0);
    insert.bindNull(":referenced_message_id");
    insert.bindNull(":reference_type");
    insert.bindNull(":reference_channel_id");
    insert.bindNull(":reference_guild_id");
    insert.bindNull(":snapshot");
    insert.bind(":context_only", 0);
    insert.execute();

    auto select = db.prepare("SELECT id, author_id, channel_id, content, timestamp, embeds FROM messages WHERE id = :id");
    select.bind(":id", int64_t(123456789012345LL));
    REQUIRE(select.step());
    REQUIRE(select.getInt64(0) == 123456789012345LL);
    REQUIRE(select.getInt64(1) == 98765432109876LL);
    REQUIRE(select.getInt64(2) == 11223344556677LL);
    REQUIRE(select.getText(3) == "Hello Styx Platform!");
    REQUIRE(select.getText(4) == "2026-10-22T10:00:00.000Z");
    REQUIRE(select.getText(5) == R"([{"title":"Test Embed"}])");
    REQUIRE_FALSE(select.step());
}

TEST_CASE("WorkerLoop - post execution and order", "[platform]")
{
    using namespace Styx::Platform;
    WorkerLoop loop("TestPost");
    REQUIRE(loop.isRunning());

    std::atomic<int> counter{0};
    std::vector<int> executedOrder;
    std::mutex orderMutex;
    std::atomic<bool> done{false};

    for (int i = 1; i <= 5; ++i) {
        loop.post([&counter, &executedOrder, &orderMutex, i] {
            std::lock_guard lock(orderMutex);
            executedOrder.push_back(i);
            counter.fetch_add(1);
        });
    }

    loop.post([&done] {
        done.store(true);
    });

    auto start = std::chrono::steady_clock::now();
    while (!done.load() && (std::chrono::steady_clock::now() - start) < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(done.load());
    REQUIRE(counter.load() == 5);
    std::vector<int> expected = {1, 2, 3, 4, 5};
    REQUIRE(executedOrder == expected);
}

TEST_CASE("WorkerLoop - timers single-shot and repeating", "[platform]")
{
    using namespace Styx::Platform;
    WorkerLoop loop("TestTimers");

    std::atomic<bool> singleShotFired{false};
    loop.setTimer(std::chrono::milliseconds(20), false, [&singleShotFired] {
        singleShotFired.store(true);
    });

    std::atomic<int> repeatCount{0};
    TimerId repeatId = loop.setTimer(std::chrono::milliseconds(15), true, [&repeatCount] {
        repeatCount.fetch_add(1);
    });

    auto start = std::chrono::steady_clock::now();
    while ((!singleShotFired.load() || repeatCount.load() < 2) &&
           (std::chrono::steady_clock::now() - start) < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(singleShotFired.load());
    REQUIRE(repeatCount.load() >= 2);

    loop.cancelTimer(repeatId);
    int countAfterCancel = repeatCount.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(repeatCount.load() <= countAfterCancel + 1);
}

TEST_CASE("WorkerLoop - watchSocket readability", "[platform]")
{
    using namespace Styx::Platform;
    WorkerLoop loop("TestSocket");

    native_sock_t rx = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(rx != kInvalidSock);

    sockaddr_in rxAddr{};
    rxAddr.sin_family = AF_INET;
    rxAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    rxAddr.sin_port = 0;
    REQUIRE(::bind(rx, reinterpret_cast<sockaddr *>(&rxAddr), sizeof(rxAddr)) == 0);

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    REQUIRE(::getsockname(rx, reinterpret_cast<sockaddr *>(&bound), &len) == 0);

    std::atomic<bool> readableFired{false};
    std::string receivedData;
    std::mutex dataMutex;

    loop.watchSocket(static_cast<SocketHandle>(rx), [&readableFired, &receivedData, &dataMutex, rx] {
        char buf[128];
        int n = ::recv(rx, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            std::lock_guard lock(dataMutex);
            receivedData = buf;
            readableFired.store(true);
        }
    });

    native_sock_t tx = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(tx != kInvalidSock);

    const char msg[] = "ping_worker_loop";
    ::sendto(tx, msg, static_cast<int>(sizeof(msg)), 0, reinterpret_cast<sockaddr *>(&bound), sizeof(bound));

    auto start = std::chrono::steady_clock::now();
    while (!readableFired.load() && (std::chrono::steady_clock::now() - start) < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(readableFired.load());
    {
        std::lock_guard lock(dataMutex);
        REQUIRE(receivedData == msg);
    }

    loop.unwatchSocket(static_cast<SocketHandle>(rx));
    closeSock(tx);
    closeSock(rx);
}

TEST_CASE("WorkerLoop - COM MTA init hook", "[platform]")
{
    using namespace Styx::Platform;
#ifdef _WIN32
    std::atomic<bool> comInitialized{false};
    WorkerOptions opts;
    opts.name = "TestCom";
    opts.initComMta = true;
    WorkerLoop loop(opts);

    loop.post([&comInitialized] {
        APTTYPE aptType;
        APTTYPEQUALIFIER aptQual;
        HRESULT hr = CoGetApartmentType(&aptType, &aptQual);
        if (SUCCEEDED(hr) && (aptType == APTTYPE_MTA || aptType == APTTYPE_NA)) {
            comInitialized.store(true);
        }
    });

    auto start = std::chrono::steady_clock::now();
    while (!comInitialized.load() && (std::chrono::steady_clock::now() - start) < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(comInitialized.load());
#else
    REQUIRE(true);
#endif
}

TEST_CASE("WorkerLoop - shutdown under load", "[platform]")
{
    using namespace Styx::Platform;
    std::atomic<bool> stopClients{false};
    std::atomic<uint64_t> tasksExecuted{0};

    auto loop = std::make_unique<WorkerLoop>("TestLoad");

    std::vector<std::thread> clients;
    for (int c = 0; c < 4; ++c) {
        clients.emplace_back([&loop, &stopClients, &tasksExecuted] {
            while (!stopClients.load(std::memory_order_relaxed)) {
                loop->post([&tasksExecuted] {
                    tasksExecuted.fetch_add(1, std::memory_order_relaxed);
                });
                std::this_thread::yield();
            }
        });
    }

    std::thread timerClient([&loop, &stopClients] {
        while (!stopClients.load(std::memory_order_relaxed)) {
            TimerId id = loop->setTimer(std::chrono::milliseconds(5), false, [] {});
            std::this_thread::yield();
            loop->cancelTimer(id);
        }
    });

    // Let it run under load for 100ms
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Shutdown the worker loop while clients are actively hammering it
    loop->stopAndWait();

    stopClients.store(true, std::memory_order_relaxed);
    for (auto &t : clients) {
        t.join();
    }
    timerClient.join();

    // Destructor of WorkerLoop
    loop.reset();
    REQUIRE(tasksExecuted.load() > 0);
}

TEST_CASE("ThreadPool - post and stopAndWait", "[platform]")
{
    using namespace Styx::Platform;
    ThreadPool pool(4, "TestPool");

    std::atomic<int> completed{0};
    constexpr int kTasks = 100;
    for (int i = 0; i < kTasks; ++i) {
        pool.post([&completed] {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            completed.fetch_add(1);
        });
    }

    auto start = std::chrono::steady_clock::now();
    while (completed.load() < kTasks && (std::chrono::steady_clock::now() - start) < std::chrono::seconds(3)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(completed.load() == kTasks);
    pool.stopAndWait();
}
