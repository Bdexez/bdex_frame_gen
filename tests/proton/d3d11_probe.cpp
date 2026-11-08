// A minimal Direct3D 11 game for tools/ci/proton-check.sh: a window, a
// swapchain, a moving bar drawn by clearing rectangles of the back buffer at
// 30 fps for a few seconds. Under Wine + DXVK it reaches the Vulkan layer as a
// Proton game does (DXVK's swapchain through winevulkan).
//   d3d11_probe.exe [SECONDS] [uncapped] [depth]
// uncapped: no limit of its own (the layer's fps_limit then holds it); with
// D3D11 fences (DXVK has them) it also reports how long after a frame began
// (its input read) the GPU had done it, as d3d12_probe.cpp.
// depth: drawn as a game draws, with a depth buffer (D24S8, cleared to 0 —
// a reversed Z — and written by the pixel shader: the bar nearer than the
// background), the scene a draw; shaders compiled at run time by
// d3dcompiler_47 (Wine's). The layer's depth option finds it there.
//   x86_64-w64-mingw32-g++ -O2 -static d3d11_probe.cpp -ld3d11 -ldxgi -o d3d11_probe.exe
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// depth: the scene as one draw — a full-screen triangle, the bar where its
// x range is, colour and depth from the pixel shader.
static const char kHlsl[] =
    "cbuffer C : register(b0) { float barX; float3 pad; };\n"
    "float4 vsmain(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 p = float2((id << 1) & 2, id & 2);\n"
    "    return float4(p * 2 - 1, 0, 1);\n"
    "}\n"
    "struct O { float4 c : SV_Target; float d : SV_Depth; };\n"
    "O psmain(float4 pos : SV_Position) {\n"
    "    O o;\n"
    "    bool bar = pos.x >= barX && pos.x < barX + 40 && pos.y >= 100 && pos.y < 260;\n"
    "    o.c = bar ? float4(0.9, 0.8, 0.2, 1) : float4(0.1, 0.12, 0.15, 1);\n"
    "    o.d = bar ? 0.1 / 8 : 0.1 / 100;\n"
    "    return o;\n"
    "}\n";
typedef HRESULT(WINAPI* PFN_Compile)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT,
                                     UINT, ID3DBlob**, ID3DBlob**);

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcA(h, m, w, l);
}

// Results to stdout, and to the file BDEX_PROBE_OUT names (under Proton the
// program's stdout goes nowhere).
static void report(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fputs(line, stdout);
    fflush(stdout);
    char path[1024];
    if (GetEnvironmentVariableA("BDEX_PROBE_OUT", path, sizeof path) > 0) {
        if (FILE* f = fopen(path, "a")) {
            fputs(line, f);
            fclose(f);
        }
    }
}

static double qpcMs() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * double(t.QuadPart) / double(freq.QuadPart);
}

// uncapped: the moment each frame's GPU work completed, from a fence of its own.
struct Watch {
    ID3D11Fence* fence;
    std::vector<double>* doneMs;
};
static DWORD WINAPI watchFrames(void* p) {
    auto* w = static_cast<Watch*>(p);
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    for (size_t f = 0; f < w->doneMs->size(); ++f) {
        if (w->fence->GetCompletedValue() < f + 1) {
            w->fence->SetEventOnCompletion(f + 1, ev);
            if (WaitForSingleObject(ev, 5000) != WAIT_OBJECT_0) break;
        }
        (*w->doneMs)[f] = qpcMs();
    }
    CloseHandle(ev);
    return 0;
}

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? atoi(argv[1]) : 8;
    bool uncapped = false, withDepth = false;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "uncapped") == 0) uncapped = true;
        if (strcmp(argv[i], "depth") == 0) withDepth = true;
    }
    WNDCLASSA wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "bdex_probe";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("bdex_probe", "bdex d3d11 probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 360,
                              nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 640;
    sd.BufferDesc.Height = 360;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = {60, 1};
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain* swap = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                               &sd, &swap, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        printf("D3D11CreateDeviceAndSwapChain failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    ID3D11Texture2D* back = nullptr;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(back, nullptr, &rtv);
    ID3D11DeviceContext1* ctx1 = nullptr;
    ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1));
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11DepthStencilState* dss = nullptr;
    ID3D11Buffer* cb = nullptr;
    if (withDepth) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 640;
        td.Height = 360;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ID3D11Texture2D* dtex = nullptr;
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        auto compile = dc ? reinterpret_cast<PFN_Compile>(GetProcAddress(dc, "D3DCompile")) : nullptr;
        ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
        if (!compile || FAILED(dev->CreateTexture2D(&td, nullptr, &dtex)) || FAILED(dev->CreateDepthStencilView(dtex, nullptr, &dsv)) ||
            FAILED(compile(kHlsl, sizeof kHlsl - 1, "probe", nullptr, nullptr, "vsmain", "vs_5_0", 0, 0, &vsb, &err)) ||
            FAILED(compile(kHlsl, sizeof kHlsl - 1, "probe", nullptr, nullptr, "psmain", "ps_5_0", 0, 0, &psb, &err)) ||
            FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs)) ||
            FAILED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps))) {
            report("d3d11 probe: depth: setup failed%s%s\n", err ? ": " : "",
                   err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            return 1;
        }
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dev->CreateDepthStencilState(&dd, &dss);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&bd, nullptr, &cb);
        report("d3d11 probe: drawing with a D24S8 depth buffer (reversed Z)\n");
    }
    const int frames = seconds * 30;
    std::vector<double> startMs(frames, 0.0), doneMs(frames, 0.0);
    ID3D11Device5* dev5 = nullptr;
    ID3D11DeviceContext4* ctx4 = nullptr;
    ID3D11Fence* frameFence = nullptr;
    HANDLE watcher = nullptr;
    Watch watch{nullptr, &doneMs};
    if (uncapped && SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&dev5))) &&
        SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&ctx4))) &&
        SUCCEEDED(dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&frameFence)))) {
        watch.fence = frameFence;
        watcher = CreateThread(nullptr, 0, watchFrames, &watch, 0, nullptr);
    }
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    for (int f = 0; f < frames; ++f) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        startMs[f] = qpcMs();  // (where a game reads its input)
        const float bg[4] = {0.1f, 0.12f, 0.15f, 1.f}, bar[4] = {0.9f, 0.8f, 0.2f, 1.f};
        if (withDepth) {  // the same bar, drawn, with its depth
            ctx->OMSetRenderTargets(1, &rtv, dsv);
            ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.f, 0);
            const float c[4] = {float((f * 8) % 600), 0.f, 0.f, 0.f};
            ctx->UpdateSubresource(cb, 0, nullptr, c, 0, 0);
            D3D11_VIEWPORT vp{0.f, 0.f, 640.f, 360.f, 0.f, 1.f};
            ctx->RSSetViewports(1, &vp);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs, nullptr, 0);
            ctx->PSSetShader(ps, nullptr, 0);
            ctx->PSSetConstantBuffers(0, 1, &cb);
            ctx->OMSetDepthStencilState(dss, 0);
            ctx->Draw(3, 0);
            if (frameFence) ctx4->Signal(frameFence, UINT64(f) + 1);
            swap->Present(0, 0);
            if (!uncapped) {
                const LONGLONG due = start.QuadPart + (LONGLONG)(f + 1) * freq.QuadPart / 30;
                for (;;) {
                    QueryPerformanceCounter(&now);
                    if (now.QuadPart >= due) break;
                    Sleep(1);
                }
            }
            continue;
        }
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, bg);
        if (ctx1) {  // a bar sliding 8 px a frame
            D3D11_RECT r{(f * 8) % 600, 100, (f * 8) % 600 + 40, 260};
            ctx1->ClearView(rtv, bar, &r, 1);
        }
        if (frameFence) ctx4->Signal(frameFence, UINT64(f) + 1);
        swap->Present(0, 0);
        if (uncapped) continue;
        // 30 fps
        const LONGLONG due = start.QuadPart + (LONGLONG)(f + 1) * freq.QuadPart / 30;
        for (;;) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= due) break;
            Sleep(1);
        }
    }
    QueryPerformanceCounter(&now);
    if (uncapped) {
        std::vector<double> lat;
        if (watcher) {
            WaitForSingleObject(watcher, 10000);
            for (int f = frames / 2; f < frames; ++f)  // (the second half: past the start, the limit in force)
                if (doneMs[f] > 0.0) lat.push_back(doneMs[f] - startMs[f]);
            std::sort(lat.begin(), lat.end());
        }
        double mean = 0.0;
        for (double v : lat) mean += v;
        const double fps = frames * double(freq.QuadPart) / double(now.QuadPart - start.QuadPart);
        if (!lat.empty())
            report("d3d11 probe: %.1f fps, frame start to GPU done: median %.1f ms, mean %.1f ms (%zu frames)\n", fps,
                   lat[lat.size() / 2], mean / lat.size(), lat.size());
        else
            report("d3d11 probe: %.1f fps\n", fps);
    }
    report("d3d11 probe: %d frames presented\n", frames);
    return 0;
}
