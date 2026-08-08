#pragma once

#include <cstddef>
#include <cstdint>

namespace flvconcat::codecs::internal {

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool read(unsigned count, std::uint32_t& value) {
        if (count > 32 || bit_offset_ + count > size_ * 8U) {
            return false;
        }
        value = 0;
        for (unsigned index = 0; index < count; ++index) {
            const auto byte = data_[(bit_offset_ + index) / 8U];
            const auto shift = 7U - static_cast<unsigned>((bit_offset_ + index) % 8U);
            value = (value << 1U) | ((byte >> shift) & 1U);
        }
        bit_offset_ += count;
        return true;
    }

    bool skip(unsigned count) {
        std::uint32_t ignored = 0;
        while (count > 0) {
            const auto chunk = count > 32 ? 32U : count;
            if (!read(chunk, ignored)) {
                return false;
            }
            count -= chunk;
        }
        return true;
    }

    bool read_unsigned_exp_golomb(std::uint32_t& value) {
        unsigned leading_zero_bits = 0;
        std::uint32_t bit = 0;
        while (true) {
            if (!read(1, bit)) {
                return false;
            }
            if (bit != 0) {
                break;
            }
            if (++leading_zero_bits > 31) {
                return false;
            }
        }
        std::uint32_t suffix = 0;
        if (leading_zero_bits != 0 && !read(leading_zero_bits, suffix)) {
            return false;
        }
        value = ((std::uint32_t{1} << leading_zero_bits) - 1U) + suffix;
        return true;
    }

    bool read_signed_exp_golomb(std::int32_t& value) {
        std::uint32_t code = 0;
        if (!read_unsigned_exp_golomb(code)) {
            return false;
        }
        if ((code & 1U) != 0) {
            value = static_cast<std::int32_t>((code + 1U) / 2U);
        } else {
            value = -static_cast<std::int32_t>(code / 2U);
        }
        return true;
    }

    [[nodiscard]] std::size_t remaining() const {
        return size_ * 8U - bit_offset_;
    }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t bit_offset_ = 0;
};

} // namespace flvconcat::codecs::internal
