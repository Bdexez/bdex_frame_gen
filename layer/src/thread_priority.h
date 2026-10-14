#pragma once
#include <string>

namespace bdex {

// Raises the calling thread's scheduling priority, as far as the system lets
// an ordinary user: the real-time round-robin class where permitted
// (RLIMIT_RTPRIO, CAP_SYS_NICE — gamemode or a realtime group may grant
// it), else a lower nice value from rtkit (org.freedesktop.RealtimeKit1
// MakeThreadHighPriority, the service PipeWire's audio threads use; libdbus
// loaded at run time, no dependency), else nothing. Returns what it got
// ("SCHED_RR", "rtkit nice -N", or "" for nothing). Windows: the MMCSS
// "Games" task, else THREAD_PRIORITY_HIGHEST. Blocks for up to a fifth of a
// second (D-Bus): call it from the thread itself, off the game's thread.
std::string raiseThreadPriority(int niceLevel = -10);

} // namespace bdex
