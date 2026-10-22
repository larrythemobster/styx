#include "Discord/Voice/SinkWants.hpp"

#include <array>

namespace Styx::Discord::Voice {
namespace {
struct LadderStep { int want; int pixels; };
// Smallest first. Each step is the resolution a sender is asked for at that want value.
constexpr std::array<LadderStep, 9> LADDER{{{20,256*144},{30,320*180},{40,384*216},{50,416*234},{60,512*288},{70,640*360},{80,672*378},{90,896*504},{100,1280*720}}};
constexpr std::int64_t PIXEL_BUDGET = 1280 * 720;
constexpr int FULL = 100, NONE = 0;
} // namespace
int sinkWantLevel(int videoSenders, int tilePixels)
{
    int level = LADDER.front().want;
    for (const LadderStep &step : LADDER) {
        if (tilePixels > 0 && tilePixels < step.pixels) { level = step.want; break; }
        if (std::int64_t(step.pixels) * videoSenders > PIXEL_BUDGET) break;
        level = step.want;
    }
    return level;
}
SinkWantsPlan planSinkWants(const SinkWantsInput &input)
{
    SinkWantsPlan plan;
    int senderCount = 0;
    for (const VideoSender &sender : input.senders) if (!sender.layers.empty()) ++senderCount;
    const int everyone = sinkWantLevel(senderCount);
    plan.wants.any = everyone;
    for (const VideoSender &sender : input.senders) {
        if (sender.layers.empty()) continue;
        if (!sender.wanted) { for (const VideoLayer &layer : sender.layers) plan.wants.bySsrc.emplace(layer.ssrc, NONE); continue; }
        const int forTile = sinkWantLevel(senderCount, sender.tilePixels);
        const bool enlarged = sender.userId == input.focusedUser && everyone != FULL;
        std::uint32_t taken = sender.layers.front().ssrc;
        if (sender.layers.size() > 1) {
            for (const VideoLayer &layer : sender.layers) {
                if (layer.quality == FULL) { plan.wants.bySsrc.emplace(layer.ssrc, enlarged ? FULL : NONE); if (enlarged) taken = layer.ssrc; }
                else if (enlarged) plan.wants.bySsrc.emplace(layer.ssrc, NONE);
                else { if (input.oneToOne) plan.wants.bySsrc.emplace(layer.ssrc, forTile); taken = layer.ssrc; }
            }
        } else if (enlarged) plan.wants.bySsrc.emplace(taken, FULL);
        else if (input.oneToOne) plan.wants.bySsrc.emplace(taken, forTile);
        plan.deliver.emplace(sender.userId, taken);
    }
    return plan;
}
} // namespace Styx::Discord::Voice
