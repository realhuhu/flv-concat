#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <unordered_map>
#include <vector>

namespace flvconcat {

enum class StreamKind : std::uint8_t { video = 0, audio = 1 };

struct PacketIndex {
    std::uint64_t tag_offset = 0;
    std::uint32_t payload_size = 0;
    StreamKind stream = StreamKind::video;
    std::int64_t timestamp_ms = 0;
    std::int32_t composition_time_ms = 0;
    std::size_t run = 0;
    std::uint8_t flv_media_header_size = 0;
    bool keyframe = false;
    std::uint64_t fingerprint = 0;
};

struct RunInfo {
    std::int64_t first_ms = std::numeric_limits<std::int64_t>::max();
    std::int64_t last_ms = 0;
    std::size_t packet_count = 0;
    std::unordered_map<std::int64_t, std::uint64_t> signatures;
};

struct ScanResult {
    std::filesystem::path path;
    std::vector<PacketIndex> packets;
    std::vector<RunInfo> video_runs;
    std::vector<RunInfo> audio_runs;
};

struct ScanOptions {
    std::int64_t run_gap_ms = 2000;
    std::size_t fingerprint_bytes = 256;
};

struct PairPlan {
    int video_run = -1;
    int audio_run = -1;
    std::int64_t base_us = 0;
    std::int64_t zero_ms = 0;
    std::int64_t span_us = 0;
    bool drop = false;
};

struct TimelineOptions {
    double pair_overlap_threshold = 0.30;
    double duplicate_threshold = 0.90;
    std::int64_t duplicate_range_tolerance_ms = 500;
    std::int64_t nominal_video_duration_us = 50'000;
    std::int64_t nominal_audio_duration_us = 23'220;
};

struct FilePlan {
    std::vector<PairPlan> pairs;
    std::int64_t start_us = 0;
    std::int64_t end_us = 0;
    std::size_t duplicate_runs_dropped = 0;
};

struct MediaInfo {
    int width = 0;
    int height = 0;
    int video_codec = 0;
    int audio_codec = 0;
    int sample_rate = 0;
    int audio_channels = 0;
    std::vector<std::uint8_t> video_config;
    std::vector<std::uint8_t> audio_config;
};

struct MergeStats {
    std::uint64_t video_packets = 0;
    std::uint64_t audio_packets = 0;
    std::uint64_t resent_audio_packets_dropped = 0;
    std::uint64_t duplicate_runs_dropped = 0;
    std::int64_t duration_us = 0;
};

} // namespace flvconcat
