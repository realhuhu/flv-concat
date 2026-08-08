#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace flvconcat::codecs {

using ByteVector = std::vector<std::uint8_t>;

struct VideoConfiguration {
    int width = 0;
    int height = 0;
    std::uint8_t nal_length_size = 0;
};

struct VideoCodecDescriptor {
    int codec_id = 0;
    int flv_codec_id = 0;
    std::string_view name;
    std::string_view compatibility_reason;
    bool (*parse_configuration)(const ByteVector&, VideoConfiguration&) = nullptr;
    bool (*configuration_compatible)(const ByteVector&, const ByteVector&) = nullptr;
};

struct AudioConfiguration {
    int sample_rate = 0;
    int channels = 0;
};

struct AudioCodecDescriptor {
    int codec_id = 0;
    int flv_sound_format = 0;
    std::string_view name;
    std::string_view compatibility_reason;
    bool (*parse_configuration)(const ByteVector&, AudioConfiguration&) = nullptr;
    bool (*configuration_compatible)(const ByteVector&, const ByteVector&) = nullptr;
};

// FLV tag values are intentionally resolved here instead of in the scanner. A
// new video/audio format only needs a descriptor and a registry entry; the
// timestamp and muxing code does not need another codec-specific branch.
const VideoCodecDescriptor* video_codec_for_flv(int flv_codec_id);
const VideoCodecDescriptor* video_codec(int codec_id);
const AudioCodecDescriptor* audio_codec_for_flv(int flv_sound_format);
const AudioCodecDescriptor* audio_codec(int codec_id);

} // namespace flvconcat::codecs
