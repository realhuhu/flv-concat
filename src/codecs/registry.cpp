#include "descriptors.hpp"

extern "C" {
#include <libavcodec/codec_id.h>
}

namespace flvconcat::codecs {

const VideoCodecDescriptor* video_codec_for_flv(int flv_codec_id) {
    switch (flv_codec_id) {
        case 7:
            return &internal::h264_descriptor();
        case 12:
            return &internal::hevc_descriptor();
        default:
            return nullptr;
    }
}

const VideoCodecDescriptor* video_codec(int codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_H264:
            return &internal::h264_descriptor();
        case AV_CODEC_ID_HEVC:
            return &internal::hevc_descriptor();
        default:
            return nullptr;
    }
}

const AudioCodecDescriptor* audio_codec_for_flv(int flv_sound_format) {
    return flv_sound_format == 10 ? &internal::aac_descriptor() : nullptr;
}

const AudioCodecDescriptor* audio_codec(int codec_id) {
    return codec_id == AV_CODEC_ID_AAC ? &internal::aac_descriptor() : nullptr;
}

} // namespace flvconcat::codecs
