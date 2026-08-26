#include "amf0_metadata.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace flvconcat {
namespace {

constexpr std::size_t kMaximumMetadataFields = 512;
constexpr std::size_t kMaximumMetadataBytes = 1024U * 1024U;
constexpr std::size_t kMaximumValueBytes = 64U * 1024U;
constexpr std::size_t kMaximumKeyBytes = 512;
constexpr std::size_t kMaximumArrayElements = 1'000'000;
constexpr unsigned kMaximumDepth = 16;

std::string normalized_key(std::string_view key) {
    std::string normalized;
    normalized.reserve(key.size());
    for (const unsigned char byte : key) {
        if (byte >= 'A' && byte <= 'Z') {
            normalized.push_back(static_cast<char>(byte - 'A' + 'a'));
        } else if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')) {
            normalized.push_back(static_cast<char>(byte));
        }
    }
    return normalized;
}

bool is_playback_metadata(std::string_view key) {
    static const std::unordered_set<std::string> keys = {
        "duration",
        "filesize",
        "datasize",
        "videosize",
        "audiosize",
        "width",
        "height",
        "displaywidth",
        "displayheight",
        "fps",
        "framerate",
        "videoframerate",
        "videodatarate",
        "audiodatarate",
        "datarate",
        "bitrate",
        "videocodecid",
        "audiocodecid",
        "audiosamplerate",
        "audiosamplesize",
        "audiochannels",
        "stereo",
        "hasvideo",
        "hasaudio",
        "hasmetadata",
        "haskeyframes",
        "canseektoend",
        "keyframes",
        "lasttimestamp",
        "lastkeyframetimestamp",
        "lastkeyframelocation",
        "numdisposableframes",
    };
    return keys.find(normalized_key(key)) != keys.end();
}

std::string clean_utf8(std::string_view input, std::size_t maximum_bytes) {
    std::string output;
    output.reserve(std::min(input.size(), maximum_bytes));
    for (std::size_t index = 0; index < input.size() && output.size() < maximum_bytes;) {
        const auto first = static_cast<unsigned char>(input[index]);
        if (first == 0) {
            if (output.size() + 3 > maximum_bytes) {
                break;
            }
            output.append("\xEF\xBF\xBD");
            ++index;
            continue;
        }
        if (first < 0x80) {
            output.push_back(static_cast<char>(first));
            ++index;
            continue;
        }

        std::size_t sequence_size = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if ((first & 0xE0U) == 0xC0U) {
            sequence_size = 2;
            codepoint = first & 0x1FU;
            minimum = 0x80;
        } else if ((first & 0xF0U) == 0xE0U) {
            sequence_size = 3;
            codepoint = first & 0x0FU;
            minimum = 0x800;
        } else if ((first & 0xF8U) == 0xF0U) {
            sequence_size = 4;
            codepoint = first & 0x07U;
            minimum = 0x10000;
        }

        bool valid = sequence_size != 0 && index + sequence_size <= input.size();
        for (std::size_t continuation = 1; valid && continuation < sequence_size;
             ++continuation) {
            const auto byte = static_cast<unsigned char>(input[index + continuation]);
            if ((byte & 0xC0U) != 0x80U) {
                valid = false;
            } else {
                codepoint = (codepoint << 6U) | (byte & 0x3FU);
            }
        }
        valid = valid && codepoint >= minimum && codepoint <= 0x10FFFFU &&
                !(codepoint >= 0xD800U && codepoint <= 0xDFFFU);
        if (!valid) {
            if (output.size() + 3 > maximum_bytes) {
                break;
            }
            output.append("\xEF\xBF\xBD");
            ++index;
            continue;
        }
        if (output.size() + sequence_size > maximum_bytes) {
            break;
        }
        output.append(input.substr(index, sequence_size));
        index += sequence_size;
    }
    return output;
}

std::string number_text(double value) {
    if (!std::isfinite(value)) {
        return {};
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return output.str();
}

std::string date_text(double milliseconds) {
    const auto wide_milliseconds = static_cast<long double>(milliseconds);
    if (!std::isfinite(milliseconds) ||
        wide_milliseconds <
            static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
        wide_milliseconds >
            static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return {};
    }

    const auto total_milliseconds = static_cast<std::int64_t>(std::llround(milliseconds));
    auto epoch_seconds = total_milliseconds / 1000;
    auto remainder = total_milliseconds % 1000;
    if (remainder < 0) {
        remainder += 1000;
        --epoch_seconds;
    }
    const auto timestamp = static_cast<std::time_t>(epoch_seconds);
    if (static_cast<std::int64_t>(timestamp) != epoch_seconds) {
        return {};
    }

    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &timestamp) != 0) {
        return {};
    }
#else
    if (!gmtime_r(&timestamp, &utc)) {
        return {};
    }
#endif
    std::array<char, 32> date{};
    if (std::strftime(date.data(), date.size(), "%Y-%m-%dT%H:%M:%S", &utc) == 0) {
        return {};
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << date.data() << '.' << std::setw(3) << std::setfill('0') << remainder << 'Z';
    return output.str();
}

class Amf0Reader {
public:
    Amf0Reader(const std::uint8_t* data, std::size_t size, MetadataMap& metadata)
        : current_(data), end_(data + size), metadata_(metadata) {
        for (const auto& [key, value] : metadata_) {
            metadata_bytes_ += key.size() + value.size();
        }
    }

    bool parse_script() {
        std::string event;
        if (!read_string_value(event)) {
            return false;
        }
        if (event == "@setDataFrame") {
            if (!read_string_value(event)) {
                return false;
            }
        }
        if (event != "onMetaData") {
            return false;
        }
        return parse_value({}, true, 0);
    }

private:
    bool read_u8(std::uint8_t& value) {
        if (current_ == end_) {
            return false;
        }
        value = *current_++;
        return true;
    }

    bool read_u16(std::uint16_t& value) {
        if (static_cast<std::size_t>(end_ - current_) < 2) {
            return false;
        }
        value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(current_[0]) << 8U) |
                                           current_[1]);
        current_ += 2;
        return true;
    }

    bool read_u32(std::uint32_t& value) {
        if (static_cast<std::size_t>(end_ - current_) < 4) {
            return false;
        }
        value = (static_cast<std::uint32_t>(current_[0]) << 24U) |
                (static_cast<std::uint32_t>(current_[1]) << 16U) |
                (static_cast<std::uint32_t>(current_[2]) << 8U) |
                current_[3];
        current_ += 4;
        return true;
    }

    bool read_double(double& value) {
        if (static_cast<std::size_t>(end_ - current_) < 8) {
            return false;
        }
        std::uint64_t bits = 0;
        for (int index = 0; index < 8; ++index) {
            bits = (bits << 8U) | current_[index];
        }
        current_ += 8;
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }

    bool read_bytes(std::size_t size, std::string& value) {
        if (size > static_cast<std::size_t>(end_ - current_)) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(current_), size);
        current_ += size;
        return true;
    }

    bool read_short_string(std::string& value) {
        std::uint16_t size = 0;
        return read_u16(size) && read_bytes(size, value);
    }

    bool read_long_string(std::string& value) {
        std::uint32_t size = 0;
        return read_u32(size) && read_bytes(size, value);
    }

    bool read_string_value(std::string& value) {
        std::uint8_t type = 0;
        if (!read_u8(type)) {
            return false;
        }
        if (type == 2) {
            return read_short_string(value);
        }
        if (type == 12) {
            return read_long_string(value);
        }
        return false;
    }

    std::string root_key(std::string_view path) const {
        const auto separator = path.find_first_of(".[");
        return std::string(path.substr(0, separator));
    }

    void add_value(const std::string& path, const std::string& raw_value) {
        if (path.empty() || raw_value.empty() || metadata_.size() >= kMaximumMetadataFields ||
            metadata_bytes_ >= kMaximumMetadataBytes) {
            return;
        }
        const auto root = root_key(path);
        if (is_playback_metadata(root)) {
            return;
        }
        auto key = clean_utf8(path, kMaximumKeyBytes);
        auto value = clean_utf8(raw_value, kMaximumValueBytes);
        if (key.empty() || value.empty() || metadata_.find(key) != metadata_.end()) {
            return;
        }
        const auto identity = normalized_key(key);
        if (!identity.empty()) {
            for (const auto& [existing_key, existing_value] : metadata_) {
                (void)existing_value;
                if (normalized_key(existing_key) == identity) {
                    return;
                }
            }
        }
        const auto available = kMaximumMetadataBytes - metadata_bytes_;
        if (key.size() >= available) {
            return;
        }
        if (key.size() + value.size() > available) {
            value = clean_utf8(raw_value,
                               std::min(kMaximumValueBytes, available - key.size()));
        }
        if (value.empty()) {
            return;
        }
        metadata_bytes_ += key.size() + value.size();
        metadata_.emplace(std::move(key), std::move(value));
    }

    bool parse_properties(const std::string& path, bool capture, unsigned depth) {
        while (current_ < end_) {
            if (static_cast<std::size_t>(end_ - current_) >= 3 && current_[0] == 0 &&
                current_[1] == 0 && current_[2] == 9) {
                current_ += 3;
                return true;
            }

            std::string name;
            if (!read_short_string(name)) {
                return false;
            }
            const auto clean_name = clean_utf8(name, kMaximumKeyBytes);
            const auto child_path = path.empty() ? clean_name : path + "." + clean_name;
            const auto root = root_key(child_path);
            if (!parse_value(child_path, capture && !is_playback_metadata(root), depth + 1)) {
                return false;
            }
        }
        return false;
    }

    bool parse_value(const std::string& path, bool capture, unsigned depth) {
        if (depth > kMaximumDepth) {
            return false;
        }
        std::uint8_t type = 0;
        if (!read_u8(type)) {
            return false;
        }
        switch (type) {
        case 0: {
            double value = 0;
            if (!read_double(value)) {
                return false;
            }
            if (capture) {
                add_value(path, number_text(value));
            }
            return true;
        }
        case 1: {
            std::uint8_t value = 0;
            if (!read_u8(value)) {
                return false;
            }
            if (capture) {
                add_value(path, value == 0 ? "false" : "true");
            }
            return true;
        }
        case 2:
        case 4: {
            std::string value;
            if (!read_short_string(value)) {
                return false;
            }
            if (capture) {
                add_value(path, value);
            }
            return true;
        }
        case 3:
            return parse_properties(path, capture, depth);
        case 5:
        case 6:
        case 13:
            return true;
        case 7: {
            std::uint16_t reference = 0;
            return read_u16(reference);
        }
        case 8: {
            std::uint32_t expected_properties = 0;
            if (!read_u32(expected_properties)) {
                return false;
            }
            (void)expected_properties;
            return parse_properties(path, capture, depth);
        }
        case 9:
            return false;
        case 10: {
            std::uint32_t count = 0;
            if (!read_u32(count) || count > kMaximumArrayElements) {
                return false;
            }
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto child_path = path + "[" + std::to_string(index) + "]";
                if (!parse_value(child_path, capture, depth + 1)) {
                    return false;
                }
            }
            return true;
        }
        case 11: {
            double milliseconds = 0;
            std::uint16_t timezone = 0;
            if (!read_double(milliseconds) || !read_u16(timezone)) {
                return false;
            }
            if (capture) {
                auto value = date_text(milliseconds);
                if (value.empty()) {
                    value = number_text(milliseconds);
                }
                add_value(path, value);
            }
            return true;
        }
        case 12:
        case 15: {
            std::string value;
            if (!read_long_string(value)) {
                return false;
            }
            if (capture) {
                add_value(path, value);
            }
            return true;
        }
        case 16: {
            std::string class_name;
            if (!read_short_string(class_name)) {
                return false;
            }
            return parse_properties(path, capture, depth);
        }
        default:
            // AMF3 and obsolete complex values cannot be skipped safely without
            // knowing their full representation. Ignore this ScriptData tag.
            return false;
        }
    }

    const std::uint8_t* current_;
    const std::uint8_t* end_;
    MetadataMap& metadata_;
    std::size_t metadata_bytes_ = 0;
};

} // namespace

bool parse_amf0_script_metadata(const std::uint8_t* data,
                                std::size_t size,
                                MetadataMap& metadata) {
    if (!data || size == 0) {
        return false;
    }
    MetadataMap parsed = metadata;
    Amf0Reader reader(data, size, parsed);
    if (!reader.parse_script()) {
        return false;
    }
    metadata = std::move(parsed);
    return true;
}

} // namespace flvconcat
