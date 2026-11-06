#include "Core/Media/ClipDecoder.hpp"
#include "Core/Media/ClipPlayer.hpp"
#include "Platform/Dispatcher.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace Styx::Core::Media;
using namespace Styx::Platform;
using namespace std::chrono_literals;
namespace {
constexpr auto GifSixFramesTenthSecondEach =
        "R0lGODlhCAAIAPf/MQAA/gD+AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0A"
        "AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0A"
        "AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0A"
        "AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0A"
        "AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0A"
        "AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0A"
        "AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0A"
        "AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0A"
        "AP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAP0AAAD/ACH/C05FVFNDQVBFMi4wAwEAAAAh+QQECgAfACwA"
        "AAAACAAIAAAIDwAFCBxIsKDBgwgTKkwYEAAh+QQFCgD/ACwHAAcAAQABAAAIBAD/BQQAIfkEBQoA/wAsAAAAAAgACAAACA8AAwgcSLCgwYMIEypMGBAAIfkE"
        "BQoA/wAsBwAHAAEAAQAACAQA/wUEACH5BAUKAP8ALAAAAAAIAAgAAAgPAAEIHEiwoMGDCBMqTBgQACH5BAUKAP8ALAcABwABAAEAAAgEAP8FBAA7";

constexpr auto WebpThreeFramesFifthSecondEach =
        "UklGRrQAAABXRUJQVlA4WAoAAAACAAAABwAABwAAQU5JTQYAAAD/////AABBTk1GKAAAAAAAAAAAAAcAAAcAAMgAAAJWUDhMDwAAAC8HwAEABxD1j/4HIqL/"
        "AQBBTk1GKAAAAAAAAAAAAAcAAAcAAMgAAABWUDhMDwAAAC8HwAEAB9D/yP4HIqL/AQBBTk1GKAAAAAAAAAAAAAcAAAcAAMgAAABWUDhMDwAAAC8HwAEABxDR"
        "//4HIqL/AQA=";

#ifdef STYX_HAVE_FFMPEG
constexpr auto Mp4SixFramesTenthSecondEach =
        "AAAAIGZ0eXBpc29tAAACAGlzb21pc28yYXZjMW1wNDEAAAMxbW9vdgAAAGxtdmhkAAAAAAAAAAAAAAAAAAAD6AAAAlgAAQAAAQAAAAAAAAAAAAAAAAEAAAAA"
        "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAAlt0cmFrAAAAXHRraGQAAAADAAAAAAAAAAAAAAAB"
        "AAAAAAAAAlgAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAACAAAAAgAAAAAAAkZWR0cwAAABxlbHN0AAAAAAAA"
        "AAEAAAJYAAAAAAABAAAAAAHTbWRpYQAAACBtZGhkAAAAAAAAAAAAAAAAAAAoAAAAGABVxAAAAAAALWhkbHIAAAAAAAAAAHZpZGUAAAAAAAAAAAAAAABWaWRl"
        "b0hhbmRsZXIAAAABfm1pbmYAAAAUdm1oZAAAAAEAAAAAAAAAAAAAACRkaW5mAAAAHGRyZWYAAAAAAAAAAQAAAAx1cmwgAAAAAQAAAT5zdGJsAAAArnN0c2QA"
        "AAAAAAAAAQAAAJ5hdmMxAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAACAAIABIAAAASAAAAAAAAAABGUxhdmM2Mi4yOC4xMDIgbGlib3BlbmgyNjQAAAAAAAAA"
        "GP//AAAAJGF2Y0MBQsAU/+EADWdCwBSMaJTAQHhEI1ABAARozjyAAAAAEHBhc3AAAAABAAAAAQAAABRidHJ0AAAAAAAABiUAAAYlAAAAGHN0dHMAAAAAAAAA"
        "AQAAAAYAAAQAAAAAFHN0c3MAAAAAAAAAAQAAAAEAAAAcc3RzYwAAAAAAAAABAAAAAQAAAAYAAAABAAAALHN0c3oAAAAAAAAAAAAAAAYAAAAbAAAACwAAAB0A"
        "AAALAAAAHQAAAAsAAAAUc3RjbwAAAAAAAAABAAADYQAAAGJ1ZHRhAAAAWm1ldGEAAAAAAAAAIWhkbHIAAAAAAAAAAG1kaXJhcHBsAAAAAAAAAAAAAAAALWls"
        "c3QAAAAlqXRvbwAAAB1kYXRhAAAAAQAAAABMYXZmNjIuMTIuMTAyAAAACGZyZWUAAAB+bWRhdAAAABdluAAECeIxQABCbjgACBXHAAEL6TqSvAAAAAdh4AB+"
        "QJ5YAAAAGWHgAL5A/xuKAAIIEcAAQtY4AAh44nzyZ/AAAAAHYeAA/kBPlgAAABlh4AE+QGfG4oAAmbxwABIEjgACBvifPJn8AAAAB2HgAX5Ad5Y=";
#endif


std::string base64(const char *text) {
    const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; unsigned value=0; int bits=0;
    for (;*text && *text!='=';++text) { auto pos=alphabet.find(*text); if(pos==std::string::npos) continue; value=(value<<6)|unsigned(pos); bits+=6; if(bits>=8) { bits-=8; out.push_back(char(value>>bits)); } }
    return out;
}
struct Fixtures {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / ("styx-clip-tests-" + randomUuid());
    std::string gif, webp;
#ifdef STYX_HAVE_FFMPEG
    std::string mp4;
#endif
    Fixtures() {
        REQUIRE(std::filesystem::create_directories(dir));
        gif=fixture("six.gif",base64(GifSixFramesTenthSecondEach));
        webp=fixture("three.webp",base64(WebpThreeFramesFifthSecondEach));
#ifdef STYX_HAVE_FFMPEG
        mp4=fixture("six.mp4",base64(Mp4SixFramesTenthSecondEach));
#endif
    }
    ~Fixtures() { std::error_code ec; std::filesystem::remove_all(dir,ec); }
    std::string fixture(const char *name,const std::string &bytes) { auto path=dir/name; std::ofstream file(path,std::ios::binary); REQUIRE(file.is_open()); file.write(bytes.data(),std::streamsize(bytes.size())); REQUIRE(file.good()); return path.string(); }
    std::string stillWebp() { auto bytes=base64(WebpThreeFramesFifthSecondEach); auto offset=bytes.find("VP8L"); REQUIRE(offset!=std::string::npos); unsigned len=static_cast<unsigned char>(bytes[offset+4]) | (unsigned(static_cast<unsigned char>(bytes[offset+5]))<<8) | (unsigned(static_cast<unsigned char>(bytes[offset+6]))<<16) | (unsigned(static_cast<unsigned char>(bytes[offset+7]))<<24); auto chunk=bytes.substr(offset,8+len+(len&1)); unsigned size=unsigned(chunk.size()+4); std::string result="RIFF"; for(int i=0;i<4;++i) result.push_back(char(size>>(i*8))); return fixture("still.webp",result+"WEBP"+chunk); }
};
struct DecodedFrame { ImageBuffer image; int64_t startMs; };
std::vector<DecodedFrame> decodeLoop(ClipDecoder &decoder, Size size) { std::vector<DecodedFrame> frames; ImageBuffer image; int64_t start=0; while(decoder.nextFrameOfLoop(image,start,size)) frames.push_back({image,start}); return frames; }
std::vector<int64_t> startsOf(const std::vector<DecodedFrame> &frames) { std::vector<int64_t> starts; for(const auto &frame:frames) starts.push_back(frame.startMs); return starts; }
enum class Dominant { Red, Green, Blue, None };
Dominant dominant(const ImageBuffer &image) { if(image.empty()) return Dominant::None; auto p=image.pixels.data()+(image.height/2*image.width+image.width/2)*4; if(p[0]>180&&p[1]<80&&p[2]<80) return Dominant::Red; if(p[1]>180&&p[0]<80&&p[2]<80) return Dominant::Green; if(p[2]>180&&p[0]<80&&p[1]<80) return Dominant::Blue; return Dominant::None; }
template<class F> bool waitUntil(F predicate, std::chrono::milliseconds timeout) { auto end=std::chrono::steady_clock::now()+timeout; do { Dispatcher::main().runPending(); if(predicate()) return true; std::this_thread::sleep_for(2ms); } while(std::chrono::steady_clock::now()<end); Dispatcher::main().runPending(); return predicate(); }
void pump(std::chrono::milliseconds duration) { waitUntil([] { return false; },duration); }
} // namespace

TEST_CASE("GIF frames, timing and rewinds", "[clips]") {
    Fixtures f; auto decoder=ClipDecoder::open(f.gif); REQUIRE(decoder); REQUIRE((decoder->nativeSize()==Size{8,8}));
    for(int loop=0;loop<4;++loop) { if(loop) REQUIRE(decoder->rewind()); auto frames=decodeLoop(*decoder,{8,8}); REQUIRE((startsOf(frames)==std::vector<int64_t>{0,100,200,300,400,500})); REQUIRE(decoder->finishedLoopMs()==600); REQUIRE(dominant(frames[0].image)==Dominant::Red); REQUIRE(dominant(frames[1].image)==Dominant::Red); REQUIRE(dominant(frames[2].image)==Dominant::Green); REQUIRE(dominant(frames[3].image)==Dominant::Green); REQUIRE(dominant(frames[4].image)==Dominant::Blue); REQUIRE(dominant(frames[5].image)==Dominant::Blue); REQUIRE(frames[0].image.channels==4); }
}
#ifdef STYX_HAVE_FFMPEG
TEST_CASE("MP4 frames, timing, scaling and rewinds", "[clips]") {
    Fixtures f; auto decoder=ClipDecoder::open(f.mp4); REQUIRE(decoder); REQUIRE((decoder->nativeSize()==Size{32,32}));
    for(int loop=0;loop<4;++loop) { if(loop) REQUIRE(decoder->rewind()); auto frames=decodeLoop(*decoder,{32,32}); REQUIRE((startsOf(frames)==std::vector<int64_t>{0,100,200,300,400,500})); REQUIRE(decoder->finishedLoopMs()==600); REQUIRE(dominant(frames[0].image)==Dominant::Red); REQUIRE(dominant(frames[2].image)==Dominant::Green); REQUIRE(dominant(frames[5].image)==Dominant::Blue); REQUIRE(frames[0].image.channels==4); }
    REQUIRE(decoder->rewind()); auto frames=decodeLoop(*decoder,{16,12}); REQUIRE(frames.size()==6); REQUIRE(frames[0].image.width==16); REQUIRE(frames[0].image.height==12); REQUIRE(dominant(frames[0].image)==Dominant::Red); REQUIRE(dominant(frames[5].image)==Dominant::Blue);
}
#endif
TEST_CASE("Animated WebP frames and timing", "[clips]") {
    Fixtures f; auto decoder=ClipDecoder::open(f.webp); REQUIRE(decoder); REQUIRE((decoder->nativeSize()==Size{8,8}));
    for(int loop=0;loop<2;++loop) { auto frames=decodeLoop(*decoder,{8,8}); REQUIRE((startsOf(frames)==std::vector<int64_t>{0,200,400})); REQUIRE(decoder->finishedLoopMs()==600); REQUIRE(dominant(frames[0].image)==Dominant::Red); REQUIRE(dominant(frames[1].image)==Dominant::Green); REQUIRE(dominant(frames[2].image)==Dominant::Blue); REQUIRE(frames[0].image.channels==4); REQUIRE(decoder->rewind()); }
}
TEST_CASE("Files that are not clips do not open", "[clips]") {
    Fixtures f; REQUIRE_FALSE(ClipDecoder::open((f.dir/"missing.gif").string())); REQUIRE_FALSE(ClipDecoder::open(f.fixture("playlist.gif",base64("I0VYVE0zVQpodHRwOi8vZXhhbXBsZS5jb20vYS50cwo=")))); REQUIRE_FALSE(ClipDecoder::open(f.stillWebp())); auto truncated=ClipDecoder::open(f.fixture("truncated.gif",base64("R0lGODlhCAAIAPf/MQ=="))); REQUIRE((!truncated || decodeLoop(*truncated,{8,8}).empty()));
    const auto png = f.fixture("still.png", base64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/aS8AAAAASUVORK5CYII="));
    REQUIRE_FALSE(ClipDecoder::open(png));
}
TEST_CASE("Player remains blank until playing, paces frames and loops", "[clips]") {
    Fixtures f; ClipPlayer player; std::vector<ClipPlayer::ClipId> changed; std::vector<Dominant> seen; auto connection=player.frameChanged.connect([&](auto id){ changed.push_back(id); if(auto frame=player.frame(id)) seen.push_back(dominant(*frame)); }); auto id=player.open(f.gif,{8,8},ClipPlayer::Fit::Contain); pump(150ms); REQUIRE(changed.empty()); REQUIRE_FALSE(player.frame(id)); player.setPlaying(id,true); REQUIRE(waitUntil([&]{return !changed.empty();},2000ms)); REQUIRE(changed.front()==id); REQUIRE(dominant(*player.frame(id))==Dominant::Red); auto start=std::chrono::steady_clock::now(); REQUIRE(waitUntil([&]{return changed.size()>=9;},5000ms)); auto elapsed=std::chrono::steady_clock::now()-start; REQUIRE(elapsed>=650ms); REQUIRE(elapsed<2500ms); REQUIRE(std::find(seen.begin(),seen.end(),Dominant::Green)!=seen.end()); REQUIRE(std::find(seen.begin(),seen.end(),Dominant::Blue)!=seen.end()); REQUIRE(std::find(seen.begin(),seen.end(),Dominant::None)==seen.end());
}
#ifdef STYX_HAVE_FFMPEG
TEST_CASE("Player stops while paused and after close", "[clips]") {
    Fixtures f; ClipPlayer player; std::vector<ClipPlayer::ClipId> changed; auto connection=player.frameChanged.connect([&](auto id){changed.push_back(id);}); auto id=player.open(f.mp4,{32,32},ClipPlayer::Fit::Contain); player.setPlaying(id,true); REQUIRE(waitUntil([&]{return !changed.empty();},2000ms)); player.setPlaying(id,false); auto frozen=*player.frame(id); changed.clear(); pump(400ms); REQUIRE(changed.empty()); REQUIRE(player.frame(id)->pixels==frozen.pixels); player.setPlaying(id,true); REQUIRE(waitUntil([&]{return !changed.empty();},2000ms)); player.close(id); changed.clear(); pump(400ms); REQUIRE(changed.empty()); REQUIRE_FALSE(player.frame(id));
}
#endif
TEST_CASE("Player reports unreadable and single-frame clips", "[clips]") {
    Fixtures f; ClipPlayer player; std::vector<ClipPlayer::ClipId> failed; auto connection=player.clipFailed.connect([&](auto id){failed.push_back(id);}); auto missing=player.open((f.dir/"missing.gif").string(),{8,8},ClipPlayer::Fit::Contain); player.setPlaying(missing,true); REQUIRE(waitUntil([&]{return !failed.empty();},2000ms)); REQUIRE(failed[0]==missing); pump(200ms); REQUIRE(failed.size()==1); auto still=player.open(f.stillWebp(),{8,8},ClipPlayer::Fit::Contain); player.setPlaying(still,true); REQUIRE(waitUntil([&]{return failed.size()==2;},2000ms)); REQUIRE(failed[1]==still);
}
#ifdef STYX_HAVE_FFMPEG
TEST_CASE("Player never upscales", "[clips]") {
    Fixtures f; ClipPlayer player; auto roomy=player.open(f.mp4,{400,300},ClipPlayer::Fit::Contain); player.setPlaying(roomy,true); REQUIRE(waitUntil([&]{return player.frame(roomy);},2000ms)); REQUIRE(player.frame(roomy)->width==32); REQUIRE(player.frame(roomy)->height==32); auto fitted=player.open(f.mp4,{16,8},ClipPlayer::Fit::Contain); player.setPlaying(fitted,true); REQUIRE(waitUntil([&]{return player.frame(fitted);},2000ms)); REQUIRE(player.frame(fitted)->width==8); REQUIRE(player.frame(fitted)->height==8); auto covering=player.open(f.mp4,{16,8},ClipPlayer::Fit::Cover); player.setPlaying(covering,true); REQUIRE(waitUntil([&]{return player.frame(covering);},2000ms)); REQUIRE(player.frame(covering)->width==16); REQUIRE(player.frame(covering)->height==16);
}
TEST_CASE("Player enforces its memory limit", "[clips]") {
    Fixtures f; ClipPlayer player; std::vector<ClipPlayer::ClipId> failed; auto connection=player.clipFailed.connect([&](auto id){failed.push_back(id);}); auto needed=ClipPlayer::memoryToPlay({32,32},{32,32}); player.setMemoryLimitForNewClips(needed-1); auto refused=player.open(f.mp4,{400,300},ClipPlayer::Fit::Contain); player.setPlaying(refused,true); REQUIRE(waitUntil([&]{return !failed.empty();},2000ms)); REQUIRE(failed[0]==refused); REQUIRE_FALSE(player.frame(refused)); player.setMemoryLimitForNewClips(needed); auto allowed=player.open(f.mp4,{400,300},ClipPlayer::Fit::Contain); player.setPlaying(allowed,true); REQUIRE(waitUntil([&]{return player.frame(allowed);},2000ms)); REQUIRE(failed.size()==1);
}
#endif
