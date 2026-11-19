// Encode and decode one clipboard UDP datagram.
#include "clipsync/packet.hpp"

#include "clipsync/byte_order.hpp"
#include "clipsync/config.hpp"
#include "clipsync/psk.hpp"

#include <cstring>

namespace clipsync {

bool encode_packet(std::uint32_t node_id, std::string_view plain, std::string_view psk,
                   std::vector<std::uint8_t> &out) {
    if (plain.size() > kMaxPayload) {
        return false;
    }
    out.resize(kPacketHeader + plain.size());
    store_be32(out.data(), kMagic);
    store_be32(out.data() + 4, node_id);
    if (!plain.empty()) {
        std::memcpy(out.data() + kPacketHeader, plain.data(), plain.size());
    }
    apply_psk(out.data() + kPacketHeader, plain.size(), psk);
    return true;
}

bool decode_packet(const std::uint8_t *data, std::size_t size, std::string_view psk, std::uint32_t &node_id,
                   std::string &plain) {
    if (data == nullptr || size < kPacketHeader || size - kPacketHeader > kMaxPayload) {
        return false;
    }
    if (load_be32(data) != kMagic) {
        return false;
    }
    node_id = load_be32(data + 4);
    plain.assign(reinterpret_cast<const char *>(data + kPacketHeader), size - kPacketHeader);
    if (!plain.empty()) {
        apply_psk(reinterpret_cast<std::uint8_t *>(&plain[0]), plain.size(), psk);
    }
    return true;
}

} // namespace clipsync
