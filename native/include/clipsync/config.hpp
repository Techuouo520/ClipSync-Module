// Shared limits, UDP layout, and on-device paths.
#pragma once

#include <cstddef>
#include <cstdint>

namespace clipsync {

inline constexpr std::uint32_t kMagic = 0x4353594Eu;
inline constexpr std::uint16_t kUdpPort = 47321;
inline constexpr std::uint16_t TCP_PORT = 47322;
inline constexpr std::uint32_t kMaxPayload = 65536;
inline constexpr std::uint32_t MAX_MESSAGE_FRAME = kMaxPayload + 32;
inline constexpr std::size_t kPacketHeader = 8;
inline constexpr int kRestartDelayMs = 500;
inline constexpr unsigned kBridgeUid = 2000;
inline constexpr char kDexPath[] = "/system/framework/clip.dex";
inline constexpr char kClassName[] = "com.clipsync.ClipHelper";
inline constexpr char kAppProcess[] = "/system/bin/app_process";
inline constexpr char kCmdDir[] = "/system/bin";
inline constexpr char kWorkDir[] = "/data/adb/clipsync";
inline constexpr char kPskPath[] = "/data/adb/clipsync/psk";
inline constexpr char kPidPath[] = "/data/adb/clipsync/clipsyncd.pid";
inline constexpr char kStatePath[] = "/data/adb/clipsync/state";
inline constexpr char kLogPath[] = "/data/local/tmp/clipsync.log";
inline constexpr char PEERS_PATH[] = "/data/adb/clipsync/peers";
inline constexpr char PEER_STATE_PATH[] = "/data/adb/clipsync/peers.state";

} // namespace clipsync
