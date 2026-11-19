// Host checks for frames, FNV-1a, PSK, echo suppression, and UDP packets.
#include "clipsync/byte_order.hpp"
#include "clipsync/config.hpp"
#include "clipsync/discovery.hpp"
#include "clipsync/echo.hpp"
#include "clipsync/fnv.hpp"
#include "clipsync/frame.hpp"
#include "clipsync/iface.hpp"
#include "clipsync/packet.hpp"
#include "clipsync/psk.hpp"
#include "clipsync/tcp_protocol.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace clipsync;

namespace {

int g_failed = 0;

void check(bool cond, const char *file, int line, const char *text) {
    if (!cond) {
        std::fprintf(stderr, "fail %s:%d %s\n", file, line, text);
        ++g_failed;
    }
}

#define CHECK(cond) check(static_cast<bool>(cond), __FILE__, __LINE__, #cond)

void test_frame_roundtrip() {
    const std::string text = "a\nb\nc";
    std::vector<std::uint8_t> encoded;
    CHECK(encode_frame(text, encoded));
    CHECK(encoded.size() == 4 + text.size());
    CHECK(load_be32(encoded.data()) == static_cast<std::uint32_t>(text.size()));
    CHECK(encoded[4] == 'a');
    CHECK(encoded[5] == '\n');
    clipsync::FrameDecoder decoder;
    std::string out;
    decoder.append(encoded.data(), 2);
    CHECK(!decoder.pop(out));
    decoder.append(encoded.data() + 2, encoded.size() - 2);
    CHECK(decoder.pop(out));
    CHECK(out == text);
    CHECK(!decoder.pop(out));
}

void test_zero_and_two_frames() {
    std::vector<std::uint8_t> blob;
    std::vector<std::uint8_t> empty;
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    CHECK(encode_frame("", empty));
    CHECK(encode_frame("one", first));
    CHECK(encode_frame("two", second));
    blob.insert(blob.end(), empty.begin(), empty.end());
    blob.insert(blob.end(), first.begin(), first.end());
    blob.insert(blob.end(), second.begin(), second.end());
    clipsync::FrameDecoder decoder;
    decoder.append(blob.data(), blob.size());
    std::string out;
    CHECK(decoder.pop(out) && out == "one");
    CHECK(decoder.pop(out) && out == "two");
    CHECK(!decoder.pop(out));
    CHECK(!decoder.failed());
}

void test_oversize_frame() {
    std::uint8_t header[4];
    store_be32(header, clipsync::kMaxPayload + 1);
    clipsync::FrameDecoder decoder;
    decoder.append(header, 4);
    std::string out;
    CHECK(!decoder.pop(out));
    CHECK(decoder.failed());
    const std::uint8_t extra[] = {'x'};
    decoder.append(extra, 1);
    CHECK(!decoder.pop(out));
    CHECK(decoder.failed());
}

void test_emoji_frame() {
    const std::string emoji = "\xF0\x9F\x98\x80";
    std::vector<std::uint8_t> encoded;
    CHECK(encode_frame(emoji, encoded));
    clipsync::FrameDecoder decoder;
    decoder.append(encoded.data(), encoded.size());
    std::string out;
    CHECK(decoder.pop(out));
    CHECK(out == emoji);
}

void test_fnv_vectors() {
    CHECK(clipsync::fnv1a64("") == 0xcbf29ce484222325ull);
    CHECK(clipsync::fnv1a64("a") == 0xaf63dc4c8601ec8cull);
    CHECK(clipsync::fnv1a64("foobar") == 0x85944171f73967e8ull);
    CHECK(clipsync::fnv1a64("a") != clipsync::fnv1a64("b"));
}

void test_psk() {
    std::uint8_t data[] = {'A', 'B', 'C', 'D'};
    clipsync::apply_psk(data, 4, "");
    CHECK(data[0] == 'A' && data[3] == 'D');
    clipsync::apply_psk(data, 4, "k");
    CHECK(data[0] == static_cast<std::uint8_t>('A' ^ 'k'));
    CHECK(data[1] == static_cast<std::uint8_t>('B' ^ 'k'));
    clipsync::apply_psk(data, 4, "k");
    CHECK(std::memcmp(data, "ABCD", 4) == 0);
    std::uint8_t mixed[] = {1, 2, 3};
    clipsync::apply_psk(mixed, 3, "xy");
    CHECK(mixed[0] == static_cast<std::uint8_t>(1 ^ 'x'));
    CHECK(mixed[1] == static_cast<std::uint8_t>(2 ^ 'y'));
    CHECK(mixed[2] == static_cast<std::uint8_t>(3 ^ 'x'));
}

void test_echo() {
    clipsync::EchoState echo;
    const auto same = clipsync::fnv1a64("a");
    const auto other = clipsync::fnv1a64("b");
    CHECK(echo.on_local(same));
    CHECK(!echo.on_remote(same));
    CHECK(echo.on_local(same));
    CHECK(echo.on_remote(other));
    CHECK(!echo.on_local(other));
    for (int index = 0; index < 1000; ++index) CHECK(!echo.on_local(other));
    CHECK(echo.is_current(other));
    echo.local_change(other);
    CHECK(echo.on_local(other));
    echo.expect_remote(same);
    CHECK(echo.on_local(other));
    CHECK(echo.on_local(same));
    echo.clear_current();
    CHECK(!echo.is_current(same));
    echo.expect_remote(other);
    CHECK(!echo.is_current(other));
    echo.confirm_remote(other);
    CHECK(echo.is_current(other));
    echo.cancel_remote();
    CHECK(echo.on_local(other));
}

void test_packet() {
    std::vector<std::uint8_t> packet;
    CHECK(encode_packet(0x01020304u, "A", "", packet));
    CHECK(packet.size() == 9);
    CHECK(packet[0] == 0x43 && packet[1] == 0x53 && packet[2] == 0x59 && packet[3] == 0x4e);
    CHECK(packet[4] == 0x01 && packet[5] == 0x02 && packet[6] == 0x03 && packet[7] == 0x04);
    CHECK(packet[8] == 'A');
    std::uint32_t node = 0;
    std::string plain;
    CHECK(decode_packet(packet.data(), packet.size(), "", node, plain));
    CHECK(node == 0x01020304u);
    CHECK(plain == "A");

    CHECK(encode_packet(7, "A", "k", packet));
    CHECK(packet[0] == 0x43 && packet[4] == 0x00 && packet[7] == 0x07);
    CHECK(packet[8] == static_cast<std::uint8_t>('A' ^ 'k'));
    CHECK(decode_packet(packet.data(), packet.size(), "k", node, plain));
    CHECK(node == 7 && plain == "A");

    CHECK(!decode_packet(packet.data(), 7, "k", node, plain));
    packet[0] = 0;
    CHECK(!decode_packet(packet.data(), packet.size(), "k", node, plain));
    std::string big(clipsync::kMaxPayload + 1, 'x');
    CHECK(!encode_packet(1, big, "", packet));
}

void test_iface_names() {
    CHECK(clipsync::is_sync_iface("wlan0"));
    CHECK(clipsync::is_sync_iface("ap0"));
    CHECK(clipsync::is_sync_iface("wlan"));
    CHECK(!clipsync::is_sync_iface("tun0"));
    CHECK(!clipsync::is_sync_iface("rmnet_data0"));
    CHECK(!clipsync::is_sync_iface("swlan0"));
    CHECK(!clipsync::is_sync_iface("Wlan0"));
}

void test_discovery_packets() {
    DiscoveryPacket probe;
    probe.nonce = 0x12345678;
    std::vector<std::uint8_t> bytes;
    CHECK(encode_discovery(probe, bytes));
    CHECK(bytes.size() == DISCOVERY_HEADER);
    CHECK(load_be32(bytes.data()) == DISCOVERY_MAGIC);
    DiscoveryPacket decoded;
    CHECK(decode_discovery(bytes.data(), bytes.size(), decoded));
    CHECK(!decoded.reply && decoded.nonce == probe.nonce);
    std::uint32_t node = 0;
    std::string text;
    CHECK(!decode_packet(bytes.data(), bytes.size(), "", node, text));

    DiscoveryPacket response{true, true, probe.nonce, 42, "Phone \xF0\x9F\x98\x80"};
    CHECK(encode_discovery(response, bytes));
    CHECK(decode_discovery(bytes.data(), bytes.size(), decoded));
    CHECK(decoded.reply && decoded.paused && decoded.node == 42 && decoded.name == response.name);
    CHECK(!decode_packet(bytes.data(), bytes.size(), "", node, text));
    CHECK(!decode_discovery(nullptr, bytes.size(), decoded));
    CHECK(!decode_discovery(bytes.data(), DISCOVERY_HEADER - 1, decoded));
    for (const std::size_t offset : {0u, 4u, 5u, 6u, 7u}) {
        auto invalid = bytes;
        invalid[offset] = 0xff;
        CHECK(!decode_discovery(invalid.data(), invalid.size(), decoded));
    }
    response.name = std::string(MAX_DEVICE_NAME, 'x');
    CHECK(encode_discovery(response, bytes));
    CHECK(decode_discovery(bytes.data(), bytes.size(), decoded));
    bytes.push_back('x');
    CHECK(!decode_discovery(bytes.data(), bytes.size(), decoded));
}

void test_discovery_validation() {
    std::vector<std::uint8_t> bytes;
    DiscoveryPacket packet;
    CHECK(!encode_discovery(packet, bytes));
    packet.nonce = 1;
    packet.name = "unexpected";
    CHECK(!encode_discovery(packet, bytes));
    packet.reply = true;
    CHECK(!encode_discovery(packet, bytes));
    packet.node = 1;
    for (const std::string &name : {std::string("a\tb"), std::string("a\nb"), std::string("a\0b", 3), std::string(97, 'x')}) {
        packet.name = name;
        CHECK(!encode_discovery(packet, bytes));
    }
    packet.name = "Phone";
    CHECK(encode_discovery(packet, bytes));
    bytes.back() = '\t';
    DiscoveryPacket decoded;
    CHECK(!decode_discovery(bytes.data(), bytes.size(), decoded));
}

void test_discovery_peers() {
    std::vector<DiscoveredPeer> peers;
    DiscoveryPacket response{true, false, 123, 1, "Phone"};
    collect_peer(peers, response, 999, "192.0.2.1");
    CHECK(peers.empty());
    collect_peer(peers, response, 123, "192.0.2.1");
    collect_peer(peers, response, 123, "192.0.2.1");
    CHECK(peers.size() == 1);
    response.paused = true;
    collect_peer(peers, response, 123, "192.0.2.2");
    CHECK(peers.size() == 1 && peers.front().paused && peers.front().address == "192.0.2.2");
    response.node = 2;
    collect_peer(peers, response, 123, "192.0.2.2");
    CHECK(peers.size() == 1 && peers.front().node == 2);
    for (std::uint32_t index = 3; index < 100; ++index) {
        response.node = index;
        collect_peer(peers, response, 123, "192.0.2." + std::to_string(index));
    }
    CHECK(peers.size() == MAX_DISCOVERED_PEERS);
}

void test_tcp_protocol() {
    TcpMessage sent{MessageKind::Hello, 42, 0, 1, "Phone"};
    std::vector<std::uint8_t> wire;
    CHECK(encode_tcp_message(sent, "key", wire));
    FrameDecoder decoder(MAX_MESSAGE_FRAME);
    std::string body;
    decoder.append(wire.data(), 3);
    CHECK(!decoder.pop(body));
    decoder.append(wire.data() + 3, wire.size() - 3);
    CHECK(decoder.pop(body));
    TcpMessage message;
    CHECK(decode_tcp_message(body, "key", message));
    CHECK(message.kind == MessageKind::Hello && message.node == 42 && message.code == 1 && message.text == "Phone");
    sent = {MessageKind::Text, 42, 7, 0, "one\ntwo\xF0\x9F\x98\x80"};
    CHECK(encode_tcp_message(sent, "key", wire));
    decoder.append(wire.data(), wire.size());
    CHECK(decoder.pop(body));
    CHECK(decode_tcp_message(body, "key", message) && message.key_matches && message.text == sent.text);
    CHECK(decode_tcp_message(body, "wrong", message) && !message.key_matches);
    body[0] = 0;
    CHECK(!decode_tcp_message(body, "key", message));
    sent.text = std::string(kMaxPayload, 'x');
    CHECK(encode_tcp_message(sent, "", wire));
    decoder.append(wire.data(), wire.size());
    CHECK(decoder.pop(body));
    CHECK(decode_tcp_message(body, "", message) && message.text.size() == kMaxPayload);
    sent.text.push_back('x');
    CHECK(!encode_tcp_message(sent, "", wire));
    sent = {MessageKind::Ack, 42, 7, static_cast<std::uint8_t>(AckCode::Applied), {}};
    CHECK(encode_tcp_message(sent, "", wire));
    decoder.append(wire.data(), wire.size());
    CHECK(decoder.pop(body) && decode_tcp_message(body, "", message));
    CHECK(message.kind == MessageKind::Ack && message.sequence == 7 && message.code == 0);
    sent.code = 99;
    CHECK(!encode_tcp_message(sent, "", wire));
}

void test_bridge_receipts() {
    constexpr std::uint64_t ID = 0x1234567800000001;
    CHECK(bridge_command(ID, "a\nb") == "W1234567800000001a\nb");
    std::uint64_t id = 0;
    bool success = false;
    CHECK(bridge_result("A1234567800000001", id, success) && success && id == ID);
    CHECK(bridge_result("E1234567800000001", id, success) && !success && id == ID);
    CHECK(!bridge_result("A0000000000000000", id, success));
    CHECK(!bridge_result("A12345678zzzzzzzz", id, success));
    CHECK(!bridge_result("A1234567800000001extra", id, success));
    CHECK(!bridge_result("Tclipboard", id, success));
}

} // namespace

int main() {
    test_frame_roundtrip();
    test_zero_and_two_frames();
    test_oversize_frame();
    test_emoji_frame();
    test_fnv_vectors();
    test_psk();
    test_echo();
    test_packet();
    test_iface_names();
    test_discovery_packets();
    test_discovery_validation();
    test_discovery_peers();
    test_tcp_protocol();
    test_bridge_receipts();
    if (g_failed != 0) {
        std::fprintf(stderr, "%d checks failed\n", g_failed);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
