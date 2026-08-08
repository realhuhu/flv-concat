#include "descriptors.hpp"

#include "bit_reader.hpp"

extern "C" {
#include <libavcodec/codec_id.h>
}

#include <array>
#include <cstdint>

namespace flvconcat::codecs::internal {
namespace {

bool read_audio_object_type(BitReader& reader, std::uint32_t& object_type) {
    if (!reader.read(5, object_type)) {
        return false;
    }
    if (object_type != 31) {
        return true;
    }
    std::uint32_t extension = 0;
    if (!reader.read(6, extension)) {
        return false;
    }
    object_type = 32 + extension;
    return true;
}

bool read_sampling_frequency(BitReader& reader, std::uint32_t& frequency) {
    static constexpr std::array<std::uint32_t, 13> frequencies = {
        96'000, 88'200, 64'000, 48'000, 44'100, 32'000, 24'000,
        22'050, 16'000, 12'000, 11'025, 8'000, 7'350};

    std::uint32_t index = 0;
    if (!reader.read(4, index)) {
        return false;
    }
    if (index == 15) {
        return reader.read(24, frequency);
    }
    if (index >= frequencies.size()) {
        return false;
    }
    frequency = frequencies[index];
    return true;
}

bool is_ga_audio_object_type(std::uint32_t object_type) {
    switch (object_type) {
        case 1:
        case 2:
        case 3:
        case 4:
        case 6:
        case 7:
        case 17:
        case 19:
        case 20:
        case 21:
        case 22:
        case 23:
            return true;
        default:
            return false;
    }
}

struct AacConfiguration {
    std::uint32_t signalling_object_type = 0;
    std::uint32_t core_object_type = 0;
    std::uint32_t sampling_frequency = 0;
    std::uint32_t extension_sampling_frequency = 0;
    std::uint32_t channel_configuration = 0;
    bool frame_length_960 = false;
    bool sync_sbr_present = false;
    std::uint32_t sync_sbr_sampling_frequency = 0;
};

bool parse_aac_configuration(const ByteVector& bytes, AacConfiguration& configuration) {
    if (bytes.empty()) {
        return false;
    }
    BitReader reader(bytes.data(), bytes.size());
    configuration = {};
    if (!read_audio_object_type(reader, configuration.signalling_object_type) ||
        !read_sampling_frequency(reader, configuration.sampling_frequency) ||
        !reader.read(4, configuration.channel_configuration) ||
        configuration.channel_configuration == 0 || configuration.channel_configuration > 7) {
        return false;
    }

    configuration.core_object_type = configuration.signalling_object_type;
    if (configuration.signalling_object_type == 5 || configuration.signalling_object_type == 29) {
        if (!read_sampling_frequency(reader, configuration.extension_sampling_frequency) ||
            !read_audio_object_type(reader, configuration.core_object_type)) {
            return false;
        }
    }
    if (!is_ga_audio_object_type(configuration.core_object_type)) {
        return false;
    }

    std::uint32_t frame_length_flag = 0;
    std::uint32_t depends_on_core_coder = 0;
    std::uint32_t extension_flag = 0;
    if (!reader.read(1, frame_length_flag) || !reader.read(1, depends_on_core_coder)) {
        return false;
    }
    if (depends_on_core_coder != 0) {
        std::uint32_t core_coder_delay = 0;
        if (!reader.read(14, core_coder_delay)) {
            return false;
        }
    }
    if (!reader.read(1, extension_flag) || extension_flag != 0) {
        return false;
    }
    configuration.frame_length_960 = frame_length_flag != 0;

    // A sync extension explicitly stating that SBR is absent is metadata-only
    // and equivalent to omitting the extension altogether.
    if (reader.remaining() >= 17) {
        std::uint32_t sync_extension_type = 0;
        if (!reader.read(11, sync_extension_type) || sync_extension_type != 0x2B7) {
            return true;
        }
        std::uint32_t extension_object_type = 0;
        if (!read_audio_object_type(reader, extension_object_type) || extension_object_type != 5) {
            return true;
        }
        std::uint32_t sbr_present = 0;
        if (!reader.read(1, sbr_present)) {
            return false;
        }
        if (sbr_present != 0) {
            configuration.sync_sbr_present = true;
            if (!read_sampling_frequency(reader, configuration.sync_sbr_sampling_frequency)) {
                return false;
            }
        }
    }
    return true;
}

bool parse_configuration(const ByteVector& bytes, AudioConfiguration& configuration) {
    AacConfiguration parsed;
    if (!parse_aac_configuration(bytes, parsed)) {
        return false;
    }
    configuration.sample_rate = static_cast<int>(parsed.sampling_frequency);
    configuration.channels = static_cast<int>(parsed.channel_configuration);
    return configuration.sample_rate > 0 && configuration.channels > 0;
}

bool configurations_compatible(const ByteVector& expected, const ByteVector& actual) {
    if (expected == actual) {
        return true;
    }
    AacConfiguration first;
    AacConfiguration second;
    return parse_aac_configuration(expected, first) && parse_aac_configuration(actual, second) &&
           first.signalling_object_type == second.signalling_object_type &&
           first.core_object_type == second.core_object_type &&
           first.sampling_frequency == second.sampling_frequency &&
           first.extension_sampling_frequency == second.extension_sampling_frequency &&
           first.channel_configuration == second.channel_configuration &&
           first.frame_length_960 == second.frame_length_960 &&
           first.sync_sbr_present == second.sync_sbr_present &&
           first.sync_sbr_sampling_frequency == second.sync_sbr_sampling_frequency;
}

} // namespace

const AudioCodecDescriptor& aac_descriptor() {
    static const AudioCodecDescriptor descriptor{
        AV_CODEC_ID_AAC, 10, "AAC",
        "AAC object type, sample rate, channel layout, or frame length differs",
        parse_configuration,
        configurations_compatible};
    return descriptor;
}

} // namespace flvconcat::codecs::internal
