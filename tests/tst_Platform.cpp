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

#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

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

TEST_CASE("Http: GET request and cancellation", "[http][!mayfail]")
{
    SECTION("Cancellation")
    {
        bool callbackFired = false;
        bool wasCancelled = false;

        HttpRequest req;
        req.url = "https://example.com";
        req.timeout = 5000ms;

        uint64_t reqId = HttpClient::instance().request(req, [&](const HttpResponse &resp) {
            callbackFired = true;
            wasCancelled = resp.cancelled;
        });

        // Cancel immediately
        HttpClient::instance().cancel(reqId);

        // Wait a short moment and pump dispatcher
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
