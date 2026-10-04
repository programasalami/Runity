#pragma once

// Little-endian byte reader/writer used by the generated codecs (see Protocol/schema/protocol.toml for the wire rules).

#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace runity::protocol {

static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big);

namespace detail {
template <class T>
T to_little(T v) noexcept {
    if constexpr (std::endian::native == std::endian::little || sizeof(T) == 1) {
        return v;
    } else {
        return std::byteswap(v);
    }
}
}  // namespace detail

/// Appends little-endian values to a growable buffer. A value that cannot be represented (a string or list longer than
/// 65535 elements) marks the writer as failed instead of writing a truncated value; check fail() before sending.
class ByteWriter {
public:
    ByteWriter() = default;
    explicit ByteWriter(std::size_t reserve) { buffer_.reserve(reserve); }

    template <class T>
        requires std::is_arithmetic_v<T>
    void write(T value) {
        if constexpr (std::is_same_v<T, bool>) {
            buffer_.push_back(value ? 1 : 0);
        } else if constexpr (std::is_floating_point_v<T>) {
            using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
            write(std::bit_cast<Bits>(value));
        } else {
            const T le = detail::to_little(value);
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&le);
            buffer_.insert(buffer_.end(), bytes, bytes + sizeof(T));
        }
    }

    void write(const std::string& s) {
        if (s.size() > std::numeric_limits<std::uint16_t>::max()) {
            failed_ = true;
            return;
        }
        write(static_cast<std::uint16_t>(s.size()));
        buffer_.insert(buffer_.end(), s.begin(), s.end());
    }

    void write_list_count(std::size_t count) {
        if (count > std::numeric_limits<std::uint16_t>::max()) {
            failed_ = true;
            return;
        }
        write(static_cast<std::uint16_t>(count));
    }

    void write_raw(std::span<const std::uint8_t> bytes) { buffer_.insert(buffer_.end(), bytes.begin(), bytes.end()); }

    /// Overwrites 4 bytes at an earlier position (used for frame length back-patching).
    void patch_u32(std::size_t offset, std::uint32_t value) {
        const std::uint32_t le = detail::to_little(value);
        std::memcpy(buffer_.data() + offset, &le, sizeof le);
    }

    [[nodiscard]] bool fail() const noexcept { return failed_; }
    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
    [[nodiscard]] std::span<const std::uint8_t> view() const noexcept { return buffer_; }
    [[nodiscard]] std::vector<std::uint8_t> take() noexcept { return std::move(buffer_); }
    void clear() noexcept {
        buffer_.clear();
        failed_ = false;
    }

private:
    std::vector<std::uint8_t> buffer_;
    bool failed_ = false;
};

/// Reads little-endian values from a byte span. Any out-of-range read marks the reader as failed; every later read fails too.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

    template <class T>
        requires std::is_arithmetic_v<T>
    [[nodiscard]] bool read(T& out) noexcept {
        if constexpr (std::is_same_v<T, bool>) {
            std::uint8_t raw = 0;
            if (!read(raw)) return false;
            if (raw > 1) return fail_now();
            out = raw == 1;
            return true;
        } else if constexpr (std::is_floating_point_v<T>) {
            using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
            Bits bits{};
            if (!read(bits)) return false;
            out = std::bit_cast<T>(bits);
            return true;
        } else {
            if (!has(sizeof(T))) return fail_now();
            T raw;
            std::memcpy(&raw, data_.data() + pos_, sizeof(T));
            pos_ += sizeof(T);
            out = detail::to_little(raw);
            return true;
        }
    }

    [[nodiscard]] bool read(std::string& out) {
        std::uint16_t len = 0;
        if (!read(len)) return false;
        if (!has(len)) return fail_now();
        out.assign(reinterpret_cast<const char*>(data_.data() + pos_), len);
        pos_ += len;
        return true;
    }

    [[nodiscard]] bool read_list_count(std::uint16_t& count) noexcept { return read(count); }

    [[nodiscard]] std::size_t remaining() const noexcept { return failed_ ? 0 : data_.size() - pos_; }
    [[nodiscard]] bool fail() const noexcept { return failed_; }

private:
    [[nodiscard]] bool has(std::size_t n) const noexcept { return !failed_ && data_.size() - pos_ >= n; }
    bool fail_now() noexcept {
        failed_ = true;
        return false;
    }

    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
    bool failed_ = false;
};

// Overloads the generated code calls for primitive fields.
template <class T>
    requires std::is_arithmetic_v<T>
inline void encode(ByteWriter& w, T v) {
    w.write(v);
}
inline void encode(ByteWriter& w, const std::string& v) { w.write(v); }

template <class T>
    requires std::is_arithmetic_v<T>
[[nodiscard]] inline bool decode(ByteReader& r, T& v) {
    return r.read(v);
}
[[nodiscard]] inline bool decode(ByteReader& r, std::string& v) { return r.read(v); }

template <class T, class Fn>
void encode_list(ByteWriter& w, const std::vector<T>& items, Fn&& encode_one) {
    w.write_list_count(items.size());
    if (w.fail()) return;
    for (const auto& item : items) encode_one(w, item);
}

template <class T, class Fn>
[[nodiscard]] bool decode_list(ByteReader& r, std::vector<T>& out, Fn&& decode_one) {
    std::uint16_t count = 0;
    if (!r.read_list_count(count)) return false;
    out.clear();
    // Every element is at least one byte, so a count larger than the remaining bytes is malformed; never reserve on trust.
    if (count > r.remaining()) return false;
    out.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        T item{};
        if (!decode_one(r, item)) return false;
        out.push_back(std::move(item));
    }
    return true;
}

}  // namespace runity::protocol
