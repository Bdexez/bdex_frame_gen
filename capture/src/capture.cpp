#include "capture.h"
#include "log.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <unknwn.h>
#include <inspectable.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace bdex {
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wgd11 = winrt::Windows::Graphics::DirectX::Direct3D11;

constexpr uint32_t kRingSize = Capture::kRingSize;

// Narrow an hresult error message for the char-based log.
static std::string narrow(wchar_t const* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

struct Slot {
    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::com_ptr<IDXGIKeyedMutex> mutex;
    HANDLE shared = nullptr;  // NT handle, closed with the slot
    uint32_t key = 0;         // key the D3D side must acquire with (see capture.h)
    bool busy = false;        // true while the main thread owns the slot

    void close() {
        if (shared) CloseHandle(shared);
        shared = nullptr;
        mutex = nullptr;
        tex = nullptr;
    }
};

// A replaced ring is kept here until the Vulkan side has freed the imports
// that reference its textures — destroying the D3D11 texture underneath an
// imported VkDeviceMemory would be use-after-free on the GPU.
struct Grave {
    uint32_t generation = 0;
    std::vector<Slot> slots;
};

struct Capture::Impl {
    HWND target = nullptr;

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    wgd11::IDirect3DDevice winrtDevice{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker arrived;
    winrt::Windows::Graphics::SizeInt32 poolSize{};

    std::vector<Slot> ring;
    std::vector<Grave> graves;
    uint32_t contentW = 0, contentH = 0;
    uint32_t nextSlot = 0;
    uint32_t generation = 0;

    HANDLE frameEvent = nullptr;  // set by the FrameArrived delegate
    HANDLE dataEvent = nullptr;   // a frame is queued for the main thread
    HANDLE quitEvent = nullptr;   // manual-reset
    HANDLE thread = nullptr;

    std::mutex mtx;
    std::vector<Frame> pending;

    // Statistics: frames copied into the ring, sampled once a second.
    std::atomic<uint32_t> captured{0};
    std::atomic<float> fps{0.0f};
    ULONGLONG statsT0 = 0;

    static DWORD WINAPI threadMain(void* p);

    // Precondition: mtx is held (or the thread is not running yet). The old
    // textures may still be imported by the Vulkan side: move them to the
    // graveyard instead of releasing them. Main calls releaseGraveyard() once
    // it has freed the matching imports.
    void recreateRing(int w, int h) {
        if (!ring.empty()) graves.push_back({generation, std::move(ring)});
        ring.clear();
        pending.clear();
        contentW = static_cast<uint32_t>(w);
        contentH = static_cast<uint32_t>(h);
        ++generation;

        for (uint32_t i = 0; i < kRingSize; ++i) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = contentW;
            desc.Height = contentH;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc = {1, 0};
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            // NT handle (CreateSharedHandle needs SHARED_NTHANDLE), so the
            // Vulkan side imports with VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT.
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                             D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            Slot s;
            winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, s.tex.put()));
            s.mutex = s.tex.as<IDXGIKeyedMutex>();
            auto res = s.tex.as<IDXGIResource1>();
            winrt::check_hresult(res->CreateSharedHandle(
                nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                &s.shared));
            ring.push_back(std::move(s));
        }
        LOGI("capture ring %ux%u (generation %u)", contentW, contentH, generation);
    }

    // Returns true when the frame pool must be recreated at `poolSize`
    // (content grew past the pool's surfaces).
    bool onFrame(wgc::Direct3D11CaptureFrame const& frame) {
        auto size = frame.ContentSize();
        if (size.Width <= 0 || size.Height <= 0) return false;
        bool growPool = size.Width > poolSize.Width || size.Height > poolSize.Height;

        std::lock_guard lk(mtx);
        if (static_cast<uint32_t>(size.Width) != contentW ||
            static_cast<uint32_t>(size.Height) != contentH)
            recreateRing(size.Width, size.Height);

        // First slot that is not owned by the main thread and whose mutex the
        // GPU has released (non-blocking: if Vulkan is still reading, skip).
        uint32_t slot = kRingSize;
        for (uint32_t i = 0; i < kRingSize; ++i) {
            uint32_t s = (nextSlot + i) % kRingSize;
            if (ring[s].busy) continue;
            HRESULT hr = ring[s].mutex->AcquireSync(ring[s].key, 0);
            if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) continue;
            winrt::check_hresult(hr);
            slot = s;
            break;
        }
        if (slot == kRingSize) return growPool;  // main is behind: drop the frame

        Slot& sl = ring[slot];
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::
                                             IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> src;
        winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(),
                                                  src.put_void()));
        // The pool surface may be larger than the content (or, for the first
        // frame after a resize, smaller): copy the intersection.
        D3D11_TEXTURE2D_DESC sd;
        src->GetDesc(&sd);
        D3D11_BOX box{0, 0, 0,
                      std::min(static_cast<UINT>(size.Width), sd.Width),
                      std::min(static_cast<UINT>(size.Height), sd.Height), 1};
        context->CopySubresourceRegion(sl.tex.get(), 0, 0, 0, 0, src.get(), 0, &box);
        sl.mutex->ReleaseSync(1);
        context->Flush();  // the release only lands once the copy is submitted
        sl.key = 1;        // until Vulkan consumes it (recycle sets it back to 0)

        sl.busy = true;
        nextSlot = (slot + 1) % kRingSize;

        // The queue never holds more than the ring; the oldest undrawn frame
        // is dropped (the Vulkan side never saw it, its slot frees right away).
        if (pending.size() >= kRingSize) {
            ring[pending.front().slot].busy = false;
            pending.erase(pending.begin());
        }
        pending.push_back({sl.shared, contentW, contentH, slot, generation});
        SetEvent(dataEvent);

        ++captured;
        ULONGLONG now = GetTickCount64();
        if (statsT0 == 0) statsT0 = now;
        if (now - statsT0 >= 1000) {
            fps = captured.exchange(0) * 1000.0f / static_cast<float>(now - statsT0);
            statsT0 = now;
        }
        return growPool;
    }
};

// Thread entry (a static member: Impl is private to Capture).
DWORD WINAPI Capture::Impl::threadMain(void* p) {
    auto* s = static_cast<Capture::Impl*>(p);
    HANDLE waits[2] = {s->frameEvent, s->quitEvent};
    try {
        for (;;) {
            DWORD r = WaitForMultipleObjects(2, waits, FALSE, 100);
            if (r == WAIT_OBJECT_0 + 1 || r == WAIT_FAILED) break;
            if (r == WAIT_TIMEOUT) continue;  // periodic quit check
            bool grow = false;
            winrt::Windows::Graphics::SizeInt32 last{};
            for (;;) {  // drain every arrived frame
                auto frame = s->pool.TryGetNextFrame();
                if (!frame) break;
                last = frame.ContentSize();
                grow = s->onFrame(frame) || grow;
                frame.Close();
            }
            if (grow) {
                // Grow the pool to the new content size; frames keep flowing
                // in the new surfaces from the next callback on.
                s->poolSize = last;
                s->pool.Recreate(s->winrtDevice,
                                 wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, last);
                LOGI("frame pool recreated at %dx%d", last.Width, last.Height);
            }
        }
    } catch (winrt::hresult_error const& e) {
        LOGE("capture thread: %s (0x%08lx)", narrow(e.message().c_str()).c_str(),
             static_cast<unsigned long>(e.code().value));
    }
    return 0;
}

Capture::~Capture() { stop(); }

bool Capture::start(HWND target) {
    stop();
    impl_ = new Impl();
    impl_->target = target;
    try {
        // Multi-threaded apartment: the free-threaded frame pool calls back
        // from the thread pool. Re-entering with the same mode is a no-op.
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        impl_->frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        impl_->dataEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        impl_->quitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        winrt::check_hresult(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, 2, D3D11_SDK_VERSION, impl_->device.put(), nullptr,
            impl_->context.put()));

        auto dxgi = impl_->device.as<IDXGIDevice>();
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
        impl_->winrtDevice = inspectable.as<wgd11::IDirect3DDevice>();

        // A capture item for the game's window (HWND interop; no picker UI).
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem,
                                                     IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForWindow(
            target, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(impl_->item)));

        auto size = impl_->item.Size();
        impl_->poolSize = size;
        impl_->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            impl_->winrtDevice, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        impl_->arrived = impl_->pool.FrameArrived(
            winrt::auto_revoke, [this](auto&&, auto&&) { SetEvent(impl_->frameEvent); });

        impl_->session = impl_->pool.CreateCaptureSession(impl_->item);
        // Both are newer than WGC itself (10 2004 / 11): on older builds the
        // cursor gets captured and the yellow border stays, nothing else changes.
        try { impl_->session.IsCursorCaptureEnabled(false); } catch (winrt::hresult_error const&) {}
        try { impl_->session.IsBorderRequired(false); } catch (winrt::hresult_error const&) {}

        impl_->recreateRing(size.Width, size.Height);

        impl_->thread = CreateThread(nullptr, 0, Impl::threadMain, impl_, 0, nullptr);
        if (!impl_->thread) winrt::check_hresult(HRESULT_FROM_WIN32(GetLastError()));

        impl_->session.StartCapture();
        LOGI("capturing window %ux%u", impl_->contentW, impl_->contentH);
        return true;
    } catch (winrt::hresult_error const& e) {
        LOGE("capture start failed: %s (0x%08lx)", narrow(e.message().c_str()).c_str(),
             static_cast<unsigned long>(e.code().value));
        stop();
        return false;
    }
}

void Capture::stop() {
    if (!impl_) return;
    if (impl_->quitEvent) SetEvent(impl_->quitEvent);
    if (impl_->thread) {
        WaitForSingleObject(impl_->thread, 2000);
        CloseHandle(impl_->thread);
    }
    if (impl_->session) {
        try { impl_->session.Close(); } catch (...) {}
    }
    impl_->arrived.revoke();
    if (impl_->pool) {
        try { impl_->pool.Close(); } catch (...) {}
    }
    // The ring textures back imported VkDeviceMemory: main shuts Vulkan down
    // before calling this, so dropping our references (and the NT handles)
    // is the last reference going away.
    for (auto& g : impl_->graves)
        for (auto& s : g.slots) s.close();
    for (auto& s : impl_->ring) s.close();
    if (impl_->frameEvent) CloseHandle(impl_->frameEvent);
    if (impl_->dataEvent) CloseHandle(impl_->dataEvent);
    if (impl_->quitEvent) CloseHandle(impl_->quitEvent);
    delete impl_;
    impl_ = nullptr;
}

bool Capture::pop(Frame& frame, uint32_t timeoutMs) {
    if (!impl_) return false;
    HANDLE waits[2] = {impl_->dataEvent, impl_->quitEvent};
    DWORD r = WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
    if (r != WAIT_OBJECT_0) return false;
    std::lock_guard lk(impl_->mtx);
    if (impl_->pending.empty()) return false;
    frame = impl_->pending.back();  // newest; older ones were overtaken
    for (size_t i = 0; i + 1 < impl_->pending.size(); ++i)
        impl_->ring[impl_->pending[i].slot].busy = false;  // key stays 1
    impl_->pending.clear();
    return true;
}

void Capture::recycle(const Frame& f, bool consumed) {
    if (!impl_ || f.slot >= kRingSize) return;
    std::lock_guard lk(impl_->mtx);
    if (f.generation != impl_->generation) return;  // ring replaced meanwhile
    Slot& s = impl_->ring[f.slot];
    s.busy = false;
    if (consumed) s.key = 0;  // Vulkan released with key 0
}

void Capture::releaseGraveyard(uint32_t generation) {
    if (!impl_) return;
    std::lock_guard lk(impl_->mtx);
    auto& g = impl_->graves;
    for (auto& gr : g)
        if (gr.generation == generation)
            for (auto& s : gr.slots) s.close();
    g.erase(std::remove_if(g.begin(), g.end(),
                           [&](Grave const& gr) { return gr.generation == generation; }),
            g.end());
}

float Capture::captureFps() const { return impl_ ? impl_->fps.load() : 0.0f; }

} // namespace bdex
