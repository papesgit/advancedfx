#include "OverlayDx11.h"
#include "Overlay.h"
#include "InputRouter.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

// Minimal Dear ImGui loader includes (stubbed or vendored)
#include "third_party/imgui/imgui.h"
#include "third_party/imgui/imgui_internal.h" // for ImGui::ClearActiveID
#include "third_party/imgui/backends/imgui_impl_win32.h"
#include "third_party/imgui/backends/imgui_impl_dx11.h"
#include "third_party/imgui_neo_sequencer/imgui_neo_sequencer.h"
#include "../AfxConsole.h"
#include "../AfxMath.h"

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
#include <vector>
#include <string>
#include <algorithm>
// The official backend header intentionally comments out the WndProc declaration to avoid pulling in windows.h.
// Forward declare it here with C++ linkage so it matches the backend definition.
LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Recording/Take folder accessor from streams module
extern const wchar_t* AfxStreams_GetTakeDir();
extern const char* AfxStreams_GetRecordNameUtf8();
#endif

namespace advancedfx { namespace overlay {

// Simple in-overlay console state
static bool g_ShowOverlayConsole = false;
static std::vector<std::string> g_OverlayConsoleLog;
static char g_OverlayConsoleInput[512] = {0};
static bool g_OverlayConsoleScrollToBottom = false;

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

    // Toggle overlay console
    ImGui::Checkbox("Show Console", &g_ShowOverlayConsole);

    // Sequencer toggle
    static bool g_ShowSequencer = false;
    ImGui::Checkbox("Show Sequencer", &g_ShowSequencer);

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
    // Interpolation toggle: Cubic (default) <-> Linear
    ImGui::SameLine();
    {
        static bool s_interpCubic = true; // default UI state: Cubic
        const char* interpLabel = s_interpCubic ? "Interp: Cubic" : "Interp: Linear";
        if (ImGui::Button(interpLabel)) {
            s_interpCubic = !s_interpCubic;
            if (s_interpCubic) {
                Afx_ExecClientCmd(
                    "mirv_campath edit interp position default; mirv_campath edit interp rotation default; mirv_campath edit interp fov default");
            } else {
                Afx_ExecClientCmd(
                    "mirv_campath edit interp position linear; mirv_campath edit interp rotation sLinear; mirv_campath edit interp fov linear");
            }
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

        // FFmpeg HLAE profiles dropdown
        ImGui::Separator();
        ImGui::TextUnformatted("FFmpeg profiles");
        ImGui::SameLine();
        static const char* s_selectedProfile = "Select profile...";
        if (ImGui::BeginCombo("##ffmpeg_profiles", s_selectedProfile)) {
            if (ImGui::Selectable("TGA Sequence")) {
                s_selectedProfile = "TGA Sequence";
                Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings afxClassic;echo [Current Record Setting];echo afxClassic - 无损 .tga 图片序列)"
                );
            }
            if (ImGui::Selectable("ProRes 4444")) {
                s_selectedProfile = "ProRes 4444";
                Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg p0  "-c:v prores  -profile:v 4 {QUOTE}{AFX_STREAM_PATH}\video.mov{QUOTE}")");
                Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings p0  ;echo [Current Record Setting];echo p0 - ProRes 4444)"
                );
            }
            if (ImGui::Selectable("ProRes 422 HQ")) {
                s_selectedProfile = "ProRes 422 HQ";
                Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg phq "-c:v prores  -profile:v 3 -pix_fmt yuv422p10le {QUOTE}{AFX_STREAM_PATH}\video.mov{QUOTE}")");
                Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings phq ;echo [Current Record Setting];echo phq - ProRes 422 HQ)"
                );
            }
            if (ImGui::Selectable("ProRes 422")) {
                s_selectedProfile = "ProRes 422";
                Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg p1  "-c:v prores  -profile:v 2 -pix_fmt yuv422p10le {QUOTE}{AFX_STREAM_PATH}\video.mov{QUOTE}")");
                Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings p1  ;echo [Current Record Setting];echo p1 - ProRes 422)"
                );
            }
            if (ImGui::Selectable("x264 Lossless")) {
                s_selectedProfile = "x264 Lossless";
                Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg c0  "-c:v libx264 -preset 0 -qp  0  -g 120 -keyint_min 1 -pix_fmt yuv422p10le {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")");
                Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings c0  ;echo [Current Record Setting];echo c0 - x264 无损)"
                );
            }
            if (ImGui::Selectable("x264 HQ")) {
                s_selectedProfile = "x264 HQ";
                Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg c1  "-c:v libx264 -preset 1 -crf 4  -qmax 20 -g 120 -keyint_min 1 -pix_fmt yuv420p -x264-params ref=3:me=hex:subme=3:merange=12:b-adapt=1:aq-mode=2:aq-strength=0.9:no-fast-pskip=1 {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")");
                Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings c1  ;echo [Current Record Setting];echo c1 - x264 高画质)"
                );
            }
            if (ImGui::Selectable("x265 Lossless")) {
                s_selectedProfile = "x265 Lossless";
                Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg he0  "-c:v libx265 -x265-params no-sao=1 -preset 0 -lossless -g 120 -keyint_min 1 -pix_fmt yuv422p {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")");
                Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings he0 ;echo [Current Record Setting];echo he0 - x265 无损)"
                );
            }
            if (ImGui::Selectable("x265 HQ")) {
                s_selectedProfile = "x265 HQ";
                Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg he1  "-c:v libx265 -x265-params no-sao=1 -preset 1 -crf 8  -qmax 20 -g 120 -keyint_min 1 -pix_fmt yuv422p {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")");
                Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings he1 ;echo [Current Record Setting];echo he1 - x265 高画质)"
                );
            }
            ImGui::EndCombo();
        }

        if (ImGui::Button("Start Recording")) {
            Afx_ExecClientCmd("demo_resume");
            AfxStreams_RecordStart();
        }
        ImGui::SameLine();
        if (ImGui::Button("End Recording")) {
            Afx_ExecClientCmd("demo_pause");
            Afx_ExecClientCmd("mirv_streams record end");
            //AfxStreams_RecordEnd();
        }
    }

    ImGui::End();

    // Sequencer window (ImGui Neo Sequencer)
    if (g_ShowSequencer) {
        ImGui::SetNextWindowSize(ImVec2(720, 260), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("HLAE Sequencer", &g_ShowSequencer)) {
            static ImGui::FrameIndexType s_seqFrame = 0;
            static ImGui::FrameIndexType s_seqStart = 0;
            static ImGui::FrameIndexType s_seqEnd = 0;
            static bool s_seqInit = false;

            // Use current demo tick as live frame pointer; grow end to the maximum observed tick.
            int curDemoTick = 0;
            if (g_MirvTime.GetCurrentDemoTick(curDemoTick)) {
                if (curDemoTick > (int)s_seqEnd) s_seqEnd = (ImGui::FrameIndexType)curDemoTick;
            }

            // Snapshot frame before drawing; if changed by user, we seek.
            ImGui::FrameIndexType prevFrame = s_seqFrame;

            // Determine content bound once (when opening): set initial view to fit content.
            if (!s_seqInit) {
                int contentMaxTick = 0;
                int tmpTick = 0;
                if (g_MirvTime.GetCurrentDemoTick(tmpTick)) contentMaxTick = tmpTick;
                extern CamPath g_CamPath;
                size_t cpCount = g_CamPath.GetSize();
                if (cpCount > 0) {
                    double curTime = g_MirvTime.curtime_get();
                    double currentDemoTime = 0.0;
                    bool haveDemoTime = g_MirvTime.GetCurrentDemoTime(currentDemoTime);
                    float ipt = g_MirvTime.interval_per_tick_get();
                    if (ipt <= 0.0f) ipt = 1.0f / 64.0f;
                    for (CamPathIterator it = g_CamPath.GetBegin(); it != g_CamPath.GetEnd(); ++it) {
                        double tRel = it.GetTime();
                        double clientTarget = g_CamPath.GetOffset() + tRel;
                        int tick = 0;
                        if (haveDemoTime) {
                            double demoTarget = clientTarget - (curTime - currentDemoTime);
                            tick = (int)llround(demoTarget / (double)ipt);
                        } else {
                            tick = (int)llround(clientTarget / (double)ipt);
                        }
                        if (tick > contentMaxTick) contentMaxTick = tick;
                    }
                }
                s_seqStart = 0;
                s_seqEnd = (ImGui::FrameIndexType)(contentMaxTick > 0 ? contentMaxTick : 1);
                s_seqInit = true;
            }

            // --- Tight sequencer area + always-visible controls row ---

            ImGuiStyle& st = ImGui::GetStyle();
            float availY    = ImGui::GetContentRegionAvail().y;

            // Keep just one row for the controls (no extra spacing baked in)
            float btnRowH   = ImGui::GetFrameHeight();

            // Estimate a compact sequencer height: header + 1 track + small margins
            int   trackCount   = 1;                          // you draw only "Campath"
            float headerH      = ImGui::GetFrameHeight();    // ruler/header bar
            float trackRowH    = ImGui::GetFrameHeight()*1.2f;
            float margins      = st.ItemSpacing.y + st.FramePadding.y;

            float desiredSeqH  = headerH + trackCount * trackRowH + margins;

            // Clamp: at least 100 px, at most what’s left after the controls row
            float maxSeqHAvail = ImMax(55.0f, availY - btnRowH);
            float seqH         = ImClamp(desiredSeqH, 60.0f, maxSeqHAvail);

            ImGui::BeginChild("seq_child", ImVec2(0.0f, seqH), false); // scrolling lives inside this child
            {
                const ImVec2 seqSize(ImGui::GetContentRegionAvail().x, 0.0f);
                if (ImGui::BeginNeoSequencer("##demo_seq", &s_seqFrame, &s_seqStart, &s_seqEnd, seqSize,
                                            ImGuiNeoSequencerFlags_AlwaysShowHeader)) {
                    // Campath timeline: populate from g_CamPath keyframes
                    {
                        extern CamPath g_CamPath;
                        size_t cpCount = g_CamPath.GetSize();
                        if (cpCount > 0) {
                            std::vector<int32_t> keyTicks;
                            keyTicks.reserve(cpCount);
                            std::vector<double> keyTimes; // original relative times (seconds)
                            keyTimes.reserve(cpCount);
                            std::vector<CamPathValue> keyValues;
                            keyValues.reserve(cpCount);

                            double curTime = g_MirvTime.curtime_get();
                            double currentDemoTime = 0.0;
                            bool haveDemoTime = g_MirvTime.GetCurrentDemoTime(currentDemoTime);
                            float ipt = g_MirvTime.interval_per_tick_get();
                            if (ipt <= 0.0f) ipt = 1.0f / 64.0f;

                            for (CamPathIterator it = g_CamPath.GetBegin(); it != g_CamPath.GetEnd(); ++it) {
                                double tRel = it.GetTime();
                                keyTimes.push_back(tRel);
                                keyValues.push_back(it.GetValue());
                                double clientTarget = g_CamPath.GetOffset() + tRel;
                                int tick = 0;
                                if (haveDemoTime) {
                                    double demoTarget = clientTarget - (curTime - currentDemoTime);
                                    tick = (int)llround(demoTarget / (double)ipt);
                                } else {
                                    tick = (int)llround(clientTarget / (double)ipt);
                                }
                                if (tick < 0) tick = 0;
                                keyTicks.push_back(tick);
                            }

                            bool open = true;
                            if (ImGui::BeginNeoTimelineEx("Campath", &open, ImGuiNeoTimelineFlags_None)) {
                                // Stable context for right-clicked keyframe
                                struct CtxKf { bool active; double time; CamPathValue value; };
                                static CtxKf s_ctx = {false, 0.0, {}};

                                int rightClickedIndex = -1;
                                for (int i = 0; i < (int)keyTicks.size(); ++i) {
                                    ImGui::NeoKeyframe(&keyTicks[i]);
                                    if (ImGui::IsNeoKeyframeRightClicked()) rightClickedIndex = i;
                                }
                                if (rightClickedIndex >= 0) {
                                    s_ctx.active = true;
                                    s_ctx.time = keyTimes[(size_t)rightClickedIndex];
                                    s_ctx.value = keyValues[(size_t)rightClickedIndex];
                                    ImGui::OpenPopup("campath_kf_ctx");
                                    ImGui::SetNextWindowPos(ImGui::GetMousePos());
                                    ImGui::SetNextWindowSizeConstraints(ImVec2(120,0), ImVec2(300,FLT_MAX));
                                }
                                if (ImGui::BeginPopup("campath_kf_ctx")) {
                                    bool doRemove = false, doGet = false, doSet = false;
                                    if (ImGui::MenuItem("Remove")) doRemove = true;
                                    if (ImGui::MenuItem("Get")) doGet = true;
                                    if (ImGui::MenuItem("Set")) doSet = true;
                                    if (ImGui::MenuItem("Edit")) { /* stub */ }
                                    ImGui::EndPopup();

                                    if (s_ctx.active) {
                                        extern CCampathDrawer g_CampathDrawer;
                                        bool prevDraw = g_CampathDrawer.Draw_get();
                                        if (doRemove) {
                                            g_CampathDrawer.Draw_set(false);
                                            g_CamPath.Remove(s_ctx.time);
                                            g_CampathDrawer.Draw_set(prevDraw);
                                            s_ctx.active = false;
                                        }
                                        if (doGet) {
                                            if (MirvInput* pMirv = Afx_GetMirvInput()) {
                                                pMirv->SetCameraControlMode(true);
                                                const CamPathValue& v = s_ctx.value;
                                                pMirv->SetTx((float)v.X);
                                                pMirv->SetTy((float)v.Y);
                                                pMirv->SetTz((float)v.Z);
                                                using namespace Afx::Math;
                                                QEulerAngles angles = v.R.ToQREulerAngles().ToQEulerAngles();
                                                pMirv->SetRx((float)angles.Pitch);
                                                pMirv->SetRy((float)angles.Yaw);
                                                pMirv->SetRz((float)angles.Roll);
                                                pMirv->SetFov((float)v.Fov);
                                            }
                                            s_ctx.active = false;
                                        }
                                        if (doSet) {
                                            double cx, cy, cz, rX, rY, rZ; float cfov;
                                            Afx_GetLastCameraData(cx, cy, cz, rX, rY, rZ, cfov);
                                            CamPathValue newVal(cx, cy, cz, rX, rY, rZ, cfov);
                                            g_CampathDrawer.Draw_set(false);
                                            g_CamPath.Remove(s_ctx.time);
                                            g_CamPath.Add(s_ctx.time, newVal);
                                            g_CampathDrawer.Draw_set(prevDraw);
                                            s_ctx.active = false;
                                        }
                                    }
                                }

                                ImGui::EndNeoTimeLine();
                            }
                        } else {
                            // Show empty Campath track for visibility
                            bool open = true;
                            if (ImGui::BeginNeoTimelineEx("Campath", &open)) {
                                ImGui::EndNeoTimeLine();
                            }
                        }
                    }

                    ImGui::EndNeoSequencer();
                }
            }
            ImGui::EndChild();
            if (ImGui::SmallButton("Toggle Pause")) {
                Afx_ExecClientCmd("demo_togglepause");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("0.5x")) {
                Afx_ExecClientCmd("demo_timescale 0.5");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("1x")) {
                Afx_ExecClientCmd("demo_timescale 1");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("2x")) {
                Afx_ExecClientCmd("demo_timescale 2");
            }
            ImGui::SameLine();
            ImGui::Text("Start: %d  End: %d  Current: %d",
                        (int)s_seqStart, (int)s_seqEnd, (int)s_seqFrame);


            // Follow current tick to keep pointer in view when not interacting.
            // If users drag the pointer, we respect their new position and seek.
            // Defer seeking until the user releases LMB after actually moving the sequencer pointer.
            static bool s_draggingPointer = false;
            static ImGui::FrameIndexType s_dragStartFrame = 0;

            const bool pointerMoved = (s_seqFrame != prevFrame);
            const bool lmbDown      = ImGui::IsMouseDown(ImGuiMouseButton_Left);
            const bool lmbReleased  = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
            // We're inside the "HLAE Sequencer" window; include child areas (headers/track rows).
            const bool seqHovered   = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

            // Begin dragging only when the sequencer pointer actually moved under LMB.
            if (!s_draggingPointer) {
                if (pointerMoved && lmbDown && seqHovered) {
                    s_draggingPointer = true;
                    s_dragStartFrame = prevFrame;
                }
            }

            if (s_draggingPointer) {
                // Finish drag on release (or if LMB is no longer down for any reason)
                if (lmbReleased || !lmbDown) {
                    if (s_seqFrame != s_dragStartFrame)
                        Afx_GotoDemoTick((int)s_seqFrame);
                    s_draggingPointer = false;
                }
            } else if (curDemoTick > 0) {
                // Not interacting: keep the pointer following the live demo tick.
                s_seqFrame = (ImGui::FrameIndexType)curDemoTick;
            }



            ImGui::End();
        }
    }

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
        static bool fovInit = false; static float s_fov = 90.0f; static float s_fovDefault = 90.0f;
        if (!fovInit) { s_fov = GetLastCameraFov(); s_fovDefault = s_fov; fovInit = true; }
        {
            float tmp = s_fov;
            bool changed = ImGui::SliderFloat("FOV", &tmp, 1.0f, 179.0f, "%.1f deg");
            bool reset = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
            if (reset) {
                s_fov = s_fovDefault;
                ImGui::ClearActiveID();
                pMirv->SetFov(s_fov);
            } else if (changed) {
                s_fov = tmp;
                pMirv->SetFov(s_fov);
            }
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
        static bool rollInit = false; static float s_roll = 0.0f; static float s_rollDefault = 0.0f;
        if (!rollInit) { s_roll = GetLastCameraRoll(); s_rollDefault = s_roll; rollInit = true; }
        {
            float tmp = s_roll;
            bool changed = ImGui::SliderFloat("Roll", &tmp, -180.0f, 180.0f, "%.1f deg");
            bool reset = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
            if (reset) {
                s_roll = s_rollDefault;
                ImGui::ClearActiveID();
                pMirv->SetRz(s_roll);
            } else if (changed) {
                s_roll = tmp;
                pMirv->SetRz(s_roll);
            }
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
        static bool ksensInit = false; static float s_ksens = 1.0f; static float s_ksensDefault = 1.0f;
        if (!ksensInit) { s_ksens = (float)pMirv->GetKeyboardSensitivty(); s_ksensDefault = s_ksens; ksensInit = true; }
        {
            float tmp = s_ksens;
            bool changed = ImGui::SliderFloat("ksens", &tmp, 0.01f, 10.0f, "%.2f");
            bool reset = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
            if (reset) {
                s_ksens = s_ksensDefault;
                ImGui::ClearActiveID();
                pMirv->SetKeyboardSensitivity(s_ksens);
            } else if (changed) {
                s_ksens = tmp;
                pMirv->SetKeyboardSensitivity(s_ksens);
            }
        }

        ImGui::End();
    }

    // Overlay Console window
    if (g_ShowOverlayConsole) {
        ImGui::Begin("HLAE Console", &g_ShowOverlayConsole);
        // Log area
        ImGui::BeginChild("ConsoleLog", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()*2.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto &line : g_OverlayConsoleLog) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (g_OverlayConsoleScrollToBottom) {
            ImGui::SetScrollHereY(1.0f);
            g_OverlayConsoleScrollToBottom = false;
        }
        ImGui::EndChild();

        // Input + Submit
        ImGui::SetNextItemWidth(-100.0f);
        bool submitted = ImGui::InputText("##cmd", g_OverlayConsoleInput, sizeof(g_OverlayConsoleInput), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Submit") || submitted) {
            if (g_OverlayConsoleInput[0] != '\0') {
                g_OverlayConsoleLog.emplace_back(std::string("> ") + g_OverlayConsoleInput);
                Afx_ExecClientCmd(g_OverlayConsoleInput);
                g_OverlayConsoleInput[0] = '\0';
                g_OverlayConsoleScrollToBottom = true;
            }
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
