#include "OverlayDx11.h"
#include "Overlay.h"
#include "InputRouter.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

// Minimal Dear ImGui loader includes (stubbed or vendored)
#include "third_party/imgui/imgui.h"
#include "third_party/imgui/backends/imgui_impl_win32.h"
#include "third_party/imgui/backends/imgui_impl_dx11.h"
#include "../AfxConsole.h"

// Campath info (points and duration): access global campath and time.
#include "../CamPath.h"
#include "../../AfxHookSource2/MirvTime.h"
#include "../../AfxHookSource2/CampathDrawer.h"

#include <d3d11.h>
#include <dxgi.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")

#include <math.h>
// The official backend header intentionally comments out the WndProc declaration to avoid pulling in windows.h.
// Forward declare it here with C++ linkage so it matches the backend definition.
LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Recording/Take folder accessor from streams module
extern const wchar_t* AfxStreams_GetTakeDir();
#endif

namespace advancedfx { namespace overlay {

OverlayDx11::OverlayDx11(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapchain, HWND hwnd)
    : m_Device(device), m_Context(context), m_Swapchain(swapchain), m_Hwnd(hwnd) {}

OverlayDx11::~OverlayDx11() { Shutdown(); }

bool OverlayDx11::Initialize() {
#ifdef _WIN32
    if (m_Initialized) return true;
    if (!m_Device || !m_Context || !m_Swapchain || !m_Hwnd) return false;

    // Ensure an ImGui context exists (stub is idempotent)
    if (!ImGui::GetCurrentContext()) ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    if (!ImGui_ImplWin32_Init(m_Hwnd)) return false;
    if (!ImGui_ImplDX11_Init(m_Device, m_Context)) return false;
    advancedfx::Message("Overlay: renderer=DX11\n");

    // Route Win32 messages to ImGui when visible
    if (!Overlay::Get().GetInputRouter()) {
        auto router = std::make_unique<InputRouter>();
        if (router->Attach(m_Hwnd)) {
            router->SetMessageCallback([](void* hwnd, unsigned int msg, uint64_t wparam, int64_t lparam) -> bool {
                bool consumed = ImGui_ImplWin32_WndProcHandler((HWND)hwnd, msg, (WPARAM)wparam, (LPARAM)lparam) ? true : false;
                if (!consumed && Overlay::Get().IsVisible()) {
                    // Consume typical mouse/keyboard messages while overlay visible
                    switch (msg) {
                        case 0x200: // WM_MOUSEMOVE
                        case 0x201: case 0x202: case 0x203: // L button
                        case 0x204: case 0x205: case 0x206: // R button
                        case 0x207: case 0x208: case 0x209: // M button
                        case 0x20A: case 0x20B: // wheel
                        case 0x20E: case 0x20F: // horizontal wheel
                        case 0x100: case 0x101: // keydown/up
                        case 0x104: case 0x105: // sys keydown/up
                            consumed = true; break;
                        default: break;
                    }
                }
                return consumed;
            });
            Overlay::Get().AttachInputRouter(std::move(router));
        }
    }

    CreateOrUpdateRtv();

    m_Initialized = true;
    return true;
#else
    return false;
#endif
}

void OverlayDx11::Shutdown() {
#ifdef _WIN32
    if (!m_Initialized) return;
    ReleaseRtv();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    // Do not destroy ImGui context here, shared across overlays.
    m_Initialized = false;
#endif
}

void OverlayDx11::BeginFrame(float dtSeconds) {
#ifdef _WIN32
    if (!m_Initialized) return;
    ImGui::GetIO().DeltaTime = dtSeconds > 0.0f ? dtSeconds : ImGui::GetIO().DeltaTime;
    ImGui_ImplWin32_NewFrame();
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();

    // Diagnostic watermark always (when overlay visible)
    ImGui::GetForegroundDrawList()->AddText(ImVec2(8,8), IM_COL32(255,255,255,255), "HLAE Overlay (diagnostic)", nullptr);

    // Minimal window content per requirements
    ImGui::Begin("HLAE Overlay");
    // Enable camera path preview toggle
    extern CCampathDrawer g_CampathDrawer;
    static bool enable_preview = false;
    static bool preview_inited = false;
    if (!preview_inited) { enable_preview = g_CampathDrawer.Draw_get(); preview_inited = true; }
    if (ImGui::Checkbox("Enable camera path preview", &enable_preview)) {
        g_CampathDrawer.Draw_set(enable_preview);
        advancedfx::Message("Overlay: mirv_campath draw enabled %d\n", enable_preview ? 1 : 0);
    }

    // Open recordings folder (current take dir)
    if (ImGui::Button("Open recordings folder")) {
        const wchar_t* takeDir = AfxStreams_GetTakeDir();
        if (takeDir && takeDir[0]) {
            // Try to open folder directly; if that fails, open parent explorer
            HINSTANCE r = ShellExecuteW(nullptr, L"open", takeDir, nullptr, nullptr, SW_SHOWNORMAL);
            if ((INT_PTR)r <= 32) {
                ShellExecuteW(nullptr, L"open", L"explorer.exe", takeDir, nullptr, SW_SHOWNORMAL);
            }
        } else {
            advancedfx::Warning("Overlay: No recordings folder set. Use 'mirv_streams record name <path>'.\n");
        }
    }
    // FPS readout (ImGui IO updated in Overlay::UpdateDeltaTime via Framerate calc)
    ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("FPS: %.1f", io.Framerate);

    // Campath information (if any)
    extern CamPath g_CamPath;
    size_t cpCount = g_CamPath.GetSize();
    if (cpCount > 0) {
        double seconds = 0.0;
        if (cpCount >= 2) {
            // Use reported duration when 2+ points exist
            seconds = g_CamPath.GetDuration();
        }
        float tickInterval = g_MirvTime.interval_per_tick_get();
        int ticks = (tickInterval > 0.0f) ? (int)round(seconds / (double)tickInterval) : 0;
        ImGui::Separator();
        ImGui::Text("Campath: points=%u, length=%d ticks (%.2f s)", (unsigned)cpCount, ticks, seconds);
    }
    ImGui::End();
#else
    (void)dtSeconds;
#endif
}

void OverlayDx11::Render() {
#ifdef _WIN32
    if (!m_Initialized) return;
    // Ensure a valid RTV is bound before rendering ImGui
    if (!m_Rtv.rtv) CreateOrUpdateRtv();
    if (m_Rtv.rtv) {
        ID3D11RenderTargetView* rtvs[1] = { m_Rtv.rtv };
        m_Context->OMSetRenderTargets(1, rtvs, nullptr);
    }
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
#endif
}

void OverlayDx11::EndFrame() {
    // Nothing specific for DX11 backend
}

void OverlayDx11::OnDeviceLost() {
#ifdef _WIN32
    if (!m_Initialized) return;
    ReleaseRtv();
    ImGui_ImplDX11_InvalidateDeviceObjects();
#endif
}

void OverlayDx11::OnResize(uint32_t, uint32_t) {
#ifdef _WIN32
    if (!m_Initialized) return;
    ReleaseRtv();
    ImGui_ImplDX11_InvalidateDeviceObjects();
    ImGui_ImplDX11_CreateDeviceObjects();
    CreateOrUpdateRtv();
#endif
}

}} // namespace

#ifdef _WIN32
namespace advancedfx { namespace overlay {

void OverlayDx11::ReleaseRtv() {
    if (m_Rtv.rtv) { m_Rtv.rtv->Release(); m_Rtv.rtv = nullptr; }
    m_Rtv.width = m_Rtv.height = 0;
}

void OverlayDx11::CreateOrUpdateRtv() {
    if (!m_Swapchain || !m_Device) return;
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(m_Swapchain->GetDesc(&desc))) return;
    UINT w = desc.BufferDesc.Width;
    UINT h = desc.BufferDesc.Height;
    if (w == 0 || h == 0) return;
    if (m_Rtv.rtv && m_Rtv.width == w && m_Rtv.height == h) return; // up to date

    ReleaseRtv();
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(m_Swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer)) || !backbuffer) return;
    ID3D11RenderTargetView* rtv = nullptr;
    HRESULT hr = m_Device->CreateRenderTargetView(backbuffer, nullptr, &rtv);
    backbuffer->Release();
    if (SUCCEEDED(hr)) {
        m_Rtv.rtv = rtv;
        m_Rtv.width = w;
        m_Rtv.height = h;
    }
}

}} // namespace
#endif
