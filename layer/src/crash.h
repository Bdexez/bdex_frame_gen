#pragma once
#include <string>

namespace bdex {

// Crash journal (Linux): on SIGSEGV / SIGBUS / SIGILL / SIGFPE whose faulting
// instruction — or one of its first callers — is in the object this code is
// linked into (the layer), writes <dir>/crash-<tag>-<pid>.txt: the signal,
// the addresses as offsets in the layer (for addr2line), and `context` (the
// version and settings). Every other fault, and this one afterwards, goes to
// the handler that was there before (Wine and Proton handle faults of their
// own, guard pages and exceptions: never swallowed). Installed once per
// process; false where unsupported (Windows) or already installed.
bool crashLogInstall(const std::string& dir, const std::string& tag, const std::string& context);

// The file a crash of this process would be written to ("" before install).
std::string crashLogPath();

} // namespace bdex
