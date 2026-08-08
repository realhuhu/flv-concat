#include "flvconcat/media.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
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

bool copy_extradata(AVCodecParameters* parameters,
                    const std::vector<std::uint8_t>& configuration,
                    std::string& error) {
    if (configuration.empty()) {
        error = "codec configuration record is empty";
        return false;
    }
    const auto allocation_size = configuration.size() + AV_INPUT_BUFFER_PADDING_SIZE;
    parameters->extradata = static_cast<std::uint8_t*>(av_mallocz(allocation_size));
    if (!parameters->extradata) {
        error = "cannot allocate codec configuration record";
        return false;
    }
    std::memcpy(parameters->extradata, configuration.data(), configuration.size());
    parameters->extradata_size = static_cast<int>(configuration.size());
    return true;
}

} // namespace

class Mp4Muxer::Impl {
public:
    ~Impl() { close_without_trailer(); }

    bool open(const std::filesystem::path& output,
              const MediaInfo& template_media,
              const MuxOptions& requested_options,
              std::string& error) {
        options = requested_options;
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

        auto* video_parameters = video_stream->codecpar;
        video_parameters->codec_type = AVMEDIA_TYPE_VIDEO;
        video_parameters->codec_id = static_cast<AVCodecID>(template_media.video_codec);
        video_parameters->width = template_media.width;
        video_parameters->height = template_media.height;
        if (!copy_extradata(video_parameters, template_media.video_config, error)) {
            close_without_trailer();
            return false;
        }

        auto* audio_parameters = audio_stream->codecpar;
        audio_parameters->codec_type = AVMEDIA_TYPE_AUDIO;
        audio_parameters->codec_id = static_cast<AVCodecID>(template_media.audio_codec);
        audio_parameters->sample_rate = template_media.sample_rate;
        if (template_media.audio_channels <= 0) {
            error = "invalid audio channel count";
            close_without_trailer();
            return false;
        }
        av_channel_layout_default(&audio_parameters->ch_layout, template_media.audio_channels);
        if (!copy_extradata(audio_parameters, template_media.audio_config, error)) {
            close_without_trailer();
            return false;
        }

        video_parameters->codec_tag = 0;
        audio_parameters->codec_tag = 0;
        video_stream->time_base = AVRational{1, 1000};
        sample_rate = audio_parameters->sample_rate;
        if (sample_rate <= 0) {
            error = "invalid audio sample rate";
            close_without_trailer();
            return false;
        }
        audio_stream->time_base = AVRational{1, sample_rate};
        av_dict_set(&context->metadata, "encoder", "FLVConcat 1.1.0", 0);

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
                    const MediaInfo& stream_template,
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
