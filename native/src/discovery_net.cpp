// Scan only on request and answer bounded discovery probes without touching the clipboard.
#include "clipsync/discovery_net.hpp"

#include "clipsync/byte_order.hpp"
#include "clipsync/discovery.hpp"
#include "clipsync/log.hpp"
#include "clipsync/net.hpp"
#include "clipsync/peer_store.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>

namespace clipsync {

std::string local_device_name() {
#ifdef __ANDROID__
    char model[PROP_VALUE_MAX]{};
    if (__system_property_get("ro.product.model", model) > 0) {
        return model;
    }
#endif
    return "ClipSync";
}

namespace {

bool send_scan_probes(int fd, const std::vector<std::uint8_t> &bytes, const std::vector<NetworkInterface> &interfaces) {
    bool sent = broadcast_probe(fd, bytes, interfaces);
    for (const auto &address : load_peer_addresses()) {
        if (send_probe_to(fd, bytes, address, interfaces))
            sent = true;
    }
    return sent;
}

bool receive_peer(int fd, std::uint32_t nonce, const std::vector<NetworkInterface> &interfaces,
                  std::vector<DiscoveredPeer> &peers) {
    std::uint8_t buffer[DISCOVERY_HEADER + MAX_DEVICE_NAME + 1];
    sockaddr_in source{};
    socklen_t length = sizeof(source);
    const ssize_t received =
        recvfrom(fd, buffer, sizeof(buffer), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&source), &length);
    if (received < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        log_errno("discovery recvfrom");
        return false;
    }
    if (std::any_of(interfaces.begin(), interfaces.end(),
                    [&](const NetworkInterface &iface) { return iface.address.s_addr == source.sin_addr.s_addr; })) {
        return true;
    }
    DiscoveryPacket packet;
    if (!decode_discovery(buffer, static_cast<std::size_t>(received), packet)) {
        return true;
    }
    char address[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &source.sin_addr, address, sizeof(address)) == nullptr) {
        log_errno("discovery inet_ntop");
        return false;
    }
    collect_peer(peers, packet, nonce, address);
    return true;
}

bool gather_peers(int fd, const DiscoveryPacket &probe, const std::vector<std::uint8_t> &bytes,
                  const std::vector<NetworkInterface> &interfaces, std::vector<DiscoveredPeer> &peers) {
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(1600);
    auto next_probe = start + std::chrono::milliseconds(700);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_probe) {
            if (!send_scan_probes(fd, bytes, interfaces)) {
                return false;
            }
            next_probe = deadline;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(std::min(next_probe, deadline) - now);
        pollfd event{fd, POLLIN, 0};
        const int ready = poll(&event, 1, std::max(1, static_cast<int>(left.count())));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_errno("discovery poll");
            return false;
        }
        if ((event.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return false;
        }
        if ((event.revents & POLLIN) != 0 && !receive_peer(fd, probe.nonce, interfaces, peers)) {
            return false;
        }
    }
    return true;
}

} // namespace

bool handle_discovery(const std::uint8_t *data, std::size_t size, const sockaddr_in &source, std::uint32_t node,
                      bool paused, std::chrono::steady_clock::time_point &last_reply) {
    if (size < 4 || load_be32(data) != DISCOVERY_MAGIC) {
        return false;
    }
    DiscoveryPacket probe;
    if (!decode_discovery(data, size, probe) || probe.reply || source.sin_port == 0) {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_reply < std::chrono::milliseconds(100)) {
        return true;
    }
    const DiscoveryPacket response{true, paused, probe.nonce, node, local_device_name()};
    std::vector<std::uint8_t> bytes;
    if (!encode_discovery(response, bytes) || !reply_to_peer(bytes, source)) {
        log_line("discovery reply skipped or failed");
    } else {
        last_reply = now;
    }
    return true;
}

int scan_devices() {
    const auto interfaces = active_interfaces();
    if (interfaces.empty()) {
        std::fprintf(stderr, "Wi-Fi 未连接\n");
        return 1;
    }
    UniqueFd fd(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (!fd) {
        std::fprintf(stderr, "无法创建扫描套接字\n");
        return 1;
    }
    DiscoveryPacket probe;
    probe.nonce = static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()) |
                  static_cast<std::uint32_t>(getpid()) | 1u;
    std::vector<std::uint8_t> bytes;
    std::vector<DiscoveredPeer> peers;
    if (!encode_discovery(probe, bytes) || !send_scan_probes(fd.get(), bytes, interfaces) ||
        !gather_peers(fd.get(), probe, bytes, interfaces, peers)) {
        std::fprintf(stderr, "扫描失败，请重试\n");
        return 1;
    }
    std::vector<std::string> addresses;
    for (const auto &peer : peers)
        addresses.push_back(peer.address);
    if (!addresses.empty() && !remember_peers(addresses)) {
        std::fprintf(stderr, "保存扫描结果失败\n");
        return 1;
    }
    if (!addresses.empty() && !notify_peer_reload())
        log_line("scan completed while daemon unavailable");
    for (const auto &peer : peers) {
        if (std::printf("peer\t%08x\t%s\t%s\t%s\n", peer.node, peer.name.c_str(), peer.address.c_str(),
                        peer.paused ? "paused" : "running") < 0) {
            return 1;
        }
    }
    return std::fflush(stdout) == 0 ? 0 : 1;
}

} // namespace clipsync
