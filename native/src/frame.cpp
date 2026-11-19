// Length-prefixed pipe frame encoder and stream decoder.
#include "clipsync/frame.hpp"

#include "clipsync/byte_order.hpp"
#include "clipsync/config.hpp"

#include <cstring>

namespace clipsync {

bool encode_frame(std::string_view payload, std::vector<std::uint8_t> &out, std::size_t limit) {
    if (payload.size() > limit) {
        return false;
    }
    out.resize(4 + payload.size());
    store_be32(out.data(), static_cast<std::uint32_t>(payload.size()));
    if (!payload.empty()) {
        std::memcpy(out.data() + 4, payload.data(), payload.size());
    }
    return true;
}

void FrameDecoder::reset() {
    buffer_.clear();
    failed_ = false;
}

void FrameDecoder::append(const std::uint8_t *data, std::size_t size) {
    if (failed_ || data == nullptr || size == 0) {
        return;
    }
    buffer_.insert(buffer_.end(), data, data + size);
}

bool FrameDecoder::failed() const { return failed_; }

bool FrameDecoder::pop(std::string &payload) {
    if (failed_) {
        return false;
    }
    while (buffer_.size() >= 4) {
        const std::uint32_t length = load_be32(buffer_.data());
        if (length > limit_) {
            failed_ = true;
            return false;
        }
        if (buffer_.size() < 4u + length) {
            return false;
        }
        if (length == 0) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + 4);
            continue;
        }
        payload.assign(reinterpret_cast<const char *>(buffer_.data() + 4), length);
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(4u + length));
        return true;
    }
    return false;
}

} // namespace clipsync
