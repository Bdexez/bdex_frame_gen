#include "x11_present_clock.h"
#include "log.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef __linux__
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace bdex {

void PresentTimeline::add(uint64_t msc, double ustMs) {
    ++presents;
    if (!have_ || msc < lastMsc_ || ustMs < lastTickMs_) {  // the first, or the window's counter restarted
        have_ = true;
        lastMsc_ = msc;
        lastTickMs_ = ustMs;
        clock_.add(ustMs);
        return;
    }
    if (msc == lastMsc_) {  // no new MSC since the previous present: it was overwritten
        ++replaced;
        return;
    }
    // A new MSC: the previous present went out with it. Its UST is that tick:
    // under Xwayland a tick follows each handed-over content (one a refresh
    // while the window keeps presenting), on an X server each vblank.
    ++shown;
    const double period = clock_.valid() ? clock_.periodMs() : hintMs_;
    if (period > 0.0) {
        const double refreshes = std::round((ustMs - lastTickMs_) / period);
        if (refreshes >= 2.0 && refreshes <= 8.0) held += uint64_t(refreshes) - 1;
    }
    lastMsc_ = msc;
    lastTickMs_ = ustMs;
    clock_.add(ustMs);
}

#ifdef __linux__

// The few libxcb entry points needed, by their C signatures (opaque
// pointers, the cookie a struct of one unsigned int).
struct X11PresentClock::Xcb {
    struct Cookie { unsigned int sequence; };
    void* lib = nullptr;
    void* presentLib = nullptr;
    void* conn = nullptr;
    void* special = nullptr;
    void* presentId = nullptr;  // xcb_extension_t xcb_present_id
    uint32_t eid = 0;
    uint32_t window = 0;
    void* (*connect)(const char*, int*) = nullptr;
    int (*hasError)(void*) = nullptr;
    void (*disconnect)(void*) = nullptr;
    int (*fd)(void*) = nullptr;
    int (*flush)(void*) = nullptr;
    uint32_t (*generateId)(void*) = nullptr;
    const uint8_t* (*extensionData)(void*, void*) = nullptr;
    void* (*registerXge)(void*, void*, uint32_t, uint32_t*) = nullptr;
    void (*unregister)(void*, void*) = nullptr;
    void* (*pollSpecial)(void*, void*) = nullptr;
    void* (*requestCheck)(void*, Cookie) = nullptr;
    Cookie (*queryVersion)(void*, uint32_t, uint32_t) = nullptr;
    void* (*queryVersionReply)(void*, Cookie, void**) = nullptr;
    Cookie (*selectInputChecked)(void*, uint32_t, uint32_t, uint32_t) = nullptr;
    Cookie (*selectInput)(void*, uint32_t, uint32_t, uint32_t) = nullptr;
    ~Xcb() {
        if (conn) {
            if (special && unregister) unregister(conn, special);
            disconnect(conn);
        }
        if (presentLib) dlclose(presentLib);
        if (lib) dlclose(lib);
    }
};

namespace {
constexpr uint32_t kEventMaskComplete = 2;  // XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY
constexpr uint16_t kEventComplete = 1;      // XCB_PRESENT_EVENT_COMPLETE_NOTIFY
constexpr uint8_t kKindPixmap = 0;          // XCB_PRESENT_COMPLETE_KIND_PIXMAP
} // namespace

std::unique_ptr<X11PresentClock> X11PresentClock::start(uint32_t window, double hintHz) {
    if (!window) return nullptr;
    auto x = std::make_unique<Xcb>();
    x->lib = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
    x->presentLib = x->lib ? dlopen("libxcb-present.so.0", RTLD_NOW | RTLD_LOCAL) : nullptr;
    if (!x->presentLib) {
        BDEX_INFO("x11_clock: libxcb-present not found");
        return nullptr;
    }
    auto sym = [&](void* l, const char* n) { return dlsym(l, n); };
#define BDEX_XCB(field, lib, name) x->field = reinterpret_cast<decltype(x->field)>(sym(x->lib, name))
    BDEX_XCB(connect, lib, "xcb_connect");
    BDEX_XCB(hasError, lib, "xcb_connection_has_error");
    BDEX_XCB(disconnect, lib, "xcb_disconnect");
    BDEX_XCB(fd, lib, "xcb_get_file_descriptor");
    BDEX_XCB(flush, lib, "xcb_flush");
    BDEX_XCB(generateId, lib, "xcb_generate_id");
    BDEX_XCB(extensionData, lib, "xcb_get_extension_data");
    BDEX_XCB(registerXge, lib, "xcb_register_for_special_xge");
    BDEX_XCB(unregister, lib, "xcb_unregister_for_special_event");
    BDEX_XCB(pollSpecial, lib, "xcb_poll_for_special_event");
    BDEX_XCB(requestCheck, lib, "xcb_request_check");
    BDEX_XCB(queryVersion, presentLib, "xcb_present_query_version");
    BDEX_XCB(queryVersionReply, presentLib, "xcb_present_query_version_reply");
    BDEX_XCB(selectInputChecked, presentLib, "xcb_present_select_input_checked");
    BDEX_XCB(selectInput, presentLib, "xcb_present_select_input");
#undef BDEX_XCB
    x->presentId = sym(x->presentLib, "xcb_present_id");
    if (!x->connect || !x->hasError || !x->disconnect || !x->fd || !x->flush || !x->generateId || !x->extensionData ||
        !x->registerXge || !x->unregister || !x->pollSpecial || !x->requestCheck || !x->queryVersion ||
        !x->queryVersionReply || !x->selectInputChecked || !x->selectInput || !x->presentId) {
        BDEX_INFO("x11_clock: libxcb lacks an entry point");
        return nullptr;
    }
    // ($DISPLAY: the game's, Wine's included; a connection of our own.)
    x->conn = x->connect(nullptr, nullptr);
    if (!x->conn || x->hasError(x->conn)) {
        if (x->conn) x->disconnect(x->conn);
        x->conn = nullptr;
        BDEX_INFO("x11_clock: no X server");
        return nullptr;
    }
    // xcb_query_extension_reply_t: `present` is its 9th byte.
    const uint8_t* ext = x->extensionData(x->conn, x->presentId);
    if (!ext || !ext[8]) {
        BDEX_INFO("x11_clock: the X server has no Present extension");
        return nullptr;
    }
    if (void* reply = x->queryVersionReply(x->conn, x->queryVersion(x->conn, 1, 2), nullptr)) free(reply);
    x->eid = x->generateId(x->conn);
    x->window = window;
    if (void* err = x->requestCheck(x->conn, x->selectInputChecked(x->conn, x->eid, window, kEventMaskComplete))) {
        free(err);
        BDEX_INFO("x11_clock: window 0x%x not watchable", window);
        return nullptr;
    }
    x->special = x->registerXge(x->conn, x->presentId, x->eid, nullptr);
    if (!x->special) return nullptr;
    x->flush(x->conn);

    std::unique_ptr<X11PresentClock> c(new X11PresentClock());
    if (pipe2(c->stopPipe_, O_CLOEXEC | O_NONBLOCK) != 0) return nullptr;
    c->timeline_.setHint(hintHz);
    c->xcb_ = std::move(x);
    c->thread_ = std::thread([p = c.get()] { p->run(); });
    BDEX_INFO("x11_clock: watching the X Present completions of window 0x%x", window);
    return c;
}

X11PresentClock::~X11PresentClock() {
    if (thread_.joinable()) {
        const char b = 1;
        (void)!write(stopPipe_[1], &b, 1);
        thread_.join();
    }
    if (xcb_ && xcb_->conn) {
        // (the selection lives on the server until the connection goes; said
        // so anyway, before the disconnect in ~Xcb)
        xcb_->selectInput(xcb_->conn, xcb_->eid, xcb_->window, 0);
        xcb_->flush(xcb_->conn);
    }
    xcb_.reset();
    for (int f : stopPipe_)
        if (f >= 0) close(f);
}

void X11PresentClock::run() {
    Xcb& x = *xcb_;
    pollfd fds[2] = {{x.fd(x.conn), POLLIN, 0}, {stopPipe_[0], POLLIN, 0}};
    for (;;) {
        // Drain first (a read for another queue may have queued ours).
        while (void* ev = x.pollSpecial(x.conn, x.special)) {
            const uint8_t* b = static_cast<const uint8_t*>(ev);
            // xcb_present_complete_notify_event_t (packed): event_type at 8,
            // kind at 10, ust at 24, msc at 36.
            uint16_t type;
            memcpy(&type, b + 8, 2);
            if (type == kEventComplete && b[10] == kKindPixmap) {
                uint64_t ust, msc;
                memcpy(&ust, b + 24, 8);
                memcpy(&msc, b + 36, 8);
                const double ustMs = double(ust) / 1000.0;  // (µs, CLOCK_MONOTONIC: the steady clock's)
                Sink sink;
                Sent s;
                double period = 0.0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    timeline_.add(msc, ustMs);
                    if (sink_ && !sent_.empty()) {
                        s = sent_.front();
                        sent_.pop_front();
                        // (until the grid is learned, the present feedback writes the log)
                        if (timeline_.valid()) {
                            sink = sink_;
                            period = timeline_.periodMs();
                        }
                    }
                }
                if (sink) sink(s, ustMs + period, msc, ustMs, b[11]);
            }
            free(ev);
        }
        if (x.hasError(x.conn)) {
            BDEX_INFO("x11_clock: X connection closed");
            return;
        }
        fds[0].revents = fds[1].revents = 0;
        if (poll(fds, 2, 200) < 0 && errno != EINTR) return;
        if (fds[1].revents) return;
        if (fds[0].revents & (POLLERR | POLLHUP)) return;
    }
}

#else  // not Linux: no X server to ask

struct X11PresentClock::Xcb {};
std::unique_ptr<X11PresentClock> X11PresentClock::start(uint32_t, double) { return nullptr; }
X11PresentClock::~X11PresentClock() = default;
void X11PresentClock::run() {}

#endif

void X11PresentClock::setHint(double hz) {
    std::lock_guard<std::mutex> lock(mutex_);
    timeline_.setHint(hz);
}

bool X11PresentClock::grid(double& lastMs, double& periodMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!timeline_.valid()) return false;
    lastMs = timeline_.lastMs();
    periodMs = timeline_.periodMs();
    return true;
}

double X11PresentClock::refreshHz() {
    std::lock_guard<std::mutex> lock(mutex_);
    return timeline_.refreshHz();
}

X11PresentClock::Counts X11PresentClock::counts() {
    std::lock_guard<std::mutex> lock(mutex_);
    return {timeline_.presents, timeline_.shown, timeline_.replaced, timeline_.held};
}

void X11PresentClock::logTo(Sink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
    sent_.clear();
}

void X11PresentClock::sent(const Sent& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sink_) return;
    if (sent_.size() >= 256) sent_.pop_front();  // (completions lost: keep the newest)
    sent_.push_back(s);
}

void X11PresentClock::unsent() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sent_.empty()) sent_.pop_back();
}

} // namespace bdex
