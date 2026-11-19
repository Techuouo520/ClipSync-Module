// Maintain bounded nonblocking streams and acknowledge only completed clipboard writes.
#include "clipsync/tcp_sync.hpp"

#include "clipsync/config.hpp"
#include "clipsync/discovery_net.hpp"
#include "clipsync/echo.hpp"
#include "clipsync/fnv.hpp"
#include "clipsync/log.hpp"
#include "clipsync/net.hpp"
#include "clipsync/peer_store.hpp"
#include "clipsync/tcp_protocol.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <deque>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace clipsync {
namespace {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
constexpr auto ACK_TIMEOUT = std::chrono::seconds(5);
constexpr auto HANDSHAKE_TIMEOUT = std::chrono::seconds(3);

struct WireFrame {
    std::uint32_t sequence = 0;
    std::vector<std::uint8_t> bytes;
};
using Frame = std::shared_ptr<const WireFrame>;

struct Peer {
    std::string address;
    std::string name = "ClipSync";
    std::string error;
    std::uint32_t node = 0;
    int fd = -1;
    bool connected = false;
    bool paused = false;
    int backoff = 1;
    Time reconnect{};
    Time next_send{};
    Time ack_deadline = Time::max();
    std::uint32_t inflight = 0;
    Frame pending;
};

struct Connection {
    UniqueFd fd;
    std::string address;
    bool outgoing = false;
    bool connecting = false;
    std::uint32_t node = 0;
    Time handshake_deadline = Time::max();
    FrameDecoder decoder{MAX_MESSAGE_FRAME};
    std::deque<Frame> output;
    std::size_t offset = 0;
};
} // namespace

struct TcpSync::Impl {
    std::uint32_t node;
    std::string psk;
    ChildProc &child;
    UniqueFd network_monitor;
    std::vector<UniqueFd> listeners;
    std::string network_signature;
    std::vector<Peer> peers;
    std::array<Connection, 64> connections;
    EchoState echo;
    bool paused = false;
    bool dirty = true;
    bool discover = true;
    std::uint32_t sequence = 0;
    Frame latest;
    std::array<std::uint64_t, 64> received{};
    std::size_t received_index = 0;
    std::uint64_t applying = 0;
    std::uint64_t applying_hash = 0;
    std::string applying_peer;
    pid_t applying_child = -1;
    Time apply_deadline = Time::max();
    std::vector<std::uint8_t> bridge_output;
    std::size_t bridge_offset = 0;

    Impl(std::uint32_t id, std::string key, ChildProc &bridge) : node(id), psk(std::move(key)), child(bridge) {
        peers.reserve(32);
    }

    Peer *find_peer(const std::string &address) {
        const auto found =
            std::find_if(peers.begin(), peers.end(), [&](const Peer &peer) { return peer.address == address; });
        return found == peers.end() ? nullptr : &*found;
    }

    Connection *find_connection(int fd) {
        for (auto &connection : connections)
            if (connection.fd && connection.fd.get() == fd)
                return &connection;
        return nullptr;
    }

    Connection *free_connection() {
        for (auto &connection : connections)
            if (!connection.fd)
                return &connection;
        return nullptr;
    }

    Peer *learn(const std::string &address) {
        if (auto *peer = find_peer(address))
            return peer;
        if (peers.size() >= 32 || !valid_peer_address(address))
            return nullptr;
        Peer peer;
        peer.address = address;
        peer.pending = latest;
        peers.push_back(std::move(peer));
        dirty = true;
        return &peers.back();
    }

    Frame frame(const TcpMessage &message) {
        auto output = std::make_shared<WireFrame>();
        output->sequence = message.sequence;
        if (!encode_tcp_message(message, psk, output->bytes))
            return {};
        return output;
    }

    void disconnect(Connection &connection) {
        if (auto *peer = find_peer(connection.address); peer && peer->fd == connection.fd.get()) {
            peer->fd = -1;
            peer->connected = false;
            peer->inflight = 0;
            peer->ack_deadline = Time::max();
            peer->reconnect = Clock::now() + std::chrono::seconds(peer->backoff);
            peer->backoff = std::min(30, peer->backoff * 2);
            dirty = true;
        }
        connection = Connection{};
    }

    bool queue(Connection &connection, Frame outgoing) {
        if (!outgoing || connection.output.size() >= 8) {
            disconnect(connection);
            return false;
        }
        connection.output.push_back(std::move(outgoing));
        return true;
    }

    void hello(Connection &connection) {
        queue(connection, frame({MessageKind::Hello, node, 0, static_cast<std::uint8_t>(paused), local_device_name()}));
    }

    void acknowledge(const std::string &address, std::uint32_t id, AckCode code) {
        auto *peer = find_peer(address);
        if (!peer || !peer->connected)
            return;
        if (auto *connection = find_connection(peer->fd)) {
            queue(*connection, frame({MessageKind::Ack, node, id, static_cast<std::uint8_t>(code), {}}));
        }
    }

    void finish_apply(bool success) {
        if (applying == 0)
            return;
        if (success) {
            echo.confirm_remote(applying_hash);
            received[received_index++ % received.size()] = applying;
            latest.reset();
            for (auto &peer : peers)
                peer.pending.reset();
        } else {
            echo.cancel_remote();
        }
        acknowledge(applying_peer, static_cast<std::uint32_t>(applying),
                    success ? AckCode::Applied : AckCode::BridgeError);
        applying = 0;
        apply_deadline = Time::max();
        bridge_output.clear();
        bridge_offset = 0;
    }

    void receive_text(Peer &peer, const TcpMessage &message) {
        const auto id = (static_cast<std::uint64_t>(message.node) << 32) | message.sequence;
        if (!message.key_matches) {
            acknowledge(peer.address, message.sequence, AckCode::KeyMismatch);
            return;
        }
        if (std::find(received.begin(), received.end(), id) != received.end()) {
            acknowledge(peer.address, message.sequence, AckCode::Applied);
            return;
        }
        if (paused) {
            acknowledge(peer.address, message.sequence, AckCode::Paused);
            return;
        }
        if (applying == id)
            return;
        if (applying != 0) {
            acknowledge(peer.address, message.sequence, AckCode::Busy);
            return;
        }
        const auto hash = fnv1a64(message.text);
        if (echo.is_current(hash)) {
            received[received_index++ % received.size()] = id;
            acknowledge(peer.address, message.sequence, AckCode::Applied);
            return;
        }
        if (child.pid <= 0 || !child.to_child) {
            acknowledge(peer.address, message.sequence, AckCode::BridgeError);
            return;
        }
        echo.expect_remote(hash);
        if (!encode_frame(bridge_command(id, message.text), bridge_output, MAX_MESSAGE_FRAME)) {
            echo.cancel_remote();
            acknowledge(peer.address, message.sequence, AckCode::BridgeError);
            return;
        }
        applying = id;
        applying_hash = hash;
        applying_peer = peer.address;
        applying_child = child.pid;
        bridge_offset = 0;
        apply_deadline = Clock::now() + ACK_TIMEOUT;
    }

    void receive_ack(Peer &peer, const TcpMessage &message) {
        if (peer.inflight != message.sequence)
            return;
        const auto previous_error = peer.error;
        const bool previous_paused = peer.paused;
        peer.inflight = 0;
        peer.ack_deadline = Time::max();
        peer.next_send = Clock::now() + std::chrono::milliseconds(500);
        const auto code = static_cast<AckCode>(message.code);
        if (code == AckCode::Applied) {
            if (peer.pending && peer.pending->sequence == message.sequence)
                peer.pending.reset();
            peer.error.clear();
            peer.next_send = Clock::now();
        } else if (code == AckCode::Paused) {
            peer.paused = true;
        } else if (code == AckCode::KeyMismatch) {
            peer.error = "key_mismatch";
            peer.next_send = Time::max();
        } else if (code == AckCode::BridgeError) {
            peer.error = "bridge_error";
        }
        dirty = dirty || previous_error != peer.error || previous_paused != peer.paused;
    }

    void receive_hello(Connection &connection, const TcpMessage &message) {
        if (message.kind != MessageKind::Hello || message.node == node) {
            disconnect(connection);
            return;
        }
        auto *peer = learn(connection.address);
        if (!peer) {
            disconnect(connection);
            return;
        }
        if (peer->connected && peer->fd != connection.fd.get()) {
            const bool preferred = connection.outgoing == (node < message.node);
            if (!preferred) {
                disconnect(connection);
                return;
            }
            if (auto *other = find_connection(peer->fd))
                disconnect(*other);
        }
        connection.node = message.node;
        connection.handshake_deadline = Time::max();
        peer->fd = connection.fd.get();
        peer->connected = true;
        peer->node = message.node;
        peer->name = message.text.empty() ? "ClipSync" : message.text;
        peer->paused = message.code != 0;
        peer->backoff = 1;
        peer->inflight = 0;
        peer->error.clear();
        peer->next_send = Clock::now();
        if (!remember_peers({peer->address}))
            log_line("remember connected peer failed");
        dirty = true;
    }

    void receive_message(Connection &connection, const TcpMessage &message) {
        if (connection.node == 0) {
            receive_hello(connection, message);
            return;
        }
        auto *peer = find_peer(connection.address);
        if (!peer || message.node != connection.node || message.kind == MessageKind::Hello) {
            disconnect(connection);
            return;
        }
        switch (message.kind) {
        case MessageKind::Text:
            receive_text(*peer, message);
            break;
        case MessageKind::Ack:
            receive_ack(*peer, message);
            break;
        case MessageKind::State:
            peer->paused = message.code != 0;
            peer->next_send = Clock::now();
            dirty = true;
            break;
        case MessageKind::Hello:
            break;
        }
    }

    void read_connection(Connection &connection) {
        std::uint8_t buffer[4096];
        for (int reads = 0; reads < 32 && connection.fd; ++reads) {
            const auto count = recv(connection.fd.get(), buffer, sizeof(buffer), 0);
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return;
            if (count <= 0) {
                disconnect(connection);
                return;
            }
            connection.decoder.append(buffer, static_cast<std::size_t>(count));
            std::string body;
            while (connection.decoder.pop(body)) {
                TcpMessage message;
                if (!decode_tcp_message(body, psk, message)) {
                    disconnect(connection);
                    return;
                }
                receive_message(connection, message);
                if (!connection.fd)
                    return;
            }
            if (connection.decoder.failed()) {
                disconnect(connection);
                return;
            }
        }
    }

    void write_connection(Connection &connection) {
        while (connection.fd && !connection.output.empty()) {
            const auto &bytes = connection.output.front()->bytes;
            const auto count = send(connection.fd.get(), bytes.data() + connection.offset,
                                    bytes.size() - connection.offset, MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return;
            if (count <= 0) {
                disconnect(connection);
                return;
            }
            connection.offset += static_cast<std::size_t>(count);
            if (connection.offset == bytes.size()) {
                connection.output.pop_front();
                connection.offset = 0;
            }
        }
    }

    void write_bridge() {
        while (bridge_offset < bridge_output.size()) {
            const auto count =
                write(child.to_child.get(), bridge_output.data() + bridge_offset, bridge_output.size() - bridge_offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return;
            if (count <= 0) {
                stop_bridge(child);
                finish_apply(false);
                return;
            }
            bridge_offset += static_cast<std::size_t>(count);
        }
    }

    void accept_connections(int listener) {
        for (int accepted = 0; accepted < 16; ++accepted) {
            sockaddr_in source{};
            socklen_t size = sizeof(source);
            UniqueFd fd(accept4(listener, reinterpret_cast<sockaddr *>(&source), &size, SOCK_CLOEXEC | SOCK_NONBLOCK));
            if (!fd) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    log_errno("TCP accept");
                return;
            }
            char address[INET_ADDRSTRLEN]{};
            auto *connection = free_connection();
            if (!connection || !inet_ntop(AF_INET, &source.sin_addr, address, sizeof(address)) ||
                !is_lan_peer(address) || !configure_tcp_socket(fd.get()))
                continue;
            connection->fd = std::move(fd);
            connection->address = address;
            connection->handshake_deadline = Clock::now() + HANDSHAKE_TIMEOUT;
            hello(*connection);
        }
    }

    bool refresh_network() {
        const auto interfaces = active_interfaces();
        std::string signature;
        for (const auto &iface : interfaces)
            signature += iface.name + ":" + std::to_string(iface.address.s_addr) + ":" +
                         std::to_string(iface.netmask.s_addr) + ";";
        if (signature == network_signature)
            return true;
        for (auto &connection : connections)
            if (connection.fd)
                disconnect(connection);
        listeners.clear();
        for (const auto &iface : interfaces) {
            auto listener = open_tcp_listener(iface);
            if (listener)
                listeners.push_back(std::move(listener));
        }
        network_signature = signature;
        for (auto &peer : peers)
            peer.reconnect = Clock::now();
        discover = true;
        return interfaces.empty() || !listeners.empty();
    }

    void network_changed() {
        char buffer[4096];
        while (true) {
            const auto count = recv(network_monitor.get(), buffer, sizeof(buffer), 0);
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                log_errno("network monitor read");
            if (count <= 0)
                break;
        }
        if (!refresh_network())
            log_line("TCP listener refresh failed");
    }

    void connect_peer(Peer &peer) {
        auto *connection = free_connection();
        if (!connection) {
            peer.reconnect = Clock::now() + std::chrono::seconds(1);
            return;
        }
        auto fd = connect_tcp_peer(peer.address);
        if (!fd) {
            peer.reconnect = Clock::now() + std::chrono::seconds(peer.backoff);
            peer.backoff = std::min(30, peer.backoff * 2);
            return;
        }
        connection->fd = std::move(fd);
        connection->address = peer.address;
        connection->outgoing = true;
        connection->connecting = true;
        connection->handshake_deadline = Clock::now() + HANDSHAKE_TIMEOUT;
        peer.fd = connection->fd.get();
        dirty = true;
    }

    void publish_state() {
        if (!dirty)
            return;
        std::string snapshot;
        for (const auto &peer : peers) {
            char id[9]{};
            std::snprintf(id, sizeof(id), "%08x", peer.node);
            std::string status = peer.fd >= 0 ? "connecting" : "offline";
            if (peer.connected)
                status = peer.error.empty() ? "connected" : peer.error;
            snapshot += "peer\t" + std::string(id) + "\t" + peer.name + "\t" + peer.address + "\t" +
                        (peer.paused ? "paused" : "running") + "\t" + status + "\n";
        }
        if (write_atomic_file(PEER_STATE_PATH, snapshot))
            dirty = false;
        else
            log_line("publish peer state failed");
    }
};

TcpSync::TcpSync(std::uint32_t node, std::string psk, ChildProc &child)
    : impl_(std::make_unique<Impl>(node, std::move(psk), child)) {}
TcpSync::~TcpSync() {
    for (auto &connection : impl_->connections)
        if (connection.fd)
            impl_->disconnect(connection);
    impl_->dirty = true;
    impl_->publish_state();
}
bool TcpSync::start() {
    impl_->network_monitor = open_network_monitor();
    if (!impl_->network_monitor || !impl_->refresh_network())
        return false;
    reload_peers();
    impl_->publish_state();
    return true;
}
void TcpSync::reload_peers() {
    for (const auto &address : load_peer_addresses()) {
        if (auto *peer = impl_->learn(address); peer && !peer->connected)
            peer->reconnect = Clock::now();
    }
    impl_->discover = true;
}
void TcpSync::learn_peer(const std::string &address) {
    if (!is_lan_peer(address))
        return;
    if (auto *peer = impl_->learn(address); peer && peer->fd < 0)
        peer->reconnect = Clock::now();
}
void TcpSync::set_paused(bool paused) {
    impl_->paused = paused;
    for (auto &connection : impl_->connections) {
        if (connection.fd && connection.node != 0)
            impl_->queue(connection,
                         impl_->frame({MessageKind::State, impl_->node, 0, static_cast<std::uint8_t>(paused), {}}));
    }
}
void TcpSync::on_bridge_frame(const std::string &value) {
    if (value == "C") {
        impl_->echo.clear_current();
        impl_->latest.reset();
        for (auto &peer : impl_->peers)
            peer.pending.reset();
        return;
    }
    if (!value.empty() && (value[0] == 'T' || value[0] == 'L')) {
        const std::string text = value.substr(1);
        if (text.empty() || text.size() > kMaxPayload)
            return;
        const auto hash = fnv1a64(text);
        if (value[0] == 'L')
            impl_->echo.local_change(hash);
        else if (!impl_->echo.on_local(hash))
            return;
        if (impl_->paused)
            return;
        if (++impl_->sequence == 0)
            ++impl_->sequence;
        impl_->latest = impl_->frame({MessageKind::Text, impl_->node, impl_->sequence, 0, text});
        for (auto &peer : impl_->peers) {
            peer.pending = impl_->latest;
            peer.next_send = Clock::now();
            if (peer.fd < 0)
                peer.reconnect = Clock::now();
        }
        impl_->discover = true;
        return;
    }
    std::uint64_t id = 0;
    bool success = false;
    if (bridge_result(value, id, success)) {
        if (id == impl_->applying)
            impl_->finish_apply(success);
    } else {
        log_line("invalid bridge response");
        stop_bridge(impl_->child);
    }
}
void TcpSync::append_poll(std::vector<pollfd> &fds) {
    fds.push_back({impl_->network_monitor.get(), POLLIN, 0});
    for (const auto &listener : impl_->listeners)
        fds.push_back({listener.get(), POLLIN, 0});
    if (impl_->applying && impl_->bridge_offset < impl_->bridge_output.size() && impl_->child.to_child) {
        fds.push_back({impl_->child.to_child.get(), POLLOUT, 0});
    }
    for (const auto &connection : impl_->connections) {
        if (!connection.fd)
            continue;
        short events = POLLIN;
        if (connection.connecting || !connection.output.empty())
            events |= POLLOUT;
        fds.push_back({connection.fd.get(), events, 0});
    }
}
void TcpSync::process_poll(const std::vector<pollfd> &fds) {
    for (const auto &event : fds) {
        if (event.fd == impl_->network_monitor.get() && event.revents) {
            impl_->network_changed();
            return;
        }
    }
    for (const auto &event : fds) {
        if (!event.revents)
            continue;
        if (std::any_of(impl_->listeners.begin(), impl_->listeners.end(),
                        [&](const UniqueFd &listener) { return event.fd == listener.get(); })) {
            impl_->accept_connections(event.fd);
            continue;
        }
        if (event.fd == impl_->child.to_child.get()) {
            impl_->write_bridge();
            continue;
        }
        auto *connection = impl_->find_connection(event.fd);
        if (!connection)
            continue;
        if (connection->connecting && (event.revents & (POLLOUT | POLLERR | POLLHUP))) {
            int error = 0;
            socklen_t size = sizeof(error);
            if (getsockopt(event.fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0) {
                impl_->disconnect(*connection);
                continue;
            }
            connection->connecting = false;
            impl_->hello(*connection);
        }
        if (event.revents & POLLIN)
            impl_->read_connection(*connection);
        if (!connection->fd)
            continue;
        if (event.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            impl_->disconnect(*connection);
            continue;
        }
        if (event.revents & POLLOUT)
            impl_->write_connection(*connection);
    }
}
void TcpSync::tick() {
    const auto now = Clock::now();
    if (impl_->applying && (now >= impl_->apply_deadline || impl_->child.pid != impl_->applying_child)) {
        if (impl_->child.pid == impl_->applying_child)
            stop_bridge(impl_->child);
        impl_->finish_apply(false);
    }
    for (auto &connection : impl_->connections)
        if (connection.fd && now >= connection.handshake_deadline)
            impl_->disconnect(connection);
    for (auto &peer : impl_->peers) {
        if (peer.fd < 0 && now >= peer.reconnect)
            impl_->connect_peer(peer);
        if (!peer.connected)
            continue;
        auto *connection = impl_->find_connection(peer.fd);
        if (!connection)
            continue;
        if (peer.inflight && now >= peer.ack_deadline) {
            impl_->disconnect(*connection);
            continue;
        }
        if (!impl_->paused && !peer.paused && peer.pending && !peer.inflight && now >= peer.next_send) {
            const auto sequence = peer.pending->sequence;
            if (impl_->queue(*connection, peer.pending)) {
                peer.inflight = sequence;
                peer.ack_deadline = now + ACK_TIMEOUT;
            }
        }
    }
    impl_->publish_state();
}
int TcpSync::timeout() const {
    Time deadline = impl_->apply_deadline;
    for (const auto &connection : impl_->connections)
        if (connection.fd)
            deadline = std::min(deadline, connection.handshake_deadline);
    for (const auto &peer : impl_->peers) {
        if (peer.fd < 0)
            deadline = std::min(deadline, peer.reconnect);
        if (peer.inflight)
            deadline = std::min(deadline, peer.ack_deadline);
        if (peer.connected && !impl_->paused && !peer.paused && peer.pending && !peer.inflight)
            deadline = std::min(deadline, peer.next_send);
    }
    if (deadline == Time::max())
        return -1;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return static_cast<int>(std::clamp<std::int64_t>(left, 0, 60000));
}
bool TcpSync::take_discovery_request() { return std::exchange(impl_->discover, false); }
} // namespace clipsync
