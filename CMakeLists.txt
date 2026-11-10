cmake_minimum_required(VERSION 3.21)

option(USE_VCPKG "Use bundled vcpkg toolchain" ON)
option(ENABLE_LOTTIE "Build with rlottie for Lottie sticker playback" ON)
option(FFMPEG_FROM_VCPKG "Build ffmpeg from source through vcpkg and link it statically" OFF)
# Capture is Windows Graphics Capture + WASAPI, so the feature is Windows-only for now.
if(CMAKE_HOST_WIN32)
    set(_screenshare_default ON)
else()
    set(_screenshare_default OFF)
endif()
option(ENABLE_SCREENSHARE "Build Go Live screen sharing and stream viewing (needs voice and ffmpeg)" ${_screenshare_default})

# Must happen before project()
if(USE_VCPKG AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

if(ENABLE_LOTTIE)
    list(APPEND VCPKG_MANIFEST_FEATURES "lottie")
endif()
if(FFMPEG_FROM_VCPKG)
    list(APPEND VCPKG_MANIFEST_FEATURES "ffmpeg")
endif()
if(ENABLE_SCREENSHARE)
    list(APPEND VCPKG_MANIFEST_FEATURES "screenshare")
endif()

project(styx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build test targets" ON)
option(ENABLE_VOICE "Build with voice/audio support" ON)
option(ENABLE_RNNOISE "Build with RNNoise noise suppression (requires ENABLE_VOICE)" ON)
option(ENABLE_RNNOISE_RTCD "Use RNNoise x86 runtime SIMD dispatch (SSE4.1/AVX2)" ON)
option(ENABLE_FFMPEG "Build with ffmpeg/libav video playback support" ON)
option(ALLOW_PLAIN_CURL "Permit building against plain libcurl without curl-impersonate" OFF)
if(BUILD_TESTS)
    enable_testing()
endif()

find_package(CURL REQUIRED)
if(NOT TARGET CURL::libcurl AND TARGET CURL::libcurl-impersonate)
    add_library(CURL::libcurl INTERFACE IMPORTED)
    set_target_properties(CURL::libcurl PROPERTIES
        INTERFACE_LINK_LIBRARIES CURL::libcurl-impersonate
        INTERFACE_INCLUDE_DIRECTORIES "${CURL_INCLUDE_DIRS}")
endif()
find_package(OpenSSL REQUIRED)
if(ENABLE_VOICE)
    if(USE_VCPKG)
        find_package(unofficial-sodium CONFIG REQUIRED)
        find_package(Opus CONFIG REQUIRED)

        set(SODIUM_TARGET unofficial-sodium::sodium)
        set(OPUS_TARGET Opus::opus)
    else()
        find_package(PkgConfig REQUIRED)

        pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
        pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)

        set(SODIUM_TARGET PkgConfig::SODIUM)
        set(OPUS_TARGET PkgConfig::OPUS)
    endif()
endif()

find_package(RmlUi CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(lunasvg CONFIG REQUIRED)
find_package(libavif CONFIG REQUIRED)
find_package(utf8proc CONFIG QUIET)
if(NOT TARGET utf8proc)
    find_path(UTF8PROC_INCLUDE_DIR NAMES utf8proc.h REQUIRED)
    find_library(UTF8PROC_LIBRARY_RELEASE NAMES utf8proc_static utf8proc REQUIRED)
    find_library(UTF8PROC_LIBRARY_DEBUG NAMES utf8proc_static utf8proc
        HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
    include(SelectLibraryConfigurations)
    select_library_configurations(UTF8PROC)
    add_library(utf8proc INTERFACE IMPORTED)
    target_include_directories(utf8proc INTERFACE "${UTF8PROC_INCLUDE_DIR}")
    target_link_libraries(utf8proc INTERFACE ${UTF8PROC_LIBRARIES})
    if(UTF8PROC_LIBRARY_RELEASE MATCHES "utf8proc_static")
        target_compile_definitions(utf8proc INTERFACE UTF8PROC_STATIC)
    endif()
endif()
find_package(PCRE2 CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Freetype REQUIRED)
find_package(harfbuzz CONFIG REQUIRED)
if(BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
endif()


if(ENABLE_VOICE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

    find_package(MLSPP QUIET)
    if(NOT TARGET MLSPP::mlspp)
        message(STATUS "mlspp not found, using subproject")
        set(DISABLE_GREASE ON CACHE BOOL "" FORCE)
        set(TESTING OFF CACHE BOOL "" FORCE)
        set(MLS_CXX_NAMESPACE "mlspp" CACHE STRING "" FORCE)
        set(CMAKE_EXPORT_PACKAGE_REGISTRY OFF CACHE BOOL "" FORCE)
        add_subdirectory(vendor/mlspp EXCLUDE_FROM_ALL)
        if(NOT TARGET MLSPP::mlspp)
            add_library(MLSPP::mlspp ALIAS mlspp)
        endif()
        if(NOT TARGET MLSPP::hpke)
            add_library(MLSPP::hpke ALIAS hpke)
        endif()
        set(MLSPP_FOUND TRUE CACHE BOOL "" FORCE)
        # Write a shim config so libdave's find_dependency(MLSPP) finds this
        # instead of the broken build-tree config that add_subdirectory generates
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mlspp-shim/MLSPPConfig.cmake"
            "set(MLSPP_FOUND TRUE)\n")
        set(MLSPP_DIR "${CMAKE_CURRENT_BINARY_DIR}/mlspp-shim" CACHE PATH "" FORCE)
    else()
        message(STATUS "Found system mlspp")
    endif()

    # Prevent find_package from finding mlspp's broken build-tree config via registry
    set(CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY ON)
    # libdave (Discord Audio/Video E2E Encryption)
    set(TESTING OFF CACHE BOOL "" FORCE)
    set(PERSISTENT_KEYS OFF CACHE BOOL "" FORCE)
    # Prevent libdave's own vcpkg manifest from being processed; our
    # project-level vcpkg.json already provides all required packages.
    set(VCPKG_MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "" FORCE)
    add_subdirectory(vendor/libdave/cpp EXCLUDE_FROM_ALL)
endif()

# find or build rnnoise
include(cmake/rnnoise.cmake)

include(cmake/ffmpeg.cmake)

if(ENABLE_SCREENSHARE AND NOT (WIN32 AND ENABLE_VOICE AND TARGET ffmpeg))
    message(STATUS "Screen sharing needs Windows, ENABLE_VOICE and ffmpeg; building without it")
    set(ENABLE_SCREENSHARE OFF)
endif()
if(ENABLE_SCREENSHARE)
    find_package(LibDataChannel CONFIG REQUIRED)
endif()

include(cmake/lottie.cmake)

set(PROJECT_SOURCES
    src/main.cpp
    src/Core/Migration.cpp
    src/Core/Snowflake.cpp
    src/Core/OrderedJson.cpp
    src/Core/ProxyConfig.cpp
    src/Core/ClientInstance.cpp
    src/Core/Calls/CallStore.cpp
    src/Core/Session.cpp
    src/Core/Logging.cpp
    src/Core/MessageManager.cpp
    src/Core/EphemeralMessages.cpp
    src/Core/CommandIndexCache.cpp
    src/Core/MessageReactors.cpp
    src/Core/MessageSegments.cpp
    src/Core/ForumManager.cpp
    src/Core/ImageManager.cpp
    src/Core/AnimatedImageCache.cpp
    src/Core/ApngDecoder.cpp
    src/Core/LottieDecoder.cpp
    src/Core/UserManager.cpp
    src/Core/TypingTracker.cpp
    src/Core/PermissionManager.cpp
    src/Core/PermissionComputer.cpp
    src/Core/RoleHierarchy.cpp
    src/Core/ReadStateManager.cpp
    src/Core/NotificationPolicy.cpp
    src/Core/NotificationText.cpp
    src/Core/SearchSyntax.cpp
    src/Core/SearchQuery.cpp
    src/Core/SearchCompletion.cpp
    src/Core/QuickSwitcher.cpp
    src/Core/Audio/Sounds.cpp
    src/Core/Audio/SoundPlayer.cpp
    src/Core/Audio/SoundMixer.cpp
    src/Core/MemberListManager.cpp
    src/Core/RelationshipManager.cpp
    src/Core/Presence/PresenceManager.cpp
    src/Core/Presence/ActivityFormat.cpp
    src/Core/MurmurHash3.cpp
    src/Core/EmojiSegmenter.cpp
    src/Core/Emoji/EmojiManager.cpp
    src/Core/Gifs/FavoriteGifRules.cpp
    src/Core/Gifs/FavoriteGifs.cpp
    src/Core/Emoji/FrecencyTracker.cpp
    src/Core/Emoji/UnicodeEmojiIndex.cpp
    src/Core/Markdown/Parser.cpp
    src/Core/Theme/Tokens.cpp
    src/Core/Theme/Manager.cpp
    src/Core/Theme/Icons.cpp
    src/Core/Theme/Generator.cpp
    src/Core/Theme/Stylesheet.cpp
    src/Core/Theme/Fonts.cpp
    src/Core/Media/Player.cpp
    src/Core/Media/MediaDecoder.cpp
    src/Core/Media/AudioOutput.cpp
    src/Core/Media/PlayerPool.cpp
    src/Core/Media/ClipDecoder.cpp
    src/Core/Media/ClipPlayer.cpp
    src/Core/Media/CurlAvio.cpp
    src/UI/BrowserCaptchaResolver.cpp
    src/UI/TrayIcon.cpp
    src/UI/CallSounds.cpp
    src/UI/PushToTalk.cpp
    src/UI/WindowsPttHook.cpp
    src/UI/X11PttHook.cpp
    src/UI/Rml/AuditLogFormatter.cpp
    src/UI/Rml/GuildSettingsController.cpp
    src/UI/Rml/GuildSettingsSection.cpp
    src/UI/Rml/ImageUpload.cpp
    src/UI/Rml/MemberModeration.cpp
    src/UI/Accounts/AccountsModel.cpp
    src/UI/ChannelList/ChannelTreeModel.cpp
    src/UI/ChannelList/ChannelFilterProxyModel.cpp
    src/UI/ChannelList/Navigation.cpp
    src/UI/Chat/BlockedRuns.cpp
    src/UI/Chat/InteractionTracker.cpp
    src/UI/Chat/SelectDraft.cpp
    src/UI/Chat/SelectCandidates.cpp
    src/UI/Chat/SlashCommands.cpp
    src/UI/ChannelList/ServerRailModel.cpp
    src/UI/Chat/ChatModel.cpp
    src/UI/Chat/GifPlayback.cpp
    src/UI/MemberList/MemberListModel.cpp
    src/UI/Gifs/GifMasonryLayout.cpp
    src/UI/Rml/EmojiGridModel.cpp
    src/UI/Rml/ExpressionPickerController.cpp
    src/Storage/DatabaseManager.cpp
    src/Storage/BaseRepository.cpp
    src/Storage/Transaction.cpp
    src/Storage/AccountRepository.cpp
    src/Storage/GuildRepository.cpp
    src/Storage/ChannelRepository.cpp
    src/Storage/MessageRepository.cpp
    src/Storage/UserRepository.cpp
    src/Storage/MemberRepository.cpp
    src/Storage/RoleRepository.cpp
    src/Discord/Client.cpp
    src/Discord/ClientMessages.cpp
    src/Discord/ClientInteractions.cpp
    src/Discord/ClientPolls.cpp
    src/Discord/PollProtocol.cpp
    src/Discord/ComponentInteraction.cpp
    src/Discord/CommandInteraction.cpp
    src/Discord/ApplicationCommands.cpp
    src/Discord/MessageSearch.cpp
    src/Discord/InviteLink.cpp
    src/Discord/GuildCreate.cpp
    src/Discord/ClientGuild.cpp
    src/Discord/ClientUploads.cpp
    src/Discord/Gateway.cpp
    src/Discord/RemoteAuthClient.cpp
    src/Discord/LoginClient.cpp
    src/Discord/IngestThread.cpp
    src/Discord/HttpClient.cpp
    src/Discord/ApiError.cpp
    src/Discord/RequestWorker.cpp
    src/Discord/CurlUtils.cpp
    src/Discord/CdnUrls.cpp
    src/Discord/StreamKey.cpp
    src/Discord/ClientIdentity.cpp
    src/Discord/ContextProperties.cpp
    src/Proto/ProtoReader.cpp
    src/Proto/ProtoWriter.cpp
    src/Proto/UserSettings.cpp
    src/Proto/PreloadedSettings.cpp
    src/Proto/FrecencySettings.cpp
    src/Core/TokenStore.cpp
    src/App.cpp
    src/UI/Rml/RmlRuntime.cpp
    src/UI/Rml/RmlView.cpp
    src/UI/Rml/Controller.cpp
    src/UI/Rml/ContextMenu.cpp
    src/UI/Rml/Shell.cpp
    src/UI/Rml/RailController.cpp
    src/UI/Rml/ChannelListController.cpp
    src/UI/Rml/UserPanelController.cpp
    src/UI/Rml/CustomStatusController.cpp
    src/UI/Rml/ChatController.cpp
    src/UI/Rml/SlashCommandPicker.cpp
    src/UI/Rml/CommandOptionsBar.cpp
    src/UI/Rml/ComponentRenderer.cpp
    src/UI/Rml/PollRenderer.cpp
    src/UI/Rml/MemberListController.cpp
    src/UI/Rml/FriendsController.cpp
    src/UI/Rml/SettingsController.cpp
    src/UI/Rml/ProfileEditorController.cpp
    src/UI/Rml/AccountsController.cpp
    src/UI/Rml/ConfirmModal.cpp
    src/UI/Rml/UserProfileController.cpp
    src/UI/Rml/ImageViewerController.cpp
    src/UI/Rml/ForumController.cpp
    src/UI/Rml/ThreadBrowserController.cpp
    src/UI/Rml/SearchController.cpp
    src/UI/Rml/SwitcherController.cpp
    src/UI/Rml/Shortcuts.cpp
    src/UI/Rml/Keybinds.cpp
    src/UI/Rml/ShortcutController.cpp
    src/UI/Rml/ShortcutsHelpController.cpp
    src/UI/Rml/JoinServerController.cpp
    src/UI/Rml/CreateServerController.cpp
    src/UI/Rml/ExpressionPickerController.cpp
    vendor/rmlui-backend/RmlUi_Renderer_GL3.cpp
)
if(WIN32)
    list(APPEND PROJECT_SOURCES resources/app/styx.rc)
endif()


if(ENABLE_VOICE)
    list(APPEND PROJECT_SOURCES
        src/UI/Rml/CallController.cpp
        src/Discord/Voice/VoiceGateway.cpp
        src/Discord/Voice/UdpTransport.cpp
        src/Discord/Voice/Socks5UdpAssociation.cpp
        src/Discord/Voice/VoiceClient.cpp
        src/Discord/Voice/RtpPacket.cpp
        src/Discord/Voice/VoiceEncryption.cpp
        src/Discord/Voice/DaveSession.cpp
        src/Core/Audio/VoiceManager.cpp
        src/Core/Audio/AudioPipeline.cpp
        src/Core/Audio/TransmitGate.cpp
        src/Core/Audio/IAudioBackend.cpp
        src/Core/Audio/MiniaudioAudioBackend.cpp
        src/Core/Audio/OpusEncoder.cpp
        src/Core/Audio/OpusDecoder.cpp
        src/Core/Audio/JitterBuffer.cpp
        src/Core/Audio/AudioMixer.cpp
        src/Core/Audio/NoiseSuppressor.cpp
    )
endif()

if(ENABLE_SCREENSHARE)
    list(APPEND PROJECT_SOURCES
        src/Discord/Stream/StreamTransport.cpp
        src/Discord/Stream/StreamConnection.cpp
        src/Discord/Stream/VideoReceiver.cpp
        src/Core/Stream/D3DDevice.cpp
        src/Core/Stream/ScreenCapture.cpp
        src/Core/Stream/FrameConverter.cpp
        src/Core/Stream/SpsVuiRewriter.cpp
        src/Core/Stream/VideoEncoder.cpp
        src/Core/Stream/LoopbackCapture.cpp
        src/Core/Stream/StreamSender.cpp
        src/Core/Stream/StreamDecoder.cpp
        src/Core/Stream/StreamManager.cpp
        src/UI/Rml/GoLiveController.cpp
    )
endif()

if(ENABLE_VOICE OR TARGET ffmpeg)
    list(APPEND PROJECT_SOURCES
        src/Core/Audio/MiniaudioImpl.cpp
        src/Core/Audio/AudioBackends.cpp
    )
endif()

find_package(unofficial-nayuki-qr-code-generator CONFIG QUIET)
if(TARGET unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator)
    message(STATUS "Using installed nayuki-qr-code-generator")
else()
    message(STATUS "nayuki-qr-code-generator not found, using vendored version")
    list(APPEND PROJECT_SOURCES vendor/qrcodegen/cpp/qrcodegen.cpp)
endif()

add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE vendor/emoji-segmenter)
if(TARGET unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator)
    target_link_libraries(${PROJECT_NAME} PRIVATE unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator)
else()
    target_include_directories(${PROJECT_NAME} PRIVATE vendor/qrcodegen/cpp)
endif()

if(WIN32)
    find_file(STYX_CURL_RUNTIME_DLL NAMES libcurl-impersonate.dll libcurl.dll
        HINTS "${CURL_INCLUDE_DIR}/../lib" "${CURL_INCLUDE_DIR}/../bin"
        NO_DEFAULT_PATH NO_CACHE)
endif()
function(styx_deploy_curl target)
    if(STYX_CURL_RUNTIME_DLL)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${STYX_CURL_RUNTIME_DLL}" "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Deploying curl runtime library" VERBATIM)
    endif()
endfunction()
styx_deploy_curl(${PROJECT_NAME})

if(BUILD_TESTS)
    function(styx_test target test_name)
        add_executable(${target} ${ARGN})
        target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(${target} PRIVATE styx_platform Catch2::Catch2WithMain)
        if(MSVC)
            target_compile_options(${target} PRIVATE /utf-8)
        endif()
        add_test(NAME ${test_name} COMMAND ${target})
        styx_deploy_curl(${target})
    endfunction()

    styx_test(TestMarkdown MarkdownTests tests/tst_Markdown.cpp src/Core/Markdown/Parser.cpp
        src/Core/EmojiSegmenter.cpp src/Core/Snowflake.cpp src/Discord/CdnUrls.cpp)
    target_include_directories(TestMarkdown PRIVATE vendor/emoji-segmenter)
    target_link_libraries(TestMarkdown PRIVATE PCRE2::8BIT utf8proc)
    styx_test(TestFrecency FrecencyTests tests/tst_Frecency.cpp src/Core/Emoji/FrecencyTracker.cpp)
    styx_test(TestNotificationPolicy NotificationPolicyTests tests/tst_NotificationPolicy.cpp src/Core/NotificationPolicy.cpp src/Core/NotificationText.cpp)
    styx_test(TestSearchSyntax SearchSyntaxTests tests/tst_SearchSyntax.cpp src/Core/SearchSyntax.cpp src/Core/Snowflake.cpp)
    styx_test(TestSearchQuery SearchQueryTests tests/tst_SearchQuery.cpp src/Core/SearchQuery.cpp src/Core/Snowflake.cpp)
    styx_test(TestSearchCompletion SearchCompletionTests tests/tst_SearchCompletion.cpp src/Core/SearchCompletion.cpp src/Core/Snowflake.cpp)
    styx_test(TestQuickSwitcher QuickSwitcherTests tests/tst_QuickSwitcher.cpp src/Core/QuickSwitcher.cpp src/Core/Snowflake.cpp)
    styx_test(TestShortcuts ShortcutsTests tests/tst_Shortcuts.cpp src/UI/Rml/Shortcuts.cpp)
    target_link_libraries(TestShortcuts PRIVATE RmlUi::RmlUi)
    styx_test(TestKeybinds KeybindsTests tests/tst_Keybinds.cpp src/UI/Rml/Keybinds.cpp src/UI/Rml/Shortcuts.cpp)
    target_link_libraries(TestKeybinds PRIVATE RmlUi::RmlUi)
    styx_test(TestNavigation NavigationTests tests/tst_Navigation.cpp src/UI/ChannelList/Navigation.cpp src/Core/Snowflake.cpp)
    styx_test(TestBlockedRuns BlockedRunsTests tests/tst_BlockedRuns.cpp src/UI/Chat/BlockedRuns.cpp)
    styx_test(TestInteractionTracker InteractionTrackerTests tests/tst_InteractionTracker.cpp src/UI/Chat/InteractionTracker.cpp src/Core/Snowflake.cpp)
    styx_test(TestSelectDraft SelectDraftTests tests/tst_SelectDraft.cpp src/UI/Chat/SelectDraft.cpp)
    styx_test(TestSelectCandidates SelectCandidatesTests tests/tst_SelectCandidates.cpp src/UI/Chat/SelectCandidates.cpp)
    styx_test(TestSlashCommands SlashCommandTests tests/tst_SlashCommands.cpp src/UI/Chat/SlashCommands.cpp src/Core/Snowflake.cpp)
    styx_test(TestInviteLink InviteLinkTests tests/tst_InviteLink.cpp src/Discord/InviteLink.cpp src/Core/Snowflake.cpp)
    styx_test(TestGuildCreate GuildCreateTests tests/tst_GuildCreate.cpp src/Discord/GuildCreate.cpp src/Core/OrderedJson.cpp src/Core/Snowflake.cpp)
    styx_test(TestUserEdits UserEditsTests tests/tst_UserEdits.cpp src/Core/OrderedJson.cpp src/Core/Snowflake.cpp)
    styx_test(TestTransmitGate TransmitGateTests tests/tst_TransmitGate.cpp src/Core/Audio/TransmitGate.cpp)
    styx_test(TestWindowsPttHook WindowsPttHookTests tests/tst_WindowsPttHook.cpp src/UI/WindowsPttHook.cpp)
    target_link_libraries(TestWindowsPttHook PRIVATE RmlUi::RmlUi)
    if(UNIX AND NOT APPLE)
        styx_test(TestX11PttHook X11PttHookTests tests/tst_X11PttHook.cpp src/UI/X11PttHook.cpp)
        target_link_libraries(TestX11PttHook PRIVATE RmlUi::RmlUi)
    endif()
    styx_test(TestSounds SoundsTests tests/tst_Sounds.cpp src/Core/Audio/Sounds.cpp src/Core/Audio/SoundMixer.cpp)
    styx_test(TestUnicodeEmoji UnicodeEmojiTests tests/tst_UnicodeEmoji.cpp
        src/Core/Emoji/UnicodeEmojiIndex.cpp src/Core/Logging.cpp)
    styx_test(TestOrderedJson OrderedJsonTests tests/tst_OrderedJson.cpp
        src/Core/OrderedJson.cpp src/Core/Snowflake.cpp src/Discord/ContextProperties.cpp)
    styx_test(TestCalls CallTests tests/tst_Calls.cpp src/Core/Calls/CallStore.cpp
        src/Core/Snowflake.cpp src/Core/OrderedJson.cpp)
    styx_test(TestVoicePayloads VoicePayloadTests tests/tst_VoicePayloads.cpp
        src/Core/OrderedJson.cpp src/Core/Snowflake.cpp)
    styx_test(TestProto ProtoTests tests/tst_Proto.cpp src/Proto/ProtoReader.cpp
        src/Proto/ProtoWriter.cpp src/Proto/FrecencySettings.cpp src/Proto/UserSettings.cpp
        src/Proto/PreloadedSettings.cpp
        src/Core/Snowflake.cpp)
    styx_test(TestPreloadedSettings PreloadedSettingsTests tests/tst_PreloadedSettings.cpp
        src/Proto/PreloadedSettings.cpp src/Proto/UserSettings.cpp
        src/Proto/ProtoReader.cpp src/Proto/ProtoWriter.cpp src/Core/Snowflake.cpp)
    styx_test(TestProxy ProxyTests tests/tst_Proxy.cpp src/Core/ProxyConfig.cpp
        src/Core/Logging.cpp src/Core/ImageManager.cpp src/Core/Snowflake.cpp
        src/Discord/Voice/Socks5UdpAssociation.cpp src/Discord/CdnUrls.cpp)
    styx_test(TestEmojiGrid EmojiGridTests tests/tst_EmojiGrid.cpp
        src/UI/Rml/EmojiGridModel.cpp src/Core/Snowflake.cpp)
    styx_test(TestCdnUrls CdnUrlTests tests/tst_CdnUrls.cpp src/Discord/CdnUrls.cpp src/Core/Snowflake.cpp)
    styx_test(TestRmlRender RmlRenderTests tests/tst_RmlRender.cpp tests/tst_MessageComponentRendering.cpp tests/tst_CommandOptionsBar.cpp
        tests/tst_PollRendering.cpp
        src/UI/Rml/RmlRuntime.cpp src/UI/Rml/RmlView.cpp src/UI/Rml/ContextMenu.cpp src/UI/Rml/ComponentRenderer.cpp src/UI/Chat/SelectDraft.cpp
        src/UI/Rml/CommandOptionsBar.cpp src/UI/Rml/PollRenderer.cpp
        vendor/rmlui-backend/RmlUi_Renderer_GL3.cpp
        src/Core/ImageManager.cpp src/Core/ProxyConfig.cpp src/Core/Snowflake.cpp
        src/Core/EmojiSegmenter.cpp src/Core/Logging.cpp src/Discord/CdnUrls.cpp)
    target_link_libraries(TestRmlRender PRIVATE RmlUi::RmlUi utf8proc SDL3::SDL3 OpenGL::GL Freetype::Freetype harfbuzz::harfbuzz)
    target_include_directories(TestRmlRender PRIVATE vendor/emoji-segmenter vendor/rmlui-backend)
    if(WIN32)
        target_link_libraries(TestRmlRender PRIVATE user32 gdi32)
    endif()
    styx_test(TestEmbeds EmbedTests tests/tst_Embeds.cpp src/Discord/CdnUrls.cpp src/Core/Snowflake.cpp)
    styx_test(TestMessageComponents MessageComponentTests tests/tst_MessageComponents.cpp tests/tst_Polls.cpp src/Core/Snowflake.cpp)
    styx_test(TestComponentInteractions ComponentInteractionTests
        tests/tst_ComponentInteraction.cpp tests/tst_InteractionEvents.cpp
        src/Discord/ComponentInteraction.cpp src/Core/Snowflake.cpp)
    styx_test(TestPollProtocol PollProtocolTests tests/tst_PollProtocol.cpp tests/tst_PollEvents.cpp
        src/Discord/PollProtocol.cpp src/Discord/ApiError.cpp src/Core/Snowflake.cpp)
    styx_test(TestCommandInteraction CommandInteractionTests tests/tst_CommandInteraction.cpp
        src/Discord/CommandInteraction.cpp src/Core/Snowflake.cpp)
    styx_test(TestApplicationCommands ApplicationCommandTests tests/tst_ApplicationCommands.cpp
        src/Discord/ApplicationCommands.cpp src/Core/Snowflake.cpp)
    styx_test(TestCommandIndexCache CommandIndexCacheTests tests/tst_CommandIndexCache.cpp
        src/Core/CommandIndexCache.cpp src/Core/Snowflake.cpp)
    styx_test(TestEphemeralMessages EphemeralMessageTests tests/tst_EphemeralMessages.cpp
        src/Core/EphemeralMessages.cpp src/Core/Snowflake.cpp)
    styx_test(TestMessageSearch MessageSearchTests tests/tst_MessageSearch.cpp src/Discord/MessageSearch.cpp src/Discord/CdnUrls.cpp src/Core/Snowflake.cpp)
    styx_test(TestMessageCache MessageCacheTests tests/tst_MessageCache.cpp
        src/Storage/DatabaseManager.cpp src/Storage/BaseRepository.cpp
        src/Storage/Transaction.cpp src/Storage/UserRepository.cpp
        src/Storage/MessageRepository.cpp src/Core/Snowflake.cpp src/Core/Logging.cpp)
    styx_test(TestFavoriteGifs FavoriteGifTests tests/tst_FavoriteGifs.cpp
        src/Core/Gifs/FavoriteGifRules.cpp src/UI/Gifs/GifMasonryLayout.cpp
        src/Discord/CdnUrls.cpp src/Core/Snowflake.cpp)
    styx_test(TestClips ClipTests tests/tst_Clips.cpp
        src/Core/Media/ClipDecoder.cpp src/Core/Media/ClipPlayer.cpp)
    target_link_libraries(TestClips PRIVATE WebP::webpdemux)
    if(TARGET ffmpeg)
        styx_test(TestApng ApngTests tests/tst_Apng.cpp src/Core/ApngDecoder.cpp)
        foreach(target TestClips TestApng)
            target_link_libraries(${target} PRIVATE ffmpeg)
            target_compile_definitions(${target} PRIVATE STYX_HAVE_FFMPEG)
        endforeach()
    endif()
endif()

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /Zi /bigobj)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/vendor/rmlui-backend)

set_target_properties(${PROJECT_NAME}
    PROPERTIES
        WIN32_EXECUTABLE TRUE
)

target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)
if(NOT ENABLE_VOICE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE STYX_NO_VOICE)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    styx_platform RmlUi::RmlUi OpenSSL::Crypto ZLIB::ZLIB PCRE2::8BIT utf8proc WebP::webpdemux)
target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3 OpenGL::GL Freetype::Freetype harfbuzz::harfbuzz)
if(ENABLE_VOICE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${SODIUM_TARGET}
        ${OPUS_TARGET}
        libdave
    )

    if(ENABLE_RNNOISE)
        target_link_libraries(${PROJECT_NAME} PRIVATE rnnoise)
        target_compile_definitions(${PROJECT_NAME} PRIVATE STYX_HAVE_RNNOISE)
    endif()
endif()
if(ENABLE_VOICE OR TARGET ffmpeg)
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/miniaudio")
    target_compile_definitions(${PROJECT_NAME} PRIVATE STYX_HAVE_MINIAUDIO)
endif()
if(TARGET ffmpeg)
    target_link_libraries(${PROJECT_NAME} PRIVATE ffmpeg)
    target_compile_definitions(${PROJECT_NAME} PRIVATE STYX_HAVE_FFMPEG)
endif()
if(ENABLE_SCREENSHARE)
    target_link_libraries(${PROJECT_NAME} PRIVATE LibDataChannel::LibDataChannel d3d11 dxgi dwmapi windowsapp mmdevapi)
    target_compile_definitions(${PROJECT_NAME} PRIVATE STYX_HAVE_SCREENSHARE)
endif()
if(TARGET lottie)
    target_link_libraries(${PROJECT_NAME} PRIVATE lottie)
    target_compile_definitions(${PROJECT_NAME} PRIVATE STYX_HAVE_RLOTTIE)
endif()
# glad (bundled with RmlUi's GL3 renderer) must be the first OpenGL header in its TU
set_source_files_properties(vendor/rmlui-backend/RmlUi_Renderer_GL3.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

include(CheckCXXSymbolExists)
get_target_property(_curl_incs CURL::libcurl INTERFACE_INCLUDE_DIRECTORIES)
if(_curl_incs)
    set(CMAKE_REQUIRED_INCLUDES ${_curl_incs})
endif()
set(_curl_saved_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
check_cxx_symbol_exists(curl_easy_impersonate "curl/curl.h" HAS_CURL_IMPERSONATE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_curl_saved_target_type}")
unset(CMAKE_REQUIRED_INCLUDES)

if(HAS_CURL_IMPERSONATE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE IS_CURL_IMPERSONATE)
elseif(ALLOW_PLAIN_CURL)
    message(WARNING "curl_easy_impersonate not found, building against plain libcurl")
else()
    message(FATAL_ERROR
        "curl_easy_impersonate not found in the curl headers at:\n"
        "  ${_curl_incs}\n"
        "Point CURL_INCLUDE_DIR at curl-impersonate's headers, or pass "
        "-DALLOW_PLAIN_CURL=ON to build against plain libcurl anyway.")
endif()


if(FFMPEG_RUNTIME_DLLS)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FFMPEG_RUNTIME_DLLS}
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMENT "Copying ffmpeg runtime libraries to build directory"
    )
endif()

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(UNIX AND NOT APPLE)
    install(FILES resources/desktop/io.github.larrythemobster.styx.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )
    install(FILES resources/app/styx.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME io.github.larrythemobster.styx.svg
    )
endif()


# ---------------------------------------------------------------------------
# Qt-free Platform Foundation (styx_platform)
# ---------------------------------------------------------------------------


set(PLATFORM_SOURCES
    src/Platform/Base64.cpp
    src/Platform/Dispatcher.cpp
    src/Platform/Http.cpp
    src/Platform/Image.cpp
    src/Platform/Paths.cpp
    src/Platform/Settings.cpp
    src/Platform/Sqlite.cpp
    src/Platform/ThreadPool.cpp
    src/Platform/Utf8.cpp
    src/Platform/Native.cpp
    src/Platform/Desktop.cpp
    src/Platform/Notifications.cpp
    src/Platform/Badge.cpp
    src/Platform/WorkerLoop.cpp
)

if(WIN32)
    list(APPEND PLATFORM_SOURCES
        src/Platform/Paths_win.cpp
        src/Platform/SecureStore_win.cpp
        src/Platform/Settings_win.cpp
        src/Platform/Utf8_win.cpp
        src/Platform/Notifications_win.cpp
        src/Platform/Badge_win.cpp
    )
else()
    list(APPEND PLATFORM_SOURCES
        src/Platform/Paths_other.cpp
        src/Platform/Utf8_other.cpp
    )
    if(APPLE)
        list(APPEND PLATFORM_SOURCES src/Platform/SecureStore_apple.cpp src/Platform/Notifications_other.cpp src/Platform/Badge_other.cpp)
    else()
        list(APPEND PLATFORM_SOURCES src/Platform/SecureStore_secret.cpp src/Platform/Notifications_dbus.cpp src/Platform/Badge_dbus.cpp)
    endif()
endif()

add_library(styx_platform STATIC ${PLATFORM_SOURCES})
target_include_directories(styx_platform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${Stb_INCLUDE_DIR}
)
target_link_libraries(styx_platform
    PUBLIC
        nlohmann_json::nlohmann_json
        unofficial::sqlite3::sqlite3
        CURL::libcurl
        OpenSSL::Crypto
        lunasvg::lunasvg
        avif
        utf8proc
        WebP::webpdemux
)
if(TARGET WebP::webp)
    target_link_libraries(styx_platform PUBLIC WebP::webp)
endif()
if(WIN32)
    target_compile_definitions(styx_platform PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(styx_platform PUBLIC ws2_32 PRIVATE advapi32 crypt32 ole32 shell32 comdlg32 user32 windowsapp)
elseif(APPLE)
    find_library(STYX_SECURITY_FRAMEWORK Security REQUIRED)
    find_library(STYX_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(styx_platform PRIVATE SDL3::SDL3
        "${STYX_SECURITY_FRAMEWORK}" "${STYX_COREFOUNDATION_FRAMEWORK}")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(STYX_SECRET REQUIRED IMPORTED_TARGET libsecret-1)
    pkg_check_modules(STYX_GIO REQUIRED IMPORTED_TARGET gio-2.0)
    pkg_check_modules(STYX_XINPUT REQUIRED IMPORTED_TARGET x11 xi)
    set_source_files_properties(src/UI/X11PttHook.cpp PROPERTIES COMPILE_DEFINITIONS STYX_HAVE_X11_PTT)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::STYX_XINPUT)
    if(TARGET TestX11PttHook)
        target_compile_definitions(TestX11PttHook PRIVATE STYX_HAVE_X11_PTT)
        target_link_libraries(TestX11PttHook PRIVATE PkgConfig::STYX_XINPUT)
    endif()
    target_link_libraries(styx_platform PRIVATE SDL3::SDL3 PkgConfig::STYX_SECRET PkgConfig::STYX_GIO)
endif()
target_compile_features(styx_platform PUBLIC cxx_std_20)
target_compile_definitions(styx_platform PRIVATE STYX_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources")
add_custom_target(styx_resources ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/resources"
    COMMENT "Staging Styx resources")
add_dependencies(${PROJECT_NAME} styx_resources)
install(DIRECTORY resources/ DESTINATION ${CMAKE_INSTALL_BINDIR}/resources)
if(STYX_CURL_RUNTIME_DLL)
    install(FILES "${STYX_CURL_RUNTIME_DLL}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(FFMPEG_RUNTIME_DLLS)
    install(FILES ${FFMPEG_RUNTIME_DLLS} DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(WIN32 AND ENABLE_VOICE)
    foreach(_runtime_target IN ITEMS ${OPUS_TARGET} libdave)
        get_target_property(_runtime_type ${_runtime_target} TYPE)
        if(_runtime_type STREQUAL "SHARED_LIBRARY" OR _runtime_type STREQUAL "MODULE_LIBRARY")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:${_runtime_target}>" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
                COMMENT "Deploying ${_runtime_target} runtime library" VERBATIM)
            install(FILES "$<TARGET_FILE:${_runtime_target}>" DESTINATION ${CMAKE_INSTALL_BINDIR})
        endif()
    endforeach()
endif()
if(MSVC)
    target_compile_options(styx_platform PRIVATE /utf-8)
endif()

# ---------------------------------------------------------------------------
# Platform Foundation Unit Tests (TestPlatform)
# ---------------------------------------------------------------------------

find_package(Catch2 3 CONFIG QUIET)
if(BUILD_TESTS AND Catch2_FOUND)
    add_executable(TestPlatform tests/tst_Platform.cpp)
    target_link_libraries(TestPlatform PRIVATE styx_platform Catch2::Catch2WithMain)
    target_include_directories(TestPlatform PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(TestPlatform PRIVATE /utf-8)
    endif()
    if(TARGET PkgConfig::STYX_GIO)
        # The D-Bus notification and badge tests talk to the session bus with GDBus.
        target_link_libraries(TestPlatform PRIVATE PkgConfig::STYX_GIO)
    endif()
    add_test(NAME PlatformTests COMMAND TestPlatform)
    styx_deploy_curl(TestPlatform)
endif()

# Video engine ported from ouwou/acheron upstream/video-streams.
include(cmake/video-protocol.cmake)
include(cmake/video-adaptation.cmake)
if(TARGET ffmpeg)
    include(cmake/video-codecs.cmake)
    include(cmake/video-capture.cmake)
endif()
