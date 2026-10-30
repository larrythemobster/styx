#include <algorithm>
// APNG fixtures exercise region placement, blend, disposal and frame timing.
#include "Core/ApngDecoder.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace Styx::Core;
namespace {
using Color = std::array<uint8_t, 4>;
constexpr Color red{255,0,0,255}, blue{0,0,255,255}, green{0,255,0,255}, transparent{0,0,0,0};
struct Point { int x=0, y=0; };
Point point(int x, int y) { return {x,y}; }
Styx::Platform::Size size(int w, int h) { return {w,h}; }
struct Image {
    int width, height;
    std::vector<uint8_t> pixels;
    void setPixel(int x, int y, Color color) { std::copy(color.begin(), color.end(), pixels.begin() + (y*width+x)*4); }
};
Image solid(Styx::Platform::Size s, Color color) {
    Image image{s.width,s.height,std::vector<uint8_t>(s.width*s.height*4)};
    for(int y=0;y<s.height;++y) for(int x=0;x<s.width;++x) image.setPixel(x,y,color);
    return image;
}
enum DisposeOp : uint8_t { DisposeNone, DisposeBackground, DisposePrevious };
enum BlendOp : uint8_t { BlendSource, BlendOver };
struct FrameSpec { Image image; Point offset{}; uint16_t delayNumerator=1, delayDenominator=10; uint8_t disposeOp=DisposeNone, blendOp=BlendSource; };
std::string be32(uint32_t n) { std::string s; for(int i=3;i>=0;--i) s.push_back(char(n>>(i*8))); return s; }
std::string be16(uint16_t n) { return {char(n>>8),char(n)}; }
uint32_t crc32(const std::string &s) { uint32_t crc=0xffffffffu; for(unsigned char c:s) { crc^=c; for(int i=0;i<8;++i) crc=(crc&1)?0xedb88320u^(crc>>1):crc>>1; } return crc^0xffffffffu; }
std::string chunk(const std::string &type,const std::string &data) { return be32(uint32_t(data.size()))+type+data+be32(crc32(type+data)); }
// A zlib stream containing uncompressed DEFLATE blocks avoids an encoder dependency.
std::string imageData(const Image &image) {
    std::string raw;
    for(int y=0;y<image.height;++y) { raw.push_back(0); raw.append(reinterpret_cast<const char*>(image.pixels.data()+y*image.width*4),image.width*4); }
    std::string out("\x78\x01",2);
    for(size_t pos=0;pos<raw.size();) { const auto count=std::min<size_t>(65535,raw.size()-pos); out.push_back(pos+count==raw.size()?1:0); out.push_back(char(count)); out.push_back(char(count>>8)); auto inv=uint16_t(~count); out.push_back(char(inv)); out.push_back(char(inv>>8)); out.append(raw,pos,count); pos+=count; }
    uint32_t a=1,b=0; for(unsigned char c:raw) { a=(a+c)%65521; b=(b+a)%65521; } return out+be32((b<<16)|a);
}
std::string ihdr(const Image &image) { return be32(image.width)+be32(image.height)+std::string("\x08\x06\x00\x00\x00",5); }
std::string encodePng(const Image &image) { return std::string("\x89PNG\r\n\x1a\n",8)+chunk("IHDR",ihdr(image))+chunk("IDAT",imageData(image))+chunk("IEND",{}); }
std::string buildApng(const std::vector<FrameSpec> &frames) {
    std::string out=std::string("\x89PNG\r\n\x1a\n",8)+chunk("IHDR",ihdr(frames[0].image))+chunk("acTL",be32(uint32_t(frames.size()))+be32(0)); uint32_t seq=0;
    for(size_t i=0;i<frames.size();++i) { const auto &f=frames[i]; std::string ctl=be32(seq++)+be32(f.image.width)+be32(f.image.height)+be32(f.offset.x)+be32(f.offset.y)+be16(f.delayNumerator)+be16(f.delayDenominator); ctl.push_back(char(f.disposeOp)); ctl.push_back(char(f.blendOp)); out+=chunk("fcTL",ctl); const auto data=imageData(f.image); out+=i==0?chunk("IDAT",data):chunk("fdAT",be32(seq++)+data); }
    return out+chunk("IEND",{});
}
struct Canvas { Styx::Platform::ImageBuffer image; Color pixel(int x,int y) const { const auto p=image.pixels.data()+(y*image.width+x)*4; return {p[0],p[1],p[2],p[3]}; } };
struct DecodedFrame { Canvas canvas; int delayMs; };
std::vector<DecodedFrame> decodeAll(const std::string &apng) { std::vector<DecodedFrame> frames; auto reader=Apng::Reader::open(apng); if(!reader) return frames; Styx::Platform::ImageBuffer canvas; int delay=0; while(reader->next(canvas,delay)) frames.push_back({{canvas},delay}); return frames; }
} // namespace

TEST_CASE("APNG testPlainPngIsNotAnimated", "[apng]")
{
    REQUIRE(!Apng::Reader::open(encodePng(solid({ 4, 4 }, red))));
    REQUIRE(!Apng::Reader::open(std::string("{\"v\":\"5.5.2\"}")));
}

TEST_CASE("APNG testReportsCanvasAndFrameCount", "[apng]")
{
    const std::string apng = buildApng({ { solid({ 6, 4 }, red) }, { solid({ 6, 4 }, blue) }, { solid({ 6, 4 }, green) } });
    const auto reader = Apng::Reader::open(apng);
    REQUIRE(reader != nullptr);
    REQUIRE(reader->canvasSize() == size(6, 4));
    REQUIRE(reader->frameCount() == 3);
}

TEST_CASE("APNG testRegionIsPlacedAtOffset", "[apng]")
{
    FrameSpec patch{ solid({ 2, 2 }, blue), point(1, 2) };
    const auto frames = decodeAll(buildApng({ { solid({ 4, 4 }, red) }, patch }));

    REQUIRE(frames.size() == 2);
    REQUIRE(frames[0].canvas.pixel(1, 2) == red);
    REQUIRE(frames[1].canvas.pixel(1, 2) == blue);
    REQUIRE(frames[1].canvas.pixel(2, 3) == blue);
    REQUIRE(frames[1].canvas.pixel(0, 0) == red);
    REQUIRE(frames[1].canvas.pixel(3, 3) == red);
}

TEST_CASE("APNG testBlendOverKeepsWhatIsUnderneath", "[apng]")
{
    FrameSpec overlay{ solid({ 4, 4 }, transparent) };
    overlay.image.setPixel(0, 0, blue);
    overlay.blendOp = BlendOver;
    const auto frames = decodeAll(buildApng({ { solid({ 4, 4 }, red) }, overlay }));

    REQUIRE(frames.size() == 2);
    REQUIRE(frames[1].canvas.pixel(0, 0) == blue);
    REQUIRE(frames[1].canvas.pixel(1, 1) == red);
}

TEST_CASE("APNG testBlendSourceReplacesWhatIsUnderneath", "[apng]")
{
    FrameSpec overlay{ solid({ 4, 4 }, transparent) };
    overlay.image.setPixel(0, 0, blue);
    overlay.blendOp = BlendSource;
    const auto frames = decodeAll(buildApng({ { solid({ 4, 4 }, red) }, overlay }));

    REQUIRE(frames.size() == 2);
    REQUIRE(frames[1].canvas.pixel(0, 0) == blue);
    REQUIRE(frames[1].canvas.pixel(1, 1)[3] == 0);
}

TEST_CASE("APNG testDisposeBackgroundClearsRegion", "[apng]")
{
    FrameSpec patch{ solid({ 2, 2 }, blue), point(0, 0) };
    patch.disposeOp = DisposeBackground;
    FrameSpec dot{ solid({ 1, 1 }, green), point(3, 3) };
    dot.blendOp = BlendOver;
    const auto frames = decodeAll(buildApng({ { solid({ 4, 4 }, red) }, patch, dot }));

    REQUIRE(frames.size() == 3);
    REQUIRE(frames[1].canvas.pixel(0, 0) == blue);
    REQUIRE(frames[2].canvas.pixel(0, 0)[3] == 0);
    REQUIRE(frames[2].canvas.pixel(2, 2) == red);
    REQUIRE(frames[2].canvas.pixel(3, 3) == green);
}

TEST_CASE("APNG testDisposePreviousRestoresRegion", "[apng]")
{
    FrameSpec patch{ solid({ 2, 2 }, blue), point(0, 0) };
    patch.disposeOp = DisposePrevious;
    FrameSpec dot{ solid({ 1, 1 }, green), point(3, 3) };
    const auto frames = decodeAll(buildApng({ { solid({ 4, 4 }, red) }, patch, dot }));

    REQUIRE(frames.size() == 3);
    REQUIRE(frames[1].canvas.pixel(0, 0) == blue);
    REQUIRE(frames[2].canvas.pixel(0, 0) == red);
}

TEST_CASE("APNG testDelayFraction", "[apng]")
{
    FrameSpec first{ solid({ 2, 2 }, red) };
    first.delayNumerator = 1;
    first.delayDenominator = 25;
    FrameSpec second{ solid({ 2, 2 }, blue) };
    second.delayNumerator = 3;
    second.delayDenominator = 2;
    const auto frames = decodeAll(buildApng({ first, second }));

    REQUIRE(frames.size() == 2);
    REQUIRE(frames[0].delayMs == 40);
    REQUIRE(frames[1].delayMs == 1500);
}

TEST_CASE("APNG testZeroDenominatorMeansHundredths", "[apng]")
{
    FrameSpec first{ solid({ 2, 2 }, red) };
    first.delayNumerator = 7;
    first.delayDenominator = 0;
    const auto frames = decodeAll(buildApng({ first, { solid({ 2, 2 }, blue) } }));

    REQUIRE(frames.size() == 2);
    REQUIRE(frames[0].delayMs == 70);
}

TEST_CASE("APNG testRegionOutsideCanvasYieldsNoAnimation", "[apng]")
{
    FrameSpec patch{ solid({ 2, 2 }, blue), point(3, 3) };
    const auto frames = decodeAll(buildApng({ { solid({ 4, 4 }, red) }, patch }));
    REQUIRE(frames.size() < 2);
}

