#include "descriptors.hpp"

#include "bit_reader.hpp"

extern "C" {
#include <libavcodec/codec_id.h>
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace flvconcat::codecs::internal {
namespace {

struct HevcConfiguration {
    std::uint8_t nal_length_size = 0;
    std::vector<std::vector<std::uint8_t>> video_parameter_sets;
    std::vector<std::vector<std::uint8_t>> sequence_parameter_sets;
    std::vector<std::vector<std::uint8_t>> picture_parameter_sets;
};

// A live encoder may regenerate VPS/SPS/PPS when a new segment starts. The
// payload can therefore differ even though both streams have the same format
// and can be remuxed with the first stream's decoder configuration. Keep only
// decoder-facing SPS fields here; reference-picture and rate-control defaults
// are intentionally not part of the merge key (matching the original tool).
struct HevcSpsFormat {
    std::array<std::uint8_t, 2> nal_header{};
    std::uint32_t profile_space = 0;
    std::uint32_t tier_flag = 0;
    std::uint32_t profile_idc = 0;
    std::uint32_t profile_compatibility = 0;
    std::uint64_t constraint_flags = 0;
    std::uint32_t level_idc = 0;
    std::uint32_t sps_id = 0;
    std::uint32_t chroma_format_idc = 0;
    std::uint32_t separate_colour_plane_flag = 0;
    std::uint32_t bit_depth_luma_minus8 = 0;
    std::uint32_t bit_depth_chroma_minus8 = 0;
    int width = 0;
    int height = 0;
};

bool operator==(const HevcSpsFormat& left, const HevcSpsFormat& right) {
    return left.nal_header == right.nal_header && left.profile_space == right.profile_space &&
           left.tier_flag == right.tier_flag && left.profile_idc == right.profile_idc &&
           left.profile_compatibility == right.profile_compatibility &&
           left.constraint_flags == right.constraint_flags && left.level_idc == right.level_idc &&
           left.sps_id == right.sps_id && left.chroma_format_idc == right.chroma_format_idc &&
           left.separate_colour_plane_flag == right.separate_colour_plane_flag &&
           left.bit_depth_luma_minus8 == right.bit_depth_luma_minus8 &&
           left.bit_depth_chroma_minus8 == right.bit_depth_chroma_minus8 &&
           left.width == right.width && left.height == right.height;
}

bool operator<(const HevcSpsFormat& left, const HevcSpsFormat& right) {
    if (left.sps_id != right.sps_id) {
        return left.sps_id < right.sps_id;
    }
    return left.nal_header < right.nal_header;
}

struct HevcPpsIdentity {
    std::array<std::uint8_t, 2> nal_header{};
    std::uint32_t pps_id = 0;
    std::uint32_t sps_id = 0;
};

bool operator==(const HevcPpsIdentity& left, const HevcPpsIdentity& right) {
    return left.nal_header == right.nal_header && left.pps_id == right.pps_id &&
           left.sps_id == right.sps_id;
}

bool operator<(const HevcPpsIdentity& left, const HevcPpsIdentity& right) {
    if (left.pps_id != right.pps_id) {
        return left.pps_id < right.pps_id;
    }
    return left.nal_header < right.nal_header;
}

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
    // HEVCDecoderConfigurationRecord. Optional hvcC header fields are not a
    // byte-for-byte compatibility key; the NAL units below are validated by
    // the semantic compatibility check.
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

bool parse_hevc_sps_format(const ByteVector& sps, HevcSpsFormat& format) {
    if (sps.size() < 3) {
        return false;
    }
    format = {};
    format.nal_header = {sps[0], sps[1]};

    const auto raw = nal_unescape(sps.data(), sps.size());
    if (raw.size() < 4) {
        return false;
    }
    internal::BitReader reader(raw.data(), raw.size());
    std::uint32_t max_sub_layers_minus1 = 0;
    if (!reader.skip(16) || !reader.skip(4) || !reader.read(3, max_sub_layers_minus1) ||
        max_sub_layers_minus1 > 6 || !reader.skip(1) ||
        !reader.read(2, format.profile_space) || !reader.read(1, format.tier_flag) ||
        !reader.read(5, format.profile_idc) ||
        !reader.read(32, format.profile_compatibility)) {
        return false;
    }

    std::uint32_t constraint_high = 0;
    std::uint32_t constraint_low = 0;
    if (!reader.read(16, constraint_high) || !reader.read(32, constraint_low) ||
        !reader.read(8, format.level_idc)) {
        return false;
    }
    format.constraint_flags = (static_cast<std::uint64_t>(constraint_high) << 32U) |
                              static_cast<std::uint64_t>(constraint_low);

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

    if (!reader.read_unsigned_exp_golomb(format.sps_id) ||
        !reader.read_unsigned_exp_golomb(format.chroma_format_idc)) {
        return false;
    }
    if (format.chroma_format_idc == 3 &&
        !reader.read(1, format.separate_colour_plane_flag)) {
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
    if (format.chroma_format_idc == 1) {
        sub_width_c = 2;
        sub_height_c = 2;
    } else if (format.chroma_format_idc == 2) {
        sub_width_c = 2;
    }
    const auto crop_width = static_cast<std::uint64_t>(sub_width_c) * (left + right);
    const auto crop_height = static_cast<std::uint64_t>(sub_height_c) * (top + bottom);
    if (coded_width <= crop_width || coded_height <= crop_height ||
        coded_width - crop_width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        coded_height - crop_height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    format.width = static_cast<int>(coded_width - crop_width);
    format.height = static_cast<int>(coded_height - crop_height);
    return reader.read_unsigned_exp_golomb(format.bit_depth_luma_minus8) &&
           reader.read_unsigned_exp_golomb(format.bit_depth_chroma_minus8);
}

bool collect_sps_formats(const HevcConfiguration& configuration,
                         std::vector<HevcSpsFormat>& formats) {
    formats.clear();
    formats.reserve(configuration.sequence_parameter_sets.size());
    for (const auto& sps : configuration.sequence_parameter_sets) {
        HevcSpsFormat format;
        if (!parse_hevc_sps_format(sps, format)) {
            return false;
        }
        formats.push_back(format);
    }
    std::sort(formats.begin(), formats.end());
    return !formats.empty();
}

bool collect_pps_identities(const HevcConfiguration& configuration,
                            std::vector<HevcPpsIdentity>& identities) {
    identities.clear();
    identities.reserve(configuration.picture_parameter_sets.size());
    for (const auto& pps : configuration.picture_parameter_sets) {
        if (pps.size() < 3) {
            return false;
        }
        const auto raw = nal_unescape(pps.data(), pps.size());
        internal::BitReader reader(raw.data(), raw.size());
        HevcPpsIdentity identity;
        identity.nal_header = {pps[0], pps[1]};
        if (!reader.skip(16) || !reader.read_unsigned_exp_golomb(identity.pps_id) ||
            !reader.read_unsigned_exp_golomb(identity.sps_id)) {
            return false;
        }
        identities.push_back(identity);
    }
    std::sort(identities.begin(), identities.end());
    return true;
}

bool collect_nal_headers(const std::vector<std::vector<std::uint8_t>>& parameter_sets,
                         std::vector<std::array<std::uint8_t, 2>>& headers) {
    headers.clear();
    headers.reserve(parameter_sets.size());
    for (const auto& parameter_set : parameter_sets) {
        if (parameter_set.size() < 2) {
            return false;
        }
        headers.push_back({parameter_set[0], parameter_set[1]});
    }
    std::sort(headers.begin(), headers.end());
    return true;
}

bool parse_configuration(const ByteVector& bytes, VideoConfiguration& configuration) {
    HevcConfiguration parsed;
    if (!parse_hvcc_configuration(bytes, parsed) || parsed.sequence_parameter_sets.empty()) {
        return false;
    }
    HevcSpsFormat format;
    if (!parse_hevc_sps_format(parsed.sequence_parameter_sets.front(), format)) {
        return false;
    }
    configuration.width = format.width;
    configuration.height = format.height;
    configuration.nal_length_size = parsed.nal_length_size;
    return true;
}

bool configurations_compatible(const ByteVector& expected, const ByteVector& actual) {
    if (expected == actual) {
        return true;
    }
    HevcConfiguration first;
    HevcConfiguration second;
    if (!parse_hvcc_configuration(expected, first) || !parse_hvcc_configuration(actual, second) ||
        first.nal_length_size != second.nal_length_size) {
        return false;
    }

    // Keep the original repair tool's behavior: regenerated VPS/SPS/PPS
    // payloads are accepted when the decoder-facing format and parameter-set
    // identities stay compatible. RPS, VUI and default slice settings are
    // encoder state and are deliberately not compared byte-for-byte.
    std::vector<HevcSpsFormat> first_sps;
    std::vector<HevcSpsFormat> second_sps;
    if (!collect_sps_formats(first, first_sps) || !collect_sps_formats(second, second_sps) ||
        first_sps != second_sps) {
        return false;
    }

    std::vector<std::array<std::uint8_t, 2>> first_vps_headers;
    std::vector<std::array<std::uint8_t, 2>> second_vps_headers;
    if (!collect_nal_headers(first.video_parameter_sets, first_vps_headers) ||
        !collect_nal_headers(second.video_parameter_sets, second_vps_headers) ||
        first_vps_headers != second_vps_headers) {
        return false;
    }

    std::vector<HevcPpsIdentity> first_pps;
    std::vector<HevcPpsIdentity> second_pps;
    if (!collect_pps_identities(first, first_pps) || !collect_pps_identities(second, second_pps) ||
        first_pps != second_pps) {
        return false;
    }

    std::vector<std::array<std::uint8_t, 2>> first_pps_headers;
    std::vector<std::array<std::uint8_t, 2>> second_pps_headers;
    return collect_nal_headers(first.picture_parameter_sets, first_pps_headers) &&
           collect_nal_headers(second.picture_parameter_sets, second_pps_headers) &&
           first_pps_headers == second_pps_headers;
}

} // namespace

const VideoCodecDescriptor& hevc_descriptor() {
    static const VideoCodecDescriptor descriptor{
        AV_CODEC_ID_HEVC, 12, "H.265", "H.265 VPS/SPS/PPS or NAL length differs",
        parse_configuration, configurations_compatible};
    return descriptor;
}

} // namespace flvconcat::codecs::internal
