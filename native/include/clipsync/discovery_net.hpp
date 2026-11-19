// On-demand discovery client and daemon probe replies on the existing UDP port.
#pragma once

#include <netinet/in.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace clipsync {

bool handle_discovery(const std::uint8_t *data, std::size_t size, const sockaddr_in &source, std::uint32_t node,
                      bool paused, std::chrono::steady_clock::time_point &last_reply);
int scan_devices();
std::string local_device_name();

} // namespace clipsync
