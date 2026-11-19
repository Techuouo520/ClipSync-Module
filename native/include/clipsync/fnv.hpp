// FNV-1a 64-bit hash of a byte string.
#pragma once

#include <cstdint>
#include <string_view>

namespace clipsync {

std::uint64_t fnv1a64(std::string_view data);

} // namespace clipsync
