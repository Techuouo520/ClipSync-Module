// Event loop for TCP sync, UDP discovery, clipboard replies, and process supervision.
#include "clipsync/daemon.hpp"

#include "clipsync/bridge.hpp"
#include "clipsync/config.hpp"
#include "clipsync/discovery.hpp"
#include "clipsync/discovery_net.hpp"
#include "clipsync/log.hpp"
#include "clipsync/net.hpp"
#include "clipsync/tcp_sync.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace clipsync {
namespace {

volatile sig_atomic_t g_toggle = 0;
volatile sig_atomic_t g_stop = 0;
volatile sig_atomic_t g_child = 0;
volatile sig_atomic_t g_reload = 0;
int g_sig_fd = -1;

void poke_signal() {
    if (g_sig_fd < 0) {
        return;
    }
    const std::uint8_t byte = 1;
    (void)write(g_sig_fd, &byte, 1);
}

void on_usr1(int) {
    g_toggle = 1;
    poke_signal();
}

void on_usr2(int) {
    g_reload = 1;
    poke_signal();
}

void on_term(int) {
    g_stop = 1;
    poke_signal();
}

void on_chld(int) {
    g_child = 1;
    poke_signal();
}

void install_signals(int sig_fd) {
    g_sig_fd = sig_fd;
    struct sigaction sa{};
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, nullptr) != 0) {
        log_errno("sigaction USR1");
    }
    sa.sa_handler = on_usr2;
    if (sigaction(SIGUSR2, &sa, nullptr) != 0)
        log_errno("sigaction USR2");
    sa.sa_handler = on_term;
    if (sigaction(SIGTERM, &sa, nullptr) != 0 || sigaction(SIGINT, &sa, nullptr) != 0) {
        log_errno("sigaction TERM");
    }
    sa.sa_handler = on_chld;
    sa.sa_flags = SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, nullptr) != 0) {
        log_errno("sigaction CHLD");
    }
    sa.sa_flags = 0;
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, nullptr) != 0 || sigaction(SIGHUP, &sa, nullptr) != 0) {
        log_errno("sigaction PIPE");
    }
}

void drain_fd(int fd) {
    std::uint8_t byte = 0;
    while (true) {
        const ssize_t got = read(fd, &byte, 1);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (got == 0) {
            return;
        }
    }
}

bool write_text(const char *path, const char *text) {
    UniqueFd fd(open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
    if (!fd) {
        log_errno(path);
        return false;
    }
    const std::size_t size = std::strlen(text);
    std::size_t off = 0;
    while (off < size) {
        const ssize_t wrote = write(fd.get(), text + off, size - off);
        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_errno(path);
            return false;
        }
        if (wrote == 0) {
            log_line("short write");
            return false;
        }
        off += static_cast<std::size_t>(wrote);
    }
    return true;
}

bool ensure_workdir() {
    if (mkdir(kWorkDir, 0700) == 0 || errno == EEXIST) {
        return true;
    }
    log_errno("mkdir");
    return false;
}

bool pid_is_live(int pid) {
    if (pid <= 1 || pid == static_cast<int>(getpid())) {
        return false;
    }
    if (kill(pid, 0) == 0) {
        return true;
    }
    return errno == EPERM;
}

bool another_daemon_running() {
    UniqueFd fd(open(kPidPath, O_RDONLY | O_CLOEXEC));
    if (!fd) {
        return false;
    }
    char buf[32] = {};
    const ssize_t got = read(fd.get(), buf, sizeof(buf) - 1);
    if (got <= 0) {
        return false;
    }
    char *end = nullptr;
    const long pid = std::strtol(buf, &end, 10);
    if (end == buf) {
        return false;
    }
    return pid_is_live(static_cast<int>(pid));
}

std::string read_psk() {
    UniqueFd fd(open(kPskPath, O_RDONLY | O_CLOEXEC));
    if (!fd) {
        if (errno != ENOENT) {
            log_errno("psk");
        }
        return {};
    }
    std::string key;
    char buf[256];
    while (key.size() < 1024) {
        const std::size_t room = 1024 - key.size();
        const std::size_t chunk = room < sizeof(buf) ? room : sizeof(buf);
        const ssize_t got = read(fd.get(), buf, chunk);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_errno("psk read");
            return {};
        }
        if (got == 0) {
            return key;
        }
        key.append(buf, static_cast<std::size_t>(got));
    }
    char extra = 0;
    const ssize_t more = read(fd.get(), &extra, 1);
    if (more < 0) {
        log_errno("psk read");
        return {};
    }
    if (more > 0) {
        log_line("psk longer than 1024 bytes");
        return {};
    }
    return key;
}

std::uint32_t random_node() {
    UniqueFd fd(open("/dev/urandom", O_RDONLY | O_CLOEXEC));
    std::uint32_t node = 0;
    if (!fd || read(fd.get(), &node, sizeof(node)) != static_cast<ssize_t>(sizeof(node)) || node == 0) {
        log_errno("urandom");
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        node = static_cast<std::uint32_t>(ticks);
        if (node == 0) {
            node = 1;
        }
    }
    return node;
}

void on_bridge_bytes(ChildProc &child, TcpSync &sync) {
    std::uint8_t buf[4096];
    const ssize_t got = read(child.from_child.get(), buf, sizeof(buf));
    if (got < 0) {
        if (errno == EINTR) {
            return;
        }
        log_errno("read bridge");
        stop_bridge(child);
        return;
    }
    if (got == 0) {
        reap_bridge(child);
        if (child.pid > 0) {
            stop_bridge(child);
        }
        return;
    }
    child.decoder.append(buf, static_cast<std::size_t>(got));
    std::string text;
    while (child.decoder.pop(text)) {
        sync.on_bridge_frame(text);
        if (child.pid <= 0)
            return;
    }
    if (child.decoder.failed()) {
        log_line("bridge frame error");
        stop_bridge(child);
    }
}

void on_datagram(int fd, bool paused, std::uint32_t node, std::uint32_t nonce, TcpSync &sync) {
    std::uint8_t buf[DISCOVERY_HEADER + MAX_DEVICE_NAME + 1];
    sockaddr_in source{};
    socklen_t source_length = sizeof(source);
    const ssize_t got = recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&source), &source_length);
    if (got < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        log_errno("recvfrom");
        return;
    }
    DiscoveryPacket packet;
    if (!decode_discovery(buf, static_cast<std::size_t>(got), packet))
        return;
    static std::chrono::steady_clock::time_point last_discovery_reply{};
    if (packet.reply) {
        if (packet.nonce != nonce || packet.node == node)
            return;
    } else {
        handle_discovery(buf, static_cast<std::size_t>(got), source, node, paused, last_discovery_reply);
    }
    char address[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &source.sin_addr, address, sizeof(address)))
        sync.learn_peer(address);
    else
        log_errno("discovery peer address");
}

std::uint32_t request_discovery(int fd) {
    DiscoveryPacket probe;
    probe.nonce = random_node();
    std::vector<std::uint8_t> bytes;
    if (!encode_discovery(probe, bytes) || !broadcast_probe(fd, bytes, active_interfaces()))
        log_line("discovery broadcast unavailable");
    return probe.nonce;
}

int poll_timeout(const ChildProc &child, std::chrono::steady_clock::time_point respawn_at) {
    if (child.pid > 0) {
        return -1;
    }
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(respawn_at - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
        return 0;
    }
    if (left.count() > 60000) {
        return 60000;
    }
    return static_cast<int>(left.count());
}

} // namespace

int run() {
    if (!ensure_workdir()) {
        return 1;
    }
    if (another_daemon_running()) {
        log_line("already running");
        std::fprintf(stderr, "clipsyncd already running\n");
        return 0;
    }
    const std::uint32_t node = random_node();
    const std::string psk = read_psk();
    char pid_text[32];
    std::snprintf(pid_text, sizeof(pid_text), "%d\n", static_cast<int>(getpid()));
    if (!write_text(kPidPath, pid_text) || !write_text(kStatePath, "running\n")) {
        return 1;
    }

    int sig_pipe[2] = {-1, -1};
    if (pipe2(sig_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        log_errno("signal pipe");
        return 1;
    }
    UniqueFd sig_read(sig_pipe[0]);
    UniqueFd sig_write(sig_pipe[1]);
    install_signals(sig_write.get());

    UniqueFd udp = open_listen_socket();
    if (!udp) {
        std::fprintf(stderr, "clipsyncd: bind %u failed\n", static_cast<unsigned>(kUdpPort));
        unlink(kPidPath);
        return 1;
    }

    ChildProc child;
    TcpSync sync(node, psk, child);
    if (!sync.start()) {
        if (unlink(kPidPath) != 0)
            log_errno("unlink pid");
        return 1;
    }
    bool paused = false;
    std::uint32_t discovery_nonce = 0;
    auto respawn_at = std::chrono::steady_clock::now();
    log_line("started");

    while (!g_stop) {
        if (g_child) {
            g_child = 0;
            if (reap_bridge(child)) {
                respawn_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRestartDelayMs);
            }
        }
        if (g_toggle) {
            g_toggle = 0;
            paused = !paused;
            sync.set_paused(paused);
            write_text(kStatePath, paused ? "paused\n" : "running\n");
            log_line(paused ? "paused" : "running");
        }
        if (g_reload) {
            g_reload = 0;
            sync.reload_peers();
        }
        if (child.pid <= 0 && std::chrono::steady_clock::now() >= respawn_at) {
            if (!spawn_bridge(child)) {
                respawn_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRestartDelayMs);
            }
        }

        const pid_t previous_child = child.pid;
        sync.tick();
        if (previous_child > 0 && child.pid <= 0)
            respawn_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRestartDelayMs);
        if (sync.take_discovery_request())
            discovery_nonce = request_discovery(udp.get());
        std::vector<pollfd> fds(3);
        fds[0].fd = udp.get();
        fds[0].events = POLLIN;
        fds[1].fd = sig_read.get();
        fds[1].events = POLLIN;
        fds[2].fd = child.from_child.get();
        fds[2].events = POLLIN;
        sync.append_poll(fds);
        int timeout = poll_timeout(child, respawn_at);
        const int network_timeout = sync.timeout();
        if (network_timeout >= 0 && (timeout < 0 || network_timeout < timeout))
            timeout = network_timeout;
        const int waited = poll(fds.data(), fds.size(), timeout);
        if (waited < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_errno("poll");
            break;
        }
        if (waited == 0) {
            continue;
        }
        if ((fds[1].revents & POLLIN) != 0) {
            drain_fd(sig_read.get());
        }
        if ((fds[0].revents & POLLIN) != 0) {
            on_datagram(udp.get(), paused, node, discovery_nonce, sync);
        }
        const pid_t polled_child = child.pid;
        sync.process_poll(fds);
        if ((fds[2].revents & (POLLIN | POLLHUP | POLLERR)) != 0 && child.from_child) {
            on_bridge_bytes(child, sync);
            if (child.pid <= 0) {
                respawn_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRestartDelayMs);
            }
        }
        if (polled_child > 0 && child.pid <= 0)
            respawn_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRestartDelayMs);
    }

    stop_bridge(child);
    if (unlink(kPidPath) != 0 && errno != ENOENT) {
        log_errno("unlink pid");
    }
    log_line("stopped");
    return 0;
}

} // namespace clipsync
