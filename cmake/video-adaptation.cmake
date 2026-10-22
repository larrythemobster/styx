# Qt-free video adaptation algorithms ported from ouwou/Acheron's video-streams branch.
# Include after styx_platform and Catch2 discovery in the root build.
add_library(styx_video_adaptation STATIC
    src/Core/Video/SendRateController.cpp
    src/Core/Video/SendQuality.cpp
    src/Core/Video/TileVisibility.cpp
    src/Core/Media/DecodeDemand.cpp
    src/Discord/Voice/SinkWants.cpp
)
target_include_directories(styx_video_adaptation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(styx_video_adaptation PUBLIC styx_platform)
target_compile_features(styx_video_adaptation PUBLIC cxx_std_20)

if(BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    add_executable(TestVideoAdaptation tests/tst_VideoAdaptation.cpp)
    target_link_libraries(TestVideoAdaptation PRIVATE styx_video_adaptation Catch2::Catch2WithMain)
    target_include_directories(TestVideoAdaptation PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(TestVideoAdaptation PRIVATE /utf-8)
    endif()
    add_test(NAME VideoAdaptationTests COMMAND TestVideoAdaptation)
endif()
