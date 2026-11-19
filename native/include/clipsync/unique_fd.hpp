// Closes a POSIX file descriptor when the owner is destroyed.
#pragma once

#include <unistd.h>

#include <cerrno>

namespace clipsync {

class UniqueFd {
  public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    UniqueFd(UniqueFd &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

    UniqueFd &operator=(UniqueFd &&other) noexcept {
        if (this != &other) {
            reset(other.fd_);
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void reset(int next = -1) {
        if (fd_ >= 0) {
            const int fd = fd_;
            fd_ = -1;
            while (close(fd) < 0 && errno == EINTR) {
            }
        }
        fd_ = next;
    }

  private:
    int fd_ = -1;
};

} // namespace clipsync
