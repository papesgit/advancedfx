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
#include "../MirvInput.h"
#include "../../AfxHookSource2/RenderSystemDX11Hooks.h"

#include <d3d11.h>
#include <dxgi.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
// The official backend header intentionally comments out the WndProc declaration to avoid pulling in windows.h.
// Forward declare it here with C++ linkage so it matches the backend definition.
LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Recording/Take folder accessor from streams module
extern const wchar_t* AfxStreams_GetTakeDir();
extern const char* AfxStreams_GetRecordNameUtf8();
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
                // Feed ImGui first so it updates IO states
                bool consumed = ImGui_ImplWin32_WndProcHandler((HWND)hwnd, msg, (WPARAM)wparam, (LPARAM)lparam) ? true : false;

                // Right-click passthrough when hovering outside overlay (no capture)
                static bool s_rmbDown = false;
                if (msg == WM_RBUTTONDOWN) s_rmbDown = true;
                if (msg == WM_RBUTTONUP) s_rmbDown = false;
                if (msg == WM_MOUSEMOVE && (wparam & MK_RBUTTON)) s_rmbDown = true; // maintain during drag

                bool overlayVisible = Overlay::Get().IsVisible();
                bool wantCaptureMouse = ImGui::GetIO().WantCaptureMouse;
                bool passThrough = overlayVisible && s_rmbDown && !wantCaptureMouse;
                Overlay::Get().SetRmbPassthroughActive(passThrough);

                if (passThrough) {
                    // Do not consume: allow the game to receive input while overlay stays open
                    return false;
                }

                // Otherwise, if overlay visible and ImGui didn't already consume, eat typical inputs
                if (!consumed && overlayVisible) {
                    switch (msg) {
                        case WM_MOUSEMOVE:
                        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
                        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
                        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
                        case WM_MOUSEWHEEL:
                        case WM_MOUSEHWHEEL:
                        case WM_KEYDOWN: case WM_KEYUP:
                        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
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
    ImGui::GetForegroundDrawList()->AddText(ImVec2(8,8), IM_COL32(255,255,255,255), "HLAE Overlay 0.1", nullptr);

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

    // Record name (path) and Open recordings folder
    {
        static char s_recName[512] = {0};
        static bool recInit = false;
        if (!recInit) {
            const char* rn = AfxStreams_GetRecordNameUtf8();
            if (rn) strncpy_s(s_recName, rn, _TRUNCATE);
            recInit = true;
        }
        // Render label separately to avoid label width affecting input width calculation
        ImGuiStyle& st = ImGui::GetStyle();
        ImGui::TextUnformatted("Record path");
        ImGui::SameLine(0.0f, st.ItemInnerSpacing.x);
        float avail = ImGui::GetContentRegionAvail().x; // remaining after label
        float btnW = ImGui::CalcTextSize("Set").x + st.FramePadding.x * 2.0f;
        // Ensure at least 2x standard spacing (plus a small margin) to the right of the box for the button
        float extra = st.ItemInnerSpacing.x * 2.0f + 8.0f;
        float boxW = avail - (btnW + extra);
        if (boxW < 150.0f) boxW = 150.0f; // reasonable minimum
        if (boxW > avail - (btnW + extra)) boxW = avail - (btnW + extra);
        if (boxW < 50.0f) boxW = 50.0f; // last resort on very small windows
        ImGui::SetNextItemWidth(boxW);
        ImGui::InputText("##recname", s_recName, sizeof(s_recName));
        ImGui::SameLine(0.0f, st.ItemInnerSpacing.x * 2.0f + 8.0f);
        if (ImGui::Button("Set##recname")) {
            AfxStreams_SetRecordNameUtf8(s_recName);
            advancedfx::Message("Overlay: mirv_streams record name set.\n");
        }
    }

    // Open recordings folder (current take dir)
    if (ImGui::Button("Open recordings folder")) {
        // Prefer explicit record name folder; if empty, fallback to last take dir
        const char* recUtf8 = AfxStreams_GetRecordNameUtf8();
        std::wstring wPath;
        if (recUtf8 && recUtf8[0]) {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, recUtf8, -1, nullptr, 0);
            if (wlen > 0) {
                wPath.resize((size_t)wlen - 1);
                MultiByteToWideChar(CP_UTF8, 0, recUtf8, -1, &wPath[0], wlen);
            }
        }
        if (wPath.empty()) {
            const wchar_t* takeDir = AfxStreams_GetTakeDir();
            if (takeDir && takeDir[0]) wPath = takeDir;
        }
        if (!wPath.empty()) {
            HINSTANCE r = ShellExecuteW(nullptr, L"open", wPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if ((INT_PTR)r <= 32) {
                ShellExecuteW(nullptr, L"open", L"explorer.exe", wPath.c_str(), nullptr, SW_SHOWNORMAL);
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

    // Campath controls
    bool campathEnabled = g_CamPath.Enabled_get();
    if (ImGui::Checkbox("Campath enabled", &campathEnabled)) {
        g_CamPath.Enabled_set(campathEnabled);
        advancedfx::Message("Overlay: mirv_campath enabled %d\n", campathEnabled ? 1 : 0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add point")) {
        double x,y,z,rx,ry,rz; float fov;
        Afx_GetLastCameraData(x,y,z,rx,ry,rz,fov);
        double t = g_MirvTime.curtime_get() - g_CamPath.GetOffset();
        g_CamPath.Add(t, CamPathValue(x,y,z, rx, ry, rz, fov));
        advancedfx::Message("Overlay: mirv_campath add (t=%.3f)\n", t);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        g_CamPath.Clear();
        advancedfx::Message("Overlay: mirv_campath clear\n");
    }
    ImGui::SameLine();
    if (ImGui::Button("Goto Start")) {
        if (g_CamPath.GetSize() > 0) {
            double firstT = g_CamPath.GetBegin().GetTime();
            double targetClientTime = g_CamPath.GetOffset() + firstT;
            double curTime = g_MirvTime.curtime_get();
            double demoNow = 0.0;
            float ipt = g_MirvTime.interval_per_tick_get();
            int targetTick = 0;
            if (ipt > 0.0f && g_MirvTime.GetCurrentDemoTime(demoNow)) {
                double targetDemoTime = targetClientTime - (curTime - demoNow);
                targetTick = (int)round(targetDemoTime / (double)ipt);
                Afx_GotoDemoTick(targetTick);
                advancedfx::Message("Overlay: mirv_skip tick to %d\n", targetTick);
            } else if (ipt > 0.0f) {
                targetTick = (int)round(targetClientTime / (double)ipt);
                Afx_GotoDemoTick(targetTick);
                advancedfx::Message("Overlay: mirv_skip tick to %d (approx)\n", targetTick);
            } else {
                advancedfx::Warning("Overlay: Failed to compute demo tick for campath start.\n");
            }
        } else {
            advancedfx::Warning("Overlay: No campath points available.\n");
        }
    }
    // Recording FPS override and screen enable
    {
        static char s_fpsText[64] = {0};
        static bool fpsInit = false;
        if (!fpsInit) {
            if (AfxStreams_GetOverrideFps()) {
                snprintf(s_fpsText, sizeof(s_fpsText), "%.2f", AfxStreams_GetOverrideFpsValue());
            } else {
                s_fpsText[0] = 0;
            }
            fpsInit = true;
        }
        // Fixed-size FPS box to fit at least 5 characters
        ImGuiStyle& st2 = ImGui::GetStyle();
        float fiveChars = ImGui::CalcTextSize("00000").x + st2.FramePadding.x * 2.0f + 2.0f;
        ImGui::SetNextItemWidth(fiveChars);
        ImGui::InputText("Record FPS", s_fpsText, sizeof(s_fpsText));
        ImGui::SameLine(0.0f, st2.ItemInnerSpacing.x);
        if (ImGui::Button("Apply FPS")) {
            if (s_fpsText[0] == 0) {
                AfxStreams_SetOverrideFpsDefault();
                advancedfx::Message("Overlay: mirv_streams record fps default\n");
            } else {
                float v = (float)atof(s_fpsText);
                if (v > 0.0f) {
                    AfxStreams_SetOverrideFpsValue(v);
                    advancedfx::Message("Overlay: mirv_streams record fps %.2f\n", v);
                } else {
                    advancedfx::Warning("Overlay: Invalid FPS value.\n");
                }
            }
        }

        bool screenEnabled = AfxStreams_GetRecordScreenEnabled();
        if (ImGui::Checkbox("Record screen enabled", &screenEnabled)) {
            AfxStreams_SetRecordScreenEnabled(screenEnabled);
        }

        if (ImGui::Button("Start Recording")) {
            Afx_ExecClientCmd("demo_resume");
            AfxStreams_RecordStart();
        }
        ImGui::SameLine();
        if (ImGui::Button("End Recording")) {
            AfxStreams_RecordEnd();
            Afx_ExecClientCmd("demo_pause");
        }
    }

    ImGui::End();

    // Mirv input camera controls/indicator
    if (MirvInput* pMirv = Afx_GetMirvInput()) {
        ImGui::Begin("Mirv Camera");
        bool camEnabled = pMirv->GetCameraControlMode();
        ImGui::Text("mirv_input camera: %s", camEnabled ? "enabled" : "disabled");
        ImGui::SameLine();
        if (ImGui::Button(camEnabled ? "Disable" : "Enable")) {
            pMirv->SetCameraControlMode(!camEnabled);
        }

        // FOV slider (reserve right space so label is always visible)
        {
            ImGuiStyle& st3 = ImGui::GetStyle();
            float avail = ImGui::GetContentRegionAvail().x;
            const char* lbl = "FOV";
            float lblW = ImGui::CalcTextSize(lbl).x;
            float rightGap = st3.ItemInnerSpacing.x * 3.0f + st3.FramePadding.x * 2.0f + 20.0f;
            float width = avail - (lblW + rightGap);
            if (width < 100.0f) width = avail * 0.6f; // fallback
            ImGui::SetNextItemWidth(width);
        }
        // FOV slider
        static bool fovInit = false; static float s_fov = 90.0f;
        if (!fovInit) { s_fov = GetLastCameraFov(); fovInit = true; }
        if (ImGui::SliderFloat("FOV", &s_fov, 1.0f, 179.0f, "%.1f deg")) {
            pMirv->SetFov(s_fov);
        }

        // Roll slider (reserve right space so label is always visible)
        {
            ImGuiStyle& st3 = ImGui::GetStyle();
            float avail = ImGui::GetContentRegionAvail().x;
            const char* lbl = "Roll";
            float lblW = ImGui::CalcTextSize(lbl).x;
            float rightGap = st3.ItemInnerSpacing.x * 3.0f + st3.FramePadding.x * 2.0f + 20.0f;
            float width = avail - (lblW + rightGap);
            if (width < 100.0f) width = avail * 0.6f; // fallback
            ImGui::SetNextItemWidth(width);
        }
        static bool rollInit = false; static float s_roll = 0.0f;
        if (!rollInit) { s_roll = GetLastCameraRoll(); rollInit = true; }
        if (ImGui::SliderFloat("Roll", &s_roll, -180.0f, 180.0f, "%.1f deg")) {
            pMirv->SetRz(s_roll);
        }

        // Keyboard sensitivity (mirv_input cfg ksens) (reserve right space so label is always visible)
        {
            ImGuiStyle& st3 = ImGui::GetStyle();
            float avail = ImGui::GetContentRegionAvail().x;
            const char* lbl = "ksens";
            float lblW = ImGui::CalcTextSize(lbl).x;
            float rightGap = st3.ItemInnerSpacing.x * 3.0f + st3.FramePadding.x * 2.0f + 20.0f;
            float width = avail - (lblW + rightGap);
            if (width < 100.0f) width = avail * 0.6f; // fallback
            ImGui::SetNextItemWidth(width);
        }
        static bool ksensInit = false; static float s_ksens = 1.0f;
        if (!ksensInit) { s_ksens = (float)pMirv->GetKeyboardSensitivty(); ksensInit = true; }
        if (ImGui::SliderFloat("ksens", &s_ksens, 0.01f, 10.0f, "%.2f")) {
            pMirv->SetKeyboardSensitivity(s_ksens);
        }

        ImGui::End();
    }
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
