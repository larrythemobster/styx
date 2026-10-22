#include "ProtoWriter.hpp"

namespace Styx {
namespace Proto {

void ProtoWriter::writeRawVarint(uint64_t value)
{
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
        if (value)
            byte |= 0x80;
        buffer.push_back(byte);
    } while (value);
}

void ProtoWriter::writeTag(uint32_t fieldNumber, WireType wireType)
{
    writeRawVarint((static_cast<uint64_t>(fieldNumber) << 3) | static_cast<uint64_t>(wireType));
}

void ProtoWriter::writeVarint(uint32_t fieldNumber, uint64_t value)
{
    writeTag(fieldNumber, WireType::VARINT);
    writeRawVarint(value);
}

void ProtoWriter::writeInt32(uint32_t fieldNumber, int32_t value)
{
    // protobuf sign-extends negative int32 to 64 bits, giving a 10 byte varint
    writeVarint(fieldNumber, static_cast<uint64_t>(static_cast<int64_t>(value)));
}

void ProtoWriter::writeString(uint32_t fieldNumber, std::string_view value)
{
    writeBytes(fieldNumber, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(value.data()), value.size()));
}

void ProtoWriter::writeBytes(uint32_t fieldNumber, std::span<const uint8_t> value)
{
    writeTag(fieldNumber, WireType::LENGTH_DELIMITED);
    writeRawVarint(static_cast<uint64_t>(value.size()));
    buffer.insert(buffer.end(), value.begin(), value.end());
}

void ProtoWriter::writePackedVarints(uint32_t fieldNumber, std::span<const uint64_t> values)
{
    if (values.empty())
        return;

    ProtoWriter packed;
    for (uint64_t value : values)
        packed.writeRawVarint(value);

    writeBytes(fieldNumber, packed.bytes());
}

} // namespace Proto
} // namespace Styx
