#pragma once

#include "flvconcat/model.hpp"

#include <cstddef>
#include <cstdint>

namespace flvconcat {

// Parses an FLV ScriptData payload containing onMetaData. Malformed or
// unrelated ScriptData is ignored by returning false. Existing entries win so
// the earliest metadata remains authoritative when a recorder emits updates.
bool parse_amf0_script_metadata(const std::uint8_t* data,
                                std::size_t size,
                                MetadataMap& metadata);

} // namespace flvconcat
