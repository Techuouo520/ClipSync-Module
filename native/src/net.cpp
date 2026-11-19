// Bind the listen socket and send broadcasts from wlan* and ap* only.
#include "clipsync/net.hpp"

#include "clipsync/config.hpp"
#include "clipsync/iface.hpp"
#include "clipsync/log.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>

namespace clipsync {
namespace {

struct IfaddrList {
    ifaddrs *head = nullptr;
    ~IfaddrList() {
        if (head != nullptr) {
            freeifaddrs(head);
        }
    }
    IfaddrList(const IfaddrList &) = delete;
    IfaddrList &operator=(const IfaddrList &) = delete;
    IfaddrList() = default;
};

bool send_on(int fd, const char *name, const sockaddr_in &destination, const std::vector<std::uint8_t> &packet) {
    const int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) != 0) {
        log_errno("SO_BROADCAST");
        return false;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, name, std::strlen(name) + 1) != 0) {
        log_errno("SO_BINDTODEVICE");
        return false;
    }
    const ssize_t sent = sendto(fd, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr *>(&destination),
                                sizeof(destination));
    if (sent < 0 || static_cast<std::size_t>(sent) != packet.size()) {
        log_errno("sendto");
        return false;
    }
    return true;
}

bool matches_peer(const NetworkInterface &iface, in_addr peer) {
    return peer.s_addr != iface.address.s_addr && peer.s_addr != iface.broadcast.s_addr &&
           (peer.s_addr & iface.netmask.s_addr) == (iface.address.s_addr & iface.netmask.s_addr);
}

} // namespace

UniqueFd open_listen_socket() {
    UniqueFd fd(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (!fd) {
        log_errno("listen socket");
        return UniqueFd();
    }
    const int yes = 1;
    if (setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
        log_errno("SO_REUSEADDR");
        return UniqueFd();
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kUdpPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        log_errno("bind");
        return UniqueFd();
    }
    return fd;
}

std::vector<NetworkInterface> active_interfaces() {
    IfaddrList list;
    if (getifaddrs(&list.head) != 0) {
        log_errno("getifaddrs");
        return {};
    }
    std::vector<NetworkInterface> interfaces;
    for (ifaddrs *ifa = list.head; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_name == nullptr || ifa->ifa_addr == nullptr || ifa->ifa_broadaddr == nullptr ||
            ifa->ifa_netmask == nullptr) {
            continue;
        }
        if (ifa->ifa_addr->sa_family != AF_INET || ifa->ifa_broadaddr->sa_family != AF_INET) {
            continue;
        }
        if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_RUNNING) == 0) {
            continue;
        }
        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0 || (ifa->ifa_flags & IFF_BROADCAST) == 0) {
            continue;
        }
        if (!is_sync_iface(ifa->ifa_name)) {
            continue;
        }
        const auto *broadcast = reinterpret_cast<sockaddr_in *>(ifa->ifa_broadaddr);
        if (broadcast->sin_addr.s_addr == 0) {
            continue;
        }
        const auto *address = reinterpret_cast<sockaddr_in *>(ifa->ifa_addr);
        const auto *netmask = reinterpret_cast<sockaddr_in *>(ifa->ifa_netmask);
        interfaces.push_back({ifa->ifa_name, address->sin_addr, broadcast->sin_addr, netmask->sin_addr});
    }
    return interfaces;
}

bool broadcast_probe(int fd, const std::vector<std::uint8_t> &packet, const std::vector<NetworkInterface> &interfaces) {
    bool sent = false;
    for (const auto &iface : interfaces) {
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(kUdpPort);
        destination.sin_addr = iface.broadcast;
        if (send_on(fd, iface.name.c_str(), destination, packet)) {
            sent = true;
        }
    }
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "", 0) != 0) {
        log_errno("clear SO_BINDTODEVICE");
        return false;
    }
    return sent;
}

bool reply_to_peer(const std::vector<std::uint8_t> &packet, const sockaddr_in &peer) {
    for (const auto &iface : active_interfaces()) {
        if (!matches_peer(iface, peer.sin_addr)) {
            continue;
        }
        UniqueFd fd(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
        if (!fd) {
            log_errno("discovery reply socket");
            return false;
        }
        if (send_on(fd.get(), iface.name.c_str(), peer, packet)) {
            return true;
        }
    }
    return false;
}

bool send_probe_to(int fd, const std::vector<std::uint8_t> &packet, const std::string &address,
                   const std::vector<NetworkInterface> &interfaces) {
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(kUdpPort);
    if (inet_pton(AF_INET, address.c_str(), &target.sin_addr) != 1)
        return false;
    bool sent = false;
    for (const auto &iface : interfaces) {
        if (matches_peer(iface, target.sin_addr) && send_on(fd, iface.name.c_str(), target, packet))
            sent = true;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "", 0) != 0) {
        log_errno("clear probe interface");
        return false;
    }
    return sent;
}

bool is_lan_peer(const std::string &address) {
    in_addr peer{};
    if (inet_pton(AF_INET, address.c_str(), &peer) != 1)
        return false;
    for (const auto &iface : active_interfaces())
        if (matches_peer(iface, peer))
            return true;
    return false;
}

bool configure_tcp_socket(int fd) {
    const int enabled = 1;
    const int idle = 30;
    const int interval = 10;
    const int count = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled)) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) != 0) {
        log_errno("TCP options");
        return false;
    }
    return true;
}

UniqueFd open_tcp_listener(const NetworkInterface &iface) {
    UniqueFd fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!fd) {
        log_errno("TCP listen socket");
        return {};
    }
    const int yes = 1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(TCP_PORT);
    address.sin_addr = iface.address;
    if (setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0 ||
        setsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE, iface.name.c_str(), iface.name.size() + 1) != 0 ||
        bind(fd.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(fd.get(), 16) != 0) {
        log_errno("TCP listen");
        return {};
    }
    return fd;
}

UniqueFd open_network_monitor() {
    UniqueFd fd(socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE));
    if (!fd) {
        log_errno("network monitor socket");
        return {};
    }
    sockaddr_nl address{};
    address.nl_family = AF_NETLINK;
    address.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR;
    if (bind(fd.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        log_errno("network monitor bind");
        return {};
    }
    return fd;
}

UniqueFd connect_tcp_peer(const std::string &address) {
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(TCP_PORT);
    if (inet_pton(AF_INET, address.c_str(), &target.sin_addr) != 1)
        return {};
    for (const auto &iface : active_interfaces()) {
        if (!matches_peer(iface, target.sin_addr))
            continue;
        UniqueFd fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        if (!fd || !configure_tcp_socket(fd.get()))
            return {};
        if (setsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE, iface.name.c_str(), iface.name.size() + 1) != 0) {
            log_errno("TCP bind interface");
            return {};
        }
        if (connect(fd.get(), reinterpret_cast<sockaddr *>(&target), sizeof(target)) == 0 || errno == EINPROGRESS)
            return fd;
        log_errno("TCP connect");
    }
    return {};
}

} // namespace clipsync
