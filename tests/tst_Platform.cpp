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
#include "Platform/Base64.hpp"
#include "Platform/Notifications.hpp"
#include "Platform/Badge.hpp"
#include "HardwareTests.hpp"
#if defined(__linux__)
#include <gio/gio.h>
#include <condition_variable>
#include <mutex>
#include <string>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <thread>
#include <vector>
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
        sig.publish(5, "hello");

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

        sig.publish(1);
        REQUIRE(countA == 1);
        REQUIRE(countB == 1);

        sig.disconnect(&ownerA);
        sig.publish(1);
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
            sig.publish(1);
            REQUIRE(count == 1);
        } // c destructs here

        sig.publish(1);
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

        sig.publish(1);

        // c1 should have run (10), c2 disconnected before its turn so not run, c3 should have run (1000)
        std::vector<int> expected = { 10, 1000 };
        REQUIRE(received == expected);
    }

    SECTION("Queued connection via Dispatcher")
    {
        Signal<int> sig;
        int result = 0;

        Connection c = sig.connect([&](int val) { result = val; }, ConnectionType::Queued);

        sig.publish(42);
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

TEST_CASE("PNG round trips preserve RGBA channels and transparency", "[image]")
{
    ImageBuffer source{2, 1, 4, {255, 0, 0, 128, 0, 0, 255, 255}};
    const auto png = Image::encodePng(source);
    const auto decoded = Image::decode(reinterpret_cast<const uint8_t *>(png.data()), png.size());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->width == 2);
    REQUIRE(decoded->height == 1);
    REQUIRE(decoded->pixels == source.pixels);
}

TEST_CASE("SVG decoding returns straight RGBA rather than premultiplied BGRA", "[image]")
{
    constexpr std::string_view svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="2" height="1"><rect width="2" height="1" fill="red" opacity="0.5"/></svg>)";
    const auto decoded = Image::decode(reinterpret_cast<const uint8_t *>(svg.data()), svg.size());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->width == 2);
    REQUIRE(decoded->height == 1);
    for (size_t pixel = 0; pixel < decoded->pixels.size(); pixel += 4) {
        REQUIRE(decoded->pixels[pixel] == 255);
        REQUIRE(decoded->pixels[pixel + 1] == 0);
        REQUIRE(decoded->pixels[pixel + 2] == 0);
        REQUIRE(decoded->pixels[pixel + 3] >= 127);
        REQUIRE(decoded->pixels[pixel + 3] <= 128);
    }
}

TEST_CASE("AVIF decoding produces opaque RGBA pixels", "[image]")
{
    const auto bytes = Styx::Platform::fromBase64("AAAAIGZ0eXBhdmlmAAAAAGF2aWZtaWYxbWlhZk1BMUEAAADrbWV0YQAAAAAAAAAhaGRscgAAAAAAAAAAcGljdAAAAAAAAAAAAAAAAAAAAAAOcGl0bQAAAAAAAQAAAB5pbG9jAAAAAEQAAAEAAQAAAAEAAAETAAAAHQAAAChpaW5mAAAAAAABAAAAGmluZmUCAAAAAAEAAGF2MDFDb2xvcgAAAABqaXBycAAAAEtpcGNvAAAAFGlzcGUAAAAAAAAAAgAAAAEAAAAQcGl4aQAAAAADCAgIAAAADGF2MUOBIAAAAAAAE2NvbHJuY2x4AAEADQAGgAAAABdpcG1hAAAAAAAAAAEAAQQBAoMEAAAAJW1kYXQSAAoEOAAmCTITEAAARmetHKM0brY+f3X7lQ2p0A==");
    REQUIRE(bytes.has_value());
    const auto decoded = Image::decode(bytes->data(), bytes->size());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->width == 2);
    REQUIRE(decoded->height == 1);
    for (size_t pixel = 0; pixel < decoded->pixels.size(); pixel += 4) {
        REQUIRE(decoded->pixels[pixel] >= 250);
        REQUIRE(decoded->pixels[pixel + 1] <= 3);
        REQUIRE(decoded->pixels[pixel + 2] <= 3);
        REQUIRE(decoded->pixels[pixel + 3] == 255);
    }
}

TEST_CASE("Animated GIF buffers outlive their input and preserve frame colors and delays", "[image]")
{
    auto frames = [] {
        const auto bytes = Styx::Platform::fromBase64("R0lGODlhAgABAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQABAAAACwAAAAAAgABAAAIBQABAAgIACH5BAEIAAEALAAAAAACAAEAgQAA/wAAAAAAAAAAAAgFAAEACAgAOw==");
        REQUIRE(bytes.has_value());
        return Image::decodeGif(bytes->data(), bytes->size());
    }();
    REQUIRE(frames.has_value());
    REQUIRE((frames->size == Styx::Platform::Size{2, 1}));
    REQUIRE(frames->count == 2);
    REQUIRE(frames->delays[0] == 40);
    REQUIRE(frames->delays[1] == 80);
    const std::vector<uint8_t> pixels(frames->pixels.get(), frames->pixels.get() + 16);
    REQUIRE((pixels == std::vector<uint8_t>{255, 0, 0, 255, 255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255}));
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

#ifdef _WIN32
TEST_CASE("WorkerLoop - COM MTA init hook", "[platform]")
{
    using namespace Styx::Platform;
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
}
#endif

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

TEST_CASE("Chat previews keep complete UTF8 characters and collapse Unicode whitespace", "[utf8]")
{
    using namespace Styx::Platform;
    REQUIRE(truncateUtf8("a😀é", 2) == "a😀");
    REQUIRE(truncateUtf8("a😀é", 0).empty());
    REQUIRE(simplified("\u00a0  Hello\r\n\tworld \u2003") == "Hello world");
    REQUIRE(caseFold("Straße Σςσ Ä") == "strasse σσσ ä");
}

TEST_CASE("Unicode case conversion includes supplementary-plane characters", "[utf8]")
{
    const std::string upper = "\u00c9\u039b\u0416\U00010400";
    const std::string lower = "\u00e9\u03bb\u0436\U00010428";
    REQUIRE(Styx::Platform::unicodeLower(upper) == lower);
    REQUIRE(Styx::Platform::unicodeUpper(lower) == upper);
}

TEST_CASE("Calendar day differences follow local dates rather than elapsed hours", "[platform]")
{
    using Styx::Platform::DateTime;
    const auto beforeMidnight = DateTime::parse("2024-04-10T23:59:59");
    const auto afterMidnight = DateTime::parse("2024-04-11T00:00:01");
    REQUIRE(beforeMidnight.secondsUntil(afterMidnight) == 2);
    REQUIRE(beforeMidnight.daysUntil(afterMidnight) == 1);
    REQUIRE(afterMidnight.daysUntil(beforeMidnight) == -1);
    REQUIRE(afterMidnight.daysUntil(afterMidnight.addSeconds(60)) == 0);
}

TEST_CASE("Notifications: toast XML escapes text and drops control characters", "[notifications]")
{
    using namespace Styx::Platform::Notifications;

    Notification simple;
    simple.title = "Tom & Jerry";
    simple.body = "<b>hi</b> \"x\" 'y'";
    REQUIRE(toastXml(simple) == "<toast><visual><binding template=\"ToastGeneric\"><text>Tom &amp; Jerry</text><text>&lt;b&gt;hi&lt;/b&gt; &quot;x&quot; &#39;y&#39;</text></binding></visual></toast>");

    Notification controlChars;
    controlChars.body = std::string("a") + "\x01" + "b" + "\x1F" + "c" + "\n" + "d" + "\t" + "e";
    REQUIRE(toastXml(controlChars).find("<text>abc\nd\te</text>") != std::string::npos);

    Notification nonAscii;
    nonAscii.body = "héllo 👋";
    REQUIRE(toastXml(nonAscii).find("<text>héllo 👋</text>") != std::string::npos);

    Notification withImage;
    withImage.image = std::filesystem::temp_directory_path() / "styx avatar.png";
    const auto imageXml = toastXml(withImage);
    REQUIRE(imageXml.find("<image placement=\"appLogoOverride\" hint-crop=\"circle\" src=\"file:///") != std::string::npos);
    REQUIRE(imageXml.find("styx%20avatar.png\"/>") != std::string::npos);
}

TEST_CASE("Notifications: a toast reaches the desktop", "[notifications]")
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    using namespace Styx::Platform::Notifications;
    if (!available())
        SKIP("desktop notifications are unavailable or turned off for Styx");

    Notification notification;
    notification.title = "Styx test";
    notification.body = "Desktop notifications work.";
    REQUIRE(show(notification, [] {}));
}

TEST_CASE("Notification marks have a white centre and red ring inside a circular edge", "[badge]")
{
    for (const int size : {8, 11, 14, 16, 24, 32, 48}) {
        const auto pixels = Styx::Platform::Badge::render(size);
        CHECK(pixels[size_t(size / 2) * size + size / 2] == 0xFFFFFFFFu);
        const auto ring = pixels[size + size / 2];
        CHECK((ring >> 24) == 255);
        CHECK(((ring >> 16) & 255) > ((ring >> 8) & 255));
        CHECK(((ring >> 16) & 255) > (ring & 255));
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const auto alpha = pixels[size_t(y) * size + x] >> 24;
                if (std::hypot(x + 0.5 - size / 2.0, y + 0.5 - size / 2.0) >= size / 2.0 + 0.5)
                    CHECK(alpha == 0);
                CHECK(alpha == (pixels[size_t(y) * size + size - x - 1] >> 24));
                CHECK(alpha == (pixels[size_t(size - y - 1) * size + x] >> 24));
            }
        }
    }
}

#if defined(__linux__)

namespace {

const char *const NOTIFICATIONS_XML =
    "<node><interface name='org.freedesktop.Notifications'>"
    "<method name='GetCapabilities'><arg type='as' direction='out'/></method>"
    "<method name='Notify'>"
    "<arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='as' direction='in'/>"
    "<arg type='a{sv}' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='u' direction='out'/></method>"
    "<signal name='ActionInvoked'><arg type='u'/><arg type='s'/></signal>"
    "<signal name='NotificationClosed'><arg type='u'/><arg type='u'/></signal>"
    "</interface></node>";

class FakeNotificationDaemon
{
public:
    static constexpr std::uint32_t ID = 42;

    std::mutex mutex;
    std::string appName;
    std::string appIcon;
    std::string summary;
    std::string body;
    std::string desktopEntry;
    std::string imagePath;
    std::vector<std::string> actions;
    int notifyCalls = 0;

    ~FakeNotificationDaemon()
    {
        // A quit from this thread is lost if the loop hasn't started running yet, so it is
        // queued on the daemon's own context instead.
        if (owned)
            g_main_context_invoke(context, [](gpointer quitting) {
                g_main_loop_quit(static_cast<GMainLoop *>(quitting));
                return G_SOURCE_REMOVE;
            }, loop);
        if (thread.joinable())
            thread.join();
        if (registration)
            g_dbus_connection_unregister_object(connection, registration);
        if (connection) {
            g_dbus_connection_close_sync(connection, nullptr, nullptr);
            g_object_unref(connection);
        }
        if (node)
            g_dbus_node_info_unref(node);
        if (loop)
            g_main_loop_unref(loop);
        if (context)
            g_main_context_unref(context);
    }

    bool start()
    {
        context = g_main_context_new();
        loop = g_main_loop_new(context, FALSE);

        thread = std::thread([this] {
            g_main_context_push_thread_default(context);

            gchar *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
            if (address) {
                connection = g_dbus_connection_new_for_address_sync(
                    address,
                    static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                    nullptr,
                    nullptr,
                    nullptr);
                g_free(address);
            }

            if (connection) {
                node = g_dbus_node_info_new_for_xml(NOTIFICATIONS_XML, nullptr);
                static const GDBusInterfaceVTable vtable{ &FakeNotificationDaemon::onMethodCall, nullptr, nullptr, {} };
                registration = g_dbus_connection_register_object(
                    connection,
                    "/org/freedesktop/Notifications",
                    node->interfaces[0],
                    &vtable,
                    this,
                    nullptr,
                    nullptr);

                GVariant *reply = g_dbus_connection_call_sync(
                    connection,
                    "org.freedesktop.DBus",
                    "/org/freedesktop/DBus",
                    "org.freedesktop.DBus",
                    "RequestName",
                    g_variant_new("(su)", "org.freedesktop.Notifications", 4u),
                    G_VARIANT_TYPE("(u)"),
                    G_DBUS_CALL_FLAGS_NONE,
                    -1,
                    nullptr,
                    nullptr);
                if (reply) {
                    guint32 result = 0;
                    g_variant_get(reply, "(u)", &result);
                    g_variant_unref(reply);
                    owned = (result == 1);
                }
            }

            {
                std::lock_guard lock(readyMutex);
                ready = true;
            }
            readyChanged.notify_all();

            if (owned)
                g_main_loop_run(loop);

            g_main_context_pop_thread_default(context);
        });

        std::unique_lock lock(readyMutex);
        readyChanged.wait(lock, [this] { return ready; });
        return owned;
    }

    void emitActionInvoked(const char *action)
    {
        g_dbus_connection_emit_signal(
            connection,
            nullptr,
            "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications",
            "ActionInvoked",
            g_variant_new("(us)", ID, action),
            nullptr);
        g_dbus_connection_flush_sync(connection, nullptr, nullptr);
    }

private:
    static void onMethodCall(
        GDBusConnection *,
        const gchar *,
        const gchar *,
        const gchar *,
        const gchar *method,
        GVariant *parameters,
        GDBusMethodInvocation *invocation,
        gpointer data)
    {
        if (g_strcmp0(method, "GetCapabilities") == 0) {
            GVariantBuilder builder;
            g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
            g_variant_builder_add(&builder, "s", "body");
            g_variant_builder_add(&builder, "s", "body-markup");
            g_variant_builder_add(&builder, "s", "actions");
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(as)", &builder));
            return;
        }

        if (g_strcmp0(method, "Notify") == 0) {
            auto *self = static_cast<FakeNotificationDaemon *>(data);
            const char *name = nullptr, *icon = nullptr, *title = nullptr, *text = nullptr;
            guint32 replaces = 0;
            GVariantIter *actionIter = nullptr;
            GVariant *hints = nullptr;
            gint32 timeout = 0;
            g_variant_get(parameters, "(&su&s&s&sas@a{sv}i)", &name, &replaces, &icon, &title, &text, &actionIter, &hints, &timeout);

            {
                std::lock_guard lock(self->mutex);
                self->appName = name;
                self->appIcon = icon;
                self->summary = title;
                self->body = text;

                const char *action = nullptr;
                while (g_variant_iter_loop(actionIter, "&s", &action))
                    self->actions.emplace_back(action);

                const char *value = nullptr;
                if (g_variant_lookup(hints, "desktop-entry", "&s", &value))
                    self->desktopEntry = value;
                value = nullptr;
                if (g_variant_lookup(hints, "image-path", "&s", &value))
                    self->imagePath = value;

                ++self->notifyCalls;
            }

            g_variant_iter_free(actionIter);
            g_variant_unref(hints);
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", ID));
            return;
        }

        g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.UnknownMethod", method);
    }

    GMainContext *context = nullptr;
    GMainLoop *loop = nullptr;
    GDBusConnection *connection = nullptr;
    GDBusNodeInfo *node = nullptr;
    guint registration = 0;
    std::thread thread;
    std::mutex readyMutex;
    std::condition_variable readyChanged;
    bool ready = false;
    bool owned = false;
};

struct LauncherListener
{
    GMainContext *context = nullptr;
    GDBusConnection *connection = nullptr;
    guint subscription = 0;
    std::string uri;
    std::int64_t count = -1;
    bool visible = true;
    int updates = 0;

    LauncherListener()
    {
        context = g_main_context_new();
        g_main_context_push_thread_default(context);

        gchar *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
        if (address) {
            connection = g_dbus_connection_new_for_address_sync(
                address,
                static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                nullptr,
                nullptr,
                nullptr);
            g_free(address);
        }

        if (connection) {
            subscription = g_dbus_connection_signal_subscribe(
                connection,
                nullptr,
                "com.canonical.Unity.LauncherEntry",
                "Update",
                nullptr,
                nullptr,
                G_DBUS_SIGNAL_FLAGS_NONE,
                &LauncherListener::onSignal,
                this,
                nullptr);
            GVariant *id = g_dbus_connection_call_sync(
                connection,
                "org.freedesktop.DBus",
                "/org/freedesktop/DBus",
                "org.freedesktop.DBus",
                "GetId",
                nullptr,
                nullptr,
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                nullptr,
                nullptr);
            if (id)
                g_variant_unref(id);
        }
    }

    ~LauncherListener()
    {
        if (subscription)
            g_dbus_connection_signal_unsubscribe(connection, subscription);
        if (connection) {
            g_dbus_connection_close_sync(connection, nullptr, nullptr);
            g_object_unref(connection);
        }
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
    }

    static void onSignal(GDBusConnection *,
                         const gchar *,
                         const gchar *,
                         const gchar *,
                         const gchar *,
                         GVariant *parameters,
                         gpointer data)
    {
        if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sa{sv})")))
            return;

        auto *self = static_cast<LauncherListener *>(data);
        const char *name = nullptr;
        GVariant *props = nullptr;
        g_variant_get(parameters, "(&s@a{sv})", &name, &props);
        self->uri = name;
        gint64 value = -1;
        g_variant_lookup(props, "count", "x", &value);
        self->count = value;
        gboolean shown = TRUE;
        g_variant_lookup(props, "count-visible", "b", &shown);
        self->visible = shown != FALSE;
        ++self->updates;
        g_variant_unref(props);
    }

    bool waitFor(int wanted)
    {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (updates < wanted && std::chrono::steady_clock::now() < deadline) {
            g_main_context_iteration(context, FALSE);
            std::this_thread::sleep_for(10ms);
        }
        return updates >= wanted;
    }
};

} // namespace

TEST_CASE("Notifications: D-Bus notifications reach the daemon and report clicks", "[notifications]")
{
    if (!g_getenv("DBUS_SESSION_BUS_ADDRESS"))
        SKIP("no D-Bus session bus");

    FakeNotificationDaemon daemon;
    if (!daemon.start())
        SKIP("another notification daemon owns the name");

    using namespace Styx::Platform::Notifications;
    REQUIRE(available());

    Notification notification;
    notification.title = "Tom & Jerry";
    notification.body = "<b>hi</b> & bye";
    notification.image = std::filesystem::path("/tmp/styx avatar.png");

    std::atomic<bool> clicked{false};
    REQUIRE(show(notification, [&clicked] { clicked = true; }));

    {
        std::lock_guard lock(daemon.mutex);
        REQUIRE(daemon.notifyCalls == 1);
        REQUIRE(daemon.appName == "Styx");
        REQUIRE(daemon.appIcon == "io.github.larrythemobster.styx");
        REQUIRE(daemon.summary == "Tom & Jerry");
        REQUIRE(daemon.body == "&lt;b&gt;hi&lt;/b&gt; &amp; bye");
        REQUIRE(daemon.actions == std::vector<std::string>{"default", "Open"});
        REQUIRE(daemon.desktopEntry == "io.github.larrythemobster.styx");
        REQUIRE(daemon.imagePath == "/tmp/styx avatar.png");
    }

    daemon.emitActionInvoked("default");

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!clicked && std::chrono::steady_clock::now() < deadline) {
        Dispatcher::main().runPending();
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(clicked);
}

TEST_CASE("Badge counts reach the launcher over D-Bus", "[badge]")
{
    if (!g_getenv("DBUS_SESSION_BUS_ADDRESS"))
        SKIP("no D-Bus session bus");

    LauncherListener listener;
    REQUIRE(listener.connection);

    Styx::Platform::Badge::setCount(nullptr, 3);
    REQUIRE(listener.waitFor(1));
    REQUIRE(listener.uri == "application://io.github.larrythemobster.styx.desktop");
    REQUIRE(listener.count == 3);
    REQUIRE(listener.visible);

    Styx::Platform::Badge::setCount(nullptr, 0);
    REQUIRE(listener.waitFor(2));
    REQUIRE(listener.count == 0);
    REQUIRE_FALSE(listener.visible);
}
#endif
