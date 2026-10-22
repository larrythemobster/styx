# Qt-free RTP video payload and RTCP control protocol, ported from upstream/video-streams.
add_library(styx_video_protocol STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../src/Discord/Voice/VideoPacketizer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Discord/Voice/VideoDepacketizer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Discord/Voice/Rtcp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Discord/Voice/TransportFeedback.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Discord/Voice/SendSideEstimator.cpp
)
target_include_directories(styx_video_protocol PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../src)
target_compile_features(styx_video_protocol PUBLIC cxx_std_20)

# The RTP tests check packets against ffmpeg's own packetizers and run frames through DAVE.
if(BUILD_TESTS AND TARGET ffmpeg AND ENABLE_VOICE)
    find_package(Catch2 3 CONFIG QUIET)
    if(Catch2_FOUND)
        add_executable(TestVideoProtocol
            ${CMAKE_CURRENT_LIST_DIR}/../tests/tst_RtpVideo.cpp
            ${CMAKE_CURRENT_LIST_DIR}/../tests/tst_SendSideEstimator.cpp
        )
        target_link_libraries(TestVideoProtocol PRIVATE styx_video_codecs styx_video_protocol Catch2::Catch2WithMain libdave MLSPP::mlspp MLSPP::bytes)
        target_include_directories(TestVideoProtocol PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../src)
        add_test(NAME VideoProtocolTests COMMAND TestVideoProtocol)
    endif()
endif()
