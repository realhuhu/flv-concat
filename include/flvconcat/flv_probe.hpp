#pragma once

#include "flvconcat/model.hpp"

#include <filesystem>
#include <string>

namespace flvconcat {

// Probe legacy and enhanced FLV sequence headers without asking libavformat to
// decode the stream. Some recorders write HEVC as legacy FLV codec_id=12,
// which FFmpeg intentionally reports as an unknown codec.
bool probe_flv(const std::filesystem::path& path, MediaInfo& info, std::string& error);

} // namespace flvconcat
