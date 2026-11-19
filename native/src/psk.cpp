// Repeating-key XOR. Empty key is a no-op.
#include "clipsync/psk.hpp"

namespace clipsync {

void apply_psk(std::uint8_t *data, std::size_t size, std::string_view psk) {
    if (data == nullptr || size == 0 || psk.empty()) {
        return;
    }
    for (std::size_t i = 0; i < size; ++i) {
        const auto mask = static_cast<std::uint8_t>(psk[i % psk.size()]);
        data[i] = static_cast<std::uint8_t>(data[i] ^ mask);
    }
}

} // namespace clipsync
