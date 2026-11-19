// Big-endian 32-bit load and store.
#pragma once

#include <cstdint>

namespace clipsync {

inline void store_be32(std::uint8_t *dst, std::uint32_t value) {
    dst[0] = static_cast<std::uint8_t>(value >> 24);
    dst[1] = static_cast<std::uint8_t>(value >> 16);
    dst[2] = static_cast<std::uint8_t>(value >> 8);
    dst[3] = static_cast<std::uint8_t>(value);
}

inline std::uint32_t load_be32(const std::uint8_t *src) {
    return (std::uint32_t(src[0]) << 24) | (std::uint32_t(src[1]) << 16) | (std::uint32_t(src[2]) << 8) |
           std::uint32_t(src[3]);
}

} // namespace clipsync
