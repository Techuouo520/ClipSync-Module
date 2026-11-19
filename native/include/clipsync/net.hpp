// IPv4 UDP listen socket and per-interface broadcast send.
#pragma once

#include "clipsync/unique_fd.hpp"

#include <netinet/in.h>

#include <cstdint>
#include <string>
#include <vector>

namespace clipsync {

UniqueFd open_listen_socket();
struct NetworkInterface {
    std::string name;
    in_addr address{};
    in_addr broadcast{};
    in_addr netmask{};
};

std::vector<NetworkInterface> active_interfaces();
bool broadcast_probe(int fd, const std::vector<std::uint8_t> &packet, const std::vector<NetworkInterface> &interfaces);
bool reply_to_peer(const std::vector<std::uint8_t> &packet, const sockaddr_in &peer);
bool send_probe_to(int fd, const std::vector<std::uint8_t> &packet, const std::string &address,
                   const std::vector<NetworkInterface> &interfaces);
bool is_lan_peer(const std::string &address);
UniqueFd open_tcp_listener(const NetworkInterface &iface);
UniqueFd open_network_monitor();
UniqueFd connect_tcp_peer(const std::string &address);
bool configure_tcp_socket(int fd);

} // namespace clipsync
