#include "crash.h"
#include "test.h"

#if defined(__linux__)
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
// A fault in the object the journal belongs to (crash.cpp is linked into
// this test binary as into the layer).
// A write to a page mapped without access: a real fault, and no undefined
// behaviour for UBSan to stop at first (a store through a null pointer is).
__attribute__((noinline)) void faultHere() {
    void* page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) _exit(6);
    *static_cast<volatile int*>(page) = 42;
}

// ASan and TSan handle SIGSEGV themselves (their report, then exit): the
// journal is not reached there.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitizer = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kSanitizer = true;
#else
constexpr bool kSanitizer = false;
#endif
#else
constexpr bool kSanitizer = false;
#endif

// A child of its own (output flushed first: it would print the parent's
// buffered lines a second time).
pid_t forkChild() {
    fflush(stdout);
    fflush(stderr);
    return fork();
}

std::string readAll(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

// The journal names the signal and the faulting instruction as an offset in
// the layer, then hands the fault on: the process still dies of SIGSEGV (no
// handler of its own: the default action), as it would without the layer.
TEST(crash_journal_written_then_chained) {
    if (kSanitizer) {
        printf("  (under a sanitizer: skipped)\n");
        return;
    }
    const std::string dir = test::tmpPath("bdex-crash-test");
    const pid_t child = forkChild();
    if (child == 0) {
        if (!bdex::crashLogInstall(dir, "unit test", "context line")) _exit(3);
        if (bdex::crashLogInstall(dir, "again", "")) _exit(4);  // once per process
        faultHere();
        _exit(5);  // not reached
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV))
        fprintf(stderr, "  child: exited %d (code %d), signal %d\n", WIFEXITED(status), WEXITSTATUS(status), WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    const std::string path = dir + "/crash-unit_test-" + std::to_string(child) + ".txt";
    const std::string text = readAll(path);
    CHECK(text.find("signal: SIGSEGV (11)") != std::string::npos);
    CHECK(text.find("pc: layer+0x") != std::string::npos);
    CHECK(text.find("context: context line") != std::string::npos);
    unlink(path.c_str());
    rmdir(dir.c_str());
}

// A previous handler of the process (Wine's, Proton's) gets the fault after
// the journal — and a fault elsewhere goes to it untouched.
namespace {
volatile sig_atomic_t g_previous = 0;
void previousHandler(int) {
    g_previous = 1;
    _exit(7);
}
} // namespace

TEST(crash_journal_keeps_the_previous_handler) {
    if (kSanitizer) return;
    const std::string dir = test::tmpPath("bdex-crash-test2");
    const pid_t child = forkChild();
    if (child == 0) {
        signal(SIGSEGV, previousHandler);
        if (!bdex::crashLogInstall(dir, "chain", "")) _exit(3);
        faultHere();
        _exit(5);
    }
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7);
    const std::string path = dir + "/crash-chain-" + std::to_string(child) + ".txt";
    CHECK(readAll(path).find("SIGSEGV") != std::string::npos);
    unlink(path.c_str());
    rmdir(dir.c_str());
}
#endif
