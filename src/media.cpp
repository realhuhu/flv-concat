#include "flvconcat/media.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace flvconcat {
namespace {

constexpr AVRational kMicroseconds = {1, 1'000'000};

std::string utf8_path(const std::filesystem::path& path) {
    return path.u8string();
}

std::string ffmpeg_error(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(code, buffer.data(), buffer.size());
    return buffer.data();
}

struct InputContext {
    AVFormatContext* value = nullptr;
    ~InputContext() { avformat_close_input(&value); }
};

struct AvcConfiguration {
    std::uint8_t nal_length_size = 0;
    std::vector<std::vector<std::uint8_t>> sequence_parameter_sets;
    std::vector<std::vector<std::uint8_t>> picture_parameter_sets;
};

bool read_be16(const std::vector<std::uint8_t>& bytes,
               std::size_t& offset,
               std::uint16_t& value) {
    if (offset + 2 > bytes.size()) {
        return false;
    }
    value = static_cast<std::uint16_t>(bytes[offset] << 8U) | bytes[offset + 1];
    offset += 2;
    return true;
}

bool read_nal_units(const std::vector<std::uint8_t>& bytes,
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

bool parse_avc_configuration(const std::vector<std::uint8_t>& bytes, AvcConfiguration& configuration) {
    // AVCDecoderConfigurationRecord (ISO/IEC 14496-15). The optional high-profile
    // extension follows the PPS list and is deliberately ignored: some FLV recorders
    // write it inconsistently even though their SPS/PPS and media packets are identical.
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

class BitReader {
public:
    explicit BitReader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    bool read(unsigned count, std::uint32_t& value) {
        if (count > 32 || bit_offset_ + count > bytes_.size() * 8U) {
            return false;
        }
        value = 0;
        for (unsigned index = 0; index < count; ++index) {
            const auto byte = bytes_[(bit_offset_ + index) / 8U];
            const auto shift = 7U - static_cast<unsigned>((bit_offset_ + index) % 8U);
            value = (value << 1U) | ((byte >> shift) & 1U);
        }
        bit_offset_ += count;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const { return bytes_.size() * 8U - bit_offset_; }

private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t bit_offset_ = 0;
};

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

bool parse_aac_configuration(const std::vector<std::uint8_t>& bytes, AacConfiguration& configuration) {
    BitReader reader(bytes);
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
    if (!reader.read(1, extension_flag)) {
        return false;
    }
    if (extension_flag != 0) {
        return false;
    }
    configuration.frame_length_960 = frame_length_flag != 0;

    // An optional sync extension may be present after GASpecificConfig. An explicit
    // "SBR not present" extension is metadata only and is intentionally equivalent
    // to its absence. A real SBR extension remains part of the compatibility check.
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

bool h264_configurations_compatible(const std::vector<std::uint8_t>& expected,
                                    const std::vector<std::uint8_t>& actual) {
    if (expected == actual) {
        return true;
    }
    AvcConfiguration expected_configuration;
    AvcConfiguration actual_configuration;
    return parse_avc_configuration(expected, expected_configuration) &&
           parse_avc_configuration(actual, actual_configuration) &&
           expected_configuration.nal_length_size == actual_configuration.nal_length_size &&
           expected_configuration.sequence_parameter_sets == actual_configuration.sequence_parameter_sets &&
           expected_configuration.picture_parameter_sets == actual_configuration.picture_parameter_sets;
}

bool aac_configurations_compatible(const std::vector<std::uint8_t>& expected,
                                   const std::vector<std::uint8_t>& actual) {
    if (expected == actual) {
        return true;
    }
    AacConfiguration expected_configuration;
    AacConfiguration actual_configuration;
    return parse_aac_configuration(expected, expected_configuration) &&
           parse_aac_configuration(actual, actual_configuration) &&
           expected_configuration.signalling_object_type == actual_configuration.signalling_object_type &&
           expected_configuration.core_object_type == actual_configuration.core_object_type &&
           expected_configuration.sampling_frequency == actual_configuration.sampling_frequency &&
           expected_configuration.extension_sampling_frequency == actual_configuration.extension_sampling_frequency &&
           expected_configuration.channel_configuration == actual_configuration.channel_configuration &&
           expected_configuration.frame_length_960 == actual_configuration.frame_length_960 &&
           expected_configuration.sync_sbr_present == actual_configuration.sync_sbr_present &&
           expected_configuration.sync_sbr_sampling_frequency == actual_configuration.sync_sbr_sampling_frequency;
}

bool open_input(const std::filesystem::path& path, InputContext& input, std::string& error) {
    AVDictionary* options = nullptr;
    av_dict_set(&options, "probesize", "50000000", 0);
    av_dict_set(&options, "analyzeduration", "10000000", 0);
    const auto name = utf8_path(path);
    const int open_result = avformat_open_input(&input.value, name.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (open_result < 0) {
        error = "FFmpeg cannot open " + name + ": " + ffmpeg_error(open_result);
        return false;
    }
    const int info_result = avformat_find_stream_info(input.value, nullptr);
    if (info_result < 0) {
        error = "FFmpeg cannot read stream information from " + name + ": " +
                ffmpeg_error(info_result);
        return false;
    }
    return true;
}

bool find_streams(AVFormatContext* context, AVStream*& video, AVStream*& audio) {
    video = nullptr;
    audio = nullptr;
    for (unsigned index = 0; index < context->nb_streams; ++index) {
        AVStream* stream = context->streams[index];
        if (!video && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video = stream;
        } else if (!audio && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio = stream;
        }
    }
    return video && audio;
}

} // namespace

bool probe_media(const std::filesystem::path& path, MediaInfo& info, std::string& error) {
    InputContext input;
    if (!open_input(path, input, error)) {
        return false;
    }

    AVStream* video = nullptr;
    AVStream* audio = nullptr;
    if (!find_streams(input.value, video, audio)) {
        error = "input must contain one video stream and one audio stream: " + utf8_path(path);
        return false;
    }
    if (video->codecpar->codec_id != AV_CODEC_ID_H264 ||
        audio->codecpar->codec_id != AV_CODEC_ID_AAC) {
        error = "only H.264 video with AAC audio is currently supported: " + utf8_path(path);
        return false;
    }

    info = {};
    info.width = video->codecpar->width;
    info.height = video->codecpar->height;
    info.video_codec = video->codecpar->codec_id;
    info.audio_codec = audio->codecpar->codec_id;
    info.sample_rate = audio->codecpar->sample_rate;
    info.audio_channels = audio->codecpar->ch_layout.nb_channels;
    if (video->codecpar->extradata && video->codecpar->extradata_size > 0) {
        info.video_config.assign(video->codecpar->extradata,
                                 video->codecpar->extradata + video->codecpar->extradata_size);
    }
    if (audio->codecpar->extradata && audio->codecpar->extradata_size > 0) {
        info.audio_config.assign(audio->codecpar->extradata,
                                 audio->codecpar->extradata + audio->codecpar->extradata_size);
    }
    return true;
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
    } else if (!h264_configurations_compatible(expected.video_config, actual.video_config)) {
        reason = "H.264 SPS/PPS or NAL length differs";
    } else if (!aac_configurations_compatible(expected.audio_config, actual.audio_config)) {
        reason = "AAC object type, sample rate, channel layout, or frame length differs";
    } else {
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

class Mp4Muxer::Impl {
public:
    ~Impl() { close_without_trailer(); }

    bool open(const std::filesystem::path& output,
              const std::filesystem::path& stream_template,
              const MuxOptions& requested_options,
              std::string& error) {
        options = requested_options;
        InputContext input;
        if (!open_input(stream_template, input, error)) {
            return false;
        }
        AVStream* input_video = nullptr;
        AVStream* input_audio = nullptr;
        if (!find_streams(input.value, input_video, input_audio)) {
            error = "stream template does not contain audio and video";
            return false;
        }

        const auto output_name = utf8_path(output);
        int result = avformat_alloc_output_context2(&context, nullptr, "mp4", output_name.c_str());
        if (result < 0 || !context) {
            error = "cannot create MP4 output context: " + ffmpeg_error(result);
            close_without_trailer();
            return false;
        }

        video_stream = avformat_new_stream(context, nullptr);
        audio_stream = avformat_new_stream(context, nullptr);
        if (!video_stream || !audio_stream) {
            error = "cannot allocate output streams";
            close_without_trailer();
            return false;
        }
        if ((result = avcodec_parameters_copy(video_stream->codecpar, input_video->codecpar)) < 0 ||
            (result = avcodec_parameters_copy(audio_stream->codecpar, input_audio->codecpar)) < 0) {
            error = "cannot copy codec parameters: " + ffmpeg_error(result);
            close_without_trailer();
            return false;
        }
        video_stream->codecpar->codec_tag = 0;
        audio_stream->codecpar->codec_tag = 0;
        video_stream->time_base = AVRational{1, 1000};
        sample_rate = audio_stream->codecpar->sample_rate;
        audio_stream->time_base = AVRational{1, sample_rate};
        av_dict_set(&context->metadata, "encoder", "FLVConcat 1.0.0", 0);

        result = avio_open(&context->pb, output_name.c_str(), AVIO_FLAG_WRITE);
        if (result < 0) {
            error = "cannot create output file " + output_name + ": " + ffmpeg_error(result);
            close_without_trailer();
            return false;
        }

        AVDictionary* header_options = nullptr;
        if (options.faststart) {
            av_dict_set(&header_options, "movflags", "+faststart", 0);
        }
        result = avformat_write_header(context, &header_options);
        av_dict_free(&header_options);
        if (result < 0) {
            error = "cannot write MP4 header: " + ffmpeg_error(result);
            close_without_trailer();
            return false;
        }
        header_written = true;
        video_time_base = video_stream->time_base;
        audio_time_base = audio_stream->time_base;
        return true;
    }

    bool write_file(const ScanResult& scan, const FilePlan& plan, std::string& error) {
        if (!header_written) {
            error = "output muxer is not open";
            return false;
        }

        std::vector<std::vector<const PacketIndex*>> video_packets(scan.video_runs.size());
        std::vector<std::vector<const PacketIndex*>> audio_packets(scan.audio_runs.size());
        for (const auto& packet : scan.packets) {
            if (packet.stream == StreamKind::video && packet.run < video_packets.size()) {
                video_packets[packet.run].push_back(&packet);
            } else if (packet.stream == StreamKind::audio && packet.run < audio_packets.size()) {
                audio_packets[packet.run].push_back(&packet);
            }
        }

        std::ifstream input(scan.path, std::ios::binary);
        if (!input) {
            error = "cannot reopen input: " + utf8_path(scan.path);
            return false;
        }
        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            error = "cannot allocate FFmpeg packet";
            return false;
        }
        std::vector<std::int64_t> last_audio_timestamp(
            scan.audio_runs.size(), std::numeric_limits<std::int64_t>::min());

        bool success = true;
        for (const auto& pair : plan.pairs) {
            if (pair.drop) {
                continue;
            }
            const std::vector<const PacketIndex*> empty;
            const auto& videos = pair.video_run >= 0
                                     ? video_packets[static_cast<std::size_t>(pair.video_run)]
                                     : empty;
            const auto& audios = pair.audio_run >= 0
                                     ? audio_packets[static_cast<std::size_t>(pair.audio_run)]
                                     : empty;
            std::size_t video_index = 0;
            std::size_t audio_index = 0;
            const auto output_time = [&pair](const PacketIndex* indexed) {
                return pair.base_us + (indexed->timestamp_ms - pair.zero_ms) * 1000;
            };

            while (video_index < videos.size() || audio_index < audios.size()) {
                bool take_video = false;
                if (audio_index >= audios.size()) {
                    take_video = true;
                } else if (video_index < videos.size()) {
                    take_video = output_time(videos[video_index]) <= output_time(audios[audio_index]);
                }
                const PacketIndex* indexed =
                    take_video ? videos[video_index] : audios[audio_index];
                auto out_us = output_time(indexed);
                const auto stream_slot = take_video ? 0U : 1U;

                if (!take_video) {
                    auto& previous = last_audio_timestamp[indexed->run];
                    if (indexed->timestamp_ms <= previous) {
                        ++stats_value.resent_audio_packets_dropped;
                        ++audio_index;
                        continue;
                    }
                    previous = indexed->timestamp_ms;
                }
                if (out_us <= last_output_us[stream_slot]) {
                    out_us = last_output_us[stream_slot] + 1000;
                }
                last_output_us[stream_slot] = out_us;

                const auto payload_offset = indexed->tag_offset + 11U +
                                            indexed->flv_media_header_size;
                input.clear();
                input.seekg(static_cast<std::streamoff>(payload_offset), std::ios::beg);
                const int allocation_result = av_new_packet(packet, indexed->payload_size);
                if (allocation_result < 0) {
                    error = "cannot allocate packet payload: " + ffmpeg_error(allocation_result);
                    success = false;
                    break;
                }
                input.read(reinterpret_cast<char*>(packet->data),
                           static_cast<std::streamsize>(indexed->payload_size));
                if (input.gcount() != static_cast<std::streamsize>(indexed->payload_size)) {
                    error = "cannot read indexed packet from " + utf8_path(scan.path);
                    av_packet_unref(packet);
                    success = false;
                    break;
                }

                std::int64_t duration_us;
                std::int32_t composition_ms = 0;
                if (take_video) {
                    duration_us = options.nominal_video_duration_us;
                    if (video_index + 1 < videos.size()) {
                        const auto candidate = output_time(videos[video_index + 1]) - out_us;
                        if (candidate > 0 && candidate <= options.maximum_video_duration_us) {
                            duration_us = candidate;
                        }
                    }
                    composition_ms = indexed->composition_time_ms;
                    if (indexed->keyframe) {
                        packet->flags |= AV_PKT_FLAG_KEY;
                    }
                } else {
                    duration_us = av_rescale_q(1024, AVRational{1, sample_rate}, kMicroseconds);
                }

                const AVRational time_base = take_video ? video_time_base : audio_time_base;
                packet->stream_index = take_video ? video_stream->index : audio_stream->index;
                packet->dts = av_rescale_q(out_us, kMicroseconds, time_base);
                auto composition_us = take_video
                                          ? static_cast<std::int64_t>(composition_ms) * 1000 +
                                                options.video_offset_us
                                          : 0;
                composition_us = std::max<std::int64_t>(0, composition_us);
                packet->pts = packet->dts + av_rescale_q(composition_us, kMicroseconds, time_base);
                packet->duration = std::max<std::int64_t>(
                    1, av_rescale_q(duration_us, kMicroseconds, time_base));

                const int write_result = av_interleaved_write_frame(context, packet);
                av_packet_unref(packet);
                if (write_result < 0) {
                    error = "cannot write MP4 packet: " + ffmpeg_error(write_result);
                    success = false;
                    break;
                }
                if (take_video) {
                    ++stats_value.video_packets;
                    ++video_index;
                } else {
                    ++stats_value.audio_packets;
                    ++audio_index;
                }
                stats_value.duration_us = std::max(stats_value.duration_us, out_us + duration_us);
            }
            if (!success) {
                break;
            }
        }
        av_packet_free(&packet);
        stats_value.duplicate_runs_dropped += plan.duplicate_runs_dropped;
        stats_value.duration_us = std::max(stats_value.duration_us, plan.end_us);
        return success;
    }

    bool finish(std::string& error) {
        if (!header_written || !context) {
            error = "output muxer is not open";
            return false;
        }
        const int trailer_result = av_write_trailer(context);
        header_written = false;
        if (context->pb) {
            avio_closep(&context->pb);
        }
        avformat_free_context(context);
        context = nullptr;
        video_stream = nullptr;
        audio_stream = nullptr;
        if (trailer_result < 0) {
            error = "cannot finalize MP4: " + ffmpeg_error(trailer_result);
            return false;
        }
        return true;
    }

    void close_without_trailer() {
        if (context) {
            if (context->pb) {
                avio_closep(&context->pb);
            }
            avformat_free_context(context);
            context = nullptr;
        }
        video_stream = nullptr;
        audio_stream = nullptr;
        header_written = false;
    }

    AVFormatContext* context = nullptr;
    AVStream* video_stream = nullptr;
    AVStream* audio_stream = nullptr;
    AVRational video_time_base{};
    AVRational audio_time_base{};
    int sample_rate = 0;
    bool header_written = false;
    MuxOptions options;
    MergeStats stats_value;
    std::array<std::int64_t, 2> last_output_us = {
        std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::min()};
};

Mp4Muxer::Mp4Muxer() : impl_(std::make_unique<Impl>()) {}
Mp4Muxer::~Mp4Muxer() = default;

bool Mp4Muxer::open(const std::filesystem::path& output,
                    const std::filesystem::path& stream_template,
                    const MuxOptions& options,
                    std::string& error) {
    return impl_->open(output, stream_template, options, error);
}

bool Mp4Muxer::write_file(const ScanResult& scan,
                          const FilePlan& plan,
                          std::string& error) {
    return impl_->write_file(scan, plan, error);
}

bool Mp4Muxer::finish(std::string& error) {
    return impl_->finish(error);
}

void Mp4Muxer::abort() {
    impl_->close_without_trailer();
}

const MergeStats& Mp4Muxer::stats() const {
    return impl_->stats_value;
}

} // namespace flvconcat
