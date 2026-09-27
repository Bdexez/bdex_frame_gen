#include "status.h"
#include "test.h"

#include <string>

using namespace bdex;

TEST(status_format_lists_every_entry) {
    std::map<uint64_t, StatusEntry> m;
    StatusEntry dev;
    dev.kind = "device";
    dev.state = "active";
    dev.device = "Some GPU";
    StatusEntry sc;
    sc.kind = "swapchain";
    sc.state = "pass-through";
    sc.reason = "swapchain format 64 not supported";
    sc.width = 800;
    sc.height = 600;
    m[1] = dev;
    m[2] = sc;
    const std::string s = statusFormat("game", 42, 1000, m);
    CHECK(s.find("process=game\npid=42\ntime=1000\n") == 0);
    CHECK(s.find("[device 1]\nstate=active\ndevice=Some GPU\n") != std::string::npos);
    CHECK(s.find("[swapchain 2]\nstate=pass-through\nreason=swapchain format 64 not supported\n") != std::string::npos);
    CHECK(s.find("size=800x600\n") != std::string::npos);
    CHECK(s.find("latency_ms=-1.0\n") != std::string::npos);
}

TEST(status_key_accepts_pointers_and_integers) {
    int x = 0;
    CHECK(statusKey(&x) != 0);
    CHECK(statusKey(uint64_t(7)) == 7);
}
