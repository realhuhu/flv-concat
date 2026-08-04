#include "flvconcat/timeline.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace flvconcat {
namespace {

bool duplicate_of(const RunInfo& candidate,
                  const RunInfo& kept,
                  const TimelineOptions& options) {
    if (candidate.signatures.empty() || kept.signatures.empty()) {
        return false;
    }
    if (candidate.first_ms < kept.first_ms - options.duplicate_range_tolerance_ms ||
        candidate.last_ms > kept.last_ms + options.duplicate_range_tolerance_ms ||
        kept.packet_count < candidate.packet_count) {
        return false;
    }

    std::size_t matches = 0;
    for (const auto& [timestamp, fingerprint] : candidate.signatures) {
        const auto found = kept.signatures.find(timestamp);
        if (found != kept.signatures.end() && found->second == fingerprint) {
            ++matches;
        }
    }
    const auto required = static_cast<std::size_t>(
        std::ceil(static_cast<double>(candidate.signatures.size()) *
                  options.duplicate_threshold));
    return matches >= required;
}

} // namespace

FilePlan build_timeline(const ScanResult& scan,
                        std::int64_t start_us,
                        const TimelineOptions& options) {
    FilePlan plan;
    plan.start_us = start_us;

    std::vector<bool> audio_used(scan.audio_runs.size(), false);
    for (std::size_t video_index = 0; video_index < scan.video_runs.size(); ++video_index) {
        PairPlan pair;
        pair.video_run = static_cast<int>(video_index);
        double best_score = options.pair_overlap_threshold;
        int best_audio = -1;
        const auto& video = scan.video_runs[video_index];
        const double video_length = static_cast<double>(video.last_ms - video.first_ms) + 1.0;

        for (std::size_t audio_index = 0; audio_index < scan.audio_runs.size(); ++audio_index) {
            if (audio_used[audio_index]) {
                continue;
            }
            const auto& audio = scan.audio_runs[audio_index];
            const auto overlap = std::min(video.last_ms, audio.last_ms) -
                                 std::max(video.first_ms, audio.first_ms);
            if (overlap <= 0) {
                continue;
            }
            const double audio_length = static_cast<double>(audio.last_ms - audio.first_ms) + 1.0;
            const double score = static_cast<double>(overlap) /
                                 (video_length + audio_length - static_cast<double>(overlap));
            if (score >= best_score) {
                best_score = score;
                best_audio = static_cast<int>(audio_index);
            }
        }
        if (best_audio >= 0) {
            pair.audio_run = best_audio;
            audio_used[static_cast<std::size_t>(best_audio)] = true;
        }
        plan.pairs.push_back(pair);
    }

    for (std::size_t audio_index = 0; audio_index < scan.audio_runs.size(); ++audio_index) {
        if (!audio_used[audio_index]) {
            PairPlan pair;
            pair.audio_run = static_cast<int>(audio_index);
            plan.pairs.push_back(pair);
        }
    }

    std::vector<int> kept_video_runs;
    for (auto& pair : plan.pairs) {
        if (pair.video_run < 0) {
            continue;
        }
        const auto& candidate = scan.video_runs[static_cast<std::size_t>(pair.video_run)];
        for (const int kept_index : kept_video_runs) {
            if (duplicate_of(candidate,
                             scan.video_runs[static_cast<std::size_t>(kept_index)],
                             options)) {
                pair.drop = true;
                ++plan.duplicate_runs_dropped;
                break;
            }
        }
        if (!pair.drop) {
            kept_video_runs.push_back(pair.video_run);
        }
    }

    auto cursor_us = start_us;
    const auto video_tail_ms = std::max<std::int64_t>(1, options.nominal_video_duration_us / 1000);
    const auto audio_tail_ms = std::max<std::int64_t>(1, options.nominal_audio_duration_us / 1000);
    for (auto& pair : plan.pairs) {
        if (pair.drop) {
            continue;
        }
        pair.base_us = cursor_us;
        auto zero_ms = std::numeric_limits<std::int64_t>::max();
        auto end_ms = std::numeric_limits<std::int64_t>::min();
        if (pair.video_run >= 0) {
            const auto& video = scan.video_runs[static_cast<std::size_t>(pair.video_run)];
            zero_ms = std::min(zero_ms, video.first_ms);
            end_ms = std::max(end_ms, video.last_ms + video_tail_ms);
        }
        if (pair.audio_run >= 0) {
            const auto& audio = scan.audio_runs[static_cast<std::size_t>(pair.audio_run)];
            zero_ms = std::min(zero_ms, audio.first_ms);
            end_ms = std::max(end_ms, audio.last_ms + audio_tail_ms);
        }
        if (zero_ms == std::numeric_limits<std::int64_t>::max()) {
            pair.zero_ms = 0;
            pair.span_us = 0;
        } else {
            pair.zero_ms = zero_ms;
            pair.span_us = std::max<std::int64_t>(0, (end_ms - zero_ms) * 1000);
        }
        cursor_us += pair.span_us;
    }
    plan.end_us = cursor_us;
    return plan;
}

} // namespace flvconcat
