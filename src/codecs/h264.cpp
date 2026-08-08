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

struct AvcConfiguration {
    std::uint8_t nal_length_size = 0;
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

bool read_nal_units(const ByteVector& bytes,
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

bool parse_avc_configuration(const ByteVector& bytes, AvcConfiguration& configuration) {
    // AVCDecoderConfigurationRecord. The optional high-profile extension is
    // intentionally ignored; the SPS/PPS NAL units carry the decoder state.
    if (bytes.size() < 7 || bytes[0] != 1) {
        return false;
    }
    const auto sps_count = static_cast<std::size_t>(bytes[5] & 0x1FU);
    if (sps_count == 0) {
        return false;
    }

    configuration = {};
    configuration.nal_length_size = static_cast<std::uint8_t>((bytes[4] & 0x03U) + 1U);
    std::size_t offset = 6;
    if (!read_nal_units(bytes, offset, sps_count, configuration.sequence_parameter_sets) ||
        offset >= bytes.size()) {
        return false;
    }
    const auto pps_count = static_cast<std::size_t>(bytes[offset++]);
    return pps_count > 0 &&
           read_nal_units(bytes, offset, pps_count, configuration.picture_parameter_sets);
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

bool skip_scaling_list(internal::BitReader& reader, int size) {
    int last_scale = 8;
    int next_scale = 8;
    for (int index = 0; index < size; ++index) {
        if (next_scale != 0) {
            std::int32_t delta_scale = 0;
            if (!reader.read_signed_exp_golomb(delta_scale)) {
                return false;
            }
            next_scale = (last_scale + delta_scale + 256) & 255;
        }
        last_scale = next_scale == 0 ? last_scale : next_scale;
    }
    return true;
}

bool h264_sps_dimensions(const std::uint8_t* sps,
                         std::size_t length,
                         int& width,
                         int& height) {
    const auto raw = nal_unescape(sps, length);
    if (raw.size() < 4) {
        return false;
    }
    internal::BitReader reader(raw.data(), raw.size());
    std::uint32_t profile = 0;
    std::uint32_t ignored = 0;
    if (!reader.read(8, ignored) || !reader.read(8, profile) || !reader.read(8, ignored) ||
        !reader.read(8, ignored)) {
        return false;
    }

    std::uint32_t sps_id = 0;
    if (!reader.read_unsigned_exp_golomb(sps_id)) {
        return false;
    }
    (void)sps_id;

    std::uint32_t chroma_format_idc = 1;
    bool separate_colour_plane = false;
    const bool high_profile =
        profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
        profile == 139 || profile == 134 || profile == 135;
    if (high_profile) {
        if (!reader.read_unsigned_exp_golomb(chroma_format_idc)) {
            return false;
        }
        if (chroma_format_idc == 3) {
            std::uint32_t separate = 0;
            if (!reader.read(1, separate)) {
                return false;
            }
            separate_colour_plane = separate != 0;
        }
        std::uint32_t ignored_ue = 0;
        if (!reader.read_unsigned_exp_golomb(ignored_ue) ||
            !reader.read_unsigned_exp_golomb(ignored_ue) || !reader.read(1, ignored)) {
            return false;
        }
        std::uint32_t scaling_matrix_present = 0;
        if (!reader.read(1, scaling_matrix_present)) {
            return false;
        }
        if (scaling_matrix_present != 0) {
            const int list_count = chroma_format_idc == 3 ? 12 : 8;
            for (int index = 0; index < list_count; ++index) {
                std::uint32_t list_present = 0;
                if (!reader.read(1, list_present)) {
                    return false;
                }
                if (list_present != 0 &&
                    !skip_scaling_list(reader, index < 6 ? 16 : 64)) {
                    return false;
                }
            }
        }
    }

    std::uint32_t log2_max_frame_num_minus4 = 0;
    std::uint32_t pic_order_cnt_type = 0;
    if (!reader.read_unsigned_exp_golomb(log2_max_frame_num_minus4) ||
        !reader.read_unsigned_exp_golomb(pic_order_cnt_type)) {
        return false;
    }
    (void)log2_max_frame_num_minus4;
    if (pic_order_cnt_type == 0) {
        if (!reader.read_unsigned_exp_golomb(ignored)) {
            return false;
        }
    } else if (pic_order_cnt_type == 1) {
        std::uint32_t delta_zero = 0;
        std::uint32_t offset_count = 0;
        std::int32_t offset_non_ref = 0;
        std::int32_t offset_ref = 0;
        if (!reader.read(1, delta_zero) || !reader.read_signed_exp_golomb(offset_non_ref) ||
            !reader.read_signed_exp_golomb(offset_ref) ||
            !reader.read_unsigned_exp_golomb(offset_count)) {
            return false;
        }
        (void)delta_zero;
        (void)offset_non_ref;
        (void)offset_ref;
        for (std::uint32_t index = 0; index < offset_count; ++index) {
            std::int32_t offset = 0;
            if (!reader.read_signed_exp_golomb(offset)) {
                return false;
            }
        }
    } else if (pic_order_cnt_type > 2) {
        return false;
    }

    std::uint32_t max_num_ref_frames = 0;
    std::uint32_t gaps_in_frame_num_allowed = 0;
    if (!reader.read_unsigned_exp_golomb(max_num_ref_frames) ||
        !reader.read(1, gaps_in_frame_num_allowed)) {
        return false;
    }
    (void)max_num_ref_frames;
    (void)gaps_in_frame_num_allowed;

    std::uint32_t pic_width_in_mbs_minus1 = 0;
    std::uint32_t pic_height_in_map_units_minus1 = 0;
    std::uint32_t frame_mbs_only_flag = 0;
    if (!reader.read_unsigned_exp_golomb(pic_width_in_mbs_minus1) ||
        !reader.read_unsigned_exp_golomb(pic_height_in_map_units_minus1) ||
        !reader.read(1, frame_mbs_only_flag)) {
        return false;
    }
    if (frame_mbs_only_flag == 0 && !reader.read(1, ignored)) {
        return false;
    }
    if (!reader.read(1, ignored)) { // direct_8x8_inference_flag
        return false;
    }

    std::uint32_t frame_cropping_flag = 0;
    if (!reader.read(1, frame_cropping_flag)) {
        return false;
    }
    std::uint32_t crop_left = 0;
    std::uint32_t crop_right = 0;
    std::uint32_t crop_top = 0;
    std::uint32_t crop_bottom = 0;
    if (frame_cropping_flag != 0 &&
        (!reader.read_unsigned_exp_golomb(crop_left) ||
         !reader.read_unsigned_exp_golomb(crop_right) ||
         !reader.read_unsigned_exp_golomb(crop_top) ||
         !reader.read_unsigned_exp_golomb(crop_bottom))) {
        return false;
    }

    int crop_unit_x = 1;
    int crop_unit_y = 2 - static_cast<int>(frame_mbs_only_flag);
    if (!separate_colour_plane) {
        switch (chroma_format_idc) {
            case 1:
                crop_unit_x = 2;
                crop_unit_y *= 2;
                break;
            case 2:
                crop_unit_x = 2;
                break;
            case 3:
                crop_unit_x = 1;
                break;
            default:
                break;
        }
    }
    const auto coded_width = (pic_width_in_mbs_minus1 + 1U) * 16U;
    const auto coded_height = (pic_height_in_map_units_minus1 + 1U) * 16U *
                              (2U - frame_mbs_only_flag);
    const auto cropped_width = coded_width - crop_unit_x * (crop_left + crop_right);
    const auto cropped_height = coded_height - crop_unit_y * (crop_top + crop_bottom);
    if (cropped_width == 0 || cropped_height == 0) {
        return false;
    }
    width = static_cast<int>(cropped_width);
    height = static_cast<int>(cropped_height);
    return true;
}

bool parse_configuration(const ByteVector& bytes, VideoConfiguration& configuration) {
    AvcConfiguration parsed;
    if (!parse_avc_configuration(bytes, parsed) || parsed.sequence_parameter_sets.empty()) {
        return false;
    }
    if (!h264_sps_dimensions(parsed.sequence_parameter_sets.front().data(),
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
    AvcConfiguration first;
    AvcConfiguration second;
    return parse_avc_configuration(expected, first) && parse_avc_configuration(actual, second) &&
           first.nal_length_size == second.nal_length_size &&
           first.sequence_parameter_sets == second.sequence_parameter_sets &&
           first.picture_parameter_sets == second.picture_parameter_sets;
}

} // namespace

const VideoCodecDescriptor& h264_descriptor() {
    static const VideoCodecDescriptor descriptor{
        AV_CODEC_ID_H264, 7, "H.264", "H.264 SPS/PPS or NAL length differs",
        parse_configuration, configurations_compatible};
    return descriptor;
}

} // namespace flvconcat::codecs::internal
