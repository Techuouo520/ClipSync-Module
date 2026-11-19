// Encode discovery separately from clipboard traffic and bound scan results.
#include "clipsync/discovery.hpp"

#include "clipsync/byte_order.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace clipsync {
namespace {

bool valid_packet(const DiscoveryPacket &packet) {
    if (packet.nonce == 0 || packet.name.size() > MAX_DEVICE_NAME) {
        return false;
    }
    if (!packet.reply) {
        return packet.node == 0 && packet.name.empty() && !packet.paused;
    }
    return packet.node != 0 && std::none_of(packet.name.begin(), packet.name.end(),
                                            [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}

} // namespace

bool encode_discovery(const DiscoveryPacket &packet, std::vector<std::uint8_t> &out) {
    if (!valid_packet(packet)) {
        return false;
    }
    out.assign(DISCOVERY_HEADER + packet.name.size(), 0);
    store_be32(out.data(), DISCOVERY_MAGIC);
    out[4] = 1;
    out[5] = packet.reply ? 2 : 1;
    out[6] = packet.paused ? 1 : 0;
    store_be32(out.data() + 8, packet.nonce);
    store_be32(out.data() + 12, packet.node);
    if (!packet.name.empty()) {
        std::memcpy(out.data() + DISCOVERY_HEADER, packet.name.data(), packet.name.size());
    }
    return true;
}

bool decode_discovery(const std::uint8_t *data, std::size_t size, DiscoveryPacket &packet) {
    if (data == nullptr || size < DISCOVERY_HEADER || size > DISCOVERY_HEADER + MAX_DEVICE_NAME) {
        return false;
    }
    if (load_be32(data) != DISCOVERY_MAGIC || data[4] != 1 || (data[5] != 1 && data[5] != 2) || data[6] > 1 ||
        data[7] != 0) {
        return false;
    }
    DiscoveryPacket decoded;
    decoded.reply = data[5] == 2;
    decoded.paused = data[6] == 1;
    decoded.nonce = load_be32(data + 8);
    decoded.node = load_be32(data + 12);
    decoded.name.assign(reinterpret_cast<const char *>(data + DISCOVERY_HEADER), size - DISCOVERY_HEADER);
    if (!valid_packet(decoded)) {
        return false;
    }
    packet = std::move(decoded);
    return true;
}

void collect_peer(std::vector<DiscoveredPeer> &peers, const DiscoveryPacket &packet, std::uint32_t nonce,
                  const std::string &address) {
    if (!packet.reply || packet.nonce != nonce || !valid_packet(packet)) {
        return;
    }
    const auto existing = std::find_if(peers.begin(), peers.end(), [&](const DiscoveredPeer &peer) {
        return peer.node == packet.node || peer.address == address;
    });
    DiscoveredPeer peer{packet.node, packet.name, address, packet.paused};
    if (existing != peers.end()) {
        *existing = std::move(peer);
    } else if (peers.size() < MAX_DISCOVERED_PEERS) {
        peers.push_back(std::move(peer));
    }
}

} // namespace clipsync
