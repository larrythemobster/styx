#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Styx {
namespace Proto {

enum class WireType : uint8_t {
    VARINT = 0,
    FIXED64 = 1,
    LENGTH_DELIMITED = 2,
    START_GROUP = 3,
    END_GROUP = 4,
    FIXED32 = 5
};

struct Tag
{
    uint32_t fieldNumber;
    WireType wireType;
};

class ProtoReader
{
public:
    explicit ProtoReader(std::span<const uint8_t> data);
    explicit ProtoReader(const std::vector<uint8_t> &data) : ProtoReader(std::span<const uint8_t>(data.data(), data.size())) {}
    explicit ProtoReader(std::string_view data) : ProtoReader(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(data.data()), data.size())) {}

    bool readTag(Tag &tag);
    bool atEnd() const;

    bool readVarint(uint64_t &value);
    bool readFixed64(uint64_t &value);
    bool readLengthDelimited(std::vector<uint8_t> &value);
    bool readLengthDelimited(std::span<const uint8_t> &value);
    bool readFixed32(uint32_t &value);

    bool skipField(WireType wireType);

    size_t position() const { return pos; }
    size_t remaining() const { return data.size() - pos; }

private:
    const std::span<const uint8_t> data;
    size_t pos = 0;

    bool readByte(uint8_t &byte);
    bool readBytes(void *dest, size_t count);
};

std::string readString(ProtoReader &reader);
std::optional<int64_t> readInt64Value(ProtoReader &reader);
std::optional<std::string> readStringValue(ProtoReader &reader);
std::optional<uint64_t> readUInt64Value(ProtoReader &reader);
bool readBoolValue(ProtoReader &reader);

} // namespace Proto
} // namespace Styx
