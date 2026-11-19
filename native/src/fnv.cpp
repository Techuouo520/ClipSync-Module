// FNV-1a 64-bit.
#include "clipsync/fnv.hpp"

namespace clipsync {

std::uint64_t fnv1a64(std::string_view data) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : data) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace clipsync
