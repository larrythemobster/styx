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

        # DecodeTests: H264 SPS parsing/rewriting and full video receive/decode chain.
        add_executable(TestH264Sps ${CMAKE_CURRENT_LIST_DIR}/../tests/tst_H264Sps.cpp)
        target_link_libraries(TestH264Sps PRIVATE styx_video_codecs Catch2::Catch2WithMain ffmpeg)
        target_include_directories(TestH264Sps PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../src ${CMAKE_CURRENT_LIST_DIR}/../tests)
        add_test(NAME H264SpsTests COMMAND TestH264Sps)

        add_executable(TestVideoDecode ${CMAKE_CURRENT_LIST_DIR}/../tests/tst_VideoDecode.cpp)
        target_link_libraries(TestVideoDecode PRIVATE styx_video_codecs Catch2::Catch2WithMain ffmpeg)
        target_include_directories(TestVideoDecode PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../src ${CMAKE_CURRENT_LIST_DIR}/../tests)
        add_test(NAME VideoDecodeTests COMMAND TestVideoDecode)

        # EncodeTests: VideoEncoder, EncodedFrameFinisher, codec selection, screen/window capture.
        add_executable(TestVideoEncode ${CMAKE_CURRENT_LIST_DIR}/../tests/tst_VideoEncode.cpp)
        target_link_libraries(TestVideoEncode PRIVATE styx_video_codecs styx_video_capture Catch2::Catch2WithMain ffmpeg)
        if(WIN32)
            target_link_libraries(TestVideoEncode PRIVATE d3d11 dxgi gdi32 user32 ole32)
        endif()
        target_include_directories(TestVideoEncode PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../src ${CMAKE_CURRENT_LIST_DIR}/../tests)
        add_test(NAME VideoEncodeTests COMMAND TestVideoEncode)
    endif()
endif()
