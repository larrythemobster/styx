# Qt-free capture backends ported from ouwou/Acheron video-streams commit 71b4907:
# screens and windows, cameras, and the sound a program plays.
# Include after styx_platform and FFmpeg discovery.
add_library(styx_video_capture STATIC
    src/Core/Video/CameraCapture.cpp
    src/Core/Video/ScreenCapture.cpp
    src/Core/Video/CardScaler.cpp
    src/Core/Video/BroadcastPipeline.cpp
    src/Core/Video/CameraPipeline.cpp
    src/Core/Video/CameraSender.cpp
    src/Core/Media/RgbScaler.cpp
    src/Core/Video/X11Capture.cpp
    src/Core/Video/PortalCapture.cpp
    src/Core/Audio/LoopbackCapture.cpp
)
target_include_directories(styx_video_capture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(styx_video_capture PUBLIC cxx_std_20)

option(STYX_HARDWARE_TESTS "Enable capture tests that access physical devices" OFF)
target_link_libraries(styx_video_capture PUBLIC styx_platform styx_video_codecs styx_video_adaptation)

target_link_libraries(styx_video_capture PUBLIC ffmpeg)
target_compile_definitions(styx_video_capture PRIVATE STYX_HAVE_AVDEVICE=1)

find_package(PkgConfig QUIET)

if(WIN32)
    target_link_libraries(styx_video_capture PRIVATE d3d11 dxgi gdi32 user32 ole32)
elseif(UNIX AND NOT APPLE)
    if(PkgConfig_FOUND)
        pkg_check_modules(X11 QUIET IMPORTED_TARGET x11 xcb xcb-composite xcb-randr)
        pkg_check_modules(PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3 libspa-0.2)
        pkg_check_modules(SDBUS QUIET IMPORTED_TARGET sdbus-c++)
    endif()
    if(X11_FOUND)
        target_link_libraries(styx_video_capture PRIVATE PkgConfig::X11)
        target_compile_definitions(styx_video_capture PRIVATE STYX_HAVE_X11=1)
    endif()
    if(PIPEWIRE_FOUND)
        target_link_libraries(styx_video_capture PRIVATE PkgConfig::PIPEWIRE)
        target_compile_definitions(styx_video_capture PRIVATE STYX_HAVE_PIPEWIRE=1)
    endif()
    # Portal capture needs its DBus client as well as PipeWire.
    if(PIPEWIRE_FOUND AND SDBUS_FOUND)
        target_link_libraries(styx_video_capture PRIVATE PkgConfig::SDBUS)
        target_compile_definitions(styx_video_capture PRIVATE STYX_HAVE_SDBUS=1)
    endif()
endif()

if(BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    add_executable(TestVideoCapture tests/tst_VideoCapture.cpp)
    target_link_libraries(TestVideoCapture PRIVATE styx_video_capture Catch2::Catch2WithMain)
    target_include_directories(TestVideoCapture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME VideoCaptureTests COMMAND TestVideoCapture)

    add_executable(TestLoopbackCapture tests/tst_LoopbackCapture.cpp)
    target_link_libraries(TestLoopbackCapture PRIVATE styx_video_capture Catch2::Catch2WithMain)
    target_include_directories(TestLoopbackCapture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/vendor/miniaudio")
    add_test(NAME LoopbackCaptureTests COMMAND TestLoopbackCapture)

    add_executable(TestCameraCapture tests/tst_CameraCapture.cpp)
    target_link_libraries(TestCameraCapture PRIVATE styx_video_capture Catch2::Catch2WithMain)
    target_include_directories(TestCameraCapture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    add_test(NAME CameraCaptureTests COMMAND TestCameraCapture)
endif()
