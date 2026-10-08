#include "crash.h"

#if defined(__linux__)
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <link.h>
#include <mutex>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#endif

namespace bdex {

#if defined(__linux__)
namespace {

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
struct sigaction g_old[sizeof(kSignals) / sizeof(kSignals[0])];
// Everything the handler needs, prepared at install time (no allocation in
// the handler).
char g_path[1024];        // <dir>/crash-<tag>- (the pid is appended in the handler)
size_t g_pathLen = 0;
char g_context[2048];
char g_object[512];       // the layer's file
uintptr_t g_base = 0;     // its load address (offsets are from there)
uintptr_t g_lo = 0, g_hi = 0;  // its loaded segments
std::atomic<bool> g_writing{false};
std::string g_fullPath;

bool ours(uintptr_t a) { return a >= g_lo && a < g_hi; }

void put(int fd, const char* s) { (void)!write(fd, s, strlen(s)); }

void putHex(int fd, uintptr_t v) {
    char b[2 + 2 * sizeof(v) + 1];
    size_t n = sizeof(b) - 1;
    b[n] = 0;
    do {
        b[--n] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v && n > 2);
    b[--n] = 'x';
    b[--n] = '0';
    put(fd, b + n);
}

void putDec(int fd, long v) {
    char b[24];
    size_t n = sizeof(b) - 1;
    b[n] = 0;
    const bool neg = v < 0;
    unsigned long u = neg ? 0ul - static_cast<unsigned long>(v) : static_cast<unsigned long>(v);
    do {
        b[--n] = char('0' + u % 10);
        u /= 10;
    } while (u && n > 1);
    if (neg) b[--n] = '-';
    put(fd, b + n);
}

void putAddr(int fd, uintptr_t a) {
    if (ours(a)) {
        put(fd, "layer+");
        putHex(fd, a - g_base);
    } else {
        putHex(fd, a);
    }
}

uintptr_t faultPc(void* uctx) {
    const ucontext_t* uc = static_cast<const ucontext_t*>(uctx);
    if (!uc) return 0;
#if defined(__x86_64__)
    return static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__i386__)
    return static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_EIP]);
#elif defined(__aarch64__)
    return static_cast<uintptr_t>(uc->uc_mcontext.pc);
#else
    return 0;
#endif
}

const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    default: return "signal";
    }
}

// The previous handler takes over: called directly when it is a function,
// or reinstated (default: the faulting instruction runs again and the
// process dies of it with its core dump, as without the layer).
void chain(int sig, siginfo_t* info, void* uctx) {
    for (size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); ++i) {
        if (kSignals[i] != sig) continue;
        const struct sigaction& old = g_old[i];
        if (old.sa_flags & SA_SIGINFO) {
            if (old.sa_sigaction) {
                old.sa_sigaction(sig, info, uctx);
                return;
            }
        } else if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) {
            old.sa_handler(sig);
            return;
        }
        sigaction(sig, &old, nullptr);
        if (old.sa_handler == SIG_IGN) signal(sig, SIG_DFL);  // ignoring a fault would loop on it
        return;
    }
}

void handler(int sig, siginfo_t* info, void* uctx) {
    const uintptr_t pc = faultPc(uctx);
    bool mine = ours(pc);
    void* frames[32];
    int n = 0;
#if defined(__GLIBC__)
    n = backtrace(frames, 32);
    // (frames 0-1: this handler and the signal trampoline; then the faulting
    // function and its callers: a fault in a driver called by the layer counts)
    for (int i = 2; i < n && i < 7 && !mine; ++i) mine = ours(reinterpret_cast<uintptr_t>(frames[i]));
#endif
    if (mine && !g_writing.exchange(true)) {
        char path[sizeof(g_path) + 24];
        memcpy(path, g_path, g_pathLen);
        size_t len = g_pathLen;
        char digits[16];
        int nd = 0;
        for (long pid = getpid(); pid > 0 && nd < 15; pid /= 10) digits[nd++] = char('0' + pid % 10);
        while (nd > 0) path[len++] = digits[--nd];
        memcpy(path + len, ".txt", 5);
        const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) {
            put(fd, "bdex-framegen crash report\n");
            put(fd, "signal: ");
            put(fd, signalName(sig));
            put(fd, " (");
            putDec(fd, sig);
            put(fd, "), code ");
            putDec(fd, info ? info->si_code : 0);
            put(fd, ", fault address ");
            putHex(fd, info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0);
            put(fd, "\npc: ");
            putAddr(fd, pc);
            put(fd, "\nthread: ");
            putDec(fd, static_cast<long>(syscall(SYS_gettid)));  // (gettid(): glibc 2.30, newer than the archives' 2.29)
            put(fd, "\nlayer: ");
            put(fd, g_object);
            put(fd, " (loaded at ");
            putHex(fd, g_base);
            put(fd, ")\nframes:\n");
            for (int i = 2; i < n; ++i) {
                put(fd, "  ");
                putAddr(fd, reinterpret_cast<uintptr_t>(frames[i]));
                put(fd, "\n");
            }
            put(fd, "symbols: addr2line -f -C -e <layer> <the layer+0x... offsets>\n");
            put(fd, "context: ");
            put(fd, g_context);
            put(fd, "\n");
            close(fd);
        }
    }
    chain(sig, info, uctx);
}

int findSelf(dl_phdr_info* info, size_t, void* data) {
    const uintptr_t self = reinterpret_cast<uintptr_t>(data);
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD) continue;
        lo = std::min<uintptr_t>(lo, info->dlpi_addr + ph.p_vaddr);
        hi = std::max<uintptr_t>(hi, info->dlpi_addr + ph.p_vaddr + ph.p_memsz);
    }
    if (self < lo || self >= hi) return 0;
    g_lo = lo;
    g_hi = hi;
    g_base = info->dlpi_addr;
    snprintf(g_object, sizeof g_object, "%s", info->dlpi_name && *info->dlpi_name ? info->dlpi_name : "(executable)");
    return 1;
}

} // namespace

bool crashLogInstall(const std::string& dir, const std::string& tag, const std::string& context) {
    static std::once_flag once;
    bool installed = false;
    std::call_once(once, [&] {
        if (dir.empty()) return;
        dl_iterate_phdr(findSelf, reinterpret_cast<void*>(&crashLogInstall));
        if (g_hi == 0) return;
        mkdir(dir.c_str(), 0755);  // (its parent, the cache directory, exists for the status files)
        std::string safeTag = tag.empty() ? std::string("game") : tag;
        for (char& c : safeTag)
            if (c == '/' || c == ' ') c = '_';
        const std::string prefix = dir + "/crash-" + safeTag + "-";
        if (prefix.size() + 24 >= sizeof g_path) return;
        memcpy(g_path, prefix.data(), prefix.size());
        g_pathLen = prefix.size();
        g_fullPath = prefix + std::to_string(getpid()) + ".txt";
        snprintf(g_context, sizeof g_context, "%s", context.c_str());
#if defined(__GLIBC__)
        void* warm[1];
        backtrace(warm, 1);  // loads libgcc_s now: not from the handler
#endif
        struct sigaction sa {};
        sa.sa_sigaction = handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        for (size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); ++i) sigaction(kSignals[i], &sa, &g_old[i]);
        installed = true;
    });
    return installed;
}

std::string crashLogPath() { return g_fullPath; }

#else

bool crashLogInstall(const std::string&, const std::string&, const std::string&) { return false; }
std::string crashLogPath() { return {}; }

#endif

} // namespace bdex
