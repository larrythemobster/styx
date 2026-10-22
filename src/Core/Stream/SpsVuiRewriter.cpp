#include "SpsVuiRewriter.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Styx {
namespace Core {
namespace Stream {

namespace {

class AnnexBBitstreamReader
{
public:
    AnnexBBitstreamReader(const uint8_t *data, size_t size)
        : m_data(data)
        , m_size(size)
    {
    }

    bool ok() const
    {
        return m_ok;
    }

    uint32_t readBits(int count)
    {
        if (count == 0) {
            return 0;
        }
        if (count < 0 || count > 32) {
            m_ok = false;
            return 0;
        }

        uint64_t result = 0;
        while (count > 0) {
            if (m_byteOffset >= m_size) {
                m_ok = false;
                return 0;
            }

            if (m_bitOffset == 0 &&
                m_byteOffset >= 2 &&
                m_data[m_byteOffset - 2] == 0 &&
                m_data[m_byteOffset - 1] == 0 &&
                m_data[m_byteOffset] == 3) {
                // Skip emulation prevention byte
                m_byteOffset++;
                if (m_byteOffset >= m_size) {
                    m_ok = false;
                    return 0;
                }
            }

            if (m_bitOffset == 0 && count >= 8) {
                result = (result << 8) | m_data[m_byteOffset++];
                count -= 8;
            } else {
                const int numBitsToRead = std::min(count, 8 - m_bitOffset);
                const uint32_t mask = (1u << numBitsToRead) - 1u;
                const uint32_t newBits =
                    (static_cast<uint32_t>(m_data[m_byteOffset]) >> (8 - m_bitOffset - numBitsToRead)) & mask;
                result = (result << numBitsToRead) | newBits;
                count -= numBitsToRead;
                m_bitOffset += numBitsToRead;
                if (m_bitOffset == 8) {
                    m_bitOffset = 0;
                    m_byteOffset++;
                }
            }
        }

        return static_cast<uint32_t>(result);
    }

    uint32_t readUnsigned(int bits)
    {
        return readBits(bits);
    }

    uint32_t readUnsignedExpGolomb()
    {
        int leadingZeros = 0;
        while (readBits(1) == 0) {
            if (!m_ok) {
                return 0;
            }
            ++leadingZeros;
            if (leadingZeros > 31) {
                m_ok = false;
                return 0;
            }
        }
        if (!m_ok) {
            return 0;
        }
        if (leadingZeros == 0) {
            return 0;
        }

        const uint32_t suffix = readBits(leadingZeros);
        if (!m_ok) {
            return 0;
        }
        return (1u << leadingZeros) - 1u + suffix;
    }

    int32_t readSignedExpGolomb()
    {
        const uint32_t unsignedVal = readUnsignedExpGolomb();
        if (!m_ok) {
            return 0;
        }
        if ((unsignedVal & 1u) == 0) {
            return -static_cast<int32_t>(unsignedVal / 2u);
        }
        return static_cast<int32_t>((unsignedVal + 1u) / 2u);
    }

private:
    const uint8_t *m_data = nullptr;
    size_t m_size = 0;
    size_t m_byteOffset = 0;
    int m_bitOffset = 0;
    bool m_ok = true;
};

class AnnexBBitstreamWriter
{
public:
    AnnexBBitstreamWriter() = default;

    void flush()
    {
        if (m_bitOffset == 0) {
            return;
        }

        // Check emulation prevention: 0x00, 0x00, followed by <= 0x03 requires 0x03 escape
        if (m_pendingByte <= 3 &&
            m_arr.size() >= 2 &&
            static_cast<uint8_t>(m_arr[m_arr.size() - 1]) == 0 &&
            static_cast<uint8_t>(m_arr[m_arr.size() - 2]) == 0) {
            m_arr.append(static_cast<char>(3));
        }

        m_arr.append(static_cast<char>(m_pendingByte));
        m_pendingByte = 0;
        m_bitOffset = 0;
    }

    void writeBits(uint64_t bits, int count)
    {
        while (count > 0) {
            if (m_bitOffset == 0) {
                if (count >= 8) {
                    m_pendingByte = static_cast<uint8_t>((bits >> (count - 8)) & 0xFF);
                    count -= 8;
                    m_bitOffset = 8;
                    flush();
                } else {
                    const uint64_t mask = (1ULL << count) - 1ULL;
                    m_pendingByte |= static_cast<uint8_t>((bits & mask) << (8 - count));
                    m_bitOffset = count;
                    count = 0;
                }
            } else {
                const int numBitsToWrite = std::min(8 - m_bitOffset, count);
                const uint64_t mask = (1ULL << numBitsToWrite) - 1ULL;
                const uint64_t bitsToWrite = (bits >> (count - numBitsToWrite)) & mask;
                m_pendingByte |= static_cast<uint8_t>(bitsToWrite << (8 - m_bitOffset - numBitsToWrite));
                count -= numBitsToWrite;
                m_bitOffset += numBitsToWrite;
                if (m_bitOffset == 8) {
                    flush();
                }
            }
        }
    }

    void writeUnsigned(uint32_t num, int count)
    {
        writeBits(num, count);
    }

    void writeUnsignedExpGolomb(uint32_t num)
    {
        const uint64_t val = static_cast<uint64_t>(num) + 1ULL;
        const int bitCount = std::bit_width(val);
        writeBits(0, bitCount - 1);
        writeBits(val, bitCount);
    }

    void writeSignedExpGolomb(int32_t num)
    {
        if (num <= 0) {
            writeUnsignedExpGolomb(static_cast<uint32_t>(-2 * static_cast<int64_t>(num)));
        } else {
            writeUnsignedExpGolomb(static_cast<uint32_t>(2 * static_cast<int64_t>(num) - 1));
        }
    }

    QByteArray toByteArray() const
    {
        return m_arr;
    }

private:
    QByteArray m_arr;
    uint8_t m_pendingByte = 0;
    int m_bitOffset = 0;
};

bool isHighProfile(uint32_t profileIdc)
{
    switch (profileIdc) {
    case 100: // High
    case 110: // High 10
    case 122: // High 4:2:2
    case 244: // High 4:4:4 Predictive
    case 44:  // CAVLC 4:4:4 Intra
    case 83:  // Scalable Baseline
    case 86:  // Scalable High
    case 118: // Stereo High
    case 128: // Multiview High
    case 138: // Multiview Depth High
    case 139:
    case 134:
    case 144: // High 4:4:4 Intra
        return true;
    default:
        return false;
    }
}

} // namespace

QByteArray rewriteSpsVui(const QByteArray &spsNal)
{
    if (spsNal.size() < 4) {
        return {};
    }

    const uint8_t nalHeader = static_cast<uint8_t>(spsNal[0]);
    if ((nalHeader & 0x1F) != 7) {
        return {};
    }

    const auto *payloadData = reinterpret_cast<const uint8_t *>(spsNal.constData() + 1);
    const size_t payloadSize = static_cast<size_t>(spsNal.size() - 1);

    AnnexBBitstreamReader reader(payloadData, payloadSize);
    AnnexBBitstreamWriter writer;

    // Rewrite the 1-byte NAL header
    writer.writeUnsigned(nalHeader, 8);

    const uint32_t profileIdc = reader.readUnsigned(8);
    writer.writeUnsigned(profileIdc, 8);

    const uint32_t constraintFlags = reader.readUnsigned(8);
    writer.writeUnsigned(constraintFlags, 8);

    const uint32_t levelIdc = reader.readUnsigned(8);
    writer.writeUnsigned(levelIdc, 8);

    const uint32_t seqParameterSetId = reader.readUnsignedExpGolomb();
    writer.writeUnsignedExpGolomb(seqParameterSetId);

    if (isHighProfile(profileIdc)) {
        const uint32_t chromaFormatIdc = reader.readUnsignedExpGolomb();
        writer.writeUnsignedExpGolomb(chromaFormatIdc);

        if (chromaFormatIdc == 3) {
            const uint32_t separateColourPlaneFlag = reader.readBits(1);
            writer.writeBits(separateColourPlaneFlag, 1);
        }

        const uint32_t bitDepthLumaMinus8 = reader.readUnsignedExpGolomb();
        writer.writeUnsignedExpGolomb(bitDepthLumaMinus8);

        const uint32_t bitDepthChromaMinus8 = reader.readUnsignedExpGolomb();
        writer.writeUnsignedExpGolomb(bitDepthChromaMinus8);

        const uint32_t qpprimeYZeroTransformBypassFlag = reader.readBits(1);
        writer.writeBits(qpprimeYZeroTransformBypassFlag, 1);

        const uint32_t seqScalingMatrixPresentFlag = reader.readBits(1);
        writer.writeBits(seqScalingMatrixPresentFlag, 1);

        if (seqScalingMatrixPresentFlag) {
            const int scalingCount = (chromaFormatIdc != 3) ? 8 : 12;
            for (int i = 0; i < scalingCount; ++i) {
                const uint32_t seqScalingListPresentFlag = reader.readBits(1);
                writer.writeBits(seqScalingListPresentFlag, 1);
                if (seqScalingListPresentFlag) {
                    const int size = (i < 6) ? 16 : 64;
                    int lastScale = 8;
                    int nextScale = 8;
                    for (int j = 0; j < size; ++j) {
                        if (nextScale != 0) {
                            const int32_t delta = reader.readSignedExpGolomb();
                            writer.writeSignedExpGolomb(delta);
                            nextScale = (lastScale + delta + 256) % 256;
                        }
                        if (nextScale != 0) {
                            lastScale = nextScale;
                        }
                    }
                }
            }
        }
    }

    const uint32_t log2MaxFrameNumMinus4 = reader.readUnsignedExpGolomb();
    writer.writeUnsignedExpGolomb(log2MaxFrameNumMinus4);

    const uint32_t picOrderCntType = reader.readUnsignedExpGolomb();
    writer.writeUnsignedExpGolomb(picOrderCntType);

    if (picOrderCntType == 0) {
        const uint32_t log2MaxPicOrderCntLsbMinus4 = reader.readUnsignedExpGolomb();
        writer.writeUnsignedExpGolomb(log2MaxPicOrderCntLsbMinus4);
    } else if (picOrderCntType == 1) {
        const uint32_t deltaPicOrderAlwaysZeroFlag = reader.readBits(1);
        writer.writeBits(deltaPicOrderAlwaysZeroFlag, 1);

        const int32_t offsetForNonRefPic = reader.readSignedExpGolomb();
        writer.writeSignedExpGolomb(offsetForNonRefPic);

        const int32_t offsetForTopToBottomField = reader.readSignedExpGolomb();
        writer.writeSignedExpGolomb(offsetForTopToBottomField);

        const uint32_t numRefFramesInPicOrderCntCycle = reader.readUnsignedExpGolomb();
        writer.writeUnsignedExpGolomb(numRefFramesInPicOrderCntCycle);

        for (uint32_t i = 0; i < numRefFramesInPicOrderCntCycle; ++i) {
            const int32_t offsetForRefFrame = reader.readSignedExpGolomb();
            writer.writeSignedExpGolomb(offsetForRefFrame);
        }
    }

    const uint32_t maxNumRefFrames = reader.readUnsignedExpGolomb();
    writer.writeUnsignedExpGolomb(maxNumRefFrames);

    const uint32_t gapsInFrameNumValueAllowedFlag = reader.readBits(1);
    writer.writeBits(gapsInFrameNumValueAllowedFlag, 1);

    const uint32_t picWidthInMbsMinus1 = reader.readUnsignedExpGolomb();
    writer.writeUnsignedExpGolomb(picWidthInMbsMinus1);

    const uint32_t picHeightInMapUnitsMinus1 = reader.readUnsignedExpGolomb();
    writer.writeUnsignedExpGolomb(picHeightInMapUnitsMinus1);

    const uint32_t frameMbsOnlyFlag = reader.readBits(1);
    writer.writeBits(frameMbsOnlyFlag, 1);

    if (frameMbsOnlyFlag == 0) {
        const uint32_t mbAdaptiveFrameFieldFlag = reader.readBits(1);
        writer.writeBits(mbAdaptiveFrameFieldFlag, 1);
    }

    const uint32_t direct8x8InferenceFlag = reader.readBits(1);
    writer.writeBits(direct8x8InferenceFlag, 1);

    const uint32_t frameCroppingFlag = reader.readBits(1);
    writer.writeBits(frameCroppingFlag, 1);

    if (frameCroppingFlag) {
        const uint32_t frameCropLeftOffset = reader.readUnsignedExpGolomb();
        writer.writeUnsignedExpGolomb(frameCropLeftOffset);

        const uint32_t frameCropRightOffset = reader.readUnsignedExpGolomb();
        writer.writeUnsignedExpGolomb(frameCropRightOffset);

        const uint32_t frameCropTopOffset = reader.readUnsignedExpGolomb();
        writer.writeUnsignedExpGolomb(frameCropTopOffset);

        const uint32_t frameCropBottomOffset = reader.readUnsignedExpGolomb();
        writer.writeUnsignedExpGolomb(frameCropBottomOffset);
    }

    auto addBitstreamRestriction = [&]() {
        // motion_vectors_over_pic_boundaries_flag: u(1), default is 1
        writer.writeBits(1, 1);
        // max_bytes_per_pic_denom: ue(v), default is 2
        writer.writeUnsignedExpGolomb(2);
        // max_bits_per_mb_denom: ue(v), default is 1
        writer.writeUnsignedExpGolomb(1);
        // log2_max_mv_length_horizontal: ue(v), default is 16
        writer.writeUnsignedExpGolomb(16);
        // log2_max_mv_length_vertical: ue(v), default is 16
        writer.writeUnsignedExpGolomb(16);
        // max_num_reorder_frames: ue(v) = 0
        writer.writeUnsignedExpGolomb(0);
        // max_dec_frame_buffering: ue(v) = maxNumRefFrames
        writer.writeUnsignedExpGolomb(maxNumRefFrames);
    };

    const uint32_t vuiParametersPresentFlag = reader.readBits(1);
    writer.writeBits(1, 1); // Always force VUI parameters present

    if (!vuiParametersPresentFlag) {
        // aspect_ratio_info_present_flag (1 bit), overscan_info_present_flag (1 bit)
        writer.writeBits(0, 2);

        // video_signal_type_present_flag (1 bit)
        writer.writeBits(0, 1);

        // chroma_loc_info_present_flag (1 bit), timing_info_present_flag (1 bit),
        // nal_hrd_parameters_present_flag (1 bit), vcl_hrd_parameters_present_flag (1 bit),
        // pic_struct_present_flag (1 bit)
        writer.writeBits(0, 5);

        // bitstream_restriction_flag (1 bit)
        writer.writeBits(1, 1);

        addBitstreamRestriction();
    } else {
        const uint32_t aspectRatioInfoPresentFlag = reader.readBits(1);
        writer.writeBits(aspectRatioInfoPresentFlag, 1);
        if (aspectRatioInfoPresentFlag) {
            const uint32_t aspectRatioIdc = reader.readUnsigned(8);
            writer.writeUnsigned(aspectRatioIdc, 8);
            if (aspectRatioIdc == 255) { // Extended_SAR
                const uint32_t sarWidth = reader.readUnsigned(16);
                writer.writeUnsigned(sarWidth, 16);
                const uint32_t sarHeight = reader.readUnsigned(16);
                writer.writeUnsigned(sarHeight, 16);
            }
        }

        const uint32_t overscanInfoPresentFlag = reader.readBits(1);
        writer.writeBits(overscanInfoPresentFlag, 1);
        if (overscanInfoPresentFlag) {
            const uint32_t overscanAppropriateFlag = reader.readBits(1);
            writer.writeBits(overscanAppropriateFlag, 1);
        }

        // Read video_signal_type if present, but do not copy it (write 0)
        const uint32_t videoSignalTypePresentFlag = reader.readBits(1);
        writer.writeBits(0, 1);
        if (videoSignalTypePresentFlag) {
            reader.readBits(3); // video_format
            reader.readBits(1); // video_full_range_flag
            const uint32_t colourDescriptionPresentFlag = reader.readBits(1);
            if (colourDescriptionPresentFlag) {
                reader.readUnsigned(8); // colour_primaries
                reader.readUnsigned(8); // transfer_characteristics
                reader.readUnsigned(8); // matrix_coefficients
            }
        }

        const uint32_t chromaLocInfoPresentFlag = reader.readBits(1);
        writer.writeBits(chromaLocInfoPresentFlag, 1);
        if (chromaLocInfoPresentFlag) {
            const uint32_t chromaSampleLocTypeTopField = reader.readUnsignedExpGolomb();
            writer.writeUnsignedExpGolomb(chromaSampleLocTypeTopField);
            const uint32_t chromaSampleLocTypeBottomField = reader.readUnsignedExpGolomb();
            writer.writeUnsignedExpGolomb(chromaSampleLocTypeBottomField);
        }

        const uint32_t timingInfoPresentFlag = reader.readBits(1);
        writer.writeBits(timingInfoPresentFlag, 1);
        if (timingInfoPresentFlag) {
            const uint32_t numUnitsInTick = reader.readUnsigned(32);
            writer.writeUnsigned(numUnitsInTick, 32);
            const uint32_t timeScale = reader.readUnsigned(32);
            writer.writeUnsigned(timeScale, 32);
            const uint32_t fixedFrameRateFlag = reader.readBits(1);
            writer.writeBits(fixedFrameRateFlag, 1);
        }

        auto copyHrdParameters = [&]() {
            const uint32_t cpbCntMinus1 = reader.readUnsignedExpGolomb();
            writer.writeUnsignedExpGolomb(cpbCntMinus1);
            const uint32_t bitRateScale = reader.readBits(4);
            writer.writeBits(bitRateScale, 4);
            const uint32_t cpbSizeScale = reader.readBits(4);
            writer.writeBits(cpbSizeScale, 4);
            for (uint32_t i = 0; i <= cpbCntMinus1; ++i) {
                const uint32_t bitRateValueMinus1 = reader.readUnsignedExpGolomb();
                writer.writeUnsignedExpGolomb(bitRateValueMinus1);
                const uint32_t cpbSizeValueMinus1 = reader.readUnsignedExpGolomb();
                writer.writeUnsignedExpGolomb(cpbSizeValueMinus1);
                const uint32_t cbrFlag = reader.readBits(1);
                writer.writeBits(cbrFlag, 1);
            }
            const uint32_t initialCpbRemovalDelayLengthMinus1 = reader.readBits(5);
            writer.writeBits(initialCpbRemovalDelayLengthMinus1, 5);
            const uint32_t cpbRemovalDelayLengthMinus1 = reader.readBits(5);
            writer.writeBits(cpbRemovalDelayLengthMinus1, 5);
            const uint32_t dpbOutputDelayLengthMinus1 = reader.readBits(5);
            writer.writeBits(dpbOutputDelayLengthMinus1, 5);
            const uint32_t timeOffsetLength = reader.readBits(5);
            writer.writeBits(timeOffsetLength, 5);
        };

        const uint32_t nalHrdParametersPresentFlag = reader.readBits(1);
        writer.writeBits(nalHrdParametersPresentFlag, 1);
        if (nalHrdParametersPresentFlag) {
            copyHrdParameters();
        }

        const uint32_t vclHrdParametersPresentFlag = reader.readBits(1);
        writer.writeBits(vclHrdParametersPresentFlag, 1);
        if (vclHrdParametersPresentFlag) {
            copyHrdParameters();
        }

        if (nalHrdParametersPresentFlag || vclHrdParametersPresentFlag) {
            const uint32_t lowDelayHrdFlag = reader.readBits(1);
            writer.writeBits(lowDelayHrdFlag, 1);
        }

        const uint32_t picStructPresentFlag = reader.readBits(1);
        writer.writeBits(picStructPresentFlag, 1);

        const uint32_t bitstreamRestrictionFlag = reader.readBits(1);
        writer.writeBits(1, 1); // Always force bitstream restriction present
        if (!bitstreamRestrictionFlag) {
            addBitstreamRestriction();
        } else {
            const uint32_t motionVectorsOverPicBoundariesFlag = reader.readBits(1);
            writer.writeBits(motionVectorsOverPicBoundariesFlag, 1);

            const uint32_t maxBytesPerPicDenom = reader.readUnsignedExpGolomb();
            writer.writeUnsignedExpGolomb(maxBytesPerPicDenom);

            const uint32_t maxBitsPerMbDenom = reader.readUnsignedExpGolomb();
            writer.writeUnsignedExpGolomb(maxBitsPerMbDenom);

            const uint32_t log2MaxMvLengthHorizontal = reader.readUnsignedExpGolomb();
            writer.writeUnsignedExpGolomb(log2MaxMvLengthHorizontal);

            const uint32_t log2MaxMvLengthVertical = reader.readUnsignedExpGolomb();
            writer.writeUnsignedExpGolomb(log2MaxMvLengthVertical);

            reader.readUnsignedExpGolomb(); // Discard original num_reorder_frames
            writer.writeUnsignedExpGolomb(0);

            reader.readUnsignedExpGolomb(); // Discard original max_dec_frame_buffering
            writer.writeUnsignedExpGolomb(maxNumRefFrames);
        }
    }

    writer.writeBits(1, 1); // rbsp_stop_one_bit
    writer.flush();

    if (!reader.ok()) {
        return {};
    }

    return writer.toByteArray();
}

QByteArray rewriteAccessUnitSps(const QByteArray &annexB)
{
    const auto *data = reinterpret_cast<const uint8_t *>(annexB.constData());
    const size_t size = static_cast<size_t>(annexB.size());
    if (size < 3) {
        return annexB;
    }

    struct NaluIndex
    {
        size_t startOffset = 0;
        size_t payloadStartOffset = 0;
        size_t payloadSize = 0;
    };

    std::vector<NaluIndex> sequences;
    const size_t end = size - 3;
    for (size_t i = 0; i <= end;) {
        if (data[i + 2] > 1) {
            i += 3;
        } else if (data[i + 2] == 1) {
            if (data[i + 1] == 0 && data[i] == 0) {
                NaluIndex index;
                index.startOffset = i;
                index.payloadStartOffset = i + 3;
                index.payloadSize = 0;
                if (index.startOffset > 0 && data[index.startOffset - 1] == 0) {
                    --index.startOffset;
                }
                if (!sequences.empty()) {
                    sequences.back().payloadSize = index.startOffset - sequences.back().payloadStartOffset;
                }
                sequences.push_back(index);
                i += 3;
            } else {
                ++i;
            }
        } else {
            ++i;
        }
    }

    if (sequences.empty()) {
        return annexB;
    }

    sequences.back().payloadSize = size - sequences.back().payloadStartOffset;

    QByteArray output;
    output.reserve(annexB.size() + 64);

    if (sequences[0].startOffset > 0) {
        output.append(annexB.constData(), static_cast<qsizetype>(sequences[0].startOffset));
    }

    static const char startCode4[4] = {0, 0, 0, 1};

    for (const auto &nalu : sequences) {
        const qsizetype startCodeLen = static_cast<qsizetype>(nalu.payloadStartOffset - nalu.startOffset);
        const char *startCodePtr = annexB.constData() + nalu.startOffset;
        const char *payloadPtr = annexB.constData() + nalu.payloadStartOffset;
        const qsizetype payloadLen = static_cast<qsizetype>(nalu.payloadSize);

        if (payloadLen <= 0) {
            output.append(startCodePtr, startCodeLen);
            continue;
        }

        const uint8_t nalType = static_cast<uint8_t>(payloadPtr[0]) & 0x1F;
        if (nalType == 7) {
            const QByteArray spsNal(payloadPtr, payloadLen);
            const QByteArray rewritten = rewriteSpsVui(spsNal);
            if (!rewritten.isEmpty()) {
                output.append(startCode4, 4);
                output.append(rewritten);
                continue;
            }
        }

        output.append(startCodePtr, startCodeLen);
        output.append(payloadPtr, payloadLen);
    }

    return output;
}

} // namespace Stream
} // namespace Core
} // namespace Styx
