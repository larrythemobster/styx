# Qt-free FFmpeg encoder/decoder and RTP receiver ported from upstream/video-streams.
add_library(styx_video_codecs STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../src/Core/Media/H264Sps.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Core/Media/VideoEncoder.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Core/Media/VideoDecoder.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Discord/Voice/VideoReceiver.cpp
)
target_include_directories(styx_video_codecs PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../src)
target_compile_features(styx_video_codecs PUBLIC cxx_std_20)
target_link_libraries(styx_video_codecs PUBLIC ffmpeg styx_video_protocol styx_platform)

if(BUILD_TESTS)
    find_package(Catch2 3 CONFIG QUIET)
    if(Catch2_FOUND)
        add_executable(TestVideoCodecs ${CMAKE_CURRENT_LIST_DIR}/../tests/tst_VideoCodecs.cpp)
        target_link_libraries(TestVideoCodecs PRIVATE styx_video_codecs Catch2::Catch2WithMain)
        target_include_directories(TestVideoCodecs PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../src)
        add_test(NAME VideoCodecTests COMMAND TestVideoCodecs)
    endif()
endif()
