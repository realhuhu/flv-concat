#include "flvconcat/scanner.hpp"

#include "flvconcat/codecs/registry.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

namespace flvconcat {
namespace {

constexpr std::int64_t kTimestampWrap = std::int64_t{1} << 32;

std::uint32_t read_be24(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 16) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           static_cast<std::uint32_t>(data[2]);
}

std::uint32_t read_be32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

bool read_exact(std::ifstream& input, void* buffer, std::size_t size) {
    input.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
    return input.gcount() == static_cast<std::streamsize>(size);
}

std::uint64_t fnv1a(const std::uint8_t* data, std::size_t size, std::uint64_t hash) {
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= prime;
    }
    return hash;
}

std::uint64_t payload_fingerprint(std::ifstream& input,
                                  std::uint64_t payload_offset,
                                  std::uint32_t payload_size,
                                  std::size_t sample_bytes) {
    constexpr std::uint64_t basis = 14695981039346656037ULL;
    std::uint64_t hash = fnv1a(reinterpret_cast<const std::uint8_t*>(&payload_size),
                               sizeof(payload_size), basis);
    if (payload_size == 0 || sample_bytes == 0) {
        return hash;
    }

    const auto wanted = std::min<std::size_t>(payload_size, sample_bytes);
    std::vector<std::uint8_t> sample(wanted);
    input.clear();
    input.seekg(static_cast<std::streamoff>(payload_offset), std::ios::beg);
    if (!read_exact(input, sample.data(), sample.size())) {
        return 0;
    }
    return fnv1a(sample.data(), sample.size(), hash);
}

void update_run(RunInfo& run, const PacketIndex& packet) {
    run.first_ms = std::min(run.first_ms, packet.timestamp_ms);
    run.last_ms = std::max(run.last_ms, packet.timestamp_ms);
    ++run.packet_count;
    if (packet.stream == StreamKind::video) {
        run.signatures.emplace(packet.timestamp_ms, packet.fingerprint);
    }
}

} // namespace

bool scan_flv(const std::filesystem::path& path,
              const ScanOptions& options,
              ScanResult& result,
              std::string& error) {
    result = {};
    result.path = path;

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open input: " + path.u8string();
        return false;
    }

    input.seekg(0, std::ios::end);
    const auto file_end = input.tellg();
    input.seekg(0, std::ios::beg);
    if (file_end < std::streamoff{13}) {
        error = "file is too small to be an FLV: " + path.u8string();
        return false;
    }

    std::array<std::uint8_t, 9> header{};
    if (!read_exact(input, header.data(), header.size()) ||
        std::memcmp(header.data(), "FLV", 3) != 0) {
        error = "invalid FLV header: " + path.u8string();
        return false;
    }

    const std::uint32_t data_offset = read_be32(header.data() + 5);
    if (data_offset < 9 || static_cast<std::uint64_t>(data_offset) + 4U >
                               static_cast<std::uint64_t>(file_end)) {
        error = "invalid FLV data offset: " + path.u8string();
        return false;
    }
    input.seekg(static_cast<std::streamoff>(data_offset + 4U), std::ios::beg);

    std::array<std::int64_t, 2> last = {
        std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::min()};
    std::array<std::int64_t, 2> wrap = {0, 0};
    std::array<int, 2> current_run = {-1, -1};

    for (;;) {
        const auto tag_position = input.tellg();
        if (tag_position < 0 || tag_position + std::streamoff{11} > file_end) {
            break;
        }

        std::array<std::uint8_t, 11> tag_header{};
        if (!read_exact(input, tag_header.data(), tag_header.size())) {
            break;
        }
        const auto tag_offset = static_cast<std::uint64_t>(tag_position);
        const std::uint8_t type = tag_header[0];
        const std::uint32_t data_size = read_be24(tag_header.data() + 1);
        const std::uint32_t raw_timestamp =
            read_be24(tag_header.data() + 4) |
            (static_cast<std::uint32_t>(tag_header[7]) << 24);
        const std::uint64_t data_position = tag_offset + 11U;
        const std::uint64_t next_tag = data_position + data_size + 4U;
        if (next_tag > static_cast<std::uint64_t>(file_end)) {
            std::ostringstream message;
            message << "truncated FLV tag at byte " << tag_offset << ": " << path.u8string();
            error = message.str();
            return false;
        }

        if (data_size > 0 && (type == 8 || type == 9)) {
            std::array<std::uint8_t, 5> media_header{};
            const auto peek_size = std::min<std::uint32_t>(
                data_size, static_cast<std::uint32_t>(media_header.size()));
            input.seekg(static_cast<std::streamoff>(data_position), std::ios::beg);
            if (!read_exact(input, media_header.data(), peek_size)) {
                error = "cannot read FLV media tag: " + path.u8string();
                return false;
            }

            const StreamKind kind = type == 9 ? StreamKind::video : StreamKind::audio;
            std::uint8_t media_header_size = 1;
            std::int32_t composition_time_ms = 0;
            bool keyframe = false;
            bool media_packet = true;

            if (kind == StreamKind::video) {
                const int codec = media_header[0] & 0x0F;
                keyframe = (media_header[0] >> 4) == 1;
                if (!codecs::video_codec_for_flv(codec)) {
                    media_packet = false;
                } else {
                    if (data_size < 5) {
                        media_packet = false;
                    } else {
                        media_header_size = 5;
                        const auto avc_packet_type = media_header[1];
                        media_packet = avc_packet_type == 1;
                        auto cts = static_cast<std::int32_t>(read_be24(media_header.data() + 2));
                        if ((cts & 0x00800000) != 0) {
                            cts -= 0x01000000;
                        }
                        composition_time_ms = cts;
                    }
                }
            } else {
                const int sound_format = media_header[0] >> 4;
                if (!codecs::audio_codec_for_flv(sound_format)) {
                    media_packet = false;
                } else {
                    if (data_size < 2) {
                        media_packet = false;
                    } else {
                        media_header_size = 2;
                        media_packet = media_header[1] == 1;
                    }
                }
            }

            if (media_packet && data_size >= media_header_size) {
                const auto stream_index = kind == StreamKind::video ? 0U : 1U;
                auto timestamp = static_cast<std::int64_t>(raw_timestamp) + wrap[stream_index];
                if (last[stream_index] != std::numeric_limits<std::int64_t>::min() &&
                    timestamp < last[stream_index] - options.run_gap_ms) {
                    const bool looks_like_wrap =
                        static_cast<std::int64_t>(raw_timestamp) <
                            last[stream_index] - (kTimestampWrap / 2) &&
                        timestamp + kTimestampWrap >= last[stream_index] - options.run_gap_ms;
                    if (looks_like_wrap) {
                        wrap[stream_index] += kTimestampWrap;
                        timestamp += kTimestampWrap;
                    } else {
                        wrap[stream_index] = 0;
                        timestamp = raw_timestamp;
                        ++current_run[stream_index];
                        if (kind == StreamKind::video) {
                            result.video_runs.emplace_back();
                        } else {
                            result.audio_runs.emplace_back();
                        }
                    }
                }

                if (current_run[stream_index] < 0) {
                    current_run[stream_index] = 0;
                    if (kind == StreamKind::video) {
                        result.video_runs.emplace_back();
                    } else {
                        result.audio_runs.emplace_back();
                    }
                }

                PacketIndex packet;
                packet.tag_offset = tag_offset;
                packet.payload_size = data_size - media_header_size;
                packet.stream = kind;
                packet.timestamp_ms = timestamp;
                packet.composition_time_ms = composition_time_ms;
                packet.run = static_cast<std::size_t>(current_run[stream_index]);
                packet.flv_media_header_size = media_header_size;
                packet.keyframe = keyframe;
                packet.fingerprint = payload_fingerprint(
                    input, data_position + media_header_size, packet.payload_size,
                    options.fingerprint_bytes);
                if (packet.fingerprint == 0 && packet.payload_size != 0) {
                    error = "cannot fingerprint FLV packet: " + path.u8string();
                    return false;
                }

                result.packets.push_back(packet);
                if (kind == StreamKind::video) {
                    update_run(result.video_runs[packet.run], packet);
                } else {
                    update_run(result.audio_runs[packet.run], packet);
                }
                last[stream_index] = timestamp;
            }
        }

        input.clear();
        input.seekg(static_cast<std::streamoff>(next_tag), std::ios::beg);
    }

    if (result.packets.empty()) {
        error = "FLV contains no audio or video media packets: " + path.u8string();
        return false;
    }
    return true;
}

} // namespace flvconcat
