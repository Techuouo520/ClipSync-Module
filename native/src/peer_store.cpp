// Merge peer addresses under a kernel file lock and replace snapshots atomically.
#include "clipsync/peer_store.hpp"
#include "clipsync/config.hpp"
#include "clipsync/log.hpp"
#include "clipsync/net.hpp"
#include "clipsync/unique_fd.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace clipsync {
namespace {
bool read_small_file(const char *path, std::string &text, std::size_t limit) {
    UniqueFd fd(open(path, O_RDONLY | O_CLOEXEC));
    if (!fd) {
        if (errno == ENOENT)
            return true;
        log_errno("open peer file");
        return false;
    }
    char buffer[512];
    while (true) {
        const auto count = read(fd.get(), buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            log_errno("read peer file");
            return false;
        }
        if (count == 0)
            return true;
        if (text.size() + static_cast<std::size_t>(count) > limit) {
            log_line("peer file too large");
            return false;
        }
        text.append(buffer, static_cast<std::size_t>(count));
    }
}

bool read_addresses(std::vector<std::string> &addresses) {
    std::string text;
    if (!read_small_file(PEERS_PATH, text, 4096))
        return false;
    std::size_t offset = 0;
    while (offset < text.size()) {
        auto end = text.find('\n', offset);
        if (end == std::string::npos)
            end = text.size();
        std::string address = text.substr(offset, end - offset);
        if (!address.empty() && address.back() == '\r')
            address.pop_back();
        if (valid_peer_address(address) && std::find(addresses.begin(), addresses.end(), address) == addresses.end()) {
            if (addresses.size() >= 32)
                return false;
            addresses.push_back(std::move(address));
        }
        offset = end + 1;
    }
    return true;
}
} // namespace

bool valid_peer_address(std::string_view address) {
    in_addr parsed{};
    const std::string text(address);
    if (inet_pton(AF_INET, text.c_str(), &parsed) != 1)
        return false;
    const auto host = ntohl(parsed.s_addr);
    return host != 0 && host != 0xffffffff && (host >> 24) != 127 && (host >> 28) < 14;
}

std::vector<std::string> load_peer_addresses() {
    std::vector<std::string> addresses;
    if (!read_addresses(addresses)) {
        log_line("read peer addresses failed");
        return {};
    }
    return addresses;
}

bool write_atomic_file(const char *path, std::string_view text) {
    const std::string temporary = std::string(path) + "." + std::to_string(getpid()) + ".tmp";
    UniqueFd fd(open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
    if (!fd) {
        log_errno("open snapshot");
        return false;
    }
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto sent = write(fd.get(), text.data() + offset, text.size() - offset);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0) {
            log_errno("write snapshot");
            if (unlink(temporary.c_str()) != 0)
                log_errno("unlink snapshot");
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }
    if (rename(temporary.c_str(), path) == 0)
        return true;
    log_errno("rename snapshot");
    if (unlink(temporary.c_str()) != 0)
        log_errno("unlink snapshot");
    return false;
}

bool remember_peers(const std::vector<std::string> &incoming) {
    if (mkdir(kWorkDir, 0700) != 0 && errno != EEXIST)
        return false;
    UniqueFd lock(open((std::string(kWorkDir) + "/peers.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600));
    if (!lock || flock(lock.get(), LOCK_EX) != 0) {
        log_errno("peer lock");
        return false;
    }
    std::vector<std::string> addresses;
    if (!read_addresses(addresses))
        return false;
    bool changed = false;
    for (const auto &address : incoming) {
        if (!valid_peer_address(address))
            return false;
        if (std::find(addresses.begin(), addresses.end(), address) != addresses.end())
            continue;
        if (addresses.size() >= 32) {
            std::fprintf(stderr, "已达到设备数量上限\n");
            return false;
        }
        addresses.push_back(address);
        changed = true;
    }
    if (!changed)
        return true;
    std::string text;
    for (const auto &address : addresses)
        text += address + "\n";
    return write_atomic_file(PEERS_PATH, text);
}

bool notify_peer_reload() {
    std::string text;
    if (!read_small_file(kPidPath, text, 32))
        return false;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    int pid = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), pid);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || pid <= 1)
        return false;
    char path[256]{};
    const auto size = readlink(("/proc/" + std::to_string(pid) + "/exe").c_str(), path, sizeof(path) - 1);
    if (size <= 0)
        return false;
    const std::string executable(path, static_cast<std::size_t>(size));
    if (!executable.ends_with("/clipsyncd") && !executable.ends_with("/clipsyncd (deleted)"))
        return false;
    return kill(static_cast<pid_t>(pid), SIGUSR2) == 0;
}

int add_peer_address(const char *address) {
    if (!valid_peer_address(address)) {
        std::fprintf(stderr, "IP 地址无效\n");
        return 2;
    }
    in_addr target{};
    if (inet_pton(AF_INET, address, &target) != 1)
        return 2;
    for (const auto &iface : active_interfaces()) {
        if (iface.address.s_addr == target.s_addr) {
            std::fprintf(stderr, "这是本机 IP 地址\n");
            return 2;
        }
    }
    if (!remember_peers({address})) {
        std::fprintf(stderr, "保存设备失败\n");
        return 1;
    }
    if (!notify_peer_reload()) {
        std::fprintf(stderr, "地址已保存，请启动同步服务\n");
        return 1;
    }
    return 0;
}
} // namespace clipsync
