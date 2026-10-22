#pragma once

#include <QByteArray>

namespace Styx {
namespace Core {
namespace Stream {

/**
 * @brief Rewrites an H.264 SPS NAL unit to ensure VUI parameters exist and specify
 *        bitstream restriction with max_num_reorder_frames=0 and
 *        max_dec_frame_buffering=max_num_ref_frames.
 *
 * @param spsNal One SPS NAL unit including its 1-byte NAL header, without start code,
 *               in escaped (emulation-prevention) form.
 * @return The rewritten NAL unit in the same form, or an empty QByteArray if parsing fails.
 */
QByteArray rewriteSpsVui(const QByteArray &spsNal);

/**
 * @brief Rewrites an H.264 Annex B access unit, replacing all SPS NAL units (type 7)
 *        with rewritten SPS NALs carrying the required VUI bitstream restriction.
 *
 * @param annexB One access unit with 3- or 4-byte start codes.
 * @return The access unit with rewritten SPS (prefixed by 4-byte start codes),
 *         leaving other NALs and their start codes byte-identical.
 */
QByteArray rewriteAccessUnitSps(const QByteArray &annexB);

} // namespace Stream
} // namespace Core
} // namespace Styx
