#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ProtoReader.hpp"

namespace Styx {
namespace Proto {

class ProtoWriter
{
public:
    void writeVarint(uint32_t fieldNumber, uint64_t value);
    void writeInt32(uint32_t fieldNumber, int32_t value);
    void writeString(uint32_t fieldNumber, std::string_view value);
    void writeBytes(uint32_t fieldNumber, std::span<const uint8_t> value);
    void writeBytes(uint32_t fieldNumber, std::string_view value);
    void writeBytes(uint32_t fieldNumber, const std::vector<uint8_t> &value)
    {
        writeBytes(fieldNumber, std::span<const uint8_t>(value.data(), value.size()));
    }
    void writePackedVarints(uint32_t fieldNumber, std::span<const uint64_t> values);
    void writePackedVarints(uint32_t fieldNumber, const std::vector<uint64_t> &values)
    {
        writePackedVarints(fieldNumber, std::span<const uint64_t>(values.data(), values.size()));
    }

    const std::string &bytes() const { return buffer; }
    std::string takeBytes() { return std::move(buffer); }

private:
    void writeTag(uint32_t fieldNumber, WireType wireType);
    void writeRawVarint(uint64_t value);

    std::string buffer;
};

} // namespace Proto
} // namespace Styx
