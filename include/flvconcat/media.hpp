#pragma once

#include "flvconcat/model.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace flvconcat {

bool probe_media(const std::filesystem::path& path, MediaInfo& info, std::string& error);
bool media_compatible(const MediaInfo& expected, const MediaInfo& actual, std::string& reason);
std::string media_summary(const MediaInfo& info);

struct MuxOptions {
    std::int64_t video_offset_us = 0;
    std::int64_t nominal_video_duration_us = 50'000;
    std::int64_t maximum_video_duration_us = 30'000'000;
    MetadataMap metadata;
    bool faststart = true;
};

class Mp4Muxer {
public:
    Mp4Muxer();
    ~Mp4Muxer();
    Mp4Muxer(const Mp4Muxer&) = delete;
    Mp4Muxer& operator=(const Mp4Muxer&) = delete;

    bool open(const std::filesystem::path& output,
              const MediaInfo& stream_template,
              const MuxOptions& options,
              std::string& error);
    bool write_file(const ScanResult& scan,
                    const FilePlan& plan,
                    const MediaInfo& source_media,
                    std::string& error);
    bool finish(std::string& error);
    void abort();
    const MergeStats& stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace flvconcat
