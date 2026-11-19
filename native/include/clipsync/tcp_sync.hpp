// Event-driven TCP peers, reconnects, and clipboard write acknowledgements.
#pragma once

#include "clipsync/bridge.hpp"
#include <memory>
#include <poll.h>
#include <string>
#include <vector>

namespace clipsync {
class TcpSync {
  public:
    TcpSync(std::uint32_t node, std::string psk, ChildProc &child);
    ~TcpSync();
    bool start();
    void reload_peers();
    void learn_peer(const std::string &address);
    void set_paused(bool paused);
    void on_bridge_frame(const std::string &frame);
    void append_poll(std::vector<pollfd> &fds);
    void process_poll(const std::vector<pollfd> &fds);
    void tick();
    int timeout() const;
    bool take_discovery_request();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace clipsync
