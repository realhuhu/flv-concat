#include "flvconcat/flv_probe.hpp"

#include "flvconcat/codecs/registry.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <utility>

namespace flvconcat {
namespace {

std::uint32_t read_be24(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 16U) |
           (static_cast<std::uint32_t>(data[1]) << 8U) | static_cast<std::uint32_t>(data[2]);
}

std::uint32_t read_be32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

bool read_exact(std::ifstream& input, void* buffer, std::size_t size) {
    input.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
    return input.gcount() == static_cast<std::streamsize>(size);
}

int flv_sample_rate(std::uint8_t sound_header) {
    static constexpr std::array<int, 4> rates = {5500, 11025, 22050, 44100};
    const auto index = static_cast<std::size_t>((sound_header >> 2U) & 0x03U);
    return rates[index];
}

std::string path_text(const std::filesystem::path& path) {
    return path.u8string();
}

} // namespace

bool probe_flv(const std::filesystem::path& path, MediaInfo& info, std::string& error) {
    info = {};
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open input: " + path_text(path);
        return false;
    }
    input.seekg(0, std::ios::end);
    const auto file_end = input.tellg();
    input.seekg(0, std::ios::beg);
    if (file_end < std::streamoff{13}) {
        error = "file is too small to be an FLV: " + path_text(path);
        return false;
    }

    std::array<std::uint8_t, 9> header{};
    if (!read_exact(input, header.data(), header.size()) ||
        header[0] != 'F' || header[1] != 'L' || header[2] != 'V') {
        error = "invalid FLV header: " + path_text(path);
        return false;
    }
    const auto data_offset = read_be32(header.data() + 5);
    if (data_offset < 9 || static_cast<std::uint64_t>(data_offset) + 4U >
                               static_cast<std::uint64_t>(file_end)) {
        error = "invalid FLV data offset: " + path_text(path);
        return false;
    }
    input.seekg(static_cast<std::streamoff>(data_offset + 4U), std::ios::beg);

    bool got_video = false;
    bool got_audio = false;
    std::string video_error;
    for (std::size_t tag_index = 0; tag_index < 200'000 && !(got_video && got_audio); ++tag_index) {
        const auto tag_position = input.tellg();
        if (tag_position < 0 || tag_position + std::streamoff{11} > file_end) {
            break;
        }
        std::array<std::uint8_t, 11> tag_header{};
        if (!read_exact(input, tag_header.data(), tag_header.size())) {
            break;
        }
        const auto type = tag_header[0];
        const auto data_size = read_be24(tag_header.data() + 1);
        const auto data_position = static_cast<std::uint64_t>(tag_position) + 11U;
        const auto next_tag = data_position + data_size + 4U;
        if (next_tag > static_cast<std::uint64_t>(file_end)) {
            std::ostringstream message;
            message << "truncated FLV tag at byte " << tag_position << ": " << path_text(path);
            error = message.str();
            return false;
        }

        if (data_size > 0 && (type == 8 || type == 9) && data_size <= 4U * 1024U * 1024U) {
            std::array<std::uint8_t, 5> media_header{};
            const auto header_size = std::min<std::uint32_t>(
                data_size, static_cast<std::uint32_t>(media_header.size()));
            input.seekg(static_cast<std::streamoff>(data_position), std::ios::beg);
            if (!read_exact(input, media_header.data(), header_size)) {
                error = "cannot read FLV media tag: " + path_text(path);
                return false;
            }

            if (type == 9 && data_size >= 5) {
                const auto* descriptor =
                    codecs::video_codec_for_flv(static_cast<int>(media_header[0] & 0x0FU));
                if (descriptor && media_header[1] == 0) {
                    const auto config_size = data_size - 5U;
                    codecs::ByteVector configuration(config_size);
                    input.seekg(static_cast<std::streamoff>(data_position + 5U), std::ios::beg);
                    if (!read_exact(input, configuration.data(), configuration.size())) {
                        error = "cannot read video sequence header: " + path_text(path);
                        return false;
                    }
                    codecs::VideoConfiguration parsed;
                    if (descriptor->parse_configuration &&
                        descriptor->parse_configuration(configuration, parsed)) {
                        info.width = parsed.width;
                        info.height = parsed.height;
                        info.video_codec = descriptor->codec_id;
                        info.video_config = std::move(configuration);
                        got_video = true;
                    } else {
                        video_error = "invalid " + std::string(descriptor->name) +
                                      " sequence header";
                    }
                }
            } else if (type == 8 && data_size >= 2 && !got_audio) {
                const auto sound_format = static_cast<int>(media_header[0] >> 4U);
                const auto* descriptor = codecs::audio_codec_for_flv(sound_format);
                if (descriptor && media_header[1] == 0) {
                    const auto config_size = data_size - 2U;
                    codecs::ByteVector configuration(config_size);
                    input.seekg(static_cast<std::streamoff>(data_position + 2U), std::ios::beg);
                    if (!read_exact(input, configuration.data(), configuration.size())) {
                        error = "cannot read audio sequence header: " + path_text(path);
                        return false;
                    }
                    info.audio_codec = descriptor->codec_id;
                    info.sample_rate = flv_sample_rate(media_header[0]);
                    info.audio_channels = (media_header[0] & 0x01U) != 0 ? 2 : 1;
                    codecs::AudioConfiguration parsed;
                    if (descriptor->parse_configuration &&
                        descriptor->parse_configuration(configuration, parsed)) {
                        // The AudioSpecificConfig is authoritative when a
                        // recorder's FLV sound-rate nibble disagrees with it.
                        info.sample_rate = parsed.sample_rate;
                        info.audio_channels = parsed.channels;
                    }
                    info.audio_config = std::move(configuration);
                    got_audio = true;
                }
            }
        }

        input.clear();
        input.seekg(static_cast<std::streamoff>(next_tag), std::ios::beg);
    }

    if (!got_video) {
        error = video_error.empty()
                    ? "FLV is missing a supported H.264/H.265 video sequence header: " +
                          path_text(path)
                    : video_error + ": " + path_text(path);
        return false;
    }
    if (!got_audio) {
        error = "FLV is missing an AAC audio sequence header: " + path_text(path);
        return false;
    }
    return true;
}

} // namespace flvconcat
