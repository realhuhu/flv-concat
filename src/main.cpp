#include "flvconcat/media.hpp"
#include "flvconcat/scanner.hpp"
#include "flvconcat/timeline.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#ifndef FLVCONCAT_VERSION
#define FLVCONCAT_VERSION "dev"
#endif

namespace {

struct CliOptions {
    std::vector<std::filesystem::path> inputs;
    std::optional<std::filesystem::path> output;
    std::optional<std::int64_t> video_offset_ms;
    std::int64_t run_gap_ms = 2000;
    double duplicate_threshold = 0.90;
    bool keep_order = false;
    bool overwrite = false;
    bool faststart = true;
};

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

void print_help(std::ostream& output) {
    output
        << "FLVConcat " << FLVCONCAT_VERSION << "\n"
        << "Repair live-recording FLV timestamps and losslessly merge to MP4.\n\n"
        << "Usage:\n"
        << "  flvconcat [options] <input.flv> [more.flv ...]\n\n"
        << "Options:\n"
        << "  -o, --output <file>          Output MP4 path\n"
        << "      --av-offset <ms>         Add to video PTS (positive = video later)\n"
        << "      --run-gap <ms>           Backward jump that starts a new run (default: 2000)\n"
        << "      --duplicate-threshold N  Matching fraction for duplicate runs (default: 0.90)\n"
        << "      --keep-order             Keep command-line order instead of filename order\n"
        << "  -f, --overwrite              Replace an existing output after a successful merge\n"
        << "      --no-faststart           Do not move MP4 metadata to the beginning\n"
        << "  -h, --help                   Show this help\n"
        << "  -V, --version                Show version\n\n"
        << "Windows: you can also drag one or more .flv files onto flvconcat.exe.\n";
}

std::int64_t parse_integer(const std::string& text, const char* option) {
    std::size_t used = 0;
    try {
        const auto value = std::stoll(text, &used);
        if (used != text.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return value;
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("invalid integer for ") + option + ": " + text);
    }
}

double parse_fraction(const std::string& text, const char* option) {
    std::size_t used = 0;
    try {
        const auto value = std::stod(text, &used);
        if (used != text.size() || value <= 0.0 || value > 1.0) {
            throw std::invalid_argument("outside range");
        }
        return value;
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("expected a number in (0, 1] for ") + option +
                                 ": " + text);
    }
}

enum class ParseResult { run, help, version };

ParseResult parse_cli(const std::vector<std::filesystem::path>& arguments, CliOptions& options) {
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string token = arguments[index].u8string();
        const auto take_value = [&](const char* option) -> const std::filesystem::path& {
            if (++index >= arguments.size()) {
                throw std::runtime_error(std::string("missing value for ") + option);
            }
            return arguments[index];
        };

        if (token == "-h" || token == "--help") {
            return ParseResult::help;
        }
        if (token == "-V" || token == "--version") {
            return ParseResult::version;
        }
        if (token == "-o" || token == "--output") {
            options.output = take_value(token.c_str());
        } else if (token == "--av-offset") {
            options.video_offset_ms =
                parse_integer(take_value(token.c_str()).u8string(), token.c_str());
        } else if (token == "--run-gap") {
            options.run_gap_ms = parse_integer(take_value(token.c_str()).u8string(), token.c_str());
            if (options.run_gap_ms <= 0) {
                throw std::runtime_error("--run-gap must be positive");
            }
        } else if (token == "--duplicate-threshold") {
            options.duplicate_threshold =
                parse_fraction(take_value(token.c_str()).u8string(), token.c_str());
        } else if (token == "--keep-order") {
            options.keep_order = true;
        } else if (token == "-f" || token == "--overwrite") {
            options.overwrite = true;
        } else if (token == "--no-faststart") {
            options.faststart = false;
        } else if (!token.empty() && token.front() == '-') {
            throw std::runtime_error("unknown option: " + token);
        } else {
            options.inputs.push_back(arguments[index]);
        }
    }
    return ParseResult::run;
}

std::optional<std::int64_t> read_offset_file(const std::filesystem::path& executable) {
    const std::vector<std::filesystem::path> candidates = {
        executable.parent_path() / "avoffset.txt",
        std::filesystem::current_path() / "avoffset.txt"};
    for (const auto& candidate : candidates) {
        std::ifstream input(candidate);
        std::int64_t value = 0;
        if (input && (input >> value)) {
            return value;
        }
    }
    return std::nullopt;
}

std::filesystem::path default_output(const std::vector<std::filesystem::path>& inputs) {
    auto result = inputs.front().parent_path() / inputs.front().stem();
    result += inputs.size() == 1 ? "_fixed.mp4" : "_merged.mp4";
    return result;
}

std::filesystem::path temporary_output(const std::filesystem::path& output) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    auto temporary = output;
    temporary += ".part-" + std::to_string(nonce) + ".mp4";
    return temporary;
}

void remove_temporary(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

int run(const std::filesystem::path& executable,
        const std::vector<std::filesystem::path>& arguments) {
    CliOptions options;
    ParseResult parse_result;
    try {
        parse_result = parse_cli(arguments, options);
    } catch (const std::exception& exception) {
        std::cerr << "error: " << exception.what() << "\n\n";
        print_help(std::cerr);
        return 2;
    }
    if (parse_result == ParseResult::help) {
        print_help(std::cout);
        return 0;
    }
    if (parse_result == ParseResult::version) {
        std::cout << "flvconcat " << FLVCONCAT_VERSION << '\n';
        return 0;
    }
    if (options.inputs.empty()) {
        print_help(std::cout);
        return 2;
    }

    for (const auto& input : options.inputs) {
        std::error_code status_error;
        if (!std::filesystem::is_regular_file(input, status_error)) {
            std::cerr << "error: input is not a regular file: " << input.u8string() << '\n';
            return 2;
        }
        if (lower_ascii(input.extension().u8string()) != ".flv") {
            std::cerr << "error: only FLV inputs are accepted: " << input.u8string() << '\n';
            return 2;
        }
    }
    if (!options.keep_order) {
        std::stable_sort(options.inputs.begin(), options.inputs.end(),
                         [](const auto& left, const auto& right) {
                             return lower_ascii(left.filename().u8string()) <
                                    lower_ascii(right.filename().u8string());
                         });
    }

    const auto output = options.output.value_or(default_output(options.inputs));
    std::error_code output_status_error;
    if (std::filesystem::exists(output, output_status_error) && !options.overwrite) {
        std::cerr << "error: output already exists (use --overwrite): " << output.u8string() << '\n';
        return 2;
    }
    if (!output.parent_path().empty() &&
        !std::filesystem::is_directory(output.parent_path(), output_status_error)) {
        std::cerr << "error: output directory does not exist: "
                  << output.parent_path().u8string() << '\n';
        return 2;
    }

    std::cout << "Checking " << options.inputs.size() << " input file(s)...\n";
    std::vector<flvconcat::MediaInfo> media(options.inputs.size());
    std::string error;
    for (std::size_t index = 0; index < options.inputs.size(); ++index) {
        if (!flvconcat::probe_media(options.inputs[index], media[index], error)) {
            std::cerr << "error: " << error << '\n';
            return 1;
        }
        std::cout << "  [" << (index + 1) << "] " << options.inputs[index].filename().u8string()
                  << "  " << flvconcat::media_summary(media[index]) << '\n';
        if (index > 0) {
            std::string reason;
            if (!flvconcat::media_compatible(media.front(), media[index], reason)) {
                std::cerr << "error: input " << (index + 1)
                          << " cannot be merged because its " << reason << '\n';
                return 1;
            }
        }
    }

    if (!options.video_offset_ms) {
        options.video_offset_ms = read_offset_file(executable).value_or(0);
    }
    std::cout << "Video presentation offset: " << std::showpos << *options.video_offset_ms
              << std::noshowpos << " ms\n";
    std::cout << "Output: " << output.u8string() << '\n';

    const auto temporary = temporary_output(output);
    flvconcat::MuxOptions mux_options;
    mux_options.video_offset_us = *options.video_offset_ms * 1000;
    mux_options.faststart = options.faststart;
    flvconcat::Mp4Muxer muxer;
    if (!muxer.open(temporary, media.front(), mux_options, error)) {
        remove_temporary(temporary);
        std::cerr << "error: " << error << '\n';
        return 1;
    }

    flvconcat::ScanOptions scan_options;
    scan_options.run_gap_ms = options.run_gap_ms;
    flvconcat::TimelineOptions timeline_options;
    timeline_options.duplicate_threshold = options.duplicate_threshold;
    timeline_options.nominal_audio_duration_us =
        1024LL * 1'000'000LL / media.front().sample_rate;
    std::int64_t cursor_us = 0;

    for (std::size_t index = 0; index < options.inputs.size(); ++index) {
        std::cout << '[' << (index + 1) << '/' << options.inputs.size() << "] Scanning "
                  << options.inputs[index].filename().u8string() << "...\n";
        flvconcat::ScanResult scan;
        if (!flvconcat::scan_flv(options.inputs[index], scan_options, scan, error)) {
            muxer.abort();
            remove_temporary(temporary);
            std::cerr << "error: " << error << '\n';
            return 1;
        }
        const auto plan = flvconcat::build_timeline(scan, cursor_us, timeline_options);
        std::cout << "  " << scan.video_runs.size() << " video run(s), "
                  << scan.audio_runs.size() << " audio run(s), "
                  << plan.duplicate_runs_dropped << " duplicate run(s) removed\n";
        if (!muxer.write_file(scan, plan, error)) {
            muxer.abort();
            remove_temporary(temporary);
            std::cerr << "error: " << error << '\n';
            return 1;
        }
        cursor_us = plan.end_us;
    }

    if (!muxer.finish(error)) {
        remove_temporary(temporary);
        std::cerr << "error: " << error << '\n';
        return 1;
    }

    if (std::filesystem::exists(output, output_status_error)) {
        std::filesystem::remove(output, output_status_error);
        if (output_status_error) {
            remove_temporary(temporary);
            std::cerr << "error: cannot replace output: " << output_status_error.message() << '\n';
            return 1;
        }
    }
    std::filesystem::rename(temporary, output, output_status_error);
    if (output_status_error) {
        remove_temporary(temporary);
        std::cerr << "error: cannot move completed file into place: "
                  << output_status_error.message() << '\n';
        return 1;
    }

    const auto& stats = muxer.stats();
    std::cout << "Done: " << stats.video_packets << " video packets, "
              << stats.audio_packets << " audio packets, "
              << stats.resent_audio_packets_dropped << " resent audio packets removed, "
              << stats.duplicate_runs_dropped << " duplicate run(s) removed, "
              << std::fixed << std::setprecision(2)
              << static_cast<double>(stats.duration_us) / 60'000'000.0 << " minutes.\n";
    return 0;
}

#ifdef _WIN32
void pause_if_launched_from_explorer() {
    std::array<DWORD, 2> processes{};
    if (GetConsoleProcessList(processes.data(), static_cast<DWORD>(processes.size())) == 1) {
        std::cout << "Press Enter to close..." << std::flush;
        std::cin.get();
    }
}
#endif

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::vector<std::filesystem::path> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    const int result = run(std::filesystem::path(argv[0]), arguments);
    pause_if_launched_from_explorer();
    return result;
}
#else
int main(int argc, char** argv) {
    std::vector<std::filesystem::path> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    return run(std::filesystem::path(argv[0]), arguments);
}
#endif
