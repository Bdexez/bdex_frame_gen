// bdex-x11-trace — what the X server says of a game's presents, and what the
// layer's refresh-grid estimator makes of it.
//
//   bdex-x11-trace watch SECONDS [> trace.txt]
//       Selects the Present extension's completion events on every window of
//       $DISPLAY and prints one line per event: "window msc ust_us mode"
//       (mode: 0 copy, 1 flip, 2 skip). Read-only: a connection of its own,
//       no window, nothing sent to the game — the same events x11_clock
//       reads in the layer. Run it while the game is on screen.
//   bdex-x11-trace replay trace.txt [HINT_HZ]
//       Feeds the trace of the window with the most events to the layer's
//       PresentTimeline (x11_present_clock.h), HINT_HZ being the refresh rate
//       the system reports (the panel's reading; 0 or absent: none), and
//       prints the grid it holds at each estimate, with the intervals in
//       units of the hint's period — whether the presents completed on the
//       screen's refresh grid or at their own times.
//
// Why: under Xwayland the completions of a game that presents no faster than
// the screen refreshes are not the screen's refreshes (docs/MEMOIRE.md,
// 10/11 and 11/11), and each family of games behaves in its own way. A trace
// taken on the player's machine replays here, and in the unit tests
// (tests/data/x11_present_*.txt).
#include "x11_present_clock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#ifdef BDEX_HAVE_XCB_PRESENT
#include <poll.h>
#include <time.h>
#include <xcb/present.h>
#include <xcb/xcb.h>
#endif

namespace {

#ifdef BDEX_HAVE_XCB_PRESENT
void collectWindows(xcb_connection_t* c, xcb_window_t w, std::vector<xcb_window_t>& out) {
    if (out.size() >= 8192) return;
    out.push_back(w);
    xcb_query_tree_reply_t* r = xcb_query_tree_reply(c, xcb_query_tree(c, w), nullptr);
    if (!r) return;
    const xcb_window_t* children = xcb_query_tree_children(r);
    for (int i = 0, n = xcb_query_tree_children_length(r); i < n; ++i) collectWindows(c, children[i], out);
    free(r);
}

double nowSeconds() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
}

int watch(double seconds) {
    xcb_connection_t* c = xcb_connect(nullptr, nullptr);
    if (xcb_connection_has_error(c)) { fprintf(stderr, "bdex-x11-trace: no X server on $DISPLAY\n"); return 1; }
    const xcb_query_extension_reply_t* ext = xcb_get_extension_data(c, &xcb_present_id);
    if (!ext || !ext->present) { fprintf(stderr, "bdex-x11-trace: the X server has no Present extension\n"); return 1; }
    std::vector<xcb_window_t> windows;
    collectWindows(c, xcb_setup_roots_iterator(xcb_get_setup(c)).data->root, windows);
    for (xcb_window_t w : windows)
        xcb_present_select_input(c, xcb_generate_id(c), w, XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);
    xcb_flush(c);
    fprintf(stderr, "bdex-x11-trace: watching %zu windows for %.0f s\n", windows.size(), seconds);
    const double end = nowSeconds() + seconds;
    pollfd pfd{xcb_get_file_descriptor(c), POLLIN, 0};
    unsigned long long events = 0;
    while (nowSeconds() < end) {
        poll(&pfd, 1, 100);
        while (xcb_generic_event_t* e = xcb_poll_for_event(c)) {
            if ((e->response_type & 0x7f) == XCB_GE_GENERIC) {
                const auto* ge = reinterpret_cast<const xcb_ge_generic_event_t*>(e);
                if (ge->extension == ext->major_opcode && ge->event_type == XCB_PRESENT_COMPLETE_NOTIFY) {
                    const auto* done = reinterpret_cast<const xcb_present_complete_notify_event_t*>(e);
                    printf("%u %llu %llu %u\n", done->window, static_cast<unsigned long long>(done->msc),
                           static_cast<unsigned long long>(done->ust), done->mode);
                    ++events;
                }
            }
            free(e);
        }
    }
    xcb_disconnect(c);
    fprintf(stderr, "bdex-x11-trace: %llu completion(s)\n", events);
    return 0;
}
#else
int watch(double) {
    fprintf(stderr, "bdex-x11-trace: built without libxcb-present (watch unavailable; replay works)\n");
    return 1;
}
#endif

struct Event { unsigned long long msc, ust; };

int replay(const char* path, double hintHz) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "bdex-x11-trace: cannot read %s\n", path); return 1; }
    std::map<unsigned, std::vector<Event>> byWindow;
    unsigned window = 0, mode = 0;
    Event e{};
    while (fscanf(f, "%u %llu %llu %u", &window, &e.msc, &e.ust, &mode) == 4) byWindow[window].push_back(e);
    fclose(f);
    if (byWindow.empty()) { fprintf(stderr, "bdex-x11-trace: no event in %s\n", path); return 1; }
    const auto most = std::max_element(byWindow.begin(), byWindow.end(),
                                       [](const auto& a, const auto& b) { return a.second.size() < b.second.size(); });
    const std::vector<Event>& events = most->second;
    const double seconds = double(events.back().ust - events.front().ust) * 1e-6;
    printf("window %u: %zu completions in %.1f s (%.1f a second)%s\n", most->first, events.size(), seconds,
           seconds > 0.0 ? double(events.size() - 1) / seconds : 0.0, byWindow.size() > 1 ? " — the busiest window" : "");

    bdex::PresentTimeline timeline;
    timeline.setHint(hintHz);
    std::map<int, int> grids;  // Hz (rounded) -> estimates that held it
    int estimates = 0, none = 0;
    std::map<int, int> spans;  // interval in tenths of the hint's period -> count
    size_t n = 0, sameMsc = 0;
    for (size_t i = 0; i < events.size(); ++i) {
        if (i > 0) {
            if (events[i].msc == events[i - 1].msc) ++sameMsc;
            const double ms = double(events[i].ust - events[i - 1].ust) * 1e-3;
            if (hintHz > 0.0 && events[i].ust != events[i - 1].ust) ++spans[int(std::lround(ms * hintHz / 100.0))];
        }
        timeline.add(events[i].msc, double(events[i].ust) * 1e-3);
        if (++n % 16 == 0) {
            ++estimates;
            if (timeline.valid()) ++grids[int(std::lround(timeline.refreshHz()))];
            else ++none;
        }
    }
    printf("presents sharing the previous one's MSC: %zu of %zu\n", sameMsc, events.size());
    if (hintHz > 0.0) {
        printf("intervals in periods of %.3f Hz (whole numbers: on the screen's grid):\n", hintHz);
        for (const auto& [tenths, count] : spans)
            if (count * 200 >= int(events.size())) printf("  %4.1f  %d\n", tenths / 10.0, count);
    }
    printf("grid held by the layer's estimator (hint %.3f Hz), %d estimates: none x%d", hintHz, estimates, none);
    for (const auto& [hz, count] : grids) printf(", %d Hz x%d", hz, count);
    printf("\nshown %llu, replaced %llu, refreshes held %llu\n", static_cast<unsigned long long>(timeline.shown),
           static_cast<unsigned long long>(timeline.replaced), static_cast<unsigned long long>(timeline.held));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[1], "watch") == 0) return watch(atof(argv[2]));
    if (argc >= 3 && strcmp(argv[1], "replay") == 0) return replay(argv[2], argc >= 4 ? atof(argv[3]) : 0.0);
    fprintf(stderr, "usage: bdex-x11-trace watch SECONDS > trace.txt\n"
                    "       bdex-x11-trace replay trace.txt [HINT_HZ]\n");
    return 2;
}
