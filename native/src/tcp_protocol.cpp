// Keep TCP metadata separate from clipboard text and verify the PSK payload marker.
#include "clipsync/tcp_protocol.hpp"

#include "clipsync/byte_order.hpp"
#include "clipsync/config.hpp"
#include "clipsync/frame.hpp"
#include "clipsync/psk.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace clipsync {
namespace {
constexpr std::uint32_t MAGIC = 0x43535432;
constexpr std::size_t HEADER = 14;

bool valid_message(const TcpMessage &message) {
    if (message.node == 0)
        return false;
    switch (message.kind) {
    case MessageKind::Hello:
        return message.sequence == 0 && message.code <= 1 && message.text.size() <= 96 &&
               std::none_of(message.text.begin(), message.text.end(),
                            [](unsigned char c) { return c < 32 || c == 127; });
    case MessageKind::Text:
        return message.sequence != 0 && message.code == 0 && !message.text.empty() &&
               message.text.size() <= kMaxPayload;
    case MessageKind::Ack:
        return message.sequence != 0 && message.code <= static_cast<unsigned>(AckCode::BridgeError) &&
               message.text.empty();
    case MessageKind::State:
        return message.sequence == 0 && message.code <= 1 && message.text.empty();
    }
    return false;
}
} // namespace

bool encode_tcp_message(const TcpMessage &message, std::string_view psk, std::vector<std::uint8_t> &out) {
    if (!valid_message(message))
        return false;
    std::string payload = message.text;
    if (message.kind == MessageKind::Text) {
        payload.insert(0, "CLIP");
        apply_psk(reinterpret_cast<std::uint8_t *>(payload.data()), payload.size(), psk);
    }
    std::string body(HEADER, '\0');
    auto *bytes = reinterpret_cast<std::uint8_t *>(body.data());
    store_be32(bytes, MAGIC);
    bytes[4] = static_cast<std::uint8_t>(message.kind);
    store_be32(bytes + 5, message.node);
    store_be32(bytes + 9, message.sequence);
    bytes[13] = message.code;
    body += payload;
    return encode_frame(body, out, MAX_MESSAGE_FRAME);
}

bool decode_tcp_message(std::string_view body, std::string_view psk, TcpMessage &message) {
    if (body.size() < HEADER || body.size() > MAX_MESSAGE_FRAME)
        return false;
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(body.data());
    if (load_be32(bytes) != MAGIC || bytes[4] < 1 || bytes[4] > 4)
        return false;
    TcpMessage decoded;
    decoded.kind = static_cast<MessageKind>(bytes[4]);
    decoded.node = load_be32(bytes + 5);
    decoded.sequence = load_be32(bytes + 9);
    decoded.code = bytes[13];
    decoded.text.assign(body.substr(HEADER));
    if (decoded.kind == MessageKind::Text) {
        if (decoded.text.size() <= 4 || decoded.text.size() > kMaxPayload + 4 || decoded.node == 0 ||
            decoded.sequence == 0 || decoded.code != 0)
            return false;
        apply_psk(reinterpret_cast<std::uint8_t *>(decoded.text.data()), decoded.text.size(), psk);
        decoded.key_matches = decoded.text.compare(0, 4, "CLIP") == 0;
        decoded.text.erase(0, 4);
    }
    if (!valid_message(decoded))
        return false;
    message = std::move(decoded);
    return true;
}

std::string bridge_command(std::uint64_t id, std::string_view text) {
    char prefix[18]{};
    std::snprintf(prefix, sizeof(prefix), "W%016llx", static_cast<unsigned long long>(id));
    return std::string(prefix) + std::string(text);
}

bool bridge_result(std::string_view frame, std::uint64_t &id, bool &success) {
    if (frame.size() != 17 || (frame[0] != 'A' && frame[0] != 'E'))
        return false;
    const auto result = std::from_chars(frame.data() + 1, frame.data() + frame.size(), id, 16);
    success = frame[0] == 'A';
    return result.ec == std::errc{} && result.ptr == frame.data() + frame.size() && id != 0;
}
} // namespace clipsync
