// TCP message framing, application acknowledgements, and correlated UTF-8 bridge commands.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace clipsync {

enum class MessageKind : std::uint8_t { Hello = 1, Text = 2, Ack = 3, State = 4 };
enum class AckCode : std::uint8_t { Applied = 0, Paused = 1, Busy = 2, KeyMismatch = 3, BridgeError = 4 };

struct TcpMessage {
    MessageKind kind = MessageKind::Hello;
    std::uint32_t node = 0;
    std::uint32_t sequence = 0;
    std::uint8_t code = 0;
    std::string text;
    bool key_matches = true;
};

bool encode_tcp_message(const TcpMessage &message, std::string_view psk, std::vector<std::uint8_t> &out);
bool decode_tcp_message(std::string_view body, std::string_view psk, TcpMessage &message);
std::string bridge_command(std::uint64_t id, std::string_view text);
bool bridge_result(std::string_view frame, std::uint64_t &id, bool &success);

} // namespace clipsync
