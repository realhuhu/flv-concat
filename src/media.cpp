#include "flvconcat/media.hpp"

#include "flvconcat/codecs/registry.hpp"
#include "flvconcat/flv_probe.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <sstream>

namespace flvconcat {

bool probe_media(const std::filesystem::path& path, MediaInfo& info, std::string& error) {
    return probe_flv(path, info, error);
}

bool media_compatible(const MediaInfo& expected, const MediaInfo& actual, std::string& reason) {
    if (expected.width != actual.width || expected.height != actual.height) {
        reason = "resolution differs";
    } else if (expected.video_codec != actual.video_codec) {
        reason = "video codec differs";
    } else if (expected.audio_codec != actual.audio_codec) {
        reason = "audio codec differs";
    } else if (expected.sample_rate != actual.sample_rate) {
        reason = "audio sample rate differs";
    } else if (expected.audio_channels != actual.audio_channels) {
        reason = "audio channel count differs";
    } else {
        const auto* video = codecs::video_codec(expected.video_codec);
        const bool video_compatible =
            video && video->configuration_compatible
                ? video->configuration_compatible(expected.video_config, actual.video_config)
                : expected.video_config == actual.video_config;
        if (!video_compatible) {
            reason = video && !video->compatibility_reason.empty()
                         ? std::string(video->compatibility_reason)
                         : "video configuration differs";
            return false;
        }

        const auto* audio = codecs::audio_codec(expected.audio_codec);
        const bool audio_compatible =
            audio && audio->configuration_compatible
                ? audio->configuration_compatible(expected.audio_config, actual.audio_config)
                : expected.audio_config == actual.audio_config;
        if (!audio_compatible) {
            reason = audio && !audio->compatibility_reason.empty()
                         ? std::string(audio->compatibility_reason)
                         : "audio configuration differs";
            return false;
        }
        reason.clear();
        return true;
    }
    return false;
}

std::string media_summary(const MediaInfo& info) {
    std::ostringstream output;
    output << info.width << 'x' << info.height << ' '
           << avcodec_get_name(static_cast<AVCodecID>(info.video_codec)) << '/'
           << avcodec_get_name(static_cast<AVCodecID>(info.audio_codec)) << ' '
           << info.sample_rate << " Hz " << info.audio_channels << " ch";
    return output.str();
}

} // namespace flvconcat
