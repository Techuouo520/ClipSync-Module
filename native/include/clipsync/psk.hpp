// Stream XOR with a repeating pre-shared key. An empty key leaves bytes unchanged.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace clipsync {

void apply_psk(std::uint8_t *data, std::size_t size, std::string_view psk);

} // namespace clipsync
