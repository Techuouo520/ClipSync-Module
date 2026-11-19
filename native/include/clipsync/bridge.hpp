// Forks app_process, drops to the shell uid, and owns the stdio pipes.
#pragma once

#include "clipsync/frame.hpp"
#include "clipsync/unique_fd.hpp"

#include <sys/types.h>

namespace clipsync {

struct ChildProc {
    pid_t pid = -1;
    UniqueFd to_child;
    UniqueFd from_child;
    FrameDecoder decoder{MAX_MESSAGE_FRAME};
};

bool spawn_bridge(ChildProc &child);
bool reap_bridge(ChildProc &child);
void stop_bridge(ChildProc &child);

} // namespace clipsync
