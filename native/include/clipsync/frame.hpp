// Length-prefixed pipe frames: uint32 big-endian plus UTF-8 bytes.
#pragma once

#include "clipsync/config.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace clipsync {

bool encode_frame(std::string_view payload, std::vector<std::uint8_t> &out, std::size_t limit = kMaxPayload);

// Pulls frames from a byte stream and rejects lengths above the configured limit.
class FrameDecoder {
  public:
    explicit FrameDecoder(std::size_t limit = kMaxPayload) : limit_(limit) {}
    void reset();
    void append(const std::uint8_t *data, std::size_t size);
    bool pop(std::string &payload);
    bool failed() const;

  private:
    std::vector<std::uint8_t> buffer_;
    bool failed_ = false;
    std::size_t limit_;
};

} // namespace clipsync
