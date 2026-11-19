// Append debug lines to the log file and stderr. Release builds do neither.
#include "clipsync/log.hpp"

#include "clipsync/config.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef CLIPSYNC_DEBUG
#include <fcntl.h>
#include <unistd.h>
#endif

namespace clipsync {

void log_line(const char *text) {
#ifdef CLIPSYNC_DEBUG
    if (text == nullptr) {
        return;
    }
    static int fd = -1;
    static bool opened = false;
    if (!opened) {
        opened = true;
        fd = open(kLogPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0) {
            std::fprintf(stderr, "clipsync: log open: %s\n", std::strerror(errno));
        }
    }
    std::string line(text);
    line.push_back('\n');
    if (fd >= 0) {
        const ssize_t wrote = write(fd, line.data(), line.size());
        if (wrote < 0) {
            std::fprintf(stderr, "clipsync: log write: %s\n", std::strerror(errno));
        }
    }
    std::fwrite(line.data(), 1, line.size(), stderr);
#else
    (void)text;
#endif
}

void log_errno(const char *what) {
#ifdef CLIPSYNC_DEBUG
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s: %s", what != nullptr ? what : "error", std::strerror(errno));
    log_line(buf);
#else
    (void)what;
#endif
}

void log_int(const char *what, int value) {
#ifdef CLIPSYNC_DEBUG
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s: %d", what != nullptr ? what : "value", value);
    log_line(buf);
#else
    (void)what;
    (void)value;
#endif
}

} // namespace clipsync
