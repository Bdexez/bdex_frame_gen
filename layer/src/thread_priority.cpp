#include "thread_priority.h"

#if defined(__linux__)
#include <cstdint>
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace bdex {

#if defined(__linux__)
namespace {

// The few libdbus entry points needed, by their C signatures (opaque
// pointers; DBusError is a small struct, given room to spare).
struct DBusErrorBuf { const char* name; const char* message; unsigned int bits; void* pad; char spare[64]; };
using ErrorInit = void (*)(DBusErrorBuf*);
using ErrorFree = void (*)(DBusErrorBuf*);
using ErrorIsSet = unsigned int (*)(const DBusErrorBuf*);
using BusGetPrivate = void* (*)(int, DBusErrorBuf*);
using NewMethodCall = void* (*)(const char*, const char*, const char*, const char*);
using AppendArgs = unsigned int (*)(void*, int, ...);
using SendBlock = void* (*)(void*, void*, int, DBusErrorBuf*);
using Unref = void (*)(void*);
using Close = void (*)(void*);

constexpr int kBusSystem = 1;  // DBUS_BUS_SYSTEM
constexpr int kTypeUint64 = 't', kTypeInt32 = 'i', kTypeInvalid = 0;

bool rtkitHighPriority(uint64_t tid, int32_t nice) {
    void* lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return false;
    auto sym = [&](const char* n) { return dlsym(lib, n); };
    auto errorInit = reinterpret_cast<ErrorInit>(sym("dbus_error_init"));
    auto errorFree = reinterpret_cast<ErrorFree>(sym("dbus_error_free"));
    auto errorIsSet = reinterpret_cast<ErrorIsSet>(sym("dbus_error_is_set"));
    auto busGet = reinterpret_cast<BusGetPrivate>(sym("dbus_bus_get_private"));
    auto newCall = reinterpret_cast<NewMethodCall>(sym("dbus_message_new_method_call"));
    auto append = reinterpret_cast<AppendArgs>(sym("dbus_message_append_args"));
    auto send = reinterpret_cast<SendBlock>(sym("dbus_connection_send_with_reply_and_block"));
    auto msgUnref = reinterpret_cast<Unref>(sym("dbus_message_unref"));
    auto connClose = reinterpret_cast<Close>(sym("dbus_connection_close"));
    auto connUnref = reinterpret_cast<Unref>(sym("dbus_connection_unref"));
    bool ok = false;
    if (errorInit && errorFree && errorIsSet && busGet && newCall && append && send && msgUnref && connClose && connUnref) {
        DBusErrorBuf err;
        errorInit(&err);
        // (a private connection: the game's own use of the system bus is
        // left alone, and it is closed again at once)
        if (void* conn = busGet(kBusSystem, &err)) {
            if (void* msg = newCall("org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1",
                                    "org.freedesktop.RealtimeKit1", "MakeThreadHighPriority")) {
                if (append(msg, kTypeUint64, &tid, kTypeInt32, &nice, kTypeInvalid)) {
                    if (void* reply = send(conn, msg, 200, &err)) {
                        ok = true;
                        msgUnref(reply);
                    }
                }
                msgUnref(msg);
            }
            connClose(conn);
            connUnref(conn);
        }
        if (errorIsSet(&err)) errorFree(&err);
    }
    dlclose(lib);
    return ok;
}

} // namespace

std::string raiseThreadPriority(int niceLevel) {
    sched_param sp{};
    sp.sched_priority = 1;  // the lowest real-time priority: above every normal thread, below audio
    if (pthread_setschedparam(pthread_self(), SCHED_RR, &sp) == 0) return "SCHED_RR";
    const uint64_t tid = static_cast<uint64_t>(syscall(SYS_gettid));
    if (rtkitHighPriority(tid, niceLevel)) return "rtkit nice " + std::to_string(niceLevel);
    return {};
}

#else

std::string raiseThreadPriority(int) { return {}; }

#endif

} // namespace bdex
