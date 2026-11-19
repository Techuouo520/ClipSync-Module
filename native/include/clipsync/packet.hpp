// UDP payload: magic, node id, then PSK-masked clipboard text.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace clipsync {

bool encode_packet(std::uint32_t node_id, std::string_view plain, std::string_view psk, std::vector<std::uint8_t> &out);

bool decode_packet(const std::uint8_t *data, std::size_t size, std::string_view psk, std::uint32_t &node_id,
                   std::string &plain);

} // namespace clipsync
