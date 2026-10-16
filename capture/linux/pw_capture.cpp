#include "pw_capture.h"

#include "log.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#include <X11/Xlib.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

#include <unistd.h>

namespace bdex {

namespace {

// ------------------------------------------------------------- portal ----

std::string cacheDir() {
    const char* x = std::getenv("XDG_CACHE_HOME");
    const char* h = std::getenv("HOME");
    return (x && *x) ? std::string(x) + "/bdex-framegen" : h ? std::string(h) + "/.cache/bdex-framegen" : std::string();
}

struct Response {
    GMainLoop* loop = nullptr;
    guint32 code = 2;
    GVariant* results = nullptr;
    bool done = false;
};

void onResponse(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* params, gpointer user) {
    auto* r = static_cast<Response*>(user);
    g_variant_get(params, "(u@a{sv})", &r->code, &r->results);
    r->done = true;
    g_main_loop_quit(r->loop);
}

gboolean onTimeout(gpointer user) {
    g_main_loop_quit(static_cast<Response*>(user)->loop);
    return G_SOURCE_REMOVE;
}

// A portal request: the method called with its handle_token, then its
// Response signal waited for (the user may take a while in the picker).
// The results (a{sv}, to unref) or null; `code` 0 = success, 1 = cancelled.
GVariant* request(GDBusConnection* bus, const char* method, GVariant* args, const std::string& token, int timeoutS,
                  guint32& code) {
    std::string sender = g_dbus_connection_get_unique_name(bus) + 1;  // (no ':')
    std::replace(sender.begin(), sender.end(), '.', '_');
    const std::string path = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
    Response r;
    GMainContext* ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    r.loop = g_main_loop_new(ctx, FALSE);
    const guint sub = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request",
                                                         "Response", path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                         onResponse, &r, nullptr);
    GError* err = nullptr;
    GVariant* ret = g_dbus_connection_call_sync(bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                                "org.freedesktop.portal.ScreenCast", method, args, nullptr,
                                                G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &err);
    if (!ret) {
        BDEX_ERR("portal %s: %s", method, err ? err->message : "failed");
        if (err) g_error_free(err);
    } else {
        g_variant_unref(ret);
        GSource* t = g_timeout_source_new_seconds(static_cast<guint>(timeoutS));
        g_source_set_callback(t, onTimeout, &r, nullptr);
        g_source_attach(t, ctx);
        if (!r.done) g_main_loop_run(r.loop);
        g_source_destroy(t);
        g_source_unref(t);
        if (!r.done) BDEX_ERR("portal %s: no answer", method);
    }
    g_dbus_connection_signal_unsubscribe(bus, sub);
    g_main_loop_unref(r.loop);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    code = r.code;
    if (r.done && r.code != 0 && r.results) {
        g_variant_unref(r.results);
        r.results = nullptr;
    }
    return r.results;
}

} // namespace

bool PwCapture::portal(bool monitor, int& fd, uint32_t& node) {
    GError* err = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!bus) {
        BDEX_ERR("no D-Bus session bus (%s): the screen-cast portal is unreachable", err ? err->message : "?");
        if (err) g_error_free(err);
        return false;
    }
    static int counter = 0;
    auto token = [&] { return "bdex" + std::to_string(getpid()) + "_" + std::to_string(++counter); };
    guint32 code = 2;
    bool ok = false;
    std::string session;

    // CreateSession
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    std::string t = token();
    g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(t.c_str()));
    g_variant_builder_add(&opts, "{sv}", "session_handle_token", g_variant_new_string(token().c_str()));
    if (GVariant* res = request(bus, "CreateSession", g_variant_new("(a{sv})", &opts), t, 10, code)) {
        const gchar* s = nullptr;
        if (g_variant_lookup(res, "session_handle", "&s", &s) && s) session = s;
        g_variant_unref(res);
    }
    if (session.empty()) {
        BDEX_ERR("screen-cast portal: no session (is xdg-desktop-portal running with a backend for this desktop?)");
        g_object_unref(bus);
        return false;
    }

    // SelectSources: a window (or a screen), the cursor left out (the
    // compositor draws the real one over the overlay), the choice kept.
    const std::string tokenFile = cacheDir() + (monitor ? "/portal-token-monitor" : "/portal-token-window");
    std::string restore;
    {
        std::ifstream in(tokenFile);
        std::getline(in, restore);
    }
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    t = token();
    g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(t.c_str()));
    g_variant_builder_add(&opts, "{sv}", "types", g_variant_new_uint32(monitor ? 1u : 2u));
    g_variant_builder_add(&opts, "{sv}", "multiple", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&opts, "{sv}", "cursor_mode", g_variant_new_uint32(1u));
    g_variant_builder_add(&opts, "{sv}", "persist_mode", g_variant_new_uint32(2u));
    if (!restore.empty()) g_variant_builder_add(&opts, "{sv}", "restore_token", g_variant_new_string(restore.c_str()));
    GVariant* res = request(bus, "SelectSources", g_variant_new("(oa{sv})", session.c_str(), &opts), t, 10, code);
    if (!res) {
        BDEX_ERR("screen-cast portal: source selection refused (code %u)", code);
    } else {
        g_variant_unref(res);
        // Start: the compositor's picker (or the kept choice).
        g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
        t = token();
        g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(t.c_str()));
        BDEX_INFO("screen-cast portal: choose the game's %s in the desktop's dialog", monitor ? "screen" : "window");
        res = request(bus, "Start", g_variant_new("(osa{sv})", session.c_str(), "", &opts), t, 180, code);
        if (!res) {
            BDEX_ERR("screen-cast portal: %s", code == 1 ? "cancelled" : "start failed");
        } else {
            GVariant* streams = g_variant_lookup_value(res, "streams", G_VARIANT_TYPE("a(ua{sv})"));
            if (streams && g_variant_n_children(streams) > 0) {
                GVariant* props = nullptr;
                g_variant_get_child(streams, 0, "(u@a{sv})", &node, &props);
                if (props) {
                    gint32 x = 0, y = 0, w = 0, h = 0;
                    if (g_variant_lookup(props, "position", "(ii)", &x, &y) && monitor) {
                        BDEX_INFO("screen-cast portal: the shared screen is at %d,%d", x, y);
                    }
                    if (g_variant_lookup(props, "size", "(ii)", &w, &h)) source_ = std::to_string(w) + "x" + std::to_string(h);
                    g_variant_unref(props);
                }
                ok = true;
            }
            if (streams) g_variant_unref(streams);
            const gchar* tok = nullptr;
            if (g_variant_lookup(res, "restore_token", "&s", &tok) && tok) {
                std::ofstream out(tokenFile, std::ios::trunc);
                out << tok << "\n";
            }
            g_variant_unref(res);
        }
    }
    if (ok) {
        // OpenPipeWireRemote: the PipeWire connection, as a file descriptor.
        g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
        GUnixFDList* fds = nullptr;
        GVariant* ret = g_dbus_connection_call_with_unix_fd_list_sync(
            bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.ScreenCast",
            "OpenPipeWireRemote", g_variant_new("(oa{sv})", session.c_str(), &opts), G_VARIANT_TYPE("(h)"),
            G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &fds, nullptr, &err);
        ok = false;
        if (ret && fds) {
            gint32 index = 0;
            g_variant_get(ret, "(h)", &index);
            fd = g_unix_fd_list_get(fds, index, &err);
            ok = fd >= 0;
        }
        if (!ok) BDEX_ERR("screen-cast portal: no PipeWire connection (%s)", err ? err->message : "?");
        if (err) g_error_free(err);
        if (ret) g_variant_unref(ret);
        if (fds) g_object_unref(fds);
    }
    // (The session stays open while this process runs: closing the bus
    // connection would end the sharing.)
    if (!ok) g_object_unref(bus);
    return ok;
}

// ----------------------------------------------------------- pipewire ----

void PwCapture::onStateChanged(void* self, int, int state, const char* error) {
    auto* c = static_cast<PwCapture*>(self);
    BDEX_DBG("PipeWire stream state: %s", pw_stream_state_as_string(static_cast<pw_stream_state>(state)));
    if ((state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) && !c->stopping_) {
        if (state == PW_STREAM_STATE_ERROR) BDEX_ERR("PipeWire stream: %s", error ? error : "error");
        else BDEX_INFO("PipeWire stream: the shared window is gone");
        c->gone_ = true;
    }
}

void PwCapture::onParamChanged(void* self, uint32_t id, const struct spa_pod* param) {
    auto* c = static_cast<PwCapture*>(self);
    if (!param || id != SPA_PARAM_Format) return;
    spa_video_info_raw info{};
    if (spa_format_video_raw_parse(param, &info) < 0) return;
    {
        std::lock_guard<std::mutex> lock(c->mutex_);
        c->width_ = info.size.width;
        c->height_ = info.size.height;
        c->swapRB_ = info.format == SPA_VIDEO_FORMAT_RGBx || info.format == SPA_VIDEO_FORMAT_RGBA;
    }
    BDEX_INFO("PipeWire stream: %ux%u, %s", info.size.width, info.size.height,
              info.format == SPA_VIDEO_FORMAT_BGRx || info.format == SPA_VIDEO_FORMAT_BGRA ? "BGRx" : "RGBx (swizzled)");
    // Mapped memory (no DMA-BUF: the frames are copied on the CPU), and the
    // crop region of a shared window.
    uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const spa_pod* params[2];
    params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd))));
    params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_region))));
    pw_stream_update_params(c->stream_, params, 2);
}

void PwCapture::onProcess(void* self) {
    auto* c = static_cast<PwCapture*>(self);
    pw_buffer* b = pw_stream_dequeue_buffer(c->stream_);
    if (!b) return;
    spa_buffer* sb = b->buffer;
    const spa_data& d = sb->datas[0];
    if (c->seq_ == 0)
        BDEX_DBG("PipeWire: first buffer (type %u, data %p, size %u, stride %d)", d.type, d.data, d.chunk ? d.chunk->size : 0,
                 d.chunk ? d.chunk->stride : 0);
    if (d.data && d.chunk && d.chunk->size) {
        std::lock_guard<std::mutex> lock(c->mutex_);
        uint32_t x0 = 0, y0 = 0, w = c->width_, h = c->height_;
        if (auto* crop = static_cast<spa_meta_region*>(spa_buffer_find_meta_data(sb, SPA_META_VideoCrop, sizeof(spa_meta_region))))
            if (crop->region.size.width && crop->region.size.height &&
                uint32_t(crop->region.position.x) + crop->region.size.width <= c->width_ &&
                uint32_t(crop->region.position.y) + crop->region.size.height <= c->height_) {
                x0 = uint32_t(crop->region.position.x);
                y0 = uint32_t(crop->region.position.y);
                w = crop->region.size.width;
                h = crop->region.size.height;
            }
        const uint32_t srcStride = d.chunk->stride > 0 ? uint32_t(d.chunk->stride) : c->width_ * 4;
        const uint8_t* src = static_cast<const uint8_t*>(d.data) + d.chunk->offset;
        if (size_t(srcStride) * (y0 + h) <= d.maxsize && w && h) {
            c->latest_.resize(size_t(w) * h * 4);
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t* s = src + size_t(y0 + y) * srcStride + size_t(x0) * 4;
                uint8_t* o = c->latest_.data() + size_t(y) * w * 4;
                if (!c->swapRB_) {
                    std::memcpy(o, s, size_t(w) * 4);
                } else {
                    for (uint32_t x = 0; x < w; ++x) {
                        o[x * 4] = s[x * 4 + 2];
                        o[x * 4 + 1] = s[x * 4 + 1];
                        o[x * 4 + 2] = s[x * 4];
                        o[x * 4 + 3] = 255;
                    }
                }
            }
            c->stride_ = w * 4;
            c->frameW_ = w;
            c->frameH_ = h;
            ++c->seq_;
            c->latestAt_ = clock::now();
        }
    }
    pw_stream_queue_buffer(c->stream_, b);
}

bool PwCapture::connect(int fd, uint32_t node) {
    pw_init(nullptr, nullptr);
    loop_ = pw_thread_loop_new("bdex-capture", nullptr);
    if (!loop_) return false;
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_ || pw_thread_loop_start(loop_) < 0) return false;
    pw_thread_loop_lock(loop_);
    core_ = pw_context_connect_fd(context_, fd, nullptr, 0);
    if (!core_) {
        pw_thread_loop_unlock(loop_);
        BDEX_ERR("PipeWire: cannot connect through the portal");
        return false;
    }
    stream_ = pw_stream_new(core_, "bdex-capture",
                            pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                              PW_KEY_MEDIA_ROLE, "Screen", nullptr));
    static pw_stream_events events = [] {
        pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.state_changed = [](void* s, enum pw_stream_state o, enum pw_stream_state n, const char* err) {
            onStateChanged(s, int(o), int(n), err);
        };
        e.param_changed = onParamChanged;
        e.process = onProcess;
        return e;
    }();
    auto* hook = new spa_hook{};
    listener_ = hook;
    pw_stream_add_listener(stream_, hook, &events, this);
    uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    spa_rectangle defSize = SPA_RECTANGLE(1920, 1080), minSize = SPA_RECTANGLE(1, 1), maxSize = SPA_RECTANGLE(8192, 8192);
    // (Producers pace themselves on maxFramerate — xdg-desktop-portal-wlr
    // sends nothing at 0/0 —: as fast as the screen, up to 360.)
    spa_fraction defRate = SPA_FRACTION(60, 1), minRate = SPA_FRACTION(0, 1), maxRate = SPA_FRACTION(360, 1);
    spa_fraction maxDef = SPA_FRACTION(360, 1), maxMin = SPA_FRACTION(1, 1);
    const spa_pod* params[1];
    params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx,
                               SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&defSize, &minSize, &maxSize), SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(&defRate, &minRate, &maxRate), SPA_FORMAT_VIDEO_maxFramerate,
        SPA_POD_CHOICE_RANGE_Fraction(&maxDef, &maxMin, &maxRate)));
    const int r = pw_stream_connect(stream_, PW_DIRECTION_INPUT, node,
                                    static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                                    params, 1);
    pw_thread_loop_unlock(loop_);
    if (r < 0) BDEX_ERR("PipeWire: stream connection failed (%d)", r);
    return r >= 0;
}

bool PwCapture::start(bool monitor, const X11Window& area) {
    area_ = area;
    int fd = -1;
    uint32_t node = 0;
    if (!portal(monitor, fd, node)) return false;
    BDEX_INFO("screen-cast portal: PipeWire node %u%s", node, source_.empty() ? "" : (" (" + source_ + ")").c_str());
    return connect(fd, node);
}

void PwCapture::stop() {
    stopping_ = true;
    if (loop_) pw_thread_loop_stop(loop_);
    if (stream_) pw_stream_destroy(stream_);
    if (core_) pw_core_disconnect(core_);
    if (context_) pw_context_destroy(context_);
    if (loop_) pw_thread_loop_destroy(loop_);
    delete static_cast<spa_hook*>(listener_);
    stream_ = nullptr;
    core_ = nullptr;
    context_ = nullptr;
    loop_ = nullptr;
    listener_ = nullptr;
    for (auto& s : slots_) {
        std::free(s.data);
        s = Slot{};
    }
}

bool PwCapture::pump(int timeoutMs) {
    const auto until = clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            damaged_ = seq_ != seqGrabbed_;
        }
        if (damaged_ || gone_ || clock::now() >= until) return damaged_;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

bool PwCapture::grab(int slot, Frame& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (latest_.empty() || slot < 0 || slot >= kSlots) return false;
    if (frameW_ != slotW_ || frameH_ != slotH_) {
        // A new size: a new generation of page-aligned frames.
        const size_t page = 4096, bytes = (size_t(frameW_) * frameH_ * 4 + page - 1) / page * page;
        for (auto& s : slots_) {
            std::free(s.data);
            s.data = static_cast<uint8_t*>(std::aligned_alloc(page, bytes));
            s.size = bytes;
        }
        slotW_ = frameW_;
        slotH_ = frameH_;
        ++generation_;
    }
    Slot& s = slots_[slot];
    if (!s.data) return false;
    std::memcpy(s.data, latest_.data(), latest_.size());
    out.slot = slot;
    out.data = s.data;
    out.width = frameW_;
    out.height = frameH_;
    out.stride = frameW_ * 4;
    out.generation = generation_;
    out.damagedAt = latestAt_;
    const auto now = clock::now();
    if (lastFrame_.time_since_epoch().count()) {
        const double dt = std::chrono::duration<double>(now - lastFrame_).count();
        if (dt > 0.0) fps_ = fps_ > 0.0 ? fps_ * 0.9 + 0.1 / dt : 1.0 / dt;
    }
    lastFrame_ = now;
    seqGrabbed_ = seq_;
    damaged_ = false;
    return true;
}

} // namespace bdex
