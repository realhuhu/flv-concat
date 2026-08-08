#include "flvconcat/media.hpp"
#include "flvconcat/scanner.hpp"
#include "flvconcat/timeline.hpp"

extern "C" {
#include <libavcodec/codec_id.h>
}

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void append_be24(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void append_be32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void append_tag(std::vector<std::uint8_t>& file,
                std::uint8_t type,
                std::uint32_t timestamp,
                const std::vector<std::uint8_t>& data) {
    file.push_back(type);
    append_be24(file, static_cast<std::uint32_t>(data.size()));
    append_be24(file, timestamp & 0x00FFFFFFU);
    file.push_back(static_cast<std::uint8_t>((timestamp >> 24) & 0xFF));
    append_be24(file, 0);
    file.insert(file.end(), data.begin(), data.end());
    append_be32(file, static_cast<std::uint32_t>(11 + data.size()));
}

void append_video(std::vector<std::uint8_t>& file,
                  std::uint32_t timestamp,
                  std::uint8_t payload) {
    append_tag(file, 9, timestamp, {0x17, 0x01, 0x00, 0x00, 0x00, payload, payload});
}

void append_audio(std::vector<std::uint8_t>& file,
                  std::uint32_t timestamp,
                  std::uint8_t payload) {
    append_tag(file, 8, timestamp, {0xAF, 0x01, payload, payload});
}

void append_h265_sequence(std::vector<std::uint8_t>& file,
                          std::uint32_t timestamp,
                          const std::vector<std::uint8_t>& configuration) {
    std::vector<std::uint8_t> data = {0x1C, 0x00, 0x00, 0x00, 0x00};
    data.insert(data.end(), configuration.begin(), configuration.end());
    append_tag(file, 9, timestamp, data);
}

void append_h264_sequence(std::vector<std::uint8_t>& file,
                          std::uint32_t timestamp,
                          const std::vector<std::uint8_t>& configuration) {
    std::vector<std::uint8_t> data = {0x17, 0x00, 0x00, 0x00, 0x00};
    data.insert(data.end(), configuration.begin(), configuration.end());
    append_tag(file, 9, timestamp, data);
}

void append_h265_video(std::vector<std::uint8_t>& file,
                       std::uint32_t timestamp,
                       std::uint8_t payload) {
    append_tag(file, 9, timestamp, {0x1C, 0x01, 0x00, 0x00, 0x00, payload, payload});
}

void append_aac_sequence(std::vector<std::uint8_t>& file, std::uint32_t timestamp) {
    append_tag(file, 8, timestamp, {0xAF, 0x00, 0x11, 0x90});
}

std::filesystem::path write_fixture(const std::string& name,
                                    const std::vector<std::uint8_t>& tags) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::vector<std::uint8_t> file = {
        'F', 'L', 'V', 0x01, 0x05, 0x00, 0x00, 0x00, 0x09,
        0x00, 0x00, 0x00, 0x00};
    file.insert(file.end(), tags.begin(), tags.end());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(file.data()),
                 static_cast<std::streamsize>(file.size()));
    return path;
}

void test_runs_pairing_and_duplicate_content() {
    std::vector<std::uint8_t> tags;
    append_video(tags, 1000, 0x11);
    append_audio(tags, 1000, 0x21);
    append_video(tags, 3000, 0x12);
    append_audio(tags, 3000, 0x22);
    append_audio(tags, 3000, 0xFE); // resent audio packet
    append_video(tags, 5000, 0x13);
    append_audio(tags, 5000, 0x23);
    append_video(tags, 1000, 0x11); // timestamp reset starts a new run
    append_audio(tags, 1000, 0x21);
    append_video(tags, 3000, 0x12); // same timestamp and content as the first run
    append_audio(tags, 3000, 0x22);

    const auto path = write_fixture("flvconcat-runs.flv", tags);
    flvconcat::ScanResult scan;
    flvconcat::ScanOptions scan_options;
    std::string error;
    check(flvconcat::scan_flv(path, scan_options, scan, error), "scanner accepts fixture: " + error);
    check(scan.video_runs.size() == 2, "scanner detects two video runs");
    check(scan.audio_runs.size() == 2, "scanner detects two audio runs");
    check(scan.audio_runs.front().packet_count == 4, "scanner retains audio resend for mux-stage filtering");

    flvconcat::TimelineOptions timeline_options;
    timeline_options.nominal_audio_duration_us = 23'220;
    const auto plan = flvconcat::build_timeline(scan, 0, timeline_options);
    check(plan.pairs.size() == 2, "timeline pairs the two audio/video runs");
    check(plan.duplicate_runs_dropped == 1, "timeline removes content-identical duplicate run");
    check(!plan.pairs[0].drop && plan.pairs[1].drop, "only the second run is marked duplicate");
    check(plan.end_us == 4'050'000, "timeline includes the final video frame duration");

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_timestamp_match_without_content_match_is_kept() {
    std::vector<std::uint8_t> tags;
    append_video(tags, 1000, 0x11);
    append_audio(tags, 1000, 0x21);
    append_video(tags, 3000, 0x12);
    append_audio(tags, 3000, 0x22);
    append_video(tags, 5000, 0x13);
    append_audio(tags, 5000, 0x23);
    append_video(tags, 1000, 0x91); // same timestamp, different encoded content
    append_audio(tags, 1000, 0x21);
    append_video(tags, 3000, 0x92);
    append_audio(tags, 3000, 0x22);

    const auto path = write_fixture("flvconcat-not-duplicate.flv", tags);
    flvconcat::ScanResult scan;
    flvconcat::ScanOptions scan_options;
    std::string error;
    check(flvconcat::scan_flv(path, scan_options, scan, error), "scanner accepts second fixture: " + error);
    const auto plan = flvconcat::build_timeline(scan, 0, {});
    check(plan.duplicate_runs_dropped == 0,
          "timestamp-identical run with different content is not removed");

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_timestamp_wrap_is_not_a_new_run() {
    std::vector<std::uint8_t> tags;
    append_video(tags, 0xFFFFFFF0U, 0x31);
    append_audio(tags, 0xFFFFFFF0U, 0x41);
    append_video(tags, 20U, 0x32);
    append_audio(tags, 20U, 0x42);

    const auto path = write_fixture("flvconcat-wrap.flv", tags);
    flvconcat::ScanResult scan;
    flvconcat::ScanOptions scan_options;
    std::string error;
    check(flvconcat::scan_flv(path, scan_options, scan, error), "scanner accepts wrap fixture: " + error);
    check(scan.video_runs.size() == 1, "32-bit video timestamp wrap stays in one run");
    check(scan.audio_runs.size() == 1, "32-bit audio timestamp wrap stays in one run");
    check(scan.packets.size() == 4, "all wrap fixture packets are indexed");
    if (scan.packets.size() == 4) {
        check(scan.packets[2].timestamp_ms > scan.packets[0].timestamp_ms,
              "wrapped timestamp is unwrapped monotonically");
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

std::vector<std::uint8_t> make_h264_configuration() {
    return {
        0x01, 0x64, 0x00, 0x33, 0xFF, 0xE1, 0x00, 0x1C,
        0x67, 0x64, 0x00, 0x33, 0xAC, 0xEC, 0x02, 0x80,
        0x0B, 0x5B, 0x01, 0x6A, 0x02, 0x02, 0x02, 0x80,
        0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x3C,
        0x47, 0x8C, 0x18, 0x9C,
        0x01, 0x00, 0x04, 0x68, 0xEF, 0xBC, 0xB0,
        0xFD, 0xF8, 0xF8, 0x00};
}

flvconcat::MediaInfo make_compatible_media_info() {
    flvconcat::MediaInfo info;
    info.width = 2560;
    info.height = 1440;
    info.video_codec = AV_CODEC_ID_H264;
    info.audio_codec = AV_CODEC_ID_AAC;
    info.sample_rate = 48'000;
    info.audio_channels = 2;
    info.video_config = make_h264_configuration();
    info.audio_config = {0x11, 0x90, 0x56, 0xE5, 0x00};
    return info;
}

void test_h264_probe() {
    std::vector<std::uint8_t> tags;
    append_h264_sequence(tags, 0, make_h264_configuration());
    append_aac_sequence(tags, 0);
    append_video(tags, 1000, 0x31);
    append_audio(tags, 1000, 0x41);
    const auto path = write_fixture("flvconcat-h264-probe.flv", tags);

    flvconcat::MediaInfo info;
    std::string error;
    const bool probe_ok = flvconcat::probe_media(path, info, error);
    check(probe_ok, "H.264 probe accepts AVC sequence header: " + error);
    if (probe_ok) {
        check(info.video_codec == AV_CODEC_ID_H264 && info.width == 2560 && info.height == 1440,
              "H.264 probe extracts SPS dimensions");
        check(info.audio_codec == AV_CODEC_ID_AAC && info.sample_rate == 48'000 &&
                  info.audio_channels == 2,
              "H.264 probe uses AAC AudioSpecificConfig parameters");
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_semantic_codec_configuration_compatibility() {
    const auto first = make_compatible_media_info();
    auto second = first;

    // These are the configuration-record differences found in the two supplied
    // 20260730 FLVs. Their H.264 SPS/PPS are identical. The first avcC record
    // signals 4:2:0, while the second's optional high-profile extension says
    // monochrome despite the actual stream decoding as yuv420p. The AAC configs
    // differ only by an optional "SBR not present" sync extension.
    second.video_config[43] = 0xFC;
    second.audio_config = {0x11, 0x90};

    std::string reason;
    check(flvconcat::media_compatible(first, second, reason),
          "semantically identical avcC/ASC configurations are accepted: " + reason);

    auto different_pps = second;
    different_pps.video_config[42] ^= 0x01;
    check(!flvconcat::media_compatible(first, different_pps, reason) &&
              reason == "H.264 SPS/PPS or NAL length differs",
          "changed H.264 PPS remains incompatible");

    auto different_aac = second;
    different_aac.audio_config = {0x12, 0x10}; // AAC-LC, 44.1 kHz, stereo
    check(!flvconcat::media_compatible(first, different_aac, reason) &&
              reason == "AAC object type, sample rate, channel layout, or frame length differs",
          "changed AAC AudioSpecificConfig remains incompatible");
}

std::vector<std::uint8_t> make_h265_configuration() {
    return {
        0x01, 0x01, 0x60, 0x00, 0x00, 0x00, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x78, 0xF0, 0x00, 0xFC, 0xFD, 0xF8, 0xF8, 0x00, 0x00, 0x0F, 0x03,
        0xA0, 0x00, 0x01, 0x00, 0x18, 0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF, 0x01,
        0x60,
        0x00, 0x00, 0x03, 0x00, 0xB0, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
        0x00, 0x78, 0x17, 0x02, 0x40, 0xA1, 0x00, 0x01, 0x00, 0x29, 0x42,
        0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0xB0, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x03, 0x00, 0x78, 0xA0, 0x02, 0x20, 0x80, 0x1E,
        0x07, 0x2F, 0x88, 0x17, 0xB9, 0x16, 0x45, 0x2F, 0xFC, 0xB9, 0xFC,
        0x4F, 0xEA, 0x6A, 0x02, 0x02, 0x02, 0x01, 0xA2, 0x00, 0x01, 0x00,
        0x07, 0x44, 0x01, 0xC0, 0x72, 0xF0, 0x53, 0x24};
}

void test_h265_probe_and_scanner() {
    const auto configuration = make_h265_configuration();
    std::vector<std::uint8_t> tags;
    append_h265_sequence(tags, 0, configuration);
    append_aac_sequence(tags, 0);
    append_h265_video(tags, 1000, 0x31);
    append_audio(tags, 1000, 0x41);

    const auto path = write_fixture("flvconcat-h265.flv", tags);
    flvconcat::MediaInfo info;
    std::string error;
    const bool probe_ok = flvconcat::probe_media(path, info, error);
    check(probe_ok, "H.265 probe accepts legacy codec_id=12: " + error);
    if (!probe_ok) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return;
    }
    check(info.video_codec == AV_CODEC_ID_HEVC, "H.265 probe reports HEVC codec");
    check(info.width == 1080 && info.height == 1920,
          "H.265 SPS dimensions account for the portrait conformance window");
    check(info.audio_codec == AV_CODEC_ID_AAC && info.sample_rate == 48'000 &&
              info.audio_channels == 2,
          "H.265 probe uses AAC AudioSpecificConfig parameters");

    flvconcat::ScanResult scan;
    flvconcat::ScanOptions options;
    check(flvconcat::scan_flv(path, options, scan, error),
          "H.265 scanner accepts codec_id=12 packets: " + error);
    check(scan.video_runs.size() == 1 && scan.audio_runs.size() == 1,
          "H.265 scanner creates one audio/video run");
    check(scan.packets.size() == 2 && scan.packets.front().flv_media_header_size == 5,
          "H.265 scanner strips the five-byte video media header");

    auto metadata_variant = info;
    metadata_variant.video_config[13] ^= 0x01; // hvcC header metadata, not VPS/SPS/PPS.
    check(flvconcat::media_compatible(info, metadata_variant, error),
          "H.265 hvcC metadata differences remain semantically compatible");

    auto changed_sps = info;
    changed_sps.video_config[57] ^= 0x01;
    check(!flvconcat::media_compatible(info, changed_sps, error),
          "H.265 SPS differences remain incompatible");

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace

int main() {
    test_runs_pairing_and_duplicate_content();
    test_timestamp_match_without_content_match_is_kept();
    test_timestamp_wrap_is_not_a_new_run();
    test_h264_probe();
    test_semantic_codec_configuration_compatibility();
    test_h265_probe_and_scanner();
    if (failures == 0) {
        std::cout << "All core tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
