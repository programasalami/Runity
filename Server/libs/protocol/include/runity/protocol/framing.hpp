#pragma once

// Frame = [u32 LE payload length][u16 LE message id][payload]. The length counts the payload only.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "runity/protocol/bytes.hpp"
#include "runity/protocol/generated/messages.hpp"

namespace runity::protocol {

inline constexpr std::size_t kFrameHeaderBytes = 6;

struct Frame {
    std::uint16_t id = 0;
    std::vector<std::uint8_t> payload;
};

/// Builds one frame for any generated message type. Returns an empty vector if the message cannot be encoded
/// (a string/list too long, or the payload exceeds kMaxPayloadBytes).
template <class Message>
[[nodiscard]] std::vector<std::uint8_t> encode_frame(const Message& message) {
    ByteWriter w(64);
    w.write(std::uint32_t{0});
    w.write(static_cast<std::uint16_t>(Message::kId));
    encode(w, message);
    const std::size_t payload = w.size() - kFrameHeaderBytes;
    if (w.fail() || payload > kMaxPayloadBytes) return {};
    w.patch_u32(0, static_cast<std::uint32_t>(payload));
    return w.take();
}

/// Incremental frame splitter for a byte stream. Feed received bytes, then pop frames until none is complete.
/// A declared payload length above the limit poisons the decoder (the connection must be closed).
class FrameDecoder {
public:
    explicit FrameDecoder(std::size_t max_payload = kMaxPayloadBytes) : max_payload_(max_payload) {}

    void feed(std::span<const std::uint8_t> bytes) { buffer_.insert(buffer_.end(), bytes.begin(), bytes.end()); }

    /// Next complete frame, or nullopt if more bytes are needed or the stream is invalid (check error()).
    [[nodiscard]] std::optional<Frame> next() {
        if (error_) return std::nullopt;
        const std::size_t available = buffer_.size() - read_pos_;
        if (available < kFrameHeaderBytes) {
            compact();
            return std::nullopt;
        }
        ByteReader header(std::span<const std::uint8_t>(buffer_).subspan(read_pos_, kFrameHeaderBytes));
        std::uint32_t length = 0;
        std::uint16_t id = 0;
        (void)header.read(length);
        (void)header.read(id);
        if (length > max_payload_) {
            error_ = true;
            return std::nullopt;
        }
        if (available < kFrameHeaderBytes + length) {
            compact();
            return std::nullopt;
        }
        Frame frame;
        frame.id = id;
        const auto begin = buffer_.begin() + static_cast<std::ptrdiff_t>(read_pos_ + kFrameHeaderBytes);
        frame.payload.assign(begin, begin + length);
        read_pos_ += kFrameHeaderBytes + length;
        return frame;
    }

    [[nodiscard]] bool error() const noexcept { return error_; }
    [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size() - read_pos_; }

private:
    void compact() {
        if (read_pos_ == 0) return;
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(read_pos_));
        read_pos_ = 0;
    }

    std::vector<std::uint8_t> buffer_;
    std::size_t read_pos_ = 0;
    std::size_t max_payload_;
    bool error_ = false;
};

}  // namespace runity::protocol
