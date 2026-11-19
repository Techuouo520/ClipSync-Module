// Persist validated peer addresses and publish connection state atomically.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace clipsync {
bool valid_peer_address(std::string_view address);
std::vector<std::string> load_peer_addresses();
bool remember_peers(const std::vector<std::string> &addresses);
bool write_atomic_file(const char *path, std::string_view text);
bool notify_peer_reload();
int add_peer_address(const char *address);
} // namespace clipsync
