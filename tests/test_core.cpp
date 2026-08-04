#include "flvconcat/scanner.hpp"
#include "flvconcat/timeline.hpp"

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

} // namespace

int main() {
    test_runs_pairing_and_duplicate_content();
    test_timestamp_match_without_content_match_is_kept();
    test_timestamp_wrap_is_not_a_new_run();
    if (failures == 0) {
        std::cout << "All core tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
