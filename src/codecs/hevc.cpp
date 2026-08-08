#include "descriptors.hpp"

#include "bit_reader.hpp"

extern "C" {
#include <libavcodec/codec_id.h>
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace flvconcat::codecs::internal {
namespace {

struct HevcConfiguration {
    std::uint8_t nal_length_size = 0;
    std::vector<std::vector<std::uint8_t>> video_parameter_sets;
    std::vector<std::vector<std::uint8_t>> sequence_parameter_sets;
    std::vector<std::vector<std::uint8_t>> picture_parameter_sets;
};

bool read_be16(const ByteVector& bytes, std::size_t& offset, std::uint16_t& value) {
    if (offset + 2 > bytes.size()) {
        return false;
    }
    value = static_cast<std::uint16_t>(bytes[offset] << 8U) | bytes[offset + 1];
    offset += 2;
    return true;
}

bool read_nal_array(const ByteVector& bytes,
                   std::size_t& offset,
                   std::size_t count,
                   std::vector<std::vector<std::uint8_t>>& destination) {
    destination.clear();
    destination.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        std::uint16_t length = 0;
        if (!read_be16(bytes, offset, length) || length == 0 || offset + length > bytes.size()) {
            return false;
        }
        destination.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                 bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
    }
    std::sort(destination.begin(), destination.end());
    return true;
}

bool parse_hvcc_configuration(const ByteVector& bytes, HevcConfiguration& configuration) {
    // HEVCDecoderConfigurationRecord: the array payloads are the decoder's
    // VPS/SPS/PPS state. Header fields which are repeated in the NAL units are
    // deliberately not used as a byte-for-byte compatibility key.
    if (bytes.size() < 23 || bytes[0] != 1) {
        return false;
    }
    configuration = {};
    configuration.nal_length_size = static_cast<std::uint8_t>((bytes[21] & 0x03U) + 1U);
    const auto array_count = static_cast<std::size_t>(bytes[22]);
    std::size_t offset = 23;
    bool got_sps = false;
    for (std::size_t index = 0; index < array_count; ++index) {
        if (offset + 3 > bytes.size()) {
            return false;
        }
        const auto nal_type = static_cast<std::uint8_t>(bytes[offset] & 0x3FU);
        offset += 1;
        std::uint16_t nal_count = 0;
        if (!read_be16(bytes, offset, nal_count)) {
            return false;
        }
        std::vector<std::vector<std::uint8_t>>* destination = nullptr;
        if (nal_type == 32) {
            destination = &configuration.video_parameter_sets;
        } else if (nal_type == 33) {
            destination = &configuration.sequence_parameter_sets;
            got_sps = true;
        } else if (nal_type == 34) {
            destination = &configuration.picture_parameter_sets;
        }
        if (destination) {
            std::vector<std::vector<std::uint8_t>> array;
            if (!read_nal_array(bytes, offset, nal_count, array)) {
                return false;
            }
            destination->insert(destination->end(), array.begin(), array.end());
        } else {
            for (std::uint16_t nal = 0; nal < nal_count; ++nal) {
                std::uint16_t length = 0;
                if (!read_be16(bytes, offset, length) || length == 0 || offset + length > bytes.size()) {
                    return false;
                }
                offset += length;
            }
        }
    }
    if (!got_sps || configuration.sequence_parameter_sets.empty()) {
        return false;
    }
    std::sort(configuration.video_parameter_sets.begin(), configuration.video_parameter_sets.end());
    std::sort(configuration.sequence_parameter_sets.begin(), configuration.sequence_parameter_sets.end());
    std::sort(configuration.picture_parameter_sets.begin(), configuration.picture_parameter_sets.end());
    return true;
}

ByteVector nal_unescape(const std::uint8_t* nal, std::size_t length) {
    ByteVector result;
    result.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        if (index + 2 < length && nal[index] == 0 && nal[index + 1] == 0 && nal[index + 2] == 3) {
            result.push_back(0);
            result.push_back(0);
            index += 2;
        } else {
            result.push_back(nal[index]);
        }
    }
    return result;
}

bool hevc_sps_dimensions(const std::uint8_t* sps,
                         std::size_t length,
                         int& width,
                         int& height) {
    const auto raw = nal_unescape(sps, length);
    if (raw.size() < 4) {
        return false;
    }
    internal::BitReader reader(raw.data(), raw.size());
    std::uint32_t ignored = 0;
    std::uint32_t max_sub_layers_minus1 = 0;
    if (!reader.skip(16) || !reader.skip(4) || !reader.read(3, max_sub_layers_minus1) ||
        !reader.skip(1) || !reader.skip(96)) {
        return false;
    }

    if (max_sub_layers_minus1 > 0) {
        std::uint32_t profile_present[7] = {};
        std::uint32_t level_present[7] = {};
        for (std::uint32_t index = 0; index < max_sub_layers_minus1; ++index) {
            if (!reader.read(1, profile_present[index]) ||
                !reader.read(1, level_present[index])) {
                return false;
            }
        }
        if (!reader.skip(2U * (8U - max_sub_layers_minus1))) {
            return false;
        }
        for (std::uint32_t index = 0; index < max_sub_layers_minus1; ++index) {
            if (profile_present[index] != 0 && !reader.skip(88)) {
                return false;
            }
            if (level_present[index] != 0 && !reader.skip(8)) {
                return false;
            }
        }
    }

    std::uint32_t ignored_ue = 0;
    std::uint32_t chroma_format_idc = 0;
    if (!reader.read_unsigned_exp_golomb(ignored_ue) ||
        !reader.read_unsigned_exp_golomb(chroma_format_idc)) {
        return false;
    }
    if (chroma_format_idc == 3 && !reader.read(1, ignored)) {
        return false;
    }

    std::uint32_t coded_width = 0;
    std::uint32_t coded_height = 0;
    if (!reader.read_unsigned_exp_golomb(coded_width) ||
        !reader.read_unsigned_exp_golomb(coded_height)) {
        return false;
    }

    std::uint32_t conformance_window_flag = 0;
    if (!reader.read(1, conformance_window_flag)) {
        return false;
    }
    std::uint32_t left = 0;
    std::uint32_t right = 0;
    std::uint32_t top = 0;
    std::uint32_t bottom = 0;
    if (conformance_window_flag != 0 &&
        (!reader.read_unsigned_exp_golomb(left) || !reader.read_unsigned_exp_golomb(right) ||
         !reader.read_unsigned_exp_golomb(top) || !reader.read_unsigned_exp_golomb(bottom))) {
        return false;
    }

    std::uint32_t sub_width_c = 1;
    std::uint32_t sub_height_c = 1;
    if (chroma_format_idc == 1) {
        sub_width_c = 2;
        sub_height_c = 2;
    } else if (chroma_format_idc == 2) {
        sub_width_c = 2;
    }
    const auto crop_width = sub_width_c * (left + right);
    const auto crop_height = sub_height_c * (top + bottom);
    if (coded_width <= crop_width || coded_height <= crop_height) {
        return false;
    }
    width = static_cast<int>(coded_width - crop_width);
    height = static_cast<int>(coded_height - crop_height);
    return true;
}

bool parse_configuration(const ByteVector& bytes, VideoConfiguration& configuration) {
    HevcConfiguration parsed;
    if (!parse_hvcc_configuration(bytes, parsed) || parsed.sequence_parameter_sets.empty()) {
        return false;
    }
    if (!hevc_sps_dimensions(parsed.sequence_parameter_sets.front().data(),
                             parsed.sequence_parameter_sets.front().size(),
                             configuration.width,
                             configuration.height)) {
        return false;
    }
    configuration.nal_length_size = parsed.nal_length_size;
    return true;
}

bool configurations_compatible(const ByteVector& expected, const ByteVector& actual) {
    if (expected == actual) {
        return true;
    }
    HevcConfiguration first;
    HevcConfiguration second;
    return parse_hvcc_configuration(expected, first) && parse_hvcc_configuration(actual, second) &&
           first.nal_length_size == second.nal_length_size &&
           first.video_parameter_sets == second.video_parameter_sets &&
           first.sequence_parameter_sets == second.sequence_parameter_sets &&
           first.picture_parameter_sets == second.picture_parameter_sets;
}

} // namespace

const VideoCodecDescriptor& hevc_descriptor() {
    static const VideoCodecDescriptor descriptor{
        AV_CODEC_ID_HEVC, 12, "H.265", "H.265 VPS/SPS/PPS or NAL length differs",
        parse_configuration, configurations_compatible};
    return descriptor;
}

} // namespace flvconcat::codecs::internal
