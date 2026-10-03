#include <doctest/doctest.h>

#include <random>
#include <string>

#include "generated/golden_samples.hpp"
#include "waw/protocol/framing.hpp"
#include "waw/protocol/generated/messages.hpp"

using namespace waw::protocol;

namespace {

std::string to_hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (auto b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 0xF];
    }
    return out;
}

std::vector<std::uint8_t> from_hex(std::string_view hex) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

}  // namespace

TEST_CASE("every message encodes to the golden bytes and decodes back") {
    const auto samples = golden::all_samples();
    REQUIRE(!samples.empty());
    for (const auto& s : samples) {
        CAPTURE(s.name);
        CHECK(to_hex(s.encode_sample()) == s.hex);
        CHECK(s.decodes_to_sample(from_hex(s.hex)));
    }
}

TEST_CASE("a message sent in the wrong direction is refused") {
    for (const auto& s : golden::all_samples()) {
        CAPTURE(s.name);
        const auto bytes = from_hex(s.hex);
        if (s.client_to_server) {
            auto r = decode_server_message(s.id, bytes);
            REQUIRE_FALSE(r.has_value());
            CHECK(r.error() == DecodeError::WrongDirection);
        } else {
            auto r = decode_client_message(s.id, bytes);
            REQUIRE_FALSE(r.has_value());
            CHECK(r.error() == DecodeError::WrongDirection);
        }
    }
}

TEST_CASE("unknown ids, truncated payloads and trailing bytes are refused") {
    CHECK(decode_client_message(60000, {}).error() == DecodeError::UnknownMessage);
    for (const auto& s : golden::all_samples()) {
        if (!s.client_to_server) continue;
        CAPTURE(s.name);
        auto bytes = from_hex(s.hex);
        if (!bytes.empty()) {
            auto truncated = bytes;
            truncated.pop_back();
            CHECK(decode_client_message(s.id, truncated).error() == DecodeError::Malformed);
        }
        bytes.push_back(0);
        CHECK(decode_client_message(s.id, bytes).error() == DecodeError::TrailingBytes);
    }
}

TEST_CASE("invalid bool and enum values are refused") {
    // Failure{code u16, fatal bool, message string}: a bool byte of 2 is invalid.
    std::vector<std::uint8_t> bad_bool = {0x01, 0x00, 0x02, 0x00, 0x00};
    CHECK(decode_server_message(static_cast<std::uint16_t>(MessageId::Failure), bad_bool).error() == DecodeError::Malformed);
    std::vector<std::uint8_t> bad_enum = {0xFF, 0x7F, 0x01, 0x00, 0x00};
    CHECK(decode_server_message(static_cast<std::uint16_t>(MessageId::Failure), bad_enum).error() == DecodeError::Malformed);
}

TEST_CASE("a list count larger than the payload is refused without allocating") {
    // MoveInput{steps list<MoveStep>} claiming 65535 steps with no data.
    std::vector<std::uint8_t> payload = {0xFF, 0xFF};
    CHECK(decode_client_message(static_cast<std::uint16_t>(MessageId::MoveInput), payload).error() == DecodeError::Malformed);
}

TEST_CASE("random bytes never crash the decoder") {
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> len(0, 64);
    for (int i = 0; i < 20000; ++i) {
        std::vector<std::uint8_t> payload(static_cast<std::size_t>(len(rng)));
        for (auto& b : payload) b = static_cast<std::uint8_t>(byte(rng));
        const auto id = static_cast<std::uint16_t>(i % 12);
        (void)decode_client_message(id, payload);
        (void)decode_server_message(static_cast<std::uint16_t>(100 + i % 12), payload);
    }
    CHECK(true);
}

TEST_CASE("frames round trip through the incremental decoder, byte by byte") {
    Ping ping{.client_time_ms = 123456};
    ChatSend chat{.text = "hello \xC3\xA9"};
    auto a = encode_frame(ping);
    auto b = encode_frame(chat);
    REQUIRE(a.size() == kFrameHeaderBytes + 4);
    std::vector<std::uint8_t> stream = a;
    stream.insert(stream.end(), b.begin(), b.end());

    FrameDecoder decoder;
    std::vector<Frame> frames;
    for (auto byte : stream) {
        decoder.feed(std::span(&byte, 1));
        while (auto f = decoder.next()) frames.push_back(std::move(*f));
    }
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].id == static_cast<std::uint16_t>(MessageId::Ping));
    auto m0 = decode_client_message(frames[0].id, frames[0].payload);
    REQUIRE(m0.has_value());
    CHECK(std::get<Ping>(*m0) == ping);
    auto m1 = decode_client_message(frames[1].id, frames[1].payload);
    REQUIRE(m1.has_value());
    CHECK(std::get<ChatSend>(*m1) == chat);
    CHECK(decoder.buffered() == 0);
}

TEST_CASE("an oversized frame length poisons the decoder") {
    FrameDecoder decoder(1024);
    std::vector<std::uint8_t> header = {0x01, 0x04, 0x00, 0x00, 0x02, 0x00};  // length 1025
    decoder.feed(header);
    CHECK_FALSE(decoder.next().has_value());
    CHECK(decoder.error());
}

TEST_CASE("an unencodable message produces no frame") {
    ChatSend chat{.text = std::string(70000, 'x')};
    CHECK(encode_frame(chat).empty());
}

TEST_CASE("message ids and names agree") {
    CHECK(message_name(static_cast<std::uint16_t>(MessageId::Snapshot)) == "Snapshot");
    CHECK(message_name(9999) == "Unknown");
    ClientMessage m = Hello{};
    CHECK(message_id(m) == MessageId::Hello);
}
