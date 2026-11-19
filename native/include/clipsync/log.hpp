// Debug log lines. Release builds compile the writes out.
#pragma once

namespace clipsync {

void log_line(const char *text);
void log_errno(const char *what);
void log_int(const char *what, int value);

} // namespace clipsync
