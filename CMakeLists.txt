cmake_minimum_required(VERSION 3.16)

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

include(cmake/qt.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTORCC ON)

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

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets Svg Test LinguistTools)
find_package(Qt${QT_VERSION_MAJOR}
    COMPONENTS
        Core
        Gui
        Widgets
        Svg
        Concurrent
        OpenGL
        OpenGLWidgets
        Test
        LinguistTools
)
# The main window is drawn with RmlUi (HTML/CSS-style UI) on an OpenGL 3.3 surface.
find_package(RmlUi CONFIG REQUIRED)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS ZlibPrivate QUIET)
qt_standard_project_setup(I18N_TRANSLATED_LANGUAGES en)

# If Qt::ZlibPrivate is not available, create a QtZlib compatibility include
# directory so that #include <QtZlib/zlib.h> resolves using system zlib.
if(NOT TARGET Qt::ZlibPrivate)
    find_package(ZLIB REQUIRED)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/qtzlib-compat/QtZlib")
    file(WRITE "${CMAKE_BINARY_DIR}/qtzlib-compat/QtZlib/zlib.h" "#include <zlib.h>\n")
    file(WRITE "${CMAKE_BINARY_DIR}/qtzlib-compat/QtZlib/zconf.h" "#include <zconf.h>\n")
    add_library(QtZlibCompat INTERFACE)
    target_include_directories(QtZlibCompat INTERFACE
        "${CMAKE_BINARY_DIR}/qtzlib-compat"
        ${ZLIB_INCLUDE_DIRS}
    )
    target_link_libraries(QtZlibCompat INTERFACE ZLIB::ZLIB)
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
# Windows Graphics Capture and process-loopback audio import WinRT/Windows 10 APIs, which
# keep the executable from even loading on Windows 7, the platform the Qt 5 build targets.
if(ENABLE_SCREENSHARE AND QT_VERSION_MAJOR LESS 6)
    message(STATUS "Screen sharing is not built with Qt 5 (Windows 7 target)")
    set(ENABLE_SCREENSHARE OFF)
endif()
if(ENABLE_SCREENSHARE)
    find_package(LibDataChannel CONFIG REQUIRED)
endif()

include(cmake/lottie.cmake)

set(PROJECT_SOURCES
    resources/resources.qrc
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
    src/UI/Rml/AuditLogFormatter.cpp
    src/UI/Rml/GuildSettingsController.cpp
    src/UI/Rml/GuildSettingsSection.cpp
    src/UI/Rml/ImageUpload.cpp
    src/UI/Rml/MemberModeration.cpp
    src/UI/Accounts/AccountsModel.cpp
    src/UI/ChannelList/ChannelTreeModel.cpp
    src/UI/ChannelList/ChannelFilterProxyModel.cpp
    src/UI/ChannelList/ServerRailModel.cpp
    src/UI/Chat/ChatModel.cpp
    src/UI/Chat/GifPlayback.cpp
    src/UI/Chat/GifPainting.cpp
    src/UI/Chat/EmojiTextObject.cpp
    src/UI/Chat/MessageTextDocument.cpp
    src/UI/Chat/MessageTextBrowser.cpp
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
    src/UI/Rml/ChatController.cpp
    src/UI/Rml/MemberListController.cpp
    src/UI/Rml/CallController.cpp
    src/UI/Rml/FriendsController.cpp
    src/UI/Rml/SettingsController.cpp
    src/UI/Rml/AccountsController.cpp
    src/UI/Rml/ConfirmModal.cpp
    src/UI/Rml/UserProfileController.cpp
    src/UI/Rml/ImageViewerController.cpp
    src/UI/Rml/ForumController.cpp
    src/UI/Rml/ThreadBrowserController.cpp
    src/UI/Rml/ExpressionPickerController.cpp
    vendor/rmlui-backend/RmlUi_Renderer_GL3.cpp
)
if(WIN32)
    list(APPEND PROJECT_SOURCES resources/app/styx.rc)
endif()


if(ENABLE_VOICE)
    list(APPEND PROJECT_SOURCES
        src/Discord/Voice/VoiceGateway.cpp
        src/Discord/Voice/UdpTransport.cpp
        src/Discord/Voice/Socks5UdpAssociation.cpp
        src/Discord/Voice/VoiceClient.cpp
        src/Discord/Voice/RtpPacket.cpp
        src/Discord/Voice/VoiceEncryption.cpp
        src/Discord/Voice/DaveSession.cpp
        src/Core/Audio/VoiceManager.cpp
        src/Core/Audio/AudioPipeline.cpp
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

qt_add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE vendor/emoji-segmenter)
if(TARGET unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator)
    target_link_libraries(${PROJECT_NAME} PRIVATE unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator)
else()
    target_include_directories(${PROJECT_NAME} PRIVATE vendor/qrcodegen/cpp)
endif()

if(BUILD_TESTS)
    qt_add_executable(TestMarkdown tests/tst_Markdown.cpp src/Core/Markdown/Parser.cpp
        src/Core/EmojiSegmenter.cpp src/Core/Snowflake.cpp)
    target_link_libraries(TestMarkdown PRIVATE Qt::Core Qt::Test)
    target_include_directories(TestMarkdown PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/vendor/emoji-segmenter)
    add_test(NAME MarkdownTests COMMAND TestMarkdown)

    qt_add_executable(TestFrecency tests/tst_Frecency.cpp src/Core/Emoji/FrecencyTracker.cpp)
    target_link_libraries(TestFrecency PRIVATE Qt::Core Catch2::Catch2WithMain)
    target_include_directories(TestFrecency PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME FrecencyTests COMMAND TestFrecency)

    qt_add_executable(TestUnicodeEmoji tests/tst_UnicodeEmoji.cpp
        src/Core/Emoji/UnicodeEmojiIndex.cpp src/Core/Logging.cpp resources/resources.qrc)
    target_link_libraries(TestUnicodeEmoji PRIVATE Qt::Core Qt::Test)
    target_include_directories(TestUnicodeEmoji PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME UnicodeEmojiTests COMMAND TestUnicodeEmoji)

    qt_add_executable(TestOrderedJson tests/tst_OrderedJson.cpp src/Core/OrderedJson.cpp
        src/Core/Snowflake.cpp src/Discord/ContextProperties.cpp)
    target_link_libraries(TestOrderedJson PRIVATE Qt::Core Qt::Test)
    target_include_directories(TestOrderedJson PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME OrderedJsonTests COMMAND TestOrderedJson)

    add_executable(TestCalls tests/tst_Calls.cpp src/Core/Calls/CallStore.cpp
        src/Core/Snowflake.cpp src/Core/OrderedJson.cpp)
    target_link_libraries(TestCalls PRIVATE Qt::Core Qt::Gui styx_platform Catch2::Catch2WithMain)
    target_include_directories(TestCalls PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(TestCalls PRIVATE /utf-8)
    endif()
    add_test(NAME CallTests COMMAND TestCalls)

    add_executable(TestVoicePayloads tests/tst_VoicePayloads.cpp
        src/Core/OrderedJson.cpp src/Core/Snowflake.cpp)
    target_link_libraries(TestVoicePayloads PRIVATE Qt::Core Catch2::Catch2WithMain)
    target_include_directories(TestVoicePayloads PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(TestVoicePayloads PRIVATE /utf-8)
    endif()
    add_test(NAME VoicePayloadTests COMMAND TestVoicePayloads)

    add_executable(TestProto tests/tst_Proto.cpp src/Proto/ProtoReader.cpp
        src/Proto/ProtoWriter.cpp src/Proto/FrecencySettings.cpp)
    target_link_libraries(TestProto PRIVATE styx_platform Catch2::Catch2WithMain)
    target_include_directories(TestProto PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ProtoTests COMMAND TestProto)

    qt_add_executable(TestProxy tests/tst_Proxy.cpp src/Core/ProxyConfig.cpp
        src/Core/Logging.cpp src/Core/ImageManager.cpp src/Core/Snowflake.cpp
        src/Discord/Voice/Socks5UdpAssociation.cpp)

    target_link_libraries(TestProxy PRIVATE Qt::Core Qt::Gui Qt::Widgets styx_platform Qt::Test)
    target_include_directories(TestProxy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ProxyTests COMMAND TestProxy)

    qt_add_executable(TestEmojiGrid tests/tst_EmojiGrid.cpp
        src/UI/Rml/EmojiGridModel.cpp src/Core/Snowflake.cpp)
    target_link_libraries(TestEmojiGrid PRIVATE Qt::Core Qt::Gui Qt::Test)
    target_include_directories(TestEmojiGrid PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME EmojiGridTests COMMAND TestEmojiGrid)

    qt_add_executable(TestCdnUrls tests/tst_CdnUrls.cpp src/Discord/CdnUrls.cpp src/Core/Snowflake.cpp)
    target_link_libraries(TestCdnUrls PRIVATE Qt::Core Qt::Test)
    target_include_directories(TestCdnUrls PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME CdnUrlTests COMMAND TestCdnUrls)

    qt_add_executable(TestRmlRender tests/tst_RmlRender.cpp resources/resources.qrc
        src/UI/Rml/RmlRuntime.cpp src/UI/Rml/RmlView.cpp src/UI/Rml/ContextMenu.cpp
        vendor/rmlui-backend/RmlUi_Renderer_GL3.cpp
        src/Core/ImageManager.cpp src/Core/ProxyConfig.cpp src/Core/Snowflake.cpp
        src/Core/EmojiSegmenter.cpp src/Core/Logging.cpp src/Discord/CdnUrls.cpp)
    target_link_libraries(TestRmlRender PRIVATE Qt::Core Qt::Gui Qt::Widgets styx_platform
        Qt::Svg Qt::OpenGL Qt::OpenGLWidgets Qt::Test RmlUi::RmlUi)
    target_include_directories(TestRmlRender PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/emoji-segmenter ${CMAKE_CURRENT_SOURCE_DIR}/vendor/rmlui-backend)
    add_test(NAME RmlRenderTests COMMAND TestRmlRender)

    qt_add_executable(TestEmbeds tests/tst_Embeds.cpp src/Discord/CdnUrls.cpp src/Core/Snowflake.cpp)
    target_link_libraries(TestEmbeds PRIVATE Qt::Core Qt::Gui Qt::Test)
    target_include_directories(TestEmbeds PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME EmbedTests COMMAND TestEmbeds)

    qt_add_executable(TestFavoriteGifs tests/tst_FavoriteGifs.cpp src/Core/Gifs/FavoriteGifRules.cpp src/UI/Gifs/GifMasonryLayout.cpp
        src/Discord/CdnUrls.cpp src/Core/Snowflake.cpp)
    target_link_libraries(TestFavoriteGifs PRIVATE Qt::Core Qt::Test)
    target_include_directories(TestFavoriteGifs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME FavoriteGifTests COMMAND TestFavoriteGifs)

    if(TARGET ffmpeg)
        qt_add_executable(TestClips tests/tst_Clips.cpp src/Core/Media/ClipDecoder.cpp src/Core/Media/ClipPlayer.cpp)
        target_link_libraries(TestClips PRIVATE Qt::Core Qt::Gui Qt::Test ffmpeg)
        target_compile_definitions(TestClips PRIVATE STYX_HAVE_FFMPEG)
        target_include_directories(TestClips PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        add_test(NAME ClipTests COMMAND TestClips)

        qt_add_executable(TestApng tests/tst_Apng.cpp src/Core/ApngDecoder.cpp)
        target_link_libraries(TestApng PRIVATE Qt::Core Qt::Gui Qt::Test ffmpeg)
        target_compile_definitions(TestApng PRIVATE STYX_HAVE_FFMPEG)
        target_include_directories(TestApng PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        add_test(NAME ApngTests COMMAND TestApng)
    endif()
endif()

qt_add_translations(${PROJECT_NAME} TS_FILES translations/styx_en.ts SOURCE_TARGETS ${PROJECT_NAME})

if (MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:/INCREMENTAL>)
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:/ZI> $<$<NOT:$<CONFIG:Debug>>:/Zi>)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)

    if(QT_VERSION_MAJOR LESS 6)
        target_compile_options(${PROJECT_NAME} PRIVATE
            "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/qt5_compat.h")
    endif()

    if(BUILD_TESTS)
        set(_msvc_test_targets TestMarkdown TestUnicodeEmoji TestOrderedJson TestFrecency TestProto TestProxy TestEmojiGrid TestCdnUrls TestEmbeds TestFavoriteGifs)
        if(TARGET TestApng)
            list(APPEND _msvc_test_targets TestApng TestClips)
        endif()
        foreach(test_target ${_msvc_test_targets})
            target_link_options(${test_target} PRIVATE $<$<CONFIG:Debug>:/INCREMENTAL>)
            target_compile_options(${test_target} PRIVATE $<$<CONFIG:Debug>:/ZI> $<$<NOT:$<CONFIG:Debug>>:/Zi>)
            target_compile_options(${test_target} PRIVATE /utf-8)
            if(QT_VERSION_MAJOR LESS 6)
                target_compile_options(${test_target} PRIVATE
                    "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/qt5_compat.h")
            endif()
        endforeach()
    endif()
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

target_link_libraries(${PROJECT_NAME}
    PUBLIC
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Svg
        Qt::Concurrent
        Qt::OpenGL
        Qt::OpenGLWidgets
        CURL::libcurl
    PRIVATE
        styx_platform
        RmlUi::RmlUi
        OpenSSL::Crypto
)
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
if(TARGET Qt::ZlibPrivate)
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt::ZlibPrivate)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE QtZlibCompat)
endif()
target_precompile_headers(${PROJECT_NAME} PUBLIC src/pch.h)
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

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/certs/cacert.pem")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/certs"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/certs/cacert.pem"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/certs/cacert.pem"
        COMMENT "Copying cacert.pem to build directory"
    )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/TwemojiCOLRv0.ttf")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/TwemojiCOLRv0.ttf"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/fonts/TwemojiCOLRv0.ttf"
        COMMENT "Copying TwemojiCOLRv0.ttf to build directory"
    )
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

find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(WebP CONFIG QUIET)

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
    src/Platform/WorkerLoop.cpp
)

if(WIN32)
    list(APPEND PLATFORM_SOURCES
        src/Platform/Paths_win.cpp
        src/Platform/SecureStore_win.cpp
        src/Platform/Settings_win.cpp
        src/Platform/Utf8_win.cpp
    )
else()
    list(APPEND PLATFORM_SOURCES
        src/Platform/Paths_other.cpp
        src/Platform/SecureStore_other.cpp
        src/Platform/Utf8_other.cpp
    )
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
)
if(TARGET WebP::webp)
    target_link_libraries(styx_platform PUBLIC WebP::webp)
endif()
if(WIN32)
    target_link_libraries(styx_platform PUBLIC ws2_32 PRIVATE advapi32 crypt32 ole32)
endif()
target_compile_features(styx_platform PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(styx_platform PRIVATE /utf-8)
endif()

# ---------------------------------------------------------------------------
# Platform Foundation Unit Tests (TestPlatform)
# ---------------------------------------------------------------------------

find_package(Catch2 3 CONFIG QUIET)
if(Catch2_FOUND)
    add_executable(TestPlatform tests/tst_Platform.cpp)
    target_link_libraries(TestPlatform PRIVATE styx_platform Catch2::Catch2WithMain)
    target_include_directories(TestPlatform PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(TestPlatform PRIVATE /utf-8)
    endif()
    add_test(NAME PlatformTests COMMAND TestPlatform)
endif()

# Video engine ported from ouwou/acheron upstream/video-streams.
include(cmake/video-protocol.cmake)
include(cmake/video-adaptation.cmake)
if(TARGET ffmpeg)
    include(cmake/video-codecs.cmake)
    include(cmake/video-capture.cmake)
endif()
