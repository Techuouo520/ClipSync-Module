// Track current text and suppress repeated remote callbacks until a local change.
#pragma once

#include <cstdint>

namespace clipsync {

class EchoState {
  public:
    void cancel_remote() { has_suppress_ = false; }
    void clear_current() {
        has_current_ = false;
        has_suppress_ = false;
    }
    bool is_current(std::uint64_t hash) const { return has_current_ && current_ == hash; }
    void confirm_remote(std::uint64_t hash) {
        current_ = hash;
        has_current_ = true;
    }
    void local_change(std::uint64_t hash) {
        has_suppress_ = false;
        confirm_remote(hash);
        last_sent_ = hash;
        has_sent_ = true;
    }
    void expect_remote(std::uint64_t hash) {
        suppress_local_ = hash;
        has_suppress_ = true;
    }
    bool on_local(std::uint64_t hash) {
        confirm_remote(hash);
        if (has_suppress_ && hash == suppress_local_) {
            return false;
        }
        local_change(hash);
        return true;
    }

    bool on_remote(std::uint64_t hash) {
        if (has_sent_ && hash == last_sent_) {
            return false;
        }
        expect_remote(hash);
        return true;
    }

  private:
    std::uint64_t last_sent_ = 0;
    std::uint64_t suppress_local_ = 0;
    bool has_sent_ = false;
    bool has_suppress_ = false;
    std::uint64_t current_ = 0;
    bool has_current_ = false;
};

} // namespace clipsync
