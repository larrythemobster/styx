// Ported from ouwou/Acheron video-streams commit 71b4907 (GPL-3.0-or-later).
#pragma once

#include "Core/Video/VideoTypes.hpp"

#include <cstring>
#include <string>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

namespace Styx::Core::Video {
inline std::string softwareScaling(const FrameSize &size, bool bt601, int format) {
    std::string scaling = "scale=w=" + std::to_string(size.width) + ":h=" + std::to_string(size.height) + ":flags=bilinear";
    if (format != AV_PIX_FMT_BGRA) scaling += std::string(":out_range=tv:out_color_matrix=") + (bt601 ? "bt601" : "bt709");
    const char *name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(format));
    return scaling + ",format=" + (name ? name : "none");
}
inline int assemble(const std::string &chain, AVBufferRef *device, AVFilterGraph **graph, AVFilterContext **sink) {
    *graph = avfilter_graph_alloc();
    if (!*graph) return AVERROR(ENOMEM);
    int result = avfilter_graph_create_filter(sink, avfilter_get_by_name("buffersink"), "sink", nullptr, nullptr, *graph);
    AVFilterInOut *inputs = nullptr;
    AVFilterInOut *outputs = nullptr;
    if (result >= 0) {
        result = avfilter_graph_parse2(*graph, chain.c_str(), &inputs, &outputs);
        if (result >= 0)
            result = outputs && !outputs->next && !inputs
                ? avfilter_link(outputs->filter_ctx, static_cast<unsigned>(outputs->pad_idx), *sink, 0)
                : AVERROR(EINVAL);
    }
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
#ifdef AVFILTER_FLAG_HWDEVICE
    for (unsigned i = 0; device && result >= 0 && i < (*graph)->nb_filters; ++i)
        if ((*graph)->filters[i]->filter->flags & AVFILTER_FLAG_HWDEVICE)
            (*graph)->filters[i]->hw_device_ctx = av_buffer_ref(device);
#else
    if (device && result >= 0) result = AVERROR(ENOSYS);
#endif
    if (result >= 0) result = avfilter_graph_config(*graph, nullptr);
    if (result < 0) { avfilter_graph_free(graph); *sink = nullptr; }
    return result;
}
class PictureConverter {
public:
    PictureConverter() = default;
    ~PictureConverter() { avfilter_graph_free(&graph); }
    PictureConverter(const PictureConverter &) = delete;
    PictureConverter &operator=(const PictureConverter &) = delete;
    int convert(AVFrame *picture, const FrameSize &size, int format, bool bt601, AVFrame *into) {
        const FrameSize from{picture->width, picture->height};
        if (!graph || from != builtFrom || picture->format != builtFromFormat || size != builtSize || format != builtFormat || bt601 != builtBt601) {
            avfilter_graph_free(&graph); source = nullptr;
            const char *input = av_get_pix_fmt_name(static_cast<AVPixelFormat>(picture->format));
            const auto chain = "buffer=video_size=" + std::to_string(from.width) + "x" + std::to_string(from.height) + ":pix_fmt=" + (input ? input : "none") + ":time_base=1/90000:pixel_aspect=1/1," + softwareScaling(size, bt601, format);
            const int built = assemble(chain, nullptr, &graph, &sink);
            if (built < 0) return built;
            for (unsigned i = 0; i < graph->nb_filters && !source; ++i)
                if (std::strcmp(graph->filters[i]->filter->name, "buffer") == 0) source = graph->filters[i];
            builtFrom = from; builtFromFormat = picture->format; builtSize = size; builtFormat = format; builtBt601 = bt601;
        }
        picture->pts = converted++;
        const int added = av_buffersrc_add_frame_flags(source, picture, AV_BUFFERSRC_FLAG_KEEP_REF);
        return added < 0 ? added : av_buffersink_get_frame(sink, into);
    }
private:
    AVFilterGraph *graph = nullptr;
    AVFilterContext *source = nullptr;
    AVFilterContext *sink = nullptr;
    FrameSize builtFrom{};
    int builtFromFormat = -1;
    FrameSize builtSize{};
    int builtFormat = -1;
    bool builtBt601 = false;
    int64_t converted = 0;
};
} // namespace Styx::Core::Video
