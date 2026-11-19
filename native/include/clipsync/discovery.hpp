// Bounded discovery packets and duplicate-free results for an on-demand LAN scan.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace clipsync {

inline constexpr std::uint32_t DISCOVERY_MAGIC = 0x43534456u;
inline constexpr std::size_t DISCOVERY_HEADER = 16;
inline constexpr std::size_t MAX_DEVICE_NAME = 96;
inline constexpr std::size_t MAX_DISCOVERED_PEERS = 32;

struct DiscoveryPacket {
    bool reply = false;
    bool paused = false;
    std::uint32_t nonce = 0;
    std::uint32_t node = 0;
    std::string name;
};

struct DiscoveredPeer {
    std::uint32_t node = 0;
    std::string name;
    std::string address;
    bool paused = false;
};

bool encode_discovery(const DiscoveryPacket &packet, std::vector<std::uint8_t> &out);
bool decode_discovery(const std::uint8_t *data, std::size_t size, DiscoveryPacket &packet);
void collect_peer(std::vector<DiscoveredPeer> &peers, const DiscoveryPacket &packet, std::uint32_t nonce,
                  const std::string &address);

} // namespace clipsync
