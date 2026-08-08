#pragma once

#include "flvconcat/codecs/registry.hpp"

namespace flvconcat::codecs::internal {

const VideoCodecDescriptor& h264_descriptor();
const VideoCodecDescriptor& hevc_descriptor();
const AudioCodecDescriptor& aac_descriptor();

} // namespace flvconcat::codecs::internal
