#include "App.hpp"
#include "UI/Rml/Shell.hpp"
#include "Storage/DatabaseManager.hpp"
#include "Core/Migration.hpp"
#include "Core/Session.hpp"
#include "Core/Logging.hpp"
#include "Core/Theme/Manager.hpp"
#include "Platform/Settings.hpp"

#include <curl/curl.h>
#include <SDL3/SDL.h>
#include <exception>
#include <string_view>
#include <SDL3/SDL_main.h>
#ifndef STYX_NO_VOICE
#include <dave/dave.h>
#include <dave/logger.h>
#endif

#ifndef STYX_NO_VOICE
static void DaveLogSink(discord::dave::LoggingSeverity severity, const char *file, int line, const std::string &message)
{
    using namespace Styx::Core;
    LogLevel level;
    switch (severity) {
    case discord::dave::LoggingSeverity::LS_ERROR: level = LogLevel::Error; break;
    case discord::dave::LoggingSeverity::LS_WARNING: level = LogLevel::Warning; break;
    case discord::dave::LoggingSeverity::LS_INFO: level = LogLevel::Info; break;
    case discord::dave::LoggingSeverity::LS_VERBOSE: level = LogLevel::Debug; break;
    default: return;
    }
    log(level, LogDave) << file << line << message;
}
#endif
int main(int argc, char *argv[])
{
    using namespace Styx;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
    int exitCode = 1;
    bool databaseReady = false;
    try {
        SDL_SetAppMetadata("Styx", nullptr, "io.github.larrythemobster.styx");
        App app(argc, argv);
        Core::Logger::init();
        Core::Migration::migrateSettingsAndData();
        Core::Migration::migrateKeychain();
        auto &theme = Core::Theme::Manager::instance();
        theme.load();
        theme.apply();
        theme.applyFonts();
#ifndef STYX_NO_VOICE
        discord::dave::SetLogSink(DaveLogSink);
#endif
        Core::log(Core::LogLevel::Info, LogCore) << "Starting Styx";
        if (!(databaseReady = Storage::DatabaseManager::instance().init())) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Fatal error", "Could not initialize the database. Styx will now close.", nullptr);
        } else {
            Core::Session session;
            UI::Shell window(&session);
            window.show();
            for (int i = 1; i < argc; ++i)
                if (std::string_view(argv[i]) == "--start-minimized") window.minimize();
            session.autoConnectAccounts();
            exitCode = app.exec();
        }
        if (databaseReady) { Storage::DatabaseManager::instance().shutdown(); databaseReady = false; }
        Platform::Settings::instance().save();
    } catch (const std::exception &error) {
        Core::log(Core::LogLevel::Error, LogCore) << "Fatal startup error:" << error.what();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Styx", error.what(), nullptr);
    }
    if (databaseReady) Storage::DatabaseManager::instance().shutdown();
#ifndef STYX_NO_VOICE
    discord::dave::SetLogSink(nullptr);
#endif
    curl_global_cleanup();
    Core::Logger::cleanup();
    return exitCode;
}
