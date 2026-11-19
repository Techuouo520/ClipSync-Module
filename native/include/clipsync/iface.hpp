// Names of interfaces that are allowed to carry clipboard broadcasts.
#pragma once

#include <string_view>

namespace clipsync {

inline bool is_sync_iface(std::string_view name) { return name.starts_with("wlan") || name.starts_with("ap"); }

} // namespace clipsync
