// Start and reap the shell-uid app_process clipboard bridge.
#include "clipsync/bridge.hpp"

#include "clipsync/config.hpp"
#include "clipsync/log.hpp"

#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

namespace clipsync {
namespace {

void close_fd(int fd) {
    if (fd < 0) {
        return;
    }
    while (close(fd) < 0 && errno == EINTR) {
    }
}

void reset_child_signals() {
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGCHLD, &sa, nullptr);
    sigaction(SIGPIPE, &sa, nullptr);
}

void clear_child(ChildProc &child) {
    child.pid = -1;
    child.to_child.reset();
    child.from_child.reset();
    child.decoder.reset();
}

} // namespace

bool spawn_bridge(ChildProc &child) {
    int inbound[2] = {-1, -1};
    int outbound[2] = {-1, -1};
    if (pipe2(inbound, O_CLOEXEC) != 0) {
        log_errno("pipe stdin");
        return false;
    }
    if (pipe2(outbound, O_CLOEXEC) != 0) {
        log_errno("pipe stdout");
        close_fd(inbound[0]);
        close_fd(inbound[1]);
        return false;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        log_errno("fork");
        close_fd(inbound[0]);
        close_fd(inbound[1]);
        close_fd(outbound[0]);
        close_fd(outbound[1]);
        return false;
    }
    if (pid == 0) {
        if (dup2(inbound[0], STDIN_FILENO) < 0 || dup2(outbound[1], STDOUT_FILENO) < 0) {
            _exit(126);
        }
        close_fd(inbound[0]);
        close_fd(inbound[1]);
        close_fd(outbound[0]);
        close_fd(outbound[1]);
        reset_child_signals();
        if (setgroups(0, nullptr) != 0 || setgid(kBridgeUid) != 0 || setuid(kBridgeUid) != 0) {
            _exit(125);
        }
        if (getuid() != kBridgeUid) {
            _exit(125);
        }
        if (setenv("CLASSPATH", kDexPath, 1) != 0) {
            _exit(124);
        }
        execl(kAppProcess, "app_process", kCmdDir, kClassName, static_cast<char *>(nullptr));
        _exit(127);
    }
    close_fd(inbound[0]);
    close_fd(outbound[1]);
    clear_child(child);
    child.pid = pid;
    child.to_child.reset(inbound[1]);
    child.from_child.reset(outbound[0]);
    const int flags = fcntl(child.to_child.get(), F_GETFL);
    if (flags < 0 || fcntl(child.to_child.get(), F_SETFL, flags | O_NONBLOCK) < 0) {
        log_errno("nonblocking bridge");
        stop_bridge(child);
        return false;
    }
    log_int("bridge pid", static_cast<int>(pid));
    return true;
}

bool reap_bridge(ChildProc &child) {
    if (child.pid <= 0) {
        return false;
    }
    int status = 0;
    const pid_t got = waitpid(child.pid, &status, WNOHANG);
    if (got == 0) {
        return false;
    }
    if (got < 0) {
        if (errno != ECHILD) {
            log_errno("waitpid");
        }
        clear_child(child);
        return true;
    }
    if (WIFEXITED(status)) {
        log_int("bridge exit", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        log_int("bridge signal", WTERMSIG(status));
    }
    clear_child(child);
    return true;
}

void stop_bridge(ChildProc &child) {
    if (child.pid > 0) {
        if (kill(child.pid, SIGTERM) != 0 && errno != ESRCH) {
            log_errno("kill bridge");
        }
        int status = 0;
        if (waitpid(child.pid, &status, 0) < 0 && errno != ECHILD) {
            log_errno("wait bridge");
        }
    }
    clear_child(child);
}

} // namespace clipsync
