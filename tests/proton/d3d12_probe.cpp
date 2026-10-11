// A minimal Direct3D 12 game for tools/ci/proton-check.sh (VKD3D-Proton's
// path, the one that deadlocked before hide_present_ext): a flip-model
// swapchain on a command queue, a moving bar drawn by clearing rectangles of
// the back buffer, a fence per frame, 30 fps for a few seconds.
//   x86_64-w64-mingw32-g++ -O2 -static d3d12_probe.cpp -ld3d12 -ldxgi -o d3d12_probe.exe
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdlib>

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcA(h, m, w, l);
}

#define CHECK(x)                                                              \
    do {                                                                      \
        HRESULT hr_ = (x);                                                    \
        if (FAILED(hr_)) {                                                    \
            printf("%s failed: 0x%08lx\n", #x, (unsigned long)hr_);           \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? atoi(argv[1]) : 8;
    constexpr UINT kBuffers = 3;
    WNDCLASSA wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "bdex_probe12";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("bdex_probe12", "bdex d3d12 probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 360,
                              nullptr, nullptr, wc.hInstance, nullptr);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const UINT w = rc.right - rc.left, h = rc.bottom - rc.top;

    IDXGIFactory4* factory = nullptr;
    CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)));
    ID3D12Device* dev = nullptr;
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&dev)));
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    CHECK(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue)));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = w;
    sd.Height = h;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBuffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* swap1 = nullptr;
    CHECK(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &swap1));
    IDXGISwapChain3* swap = nullptr;
    CHECK(swap1->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swap)));

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kBuffers;
    ID3D12DescriptorHeap* heap = nullptr;
    CHECK(dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&heap)));
    const UINT rtvStep = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Resource* back[kBuffers] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[kBuffers];
    for (UINT i = 0; i < kBuffers; ++i) {
        CHECK(swap->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&back[i])));
        rtv[i] = heap->GetCPUDescriptorHandleForHeapStart();
        rtv[i].ptr += i * rtvStep;
        dev->CreateRenderTargetView(back[i], nullptr, rtv[i]);
    }
    ID3D12CommandAllocator* alloc[kBuffers] = {};
    for (UINT i = 0; i < kBuffers; ++i)
        CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                          reinterpret_cast<void**>(&alloc[i])));
    ID3D12GraphicsCommandList* list = nullptr;
    CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr,
                                 __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list)));
    list->Close();
    ID3D12Fence* fence = nullptr;
    CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence)));
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceValue[kBuffers] = {}, nextValue = 1;

    const int frames = seconds * 30;
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    for (int f = 0; f < frames; ++f) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        const UINT i = swap->GetCurrentBackBufferIndex();
        if (fence->GetCompletedValue() < fenceValue[i]) {
            fence->SetEventOnCompletion(fenceValue[i], event);
            WaitForSingleObject(event, INFINITE);
        }
        alloc[i]->Reset();
        list->Reset(alloc[i], nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = back[i];
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        const float bg[4] = {0.1f, 0.12f, 0.15f, 1.f}, bar[4] = {0.9f, 0.8f, 0.2f, 1.f};
        list->ClearRenderTargetView(rtv[i], bg, 0, nullptr);
        const LONG x = (f * 8) % (LONG)(w > 48 ? w - 40 : 1);  // a bar sliding 8 px a frame
        const D3D12_RECT r{x, (LONG)h / 4, x + 40, (LONG)h * 3 / 4};
        list->ClearRenderTargetView(rtv[i], bar, 1, &r);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        CHECK(swap->Present(0, 0));
        fenceValue[i] = nextValue++;
        queue->Signal(fence, fenceValue[i]);
        const LONGLONG due = start.QuadPart + (LONGLONG)(f + 1) * freq.QuadPart / 30;  // 30 fps
        for (;;) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= due) break;
            Sleep(1);
        }
    }
    queue->Signal(fence, nextValue);
    fence->SetEventOnCompletion(nextValue, event);
    WaitForSingleObject(event, 5000);
    printf("d3d12 probe: %d frames presented\n", frames);
    return 0;
}
