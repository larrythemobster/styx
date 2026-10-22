#include <catch2/catch_test_macros.hpp>

#include "Core/Media/H264Sps.hpp"
#include "Core/Media/VideoEncoder.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

using namespace Styx;
using namespace Styx::Core::Media;
using Discord::Voice::EncodedVideoFrame;
using Discord::Voice::VideoCodec;

namespace {

// Each SPS is a whole NAL unit as ffmpeg 8.1's command line had an encoder of this machine write
// it, from three pictures of its `testsrc2` pattern: 160 by 128 for the graphics card's encoders,
// which take nothing smaller, and 128 by 96 for the others.
//
// h264_amf as VideoEncoder sets it, -usage lowlatency -bf 0, in the high and the main profile,
// and with nothing asked of it.
const char *const AMF_LOW_LATENCY_HIGH = "6764042aacb28508dff80008000a10202014000003000400000300f23c221196";
const char *const AMF_LOW_LATENCY_MAIN = "674d042ad942846ffc000400050810100a000003000200000300791e1108cb";
const char *const AMF_DEFAULT_HIGH = "6764042aacd142846ffc000400050810100a000003000200000300791e1108cb";

// h264_mf with -hw_encoding 1 and 0.
const char *const MF_HARDWARE = "67420428e8a14237fe00020002840404050000030001000003003c8f08846580";
const char *const MF_SOFTWARE = "6742c00b95a0836840000003004000000f03682211a8";

// libopenh264, the second from pictures of 130 by 98, which have to be cropped.
const char *const OPENH264 = "6742c0148c6820d30101e1108d40";
const char *const OPENH264_CROPPED = "6742c00b8c6824f622230101e1108d40";

// With B-frames: h264_amf -usage transcoding -bf 2, and h264_mf -hw_encoding 0 -profile:v 100 -bf 2.
const char *const AMF_B_FRAMES = "6764042aace50a11bff00010001420404028000003000800000301e4784422cb";
const char *const MF_SOFTWARE_B_FRAMES = "6764000bac2cac20da10000003001000000303c0da08845380";

// No encoder here leaves out the VUI or the restriction, or says it holds more pictures than it
// refers to. These are MF_SOFTWARE with its end written anew by a script that does its own bit
// writing: cut off before the VUI, cut off before the restriction, and with a
// max_dec_frame_buffering of 3. ffmpeg's trace_headers reads each of them as that.
const char *const MF_SOFTWARE_WITHOUT_VUI = "6742c00b95a08364";
const char *const MF_SOFTWARE_WITHOUT_RESTRICTION = "6742c00b95a0836840000003004000000f01";
const char *const MF_SOFTWARE_BUFFERING_ABOVE_REFERENCES = "6742c00b95a0836840000003004000000f0368221192";

// Nor does one use the rest of the syntax, so the same script wrote an SPS that does: High 4:4:4
// Predictive with twelve scaling lists to say, picture order count type 1, fields, cropping, and
// a VUI that has everything, both HRDs among it. trace_headers reads it and the three made of
// it as they are named, and reads 3 as their max_num_ref_frames.
const char *const EVERY_PART = "67f40029210db21c4262868ecc548490880c895d10ae8857442ba215d10ae8857442ba215d10ae8857442ba215d1088c871042147122cae9c9ff8012800bfb808680d92000007d20001d4c1a3500271000fa00009c4000fa06ef3e335007d0002ee3779f1c428b1924";
const char *const EVERY_PART_REORDERING = "67f40029210db21c4262868ecc548490880c895d10ae8857442ba215d10ae8857442ba215d10ae8857442ba215d1088c871042147122cae9c9ff8012800bfb808680d92000007d20001d4c1a3500271000fa00009c4000fa06ef3e335007d0002ee3779f1c428b18cd";
const char *const EVERY_PART_WITHOUT_RESTRICTION = "67f40029210db21c4262868ecc548490880c895d10ae8857442ba215d10ae8857442ba215d10ae8857442ba215d1088c871042147122cae9c9ff8012800bfb808680d92000007d20001d4c1a3500271000fa00009c4000fa06ef3e335007d0002ee3779f1a";
const char *const EVERY_PART_WITHOUT_VUI = "67f40029210db21c4262868ecc548490880c895d10ae8857442ba215d10ae8857442ba215d10ae8857442ba215d1088c871042147122cae9c880";

// EVERY_PART_REORDERING with the most its NAL HRD and its picture order cycle may hold, and
// with one more, which trace_headers refuses.
const char *const THIRTY_TWO_CODED_PICTURE_BUFFERS = "67f40029210db21c4262868ecc548490880c895d10ae8857442ba215d10ae8857442ba215d10ae8857442ba215d1088c871042147122cae9c9ff8012800bfb808680d92000007d20001d4c182035c9d9443ca2461738d081f12110a09c5854302f1a190e0dc7874100fc22084120470982505013c2a0a4160570b82d06017c320c41a0670d8350701bc3a0e41e0770f83d02007fbbcf8cd401f4000bb8dde7c710a2c63340";
const char *const THIRTY_THREE_CODED_PICTURE_BUFFERS = "67f40029210db21c4262868ecc548490880c895d10ae8857442ba215d10ae8857442ba215d10ae8857442ba215d1088c871042147122cae9c9ff8012800bfb808680d92000007d20001d4c182135c9d9443ca2461738d081f12110a09c5854302f1a190e0dc7874100fc22084120470982505013c2a0a4160570b82d06017c320c41a0670d8350701bc3a0e41e0770f83d02007f0420415de7c66a00fa0005dc6ef3e388516319a0";
const char *const ORDER_CYCLE_OF_255 = "67f40029210db21c4262868ecc548490880c895d10ae8857442ba215d10ae8857442ba215d10ae8857442ba215d1088c87100100fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe48b2ba727fe004a002fee021a0364800001f48000753068d4009c4003e80002710003e81bbcf8cd401f4000bb8dde7c710a2c63340";
const char *const ORDER_CYCLE_OF_256 = "67f40029210db21c4262868ecc548490880c895d10ae8857442ba215d10ae8857442ba215d10ae8857442ba215d1088c87100101ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff24595d393ff00250017f7010d01b2400000fa40003a98346a004e2001f40001388001f40dde7c66a00fa0005dc6ef3e388516319a0";

// What the official client makes of the ones it rewrites, by its rule as that script applies
// it, and as trace_headers then reads them. The others it makes into MF_SOFTWARE, whose own
// restriction is the one it adds, and into EVERY_PART.
const char *const AMF_B_FRAMES_REWRITTEN = "6764042aace50a11bff00010001420404028000003000800000301e47844232c";
const char *const MF_SOFTWARE_B_FRAMES_REWRITTEN = "6764000bac2cac20da10000003001000000303c0da08846e";
const char *const MF_SOFTWARE_WITHOUT_VUI_REWRITTEN = "6742c00b95a0836806d0442350";
const char *const EVERY_PART_WITHOUT_RESTRICTION_REWRITTEN = "67f40029210db21c4262868ecc548490880c895d10ae8857442ba215d10ae8857442ba215d10ae8857442ba215d1088c871042147122cae9c9ff8012800bfb808680d92000007d20001d4c1a3500271000fa00009c4000fa06ef3e335007d0002ee3779f1ed0442324";
const char *const EVERY_PART_WITHOUT_VUI_REWRITTEN = "67f40029210db21c4262868ecc548490880c895d10ae8857442ba215d10ae8857442ba215d10ae8857442ba215d1088c871042147122cae9c900da08846480";
const char *const THIRTY_TWO_CODED_PICTURE_BUFFERS_REWRITTEN = "67f40029210db21c4262868ecc548490880c895d10ae8857442ba215d10ae8857442ba215d10ae8857442ba215d1088c871042147122cae9c9ff8012800bfb808680d92000007d20001d4c182035c9d9443ca2461738d081f12110a09c5854302f1a190e0dc7874100fc22084120470982505013c2a0a4160570b82d06017c320c41a0670d8350701bc3a0e41e0770f83d02007fbbcf8cd401f4000bb8dde7c710a2c649";
const char *const ORDER_CYCLE_OF_255_REWRITTEN = "67f40029210db21c4262868ecc548490880c895d10ae8857442ba215d10ae8857442ba215d10ae8857442ba215d1088c87100100fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe48b2ba727fe004a002fee021a0364800001f48000753068d4009c4003e80002710003e81bbcf8cd401f4000bb8dde7c710a2c649";

// Streams of those encoders, an access unit to a string: three pictures from h264_mf, and from
// each of the two with B-frames its key frame and the P-frame after it. The B-frames that
// follow are shown before that P-frame, and a decoder told that nothing is reordered lets go
// of a picture that comes too late to be shown.
const std::array MF_SOFTWARE_STREAM = { "000000010910000000016742c00b95a0836840000003004000000f03682211a80000000168ce3c800000000106052f02f86150fc704172b73248f3a72a3d344d6963726f736f667420482e32363420456e636f6465722056312e352e330080000000010605e4cbb21392987343daa8a6c74298356cf57372633a3320683a393620773a313238206670733a33302e3030302070663a3636206c766c3a3220623a30206271703a3320676f703a3930206964723a393020736c633a3120636d703a302072633a302071703a323620726174653a323030303030207065616b3a3020627566663a30207265663a3120737263683a33322061737263683a3120737562703a31207061723a362033203320726e643a302063616261633a30206c703a322063746e743a30206175643a31206c61743a302077726b3a3136207675693a31206c79723a31203c3c0080000000016588804048fed1158304ff159721d9d2f2d5620aa4f6a97297aca5202213286e024026da4fdc0d0318c026481952c0f95c5e09a23111e1fae6fc59b0aa55eb7329562c851a151b6b612be1204b779086318e9ed6b241b36b22388c7f6a08638cb954994bd662c5537c638cc0d0ec55009a0fc0599748d460d9319f8df3988e23f4c8d0ff9b26534caa4ca5f0bc827ec63009bac43b75ad02cfe345579fc0a16f84eb5f60ef70ece92b49cab841156296925984fb10c1008221aa0c069ed069e0d4c76fc87721dbf2b6efc4709911c06141a2b297a974ab411a6d52d53ab0e63a70dac398a6e081040280f62ba9e6e343946635ce5198d73087cb98a975460d93558364c44df82037e3753637629e62e7bfde30244103805642a79dcc0c8cfcfce485bec58804c10ddc1c40261f800080610fb057ce340c76f09d139f49cfa43310e505027fe3572316ad110efffb6d8b5826a2dfc7fe0100d70c7045bf040142dfae1f1d10ed7e6c994c91127030f7c096e47e7e7b8ea7fff6fb0df365aa685bb7213f06bc8a47e7fa9b9d14ddb087008fec15f53609680c0346434c8c69069cadcb65773870a8430ec35e6f948a81cc1840c35368d4b343019f7dc7673964464b985d402aa9a4b9fdbff2c0c9969d70897729c02c71f87868167a40e7606633682be047a996441abe5ff0a8006305637bf40e1d07be0e008881408dd1d967daca2a5aa96be596b0c28dd5b7edffd10415bbd71a74271a07fc7fe3b7879ec1540e8420446ad5158b38b157731ede00cca44cc0fed7ff00d6c16f7f0811795f815d775580e38655668d7da5aa799f3a67d4cea4cc705c3860040de6fe2ea1e1c08111a1183748e320dbb86e0d600bf4f3a9c82e9ebf97f05d9b33084242fad7aff0061b668817dafc3c033a0ec9fedc3442c209ca32ad7fc0a14396dabab8625c40d7fda2714e32804052202d881622058880588805814ec62128814892c4f2c448589e1629e9e1670d32f7fe8afe1ab83031f0ab1a5af3019d7e0d740a239509e217d5c538f20109c40be24b11258890b11216052c4b8802dd6b880162200584f4f14e0ebf2fdf0cf0a795f0", "00000001093000000001619a038091e69a189173f9f131fa51d464559048156393fe7a237f03bd711077ae7d0472e682813f341343152066444a1dacb0db9d43dff954fd11e482b1abd26d917ad71d05bcd99b3fd86dc043ef7e7ff4f19ed08468df80262fd01e4390157f74ee11de4e7c5f223c69214353ebffe82026f6d8b9e688ac0ef5c877ae40ebae43aeb91eee2016880134977325dcca2ae65157342df5f047b1bfecef356cef80", "00000001093000000001619a058091fe7842cd8a10030eded83f063625011d521f061fcb9fe4b820e5c40e4f1857e71859ce2e6784cced3f5a0f7e823971183fd718ad0ca18a841521d23cf6fa7873871409466463f3fe3af5fabeae3c093819ec708e9bcb6e3a3161b743df1fa79dcf8d27f52579ccbe7124c9c33e5c04c05a83ae24c34f90c97b025d6220eef9644b7b7f8620eacc47311829509d7fcc7dcf1ae7a2c7f2ee00a29cb53f21b7ffe9fd01bf8caacd1afb5ec5bdd7c3397e8efc3fd83a4681ae040fcf4efcbff0056d6ee7e609a7820c0ca726d7c00c035230e0fd5fbf621ae788a88ac29b924dc832572525723f3f108981895aeb8801631002c23bfcb5de2dd89be7a129df8019cfec8547ef8980" };
const std::array MF_SOFTWARE_B_FRAMES_STREAM = { "000000010910000000016764000bac2cac20da10000003001000000303c0da088453800000000168ce3c300000000106052f02f86150fc704172b73248f3a72a3d344d6963726f736f667420482e32363420456e636f6465722056312e352e330080000000010605e5cbb21392987343daa8a6c74298356cf57372633a3320683a393620773a313238206670733a33302e3030302070663a313030206c766c3a3220623a32206271703a3320676f703a3930206964723a393020736c633a3120636d703a302072633a302071703a323620726174653a323030303030207065616b3a3020627566663a30207265663a3220737263683a33322061737263683a3120737562703a31207061723a362033203320726e643a302063616261633a30206c703a322063746e743a30206175643a31206c61743a302077726b3a3136207675693a31206c79723a31203c3c008000000001658880400048fed1158304ff159721d9d2f2d5620aa4f6a97297aca5202213286e024026da4fdc0d0318c026481952c0f95c5e09a23111e1f45cdf8b36154abd6e652ac8a1468546dad84af84812dde4218c63a7b5ac906cdac88e231c8f6a08638cb954994be0a705537c638cc0d0ec55009a0fc0599748d460d9319f8df3988e23f4c8d0ff9b26534caa4ca5f0bc827ec63009bac43b75ad02cfe345579fc0a16f84eb5f60ef70ece92b49cab841156296925984fb10c1008221aa0c069ed069e0d4c76fc87721dbf2b6efc4709911c06141a2b297a974ab411a6d52d53ab0e63a70dac398a6e081040280f62ba9e6e343946635ce5198d73087cb98a975460d93558364c44df82037e3753637629e62e7bfde30244103805642a79dcc0c8cfcfce485bec58804c10ddc1c40261f800080610fb057ce340c76f09d139f49cfa4332e505027fe3572316a7c221dfff6db16aa09a8b7f1ff25280663a470457c3c10050b78e31e90e839e6c994c70230938187be04b723f3f3dc753ffffec377f2a9af08d909f835e4523f3fd4dce8a6ed8438047f60afa9b04b40601a321a64634834e56e5b2bb9c3854218761af37ca4540e60c2061a9b46a59a180cfbee3b39cb22325cc2ea01554e4b9fdbff2c0c9969d70897729c02c05d47da3fa7fe0e7606633682be047a996441abe5fc615000c60ac6f7e81c3a0f7c1c011102811ba3b2cfb59454b552d7cb2d61851bab6fedffd10415bbd71a74271a07fc7fe1b78db3d82a81d084088d5aa2b167162aee63dbc0199489981fdaffe01ad82defe1022f2bf02baeeab01c70caacd1afb4b54ffc3ff611ce887d4cea4cc705c3860040de6fe2ea1e1c08111a1183748e320dbb86e0d600bf4f3a9c82e9dbfff82ecd998421217d6bd7f8030db3440bed7e19c033a05ddb86885841394655aff8142872db57570c4b881affb44e29c650080a4405b102c440b1100b1100b029c7e82128814892c4f2c448589e1629e9e1670d32f7fe8afe1ab83031f0ab1a5af3019d7e0d740a239509d442fab8a71e402138817c496224b112162242c0a518a0971005bad71002c4400b09e9e29c1d7e5fbe19e14f4f80", "00000001093000000001619a02078091fe7842cd8a10030eded83f063625011e921f061fc11e7fb2de191e1cc9868c0e583e356bf0c0c0c419e9a9c6abcfe0eb7f07a7ffff57c23be8d10fd6820a4c1d232bb7d3c1370e29b8aad7f8ebd7fc6dea278a9538e3e3b96b9a7a237e880ebae420ebae13ab0f0e4f8f876203618147e9869f20c56c046ad3904ddf2c8c8e589b1eb2946d637dfb570e83d860d2442069ba3b2de76c972d54b79dd70e6b26218a9f9743bd720c1628c9c47bd7f8022fee9dc7a3bdd7363e210c06c6f9be7ce4a0b81cc609982dab0f99023df9ea4dc37ebce2114921b7367fff6d1187f6b1204b46cabbc987a8403fa05f4be4f02881861169a856d2b21802417cc0790e40526fbd107c2111e5ae788251016c496224b11258892c4df0ffe0aa21130624eb58801622005842df5f3ceb7f4427fc38ff90dfac3f2cd920" };
const std::array AMF_B_FRAMES_STREAM = { "000000010910000000016764042aace50a11bff00010001420404028000003000800000301e4784422cb0000000168ee3c8000000001658884004dfffebcf48617d60fafd47e9c7993f14946f6c9876c2b2c1af3d9810b7cc0da2603f145f5d43e85029c2be413775a0dcd25356f39f4163a9d3cb07e8e30f417ea049599f9131fa27b29b9d768d9b9f424b91848748aa12482463cf9fca1b41ef757feeec4bde71bf26fe9a7e95877df0d5a6c7da50ce05e655184de8622257bafeee054caf4f67328b528d73e7e16eb5ab3a06e9a614fb27d7299cedf0c2f10bd863d940a1fe1dbaf00dff4260e37892093e9f199c69cc6753bfe375fa32ab1a6ecec879f25b0728510ee37e82b134ae1a8134887cec30ecfe7fa7262135c4b77a88514a9d1eb0822887ead70c9e4c06fb5eef3a9bd21b1d9882380c98f66d3559e32663371b0388123190ec3992174d1a3fc6ced13187102f4e92f764d900abec4d8224a3de70e7b2b08cab06de9270c0d45868a528cc52750188424ffa99671a916f47f7e91151cdf2b329abc754bf381103ebfdf7afa3eb21829b8a95889b54ffa41cf7a10c1cba89f336d12846d1c7326fb93f5fa23de025c2aa6697111a42f99bf1fa7ab3018e5d984a2db6709b7936885b522d76ca7b0d7a998d1a0652733fb8b7c9bb1db5397306e1397f366ba431a31147994d7cdd3df48d6a35cfda9bb5fcf50a537c38a0bf90f623668b1c8a54914893e116eeebfc8c7227a23bf2cf47d523d099ac089023ccca9bab90fa49403e8c1e8163ec63253007bb99dbb498ef763d4f9cc3275430b1d5d087a0ddd7ad8802d1f45da0f963fb45941a101034c3a7ff87e279c07c00aa9300a3c2be31b7d2a94b53eff9023d52ac3ce43b201866141536d2c9b01773b44c4c8698f237c0efd56fc17e642155b94752e53ab111fe49404f069c854a3cbce622702c3bd71d5019c3cb9b236eb46ea9a0d70a547fccd4b7eaffca59c8680654c415a6192c9273fd52d07f9ef45f76d173512a283fb736f4d3e4d5b70c0c7a40c7475e0ada9803e5f6dbe83559bc6843db6e63508ea92491d3018695c04039ff54372ba684d564f15df501dfb294e02275de7bb5a651ab7cf1d012ed2eae502b34d168252e079ccc6a1d0b18ef9cf80b69ac6a14d286719a46bd39a35b26b3d26b82d2ab6ef0448f3276080abe78e02a581ac372d2f8d55c6c02a7dfde75f522cb863eae2c1b97257a83e4ec770a5d42cf7233ee80d140da28cd8dd70eb25f31fd2331b9beb619321ca6196f6ea", "00000001093000000001419a2c209bffec8edfb3036e28777c4eb12a544fc77fb9e638a6f947684a8f132ea60fa804388a6b9b778fae9728b96aaad21fd9397ff9bcd083dc46fa2611966edb0479f7ef5b8ca89262ac94544f8efa554c4bda1f0011968c72df0c208a155e89b8526349690d764c70846a6a99366151fc9e61fb979bc5d8586b5be73c1b9e56db8027a9016524a0595b35c42b81f891d5263826bbe0acf06a0b8fa683ad8c08d593cb4eeb7aac974ca35cd80f57b4aed6fb078bb2e1972501a3ad8a655582d4105bb0591c5cdb8ba4207c958ab9ac6cdcc6f5aeb30b460f3113ed1b674b40fa4e1dfc3195290ebea968454696e260823d2aadb0e83d8629578312a8a6e3de3a242c91e1858f701c5f6f76f9a5ed2f04ab584856dfb860aaf2c2e3971d4cd3d1c5c1d95fc95a20b109602f28877de77b5e3fd382310078cd96b727b0de58ad5960cbc88131f1e3e368f1bfe1b308f846c4682912f40815d11fb52ca7666a8b6a6e80b812953b7e04fb2e67e7fca8" };

struct Sample
{
    const char *name;
    const char *sps;
    // As trace_headers reads it.
    H264::SpsReordering says;
    // Null for one the official client keeps.
    const char *rewritten;
};

const std::array SAMPLES = {
    Sample{ "h264_amf, low latency, high", AMF_LOW_LATENCY_HIGH, { true, true, 0, 4, 4 }, nullptr },
    Sample{ "h264_amf, low latency, main", AMF_LOW_LATENCY_MAIN, { true, true, 0, 4, 4 }, nullptr },
    Sample{ "h264_amf, as it comes", AMF_DEFAULT_HIGH, { true, true, 0, 4, 4 }, nullptr },
    Sample{ "h264_mf, hardware", MF_HARDWARE, { true, true, 0, 4, 4 }, nullptr },
    Sample{ "h264_mf, software", MF_SOFTWARE, { true, true, 0, 1, 1 }, nullptr },
    Sample{ "libopenh264", OPENH264, { true, true, 0, 1, 1 }, nullptr },
    Sample{ "libopenh264, cropped", OPENH264_CROPPED, { true, true, 0, 1, 1 }, nullptr },
    Sample{ "h264_amf, B-frames", AMF_B_FRAMES, { true, true, 2, 4, 4 }, AMF_B_FRAMES_REWRITTEN },
    Sample{ "h264_mf, software, B-frames", MF_SOFTWARE_B_FRAMES, { true, true, 1, 2, 2 }, MF_SOFTWARE_B_FRAMES_REWRITTEN },
    Sample{ "h264_mf, VUI taken out", MF_SOFTWARE_WITHOUT_VUI, { false, false, 0, 0, 1 }, MF_SOFTWARE_WITHOUT_VUI_REWRITTEN },
    Sample{ "h264_mf, restriction taken out", MF_SOFTWARE_WITHOUT_RESTRICTION, { true, false, 0, 0, 1 }, MF_SOFTWARE },
    Sample{ "h264_mf, more held than referred to", MF_SOFTWARE_BUFFERING_ABOVE_REFERENCES, { true, true, 0, 3, 1 }, MF_SOFTWARE },
    Sample{ "every part", EVERY_PART, { true, true, 0, 3, 3 }, nullptr },
    Sample{ "every part, reordering", EVERY_PART_REORDERING, { true, true, 2, 5, 3 }, EVERY_PART },
    Sample{ "every part, no restriction", EVERY_PART_WITHOUT_RESTRICTION, { true, false, 0, 0, 3 }, EVERY_PART_WITHOUT_RESTRICTION_REWRITTEN },
    Sample{ "every part, no VUI", EVERY_PART_WITHOUT_VUI, { false, false, 0, 0, 3 }, EVERY_PART_WITHOUT_VUI_REWRITTEN },
    Sample{ "32 coded picture buffers", THIRTY_TWO_CODED_PICTURE_BUFFERS, { true, true, 2, 5, 3 }, THIRTY_TWO_CODED_PICTURE_BUFFERS_REWRITTEN },
    Sample{ "picture order cycle of 255", ORDER_CYCLE_OF_255, { true, true, 2, 5, 3 }, ORDER_CYCLE_OF_255_REWRITTEN },
};

using Bytes = std::vector<uint8_t>;

inline uint8_t hexNibble(char c)
{
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    return 0;
}

Bytes bytes(const char *hexStr)
{
    Bytes out;
    if (!hexStr) return out;
    const size_t len = std::strlen(hexStr);
    out.reserve(len / 2);
    for (size_t i = 0; i + 1 < len; i += 2) {
        out.push_back(static_cast<uint8_t>((hexNibble(hexStr[i]) << 4) | hexNibble(hexStr[i + 1])));
    }
    return out;
}

std::string hex(std::span<const uint8_t> data)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(data.size() * 2);
    for (uint8_t b : data) {
        s.push_back(digits[(b >> 4) & 0x0F]);
        s.push_back(digits[b & 0x0F]);
    }
    return s;
}

Bytes annexB(const std::vector<Bytes> &nals, size_t startCodeSize = 4)
{
    Bytes unit;
    for (const Bytes &nal : nals) {
        unit.insert(unit.end(), startCodeSize - 1, 0);
        unit.push_back(1);
        unit.insert(unit.end(), nal.begin(), nal.end());
    }
    return unit;
}

// The NAL units of an access unit whose start codes are all of four bytes.
std::vector<Bytes> nalsOf(const Bytes &unit)
{
    static constexpr std::array<uint8_t, 4> START_CODE = { 0, 0, 0, 1 };
    std::vector<Bytes> nals;
    for (auto at = unit.begin(); at != unit.end();) {
        const auto begin = at + START_CODE.size();
        at = std::search(begin, unit.end(), START_CODE.begin(), START_CODE.end());
        nals.emplace_back(begin, at);
    }
    return nals;
}

bool isSps(const Bytes &nal)
{
    return (nal[0] & 0x1F) == 7;
}

std::vector<Bytes> withSps(std::vector<Bytes> nals, const Bytes &sps)
{
    for (Bytes &nal : nals) {
        if (isSps(nal))
            nal = sps;
    }
    return nals;
}

template <size_t Count>
std::vector<Bytes> accessUnits(const std::array<const char *, Count> &stream)
{
    std::vector<Bytes> units;
    for (const char *unit : stream)
        units.push_back(bytes(unit));
    return units;
}

bool looksLikeAStartCode(std::span<const uint8_t> nal)
{
    for (size_t i = 0; i + 3 <= nal.size(); i++) {
        if (nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] <= 2)
            return true;
    }
    return false;
}

struct Decoding
{
    std::vector<std::vector<uint8_t>> pictures;
    // How many pictures the decoder holds back to put them in order, once it has the key frame.
    int heldBack = -1;
};

// ffmpeg's decoder, told to go by what the stream says of itself and not by what it sees.
std::optional<Decoding> decoded(const std::vector<Bytes> &accessUnits)
{
    const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *context = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (!context)
        return std::nullopt;
    context->strict_std_compliance = FF_COMPLIANCE_STRICT;
    context->thread_count = 1;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    Decoding decoding;
    bool failed = avcodec_open2(context, decoder, nullptr) < 0;
    const auto collect = [&] {
        while (avcodec_receive_frame(context, frame) == 0) {
            const AVPixelFormat format = static_cast<AVPixelFormat>(frame->format);
            std::vector<uint8_t> picture(av_image_get_buffer_size(format, frame->width, frame->height, 1), 0);
            av_image_copy_to_buffer(picture.data(), static_cast<int>(picture.size()), frame->data, frame->linesize, format, frame->width, frame->height, 1);
            decoding.pictures.push_back(std::move(picture));
            av_frame_unref(frame);
        }
    };
    for (size_t i = 0; i < accessUnits.size() && !failed; i++) {
        failed = av_new_packet(packet, static_cast<int>(accessUnits[i].size())) < 0;
        if (failed)
            break;
        memcpy(packet->data, accessUnits[i].data(), accessUnits[i].size());
        failed = avcodec_send_packet(context, packet) < 0;
        av_packet_unref(packet);
        collect();
        if (i == 0)
            decoding.heldBack = context->has_b_frames;
    }
    if (!failed) {
        avcodec_send_packet(context, nullptr);
        collect();
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    return failed ? std::nullopt : std::optional(decoding);
}

struct Finished
{
    bool whole = false;
    bool keyframe = false;
    Bytes data;
};

Finished finish(EncodedFrameFinisher &finisher, const Bytes &data)
{
    EncodedVideoFrame frame;
    frame.data = data;
    Finished finished;
    finished.whole = finisher.finish(frame);
    finished.keyframe = frame.keyframe;
    finished.data = frame.data;
    return finished;
}

} // namespace

TEST_CASE("An SPS is read as ffmpeg reads it", "[video][h264][sps]")
{
    for (const auto &sample : SAMPLES) {
        DYNAMIC_SECTION(sample.name) {
            const std::optional<H264::SpsReordering> read = H264::spsReordering(bytes(sample.sps));
            REQUIRE(read.has_value());
            REQUIRE(read->hasVui == sample.says.hasVui);
            REQUIRE(read->hasBitstreamRestriction == sample.says.hasBitstreamRestriction);
            REQUIRE(read->maxNumReorderFrames == sample.says.maxNumReorderFrames);
            REQUIRE(read->maxDecFrameBuffering == sample.says.maxDecFrameBuffering);
            REQUIRE(read->maxNumRefFrames == sample.says.maxNumRefFrames);
            REQUIRE(read->isKeptByOfficialClient() == (sample.rewritten == nullptr));
        }
    }
}

TEST_CASE("An SPS comes out as the official client leaves it", "[video][h264][sps]")
{
    for (const auto &sample : SAMPLES) {
        DYNAMIC_SECTION(sample.name) {
            const Bytes sps = bytes(sample.sps);
            const std::optional<Bytes> rewritten = H264::rewrittenSps(sps);
            if (!sample.rewritten) {
                REQUIRE(!rewritten.has_value());
                return;
            }
            REQUIRE(rewritten.has_value());
            REQUIRE(hex(*rewritten) == sample.rewritten);
            REQUIRE(!looksLikeAStartCode(*rewritten));

            const H264::SpsReordering kept{ true, true, 0, sample.says.maxNumRefFrames, sample.says.maxNumRefFrames };
            REQUIRE(H264::spsReordering(*rewritten) == kept);
            REQUIRE(!H264::rewrittenSps(*rewritten).has_value());
        }
    }
}

TEST_CASE("A rewritten stream decodes to the same pictures", "[video][h264][sps]")
{
    const std::vector<Bytes> plain = accessUnits(MF_SOFTWARE_STREAM);
    const auto withSpsOf = [&plain](const char *sps) {
        std::vector<Bytes> stream = plain;
        stream[0] = annexB(withSps(nalsOf(plain[0]), bytes(sps)));
        return stream;
    };

    struct TestCase {
        const char *name;
        std::vector<Bytes> stream;
        bool heldBackBefore;
    };

    const std::vector<TestCase> cases = {
        // Without a restriction a decoder that goes by the standard has to allow for as much
        // reordering as the level permits.
        { "no VUI", withSpsOf(MF_SOFTWARE_WITHOUT_VUI), true },
        { "no restriction", withSpsOf(MF_SOFTWARE_WITHOUT_RESTRICTION), true },
        { "more held than referred to", withSpsOf(MF_SOFTWARE_BUFFERING_ABOVE_REFERENCES), false },
        { "h264_mf, set to B-frames", accessUnits(MF_SOFTWARE_B_FRAMES_STREAM), true },
        { "h264_amf, set to B-frames", accessUnits(AMF_B_FRAMES_STREAM), true },
    };

    for (const auto &testCase : cases) {
        DYNAMIC_SECTION(testCase.name) {
            std::vector<Bytes> rewritten = testCase.stream;
            REQUIRE(H264::rewriteSpsIn(rewritten[0]));
            for (size_t i = 1; i < rewritten.size(); i++)
                REQUIRE(!H264::rewriteSpsIn(rewritten[i]));

            const std::optional<Decoding> before = decoded(testCase.stream);
            const std::optional<Decoding> after = decoded(rewritten);
            REQUIRE((before && after));
            REQUIRE(before->pictures.size() == testCase.stream.size());
            REQUIRE(before->pictures[0] != before->pictures[1]);
            REQUIRE(before->pictures == after->pictures);
            REQUIRE((before->heldBack > 0) == testCase.heldBackBefore);
            REQUIRE(after->heldBack == 0);
        }
    }
}

TEST_CASE("Emulation prevention goes both ways", "[video][h264][sps]")
{
    struct TestCase {
        const char *name;
        const char *payload;
        const char *nal;
    };

    const std::vector<TestCase> cases = {
        { "nothing to prevent", "6742c00b", "6742c00b" },
        { "zeros apart", "0001000201", "0001000201" },
        { "two zeros and a four", "67000004", "67000004" },
        { "three zeros", "6700000080", "670000030080" },
        { "a start code", "6700000180", "670000030180" },
        { "two zeros and a two", "6700000280", "670000030280" },
        { "two zeros and a three", "6700000380", "670000030380" },
        { "five zeros", "67000000000080", "670000030000030080" },
        { "one after another", "67000001000001", "670000030100000301" },
        { "zeros at the end", "67800000", "6780000003" },
        { "h264_amf's timing", "14000000040000000f23c0", "140000030004000003000f23c0" },
    };

    for (const auto &testCase : cases) {
        DYNAMIC_SECTION(testCase.name) {
            REQUIRE(hex(H264::withEmulationPrevention(bytes(testCase.payload))) == testCase.nal);
            REQUIRE(hex(H264::withoutEmulationPrevention(bytes(testCase.nal))) == testCase.payload);
            REQUIRE(!looksLikeAStartCode(H264::withEmulationPrevention(bytes(testCase.payload))));
        }
    }
}

TEST_CASE("An SPS cut short is left alone", "[video][h264][sps]")
{
    for (const auto &sample : SAMPLES) {
        DYNAMIC_SECTION(sample.name) {
            const Bytes sps = bytes(sample.sps);
            for (size_t length = 0; length < sps.size(); length++) {
                const std::span<const uint8_t> cut(sps.data(), length);
                INFO("length: " << length);
                REQUIRE(!H264::spsReordering(cut).has_value());
                REQUIRE(!H264::rewrittenSps(cut).has_value());

                const Bytes unit = annexB({ Bytes(cut.begin(), cut.end()) });
                Bytes given = unit;
                REQUIRE(!H264::rewriteSpsIn(given));
                REQUIRE(given == unit);
            }
        }
    }
}

TEST_CASE("What is no SPS is left alone", "[video][h264][sps]")
{
    REQUIRE(!H264::rewrittenSps({}).has_value());
    REQUIRE(!H264::spsReordering({}).has_value());

    // A PPS and a slice that hold what an SPS would.
    for (int header : { 0x68, 0x65, 0x41, 0x0F }) {
        Bytes other = bytes(AMF_B_FRAMES);
        other[0] = static_cast<uint8_t>(header);
        REQUIRE(!H264::spsReordering(other).has_value());
        REQUIRE(!H264::rewrittenSps(other).has_value());
    }

    for (size_t length = 1; length < 200; length++) {
        Bytes zeros(length, 0);
        zeros[0] = 0x67;
        REQUIRE(!H264::spsReordering(zeros).has_value());
        REQUIRE(!H264::rewrittenSps(zeros).has_value());
    }

    // The longest number there is has 31 zeros before its one. This is seq_parameter_set_id,
    // after which a run of ones reads as an SPS of sorts.
    Bytes longestNumber = { 0x67, 0x42, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x01 };
    longestNumber.insert(longestNumber.end(), 40, 0xFF);
    REQUIRE(H264::spsReordering(longestNumber).has_value());
    Bytes tooLongANumber = { 0x67, 0x42, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x80 };
    tooLongANumber.insert(tooLongANumber.end(), 40, 0xFF);
    REQUIRE(!H264::spsReordering(tooLongANumber).has_value());
    REQUIRE(!H264::rewrittenSps(tooLongANumber).has_value());

    for (const char *beyondTheStandard : { THIRTY_THREE_CODED_PICTURE_BUFFERS, ORDER_CYCLE_OF_256 }) {
        REQUIRE(!H264::spsReordering(bytes(beyondTheStandard)).has_value());
        REQUIRE(!H264::rewrittenSps(bytes(beyondTheStandard)).has_value());
    }
}

// Bytes at random under an SPS's header read as an SPS often enough, which the official client
// would rewrite like any other.
TEST_CASE("Whatever is rewritten is then kept", "[video][h264][sps]")
{
    std::mt19937 random(7);
    int rewrittenCount = 0;
    for (int i = 0; i < 20000; i++) {
        Bytes sps(2 + random() % 64);
        std::generate(sps.begin(), sps.end(), [&random] { return static_cast<uint8_t>(random()); });
        sps[0] = 0x67;

        const std::optional<Bytes> rewritten = H264::rewrittenSps(sps);
        if (!rewritten)
            continue;
        rewrittenCount++;
        const std::optional<H264::SpsReordering> read = H264::spsReordering(*rewritten);
        INFO(hex(sps));
        REQUIRE((read && read->isKeptByOfficialClient()));
        REQUIRE(read->maxNumRefFrames == H264::spsReordering(sps)->maxNumRefFrames);
        REQUIRE(!H264::rewrittenSps(*rewritten));
        REQUIRE(!looksLikeAStartCode(*rewritten));
    }
    REQUIRE(rewrittenCount > 100);
}

TEST_CASE("Everything around an SPS is left as it was", "[video][h264][sps]")
{
    const std::vector<Bytes> nals = nalsOf(bytes(AMF_B_FRAMES_STREAM[0]));
    REQUIRE(nals.size() == size_t(4));
    const Bytes &delimiter = nals[0], &sps = nals[1], &pps = nals[2], &picture = nals[3];
    REQUIRE(hex(sps) == AMF_B_FRAMES);
    const Bytes rewritten = bytes(AMF_B_FRAMES_REWRITTEN);

    for (size_t startCodeSize : { size_t(3), size_t(4) }) {
        Bytes unit = annexB(nals, startCodeSize);
        REQUIRE(H264::rewriteSpsIn(unit));
        REQUIRE(hex(unit) == hex(annexB({ delimiter, rewritten, pps, picture }, startCodeSize)));
        REQUIRE(!H264::rewriteSpsIn(unit));
    }

    // Start codes of both lengths, an SPS that comes last, and more than one of them.
    const Bytes otherSps = bytes(MF_SOFTWARE_WITHOUT_VUI), otherRewritten = bytes(MF_SOFTWARE_WITHOUT_VUI_REWRITTEN), keptSps = bytes(MF_SOFTWARE);
    const auto mixed = [&](const Bytes &first, const Bytes &second) {
        Bytes unit = { 0xDE, 0xAD };
        for (const Bytes &part : { annexB({ first }, 3), annexB({ pps, keptSps }), annexB({ picture }, 3), annexB({ second }) })
            unit.insert(unit.end(), part.begin(), part.end());
        return unit;
    };
    Bytes unit = mixed(sps, otherSps);
    REQUIRE(H264::rewriteSpsIn(unit));
    REQUIRE(hex(unit) == hex(mixed(rewritten, otherRewritten)));
}

TEST_CASE("An access unit with nothing to rewrite is left as it was", "[video][h264][sps]")
{
    std::vector<Bytes> units = accessUnits(MF_SOFTWARE_STREAM);
    const std::vector<Bytes> withBFrames = accessUnits(AMF_B_FRAMES_STREAM);
    units.insert(units.end(), withBFrames.begin() + 1, withBFrames.end());
    units.push_back({});
    units.push_back({ 0, 0, 1 });
    units.push_back({ 0, 0, 0, 1, 0x67 });
    // An SPS that would be rewritten, were there a start code before it.
    units.push_back(bytes(MF_SOFTWARE_B_FRAMES));

    for (const Bytes &unit : units) {
        Bytes given = unit;
        REQUIRE(!H264::rewriteSpsIn(given));
        REQUIRE(given == unit);
    }
}

TEST_CASE("A key frame leaves the finisher with an SPS that is kept", "[video][h264][sps]")
{
    const std::vector<Bytes> nals = nalsOf(bytes(AMF_B_FRAMES_STREAM[0]));
    const Bytes &delimiter = nals[0], &sps = nals[1], &pps = nals[2], &picture = nals[3];
    const Bytes rewritten = bytes(AMF_B_FRAMES_REWRITTEN);
    const std::string whole = hex(annexB({ rewritten, pps, picture }));

    {
        EncodedFrameFinisher finisher(VideoCodec::H264);
        Finished finished = finish(finisher, annexB({ sps, pps, picture }));
        REQUIRE((finished.whole && finished.keyframe));
        REQUIRE(hex(finished.data) == whole);

        // The SPS it keeps for a key frame that comes without one is the rewritten one as well.
        finished = finish(finisher, annexB({ picture }));
        REQUIRE((finished.whole && finished.keyframe));
        REQUIRE(hex(finished.data) == whole);
    }
    {
        EncodedFrameFinisher finisher(VideoCodec::H264);
        const Finished finished = finish(finisher, annexB({ delimiter, sps, pps, picture }));
        REQUIRE((finished.whole && finished.keyframe));
        REQUIRE(hex(finished.data) == whole);
    }
    {
        EncodedFrameFinisher finisher(VideoCodec::H264);
        const Bytes extradata = annexB({ sps, pps });
        finisher.rememberParameterSets(extradata.data(), extradata.size());
        const Finished finished = finish(finisher, annexB({ delimiter, picture }));
        REQUIRE((finished.whole && finished.keyframe));
        REQUIRE(hex(finished.data) == whole);
    }
    {
        // What VideoEncoder has h264_amf write is kept, and so comes out as it went in.
        const std::vector<Bytes> kept = { bytes(AMF_LOW_LATENCY_HIGH), pps, picture };
        EncodedFrameFinisher finisher(VideoCodec::H264);
        const Finished finished = finish(finisher, annexB(kept));
        REQUIRE((finished.whole && finished.keyframe));
        REQUIRE(hex(finished.data) == hex(annexB(kept)));
    }
}

TEST_CASE("The finisher rewrites only H264", "[video][h264][sps]")
{
    const Bytes unit = bytes(AMF_B_FRAMES_STREAM[0]);
    for (VideoCodec codec : { VideoCodec::H265, VideoCodec::VP8 }) {
        EncodedFrameFinisher finisher(codec);
        finisher.rememberParameterSets(unit.data(), unit.size());
        const Finished finished = finish(finisher, unit);
        REQUIRE(finished.whole);
        REQUIRE(hex(finished.data) == hex(unit));
    }
}
