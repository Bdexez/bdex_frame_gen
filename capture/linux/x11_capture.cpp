#include "x11_capture.h"

#include "log.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace bdex {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

long windowPid(Display* dpy, Window w) {
    const Atom a = XInternAtom(dpy, "_NET_WM_PID", True);
    if (a == None) return 0;
    Atom type;
    int format;
    unsigned long n = 0, after = 0;
    unsigned char* data = nullptr;
    long pid = 0;
    if (XGetWindowProperty(dpy, w, a, 0, 1, False, XA_CARDINAL, &type, &format, &n, &after, &data) == Success &&
        data && n == 1 && format == 32)
        pid = static_cast<long>(*reinterpret_cast<unsigned long*>(data));
    if (data) XFree(data);
    return pid;
}

std::string windowTitle(Display* dpy, Window w) {
    const Atom netName = XInternAtom(dpy, "_NET_WM_NAME", True), utf8 = XInternAtom(dpy, "UTF8_STRING", True);
    std::string title;
    if (netName != None && utf8 != None) {
        Atom type;
        int format;
        unsigned long n = 0, after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(dpy, w, netName, 0, 1024, False, utf8, &type, &format, &n, &after, &data) == Success &&
            data && n > 0)
            title.assign(reinterpret_cast<char*>(data), n);
        if (data) XFree(data);
    }
    if (title.empty()) {
        char* name = nullptr;
        if (XFetchName(dpy, w, &name) && name) {
            title = name;
            XFree(name);
        }
    }
    return title;
}

bool hasWmState(Display* dpy, Window w) {
    const Atom a = XInternAtom(dpy, "WM_STATE", True);
    if (a == None) return false;
    Atom type = None;
    int format;
    unsigned long n = 0, after = 0;
    unsigned char* data = nullptr;
    XGetWindowProperty(dpy, w, a, 0, 0, False, AnyPropertyType, &type, &format, &n, &after, &data);
    if (data) XFree(data);
    return type != None;
}

// The application window under a window manager's frame (XmuClientWindow).
Window clientWindow(Display* dpy, Window w, int depth = 0) {
    if (hasWmState(dpy, w)) return w;
    if (depth > 4) return 0;
    Window root, parent, *children = nullptr;
    unsigned int n = 0;
    if (!XQueryTree(dpy, w, &root, &parent, &children, &n)) return 0;
    Window found = 0;
    for (unsigned int i = 0; i < n && !found; ++i) found = clientWindow(dpy, children[i], depth + 1);
    if (children) XFree(children);
    return found;
}

bool describe(Display* dpy, Window w, X11Window& out) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa) || wa.map_state != IsViewable || wa.c_class == InputOnly ||
        wa.width < 32 || wa.height < 32)
        return false;
    out.id = w;
    out.pid = windowPid(dpy, w);
    out.title = windowTitle(dpy, w);
    out.process = processNameOf(out.pid);
    Window child;
    XTranslateCoordinates(dpy, w, DefaultRootWindow(dpy), 0, 0, &out.x, &out.y, &child);
    out.width = wa.width;
    out.height = wa.height;
    return true;
}

} // namespace

std::string processNameOf(long pid) {
    if (pid <= 0) return "";
    std::ifstream in("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    std::string arg, exe, first;
    while (std::getline(in, arg, '\0')) {
        if (first.empty()) first = arg;
        if (lower(arg).size() > 4 && lower(arg).compare(lower(arg).size() - 4, 4, ".exe") == 0) exe = arg;
    }
    std::string name = exe.empty() ? first : exe;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    name = lower(name);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) name.resize(name.size() - 4);
    return name;
}

std::vector<X11Window> X11Capture::list() {
    std::vector<X11Window> out;
    Window root, parent, *children = nullptr;
    unsigned int n = 0;
    if (!XQueryTree(dpy_, DefaultRootWindow(dpy_), &root, &parent, &children, &n)) return out;
    for (unsigned int i = 0; i < n; ++i) {
        // Under a window manager the application window is inside its frame;
        // without one (Xvfb, gamescope's Xwayland) it is the root's child.
        Window c = clientWindow(dpy_, children[i]);
        if (!c) c = children[i];
        X11Window w;
        if (!describe(dpy_, c, w)) continue;
        if (w.title.empty() && w.pid == 0) continue;
        if (w.title.rfind("bdex-capture", 0) == 0) continue;  // our overlay
        out.push_back(w);
    }
    if (children) XFree(children);
    return out;
}

bool X11Capture::find(X11Window& out, Window id, long pid, const std::string& title) {
    if (id) return describe(dpy_, id, out);
    const std::string want = lower(title);
    for (const auto& w : list()) {
        if (pid && w.pid != pid) continue;
        if (!want.empty() && lower(w.title).find(want) == std::string::npos && w.process != want) continue;
        out = w;
        return true;
    }
    return false;
}

bool X11Capture::pick(X11Window& out) {
    const Window root = DefaultRootWindow(dpy_);
    const Cursor cross = XCreateFontCursor(dpy_, XC_crosshair);
    if (XGrabPointer(dpy_, root, False, ButtonPressMask, GrabModeSync, GrabModeAsync, root, cross, CurrentTime) !=
        GrabSuccess) {
        XFreeCursor(dpy_, cross);
        return false;
    }
    Window picked = 0;
    while (!picked) {
        XAllowEvents(dpy_, SyncPointer, CurrentTime);
        XEvent ev;
        XWindowEvent(dpy_, root, ButtonPressMask, &ev);
        picked = ev.xbutton.subwindow ? ev.xbutton.subwindow : root;
    }
    XUngrabPointer(dpy_, CurrentTime);
    XFreeCursor(dpy_, cross);
    XSync(dpy_, False);
    Window c = clientWindow(dpy_, picked);
    return describe(dpy_, c ? c : picked, out);
}

bool X11Capture::start(const X11Window& w) {
    int major = 0, minor = 0, ev = 0, err = 0;
    if (!XCompositeQueryExtension(dpy_, &ev, &err) || !XCompositeQueryVersion(dpy_, &major, &minor) ||
        (major == 0 && minor < 2)) {
        BDEX_ERR("the X server has no Composite 0.2 extension");
        return false;
    }
    if (!XDamageQueryExtension(dpy_, &damageEvent_, &damageError_)) {
        BDEX_ERR("the X server has no DAMAGE extension");
        return false;
    }
    if (!XShmQueryExtension(dpy_)) {
        BDEX_ERR("the X server has no MIT-SHM extension (a remote display?)");
        return false;
    }
    win_ = w;
    // The window's content kept off-screen, whatever covers it (the overlay).
    XCompositeRedirectWindow(dpy_, win_.id, CompositeRedirectAutomatic);
    XSelectInput(dpy_, win_.id, StructureNotifyMask);
    damage_ = XDamageCreate(dpy_, win_.id, XDamageReportNonEmpty);
    started_ = true;
    if (!namePixmap() || !createSlots()) {
        stop();
        return false;
    }
    damaged_ = true;  // a first frame now
    damagedAt_ = clock::now();
    BDEX_INFO("capturing window 0x%lx \"%s\" (pid %ld, %s), %ux%u", win_.id, win_.title.c_str(), win_.pid,
              win_.process.c_str(), pixW_, pixH_);
    return true;
}

void X11Capture::stop() {
    if (!started_) return;
    destroySlots();
    if (pixmap_) XFreePixmap(dpy_, pixmap_);
    pixmap_ = 0;
    if (damage_ && !gone_) XDamageDestroy(dpy_, damage_);
    damage_ = 0;
    if (!gone_) {
        XCompositeUnredirectWindow(dpy_, win_.id, CompositeRedirectAutomatic);
        XSelectInput(dpy_, win_.id, NoEventMask);
    }
    XSync(dpy_, False);
    started_ = false;
}

bool X11Capture::namePixmap() {
    if (pixmap_) XFreePixmap(dpy_, pixmap_);
    pixmap_ = XCompositeNameWindowPixmap(dpy_, win_.id);
    Window root;
    int x, y;
    unsigned int w = 0, h = 0, border, depth;
    if (!pixmap_ || !XGetGeometry(dpy_, pixmap_, &root, &x, &y, &w, &h, &border, &depth) || w == 0 || h == 0) {
        BDEX_ERR("cannot name the window's pixmap (not mapped?)");
        return false;
    }
    pixW_ = w;
    pixH_ = h;
    return true;
}

bool X11Capture::createSlots() {
    destroySlots();
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy_, win_.id, &wa)) return false;
    const long page = sysconf(_SC_PAGESIZE);
    for (auto& s : slots_) {
        s.image = XShmCreateImage(dpy_, wa.visual, static_cast<unsigned>(wa.depth), ZPixmap, nullptr, &s.info, pixW_,
                                  pixH_);
        if (!s.image || s.image->bits_per_pixel != 32) {
            BDEX_ERR("window depth %d: only 32-bit pixels (24/32-bit depth) are supported", wa.depth);
            return false;
        }
        const size_t bytes = static_cast<size_t>(s.image->bytes_per_line) * pixH_;
        s.size = (bytes + page - 1) / page * page;  // whole pages, for the host-memory import
        s.info.shmid = shmget(IPC_PRIVATE, s.size, IPC_CREAT | 0600);
        if (s.info.shmid < 0) {
            BDEX_ERR("shmget(%zu) failed", s.size);
            return false;
        }
        s.info.shmaddr = s.image->data = static_cast<char*>(shmat(s.info.shmid, nullptr, 0));
        s.info.readOnly = False;
        if (s.info.shmaddr == reinterpret_cast<char*>(-1)) {
            s.info.shmaddr = s.image->data = nullptr;
            BDEX_ERR("shmat failed");
            return false;
        }
        XShmAttach(dpy_, &s.info);
    }
    XSync(dpy_, False);
    for (auto& s : slots_) shmctl(s.info.shmid, IPC_RMID, nullptr);  // freed once both sides detach
    ++generation_;
    return true;
}

void X11Capture::destroySlots() {
    bool any = false;
    for (auto& s : slots_) {
        if (s.info.shmaddr) {
            XShmDetach(dpy_, &s.info);
            any = true;
        }
    }
    if (any) XSync(dpy_, False);
    for (auto& s : slots_) {
        if (s.info.shmaddr) shmdt(s.info.shmaddr);
        if (s.image) {
            s.image->data = nullptr;
            XDestroyImage(s.image);
        }
        s = Slot{};
    }
}

void X11Capture::updateGeometry() {
    X11Window w;
    if (!describe(dpy_, win_.id, w)) return;
    if (w.x != win_.x || w.y != win_.y || w.width != win_.width || w.height != win_.height) {
        win_.x = w.x;
        win_.y = w.y;
        win_.width = w.width;
        win_.height = w.height;
        moved_ = true;
    }
}

bool X11Capture::pump(int timeoutMs) {
    // (A frame already noticed — while a predicted frame waited — is not
    // waited for again: at 60 fps the 20 ms poll dropped every other frame.)
    if (!damaged_ && !XPending(dpy_) && timeoutMs > 0) {
        pollfd p{ConnectionNumber(dpy_), POLLIN, 0};
        poll(&p, 1, timeoutMs);
    }
    bool resized = false;
    while (XPending(dpy_)) {
        XEvent ev;
        XNextEvent(dpy_, &ev);
        if (ev.type == damageEvent_ + XDamageNotify) {
            const auto now = clock::now();
            if (!damaged_) {
                if (lastDamage_.time_since_epoch().count() != 0) {
                    const double dt = std::chrono::duration<double>(now - lastDamage_).count();
                    if (dt > 0.0005 && dt < 1.0) fps_ = fps_ > 0.0 ? fps_ * 0.9 + 0.1 / dt : 1.0 / dt;
                }
                lastDamage_ = now;
                damagedAt_ = now;
            }
            damaged_ = true;
        } else if (ev.type == ConfigureNotify && ev.xconfigure.window == win_.id) {
            if (static_cast<uint32_t>(ev.xconfigure.width) != pixW_ || static_cast<uint32_t>(ev.xconfigure.height) != pixH_)
                resized = true;
            updateGeometry();
        } else if (ev.type == DestroyNotify && ev.xdestroywindow.window == win_.id) {
            gone_ = true;
        } else if (ev.type == UnmapNotify && ev.xunmap.window == win_.id) {
            mapped_ = false;
        } else if (ev.type == MapNotify && ev.xmap.window == win_.id) {
            mapped_ = true;
            resized = true;  // a mapped redirected window gets a new pixmap
        }
    }
    if (resized && !gone_ && mapped_) {
        if (namePixmap()) createSlots();
        damaged_ = true;
    }
    // Moves of the window manager's frame reach the client only as synthetic
    // events, if at all: the main loop also calls this every 100 ms.
    if (!gone_) updateGeometry();
    return damaged_;
}

bool X11Capture::grab(int slot, Frame& out) {
    if (!started_ || gone_ || slot < 0 || slot >= kSlots || !slots_[slot].image) return false;
    // Subtract first: damage that happens during the copy triggers the next grab.
    XDamageSubtract(dpy_, damage_, None, None);
    const auto at = damagedAt_;
    damaged_ = false;
    if (!XShmGetImage(dpy_, pixmap_, slots_[slot].image, 0, 0, AllPlanes)) return false;
    out.slot = slot;
    out.data = reinterpret_cast<const uint8_t*>(slots_[slot].image->data);
    out.width = pixW_;
    out.height = pixH_;
    out.stride = static_cast<uint32_t>(slots_[slot].image->bytes_per_line);
    out.generation = generation_;
    out.damagedAt = at;
    return true;
}

} // namespace bdex
