#include "OverlayDx11.h"
#include "Overlay.h"
#include "InputRouter.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

// Minimal Dear ImGui loader includes (stubbed or vendored)
#include "third_party/imgui/imgui.h"
#include "third_party/imgui_filebrowser/imfilebrowser.h"
#include "third_party/imgui/imgui_internal.h" // for ImGui::ClearActiveID
#include "third_party/imgui/backends/imgui_impl_win32.h"
#include "third_party/imgui/backends/imgui_impl_dx11.h"
#include "third_party/imgui_neo_sequencer/imgui_neo_sequencer.h"
#include "third_party/imguizmo/ImGuizmo.h"
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
#include <filesystem>
// The official backend header intentionally comments out the WndProc declaration to avoid pulling in windows.h.
// Forward declare it here with C++ linkage so it matches the backend definition.
LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Recording/Take folder accessor from streams module
extern const wchar_t* AfxStreams_GetTakeDir();
extern const char* AfxStreams_GetRecordNameUtf8();
// Global campath instance (provided by shared CamPath module)
extern CamPath g_CamPath;
#endif

namespace advancedfx { namespace overlay {

struct CampathCtx {
    bool active = false;
    double time = 0.0;
    CamPathValue value{};
};

// Global (file-scope) context for “last selected/edited campath key”
static CampathCtx g_LastCampathCtx;
static ImGuizmo::OPERATION g_GizmoOp   = ImGuizmo::TRANSLATE;
static ImGuizmo::MODE      g_GizmoMode = ImGuizmo::LOCAL;
// Signal to sequencer to refresh its cached keyframe values after edits
static bool g_SequencerNeedsRefresh = false;
// Gizmo debug controls (file-scope so both UI and render path can access)
//

// Simple in-overlay console state
static bool g_ShowOverlayConsole = false;
static std::vector<std::string> g_OverlayConsoleLog;
static char g_OverlayConsoleInput[512] = {0};
static bool g_OverlayConsoleScrollToBottom = false;
// Sequencer toggle state (shared between tabs and window)
static bool g_ShowSequencer = false;
// Sequencer preview behavior: when true, keep the preview pose after releasing slider;
// when false, restore prior pose if Mirv Camera was enabled before preview.
static bool g_PreviewFollow = false;
// Sequencer preview normalized position [0..1]
static float g_PreviewNorm = 0.0f;

// Helper: constrain window height while allowing horizontal resize
// (removed: height clamp helper; switching to live auto-height adjustment per-frame)

// UI state mirrored for Mirv camera sliders so external changes (e.g., wheel in passthrough)
// stay in sync with the controls below.
static bool  g_uiFovInit   = false; static float  g_uiFov = 90.0f;   static float g_uiFovDefault = 90.0f;
static bool  g_uiRollInit  = false; static float  g_uiRoll = 0.0f;   static float g_uiRollDefault = 0.0f;
static bool  g_uiKsensInit = false; static float  g_uiKsens = 1.0f;  static float g_uiKsensDefault = 1.0f;

// Overlay UI scale (DPI) control
// - s_UiScale drives ImGui IO FontGlobalScale and style sizes via ScaleAllSizes
// - We keep previous value to scale relatively and avoid cumulative drift
static float g_UiScale = 1.0f;

// Persisted last-used directories (saved in imgui.ini via custom settings handler)
struct OverlayPathsSettings {
    std::string campathDir;
    std::string recordBrowseDir;
};
static OverlayPathsSettings g_OverlayPaths;

// ImGui ini handler for persisting overlay paths
static void* OverlayPaths_ReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
    return (0 == strcmp(name, "Paths")) ? (void*)&g_OverlayPaths : nullptr;
}
static void OverlayPaths_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    OverlayPathsSettings* s = (OverlayPathsSettings*)entry;
    const char* eq = strchr(line, '=');
    if (!eq) return;
    std::string key(line, eq - line);
    std::string val(eq + 1);
    if (key == "CampathDir") s->campathDir = val;
    else if (key == "RecordBrowseDir") s->recordBrowseDir = val;
}
static void OverlayPaths_WriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* out_buf) {
    out_buf->appendf("[%s][%s]\n", handler->TypeName, "Paths");
    if (!g_OverlayPaths.campathDir.empty()) out_buf->appendf("CampathDir=%s\n", g_OverlayPaths.campathDir.c_str());
    if (!g_OverlayPaths.recordBrowseDir.empty()) out_buf->appendf("RecordBrowseDir=%s\n", g_OverlayPaths.recordBrowseDir.c_str());
    out_buf->append("\n");
}

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
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    if (!ImGui_ImplWin32_Init(m_Hwnd)) return false;
    if (!ImGui_ImplDX11_Init(m_Device, m_Context)) return false;
    advancedfx::Message("Overlay: renderer=DX11\n");

    // Register custom ImGui ini settings handler for last-used directories
    if (!ImGui::FindSettingsHandler("HLAEOverlayPaths")) {
        ImGuiSettingsHandler ini;
        ini.TypeName = "HLAEOverlayPaths";
        ini.TypeHash = ImHashStr(ini.TypeName);
        ini.ReadOpenFn = OverlayPaths_ReadOpen;
        ini.ReadLineFn = OverlayPaths_ReadLine;
        ini.WriteAllFn = OverlayPaths_WriteAll;
        ImGui::AddSettingsHandler(&ini);
        ImGuiIO& io2 = ImGui::GetIO();
        if (io2.IniFilename && io2.IniFilename[0]) {
            ImGui::LoadIniSettingsFromDisk(io2.IniFilename);
        }
    }

    // Route Win32 messages to ImGui when visible
    if (!Overlay::Get().GetInputRouter()) {
        auto router = std::make_unique<InputRouter>();
        if (router->Attach(m_Hwnd)) {
            router->SetMessageCallback([](void* hwnd, unsigned int msg, uint64_t wparam, int64_t lparam) -> bool {
                // Feed ImGui first so it updates IO states
                bool consumed = ImGui_ImplWin32_WndProcHandler((HWND)hwnd, msg, (WPARAM)wparam, (LPARAM)lparam) ? true : false;

                // Never consume critical window management messages; always pass to the game.
                switch (msg) {
                    case WM_ENTERSIZEMOVE:
                    case WM_EXITSIZEMOVE:
                    case WM_SIZING:
                    case WM_SIZE:
                    case WM_WINDOWPOSCHANGING:
                    case WM_WINDOWPOSCHANGED:
                    case WM_DISPLAYCHANGE:
                    case WM_DPICHANGED:
                        consumed = false;
                        // Proactively drop backbuffer resources on resize-related messages to avoid blocking engine ResizeBuffers.
                        if (msg == WM_SIZE) {
                            advancedfx::overlay::Overlay::Get().OnDeviceLost();
                        }
                        break;
                    default:
                        break;
                }

                // Right-click passthrough when hovering outside overlay (no capture)
                static bool s_rmbDown = false;
                if (msg == WM_RBUTTONDOWN) s_rmbDown = true;
                if (msg == WM_RBUTTONUP) s_rmbDown = false;
                if (msg == WM_MOUSEMOVE && (wparam & MK_RBUTTON)) s_rmbDown = true; // maintain during drag

                bool overlayVisible = Overlay::Get().IsVisible();
                bool wantCaptureMouse = ImGui::GetIO().WantCaptureMouse;
                bool passThrough = overlayVisible && s_rmbDown && !wantCaptureMouse;
                Overlay::Get().SetRmbPassthroughActive(passThrough);

                // While in RMB passthrough mode, intercept scroll wheel to control mirv camera instead of passing to game
                if (passThrough && msg == WM_MOUSEWHEEL) {
                    int zDelta = GET_WHEEL_DELTA_WPARAM((WPARAM)wparam);
                    int steps = zDelta / WHEEL_DELTA;
                    if (steps != 0) {
                        if (MirvInput* pMirv = Afx_GetMirvInput()) {
                            const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                            const bool ctrlDown  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                            if (ctrlDown) {
                                // Adjust Roll
                                float roll = GetLastCameraRoll();
                                float stepDeg = 1.0f;
                                roll += stepDeg * (float)steps;
                                // Clamp to [-180, 180]
                                if (roll < -180.0f) roll = -180.0f;
                                if (roll >  180.0f) roll =  180.0f;
                                pMirv->SetRz(roll);
                                g_uiRoll = roll; g_uiRollInit = true; // sync UI
                            } else if (shiftDown) {
                                // Adjust FOV
                                float fov = GetLastCameraFov();
                                float stepDeg = 1.0f;
                                fov += stepDeg * (float)steps;
                                if (fov < 1.0f)   fov = 1.0f;
                                if (fov > 179.0f) fov = 179.0f;
                                pMirv->SetFov(fov);
                                g_uiFov = fov; g_uiFovInit = true; // sync UI
                            } else {
                                // Adjust keyboard sensitivity (ksens)
                                double ks = pMirv->GetKeyboardSensitivty();
                                double step = 0.25; // fine-grained
                                ks += step * (double)steps;
                                if (ks < 0.01) ks = 0.01;
                                if (ks > 10.0) ks = 10.0;
                                pMirv->SetKeyboardSensitivity(ks);
                                g_uiKsens = (float)ks; g_uiKsensInit = true; // sync UI
                            }
                        }
                    }
                    return true; // consume wheel
                }

                // In passthrough, swallow Shift/Ctrl so they don't reach the game (used only for our scroll modifiers)
                if (passThrough) {
                    switch (msg) {
                        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
                            WPARAM vk = (WPARAM)wparam;
                            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL)
                                return true; // consume modifiers
                            break;
                        }
                        default: break;
                    }
                }

                if (passThrough) {
                    // Do not consume other inputs: allow the game to receive input while overlay stays open
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

    UpdateBackbufferSize();

    m_Initialized = true;
    return true;
#else
    return false;
#endif
}

void OverlayDx11::Shutdown() {
#ifdef _WIN32
    if (!m_Initialized) return;
    m_Rtv.width = m_Rtv.height = 0;
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
    // Make window non-resizable and auto-size to its content/DPI
    ImGui::Begin("HLAE Overlay", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
    if (ImGui::BeginTabBar("##hlae_tabs")) {
        // Campath
        if (ImGui::BeginTabItem("Campath")) {
            // Enable camera path preview toggle
            extern CCampathDrawer g_CampathDrawer;
            static bool enable_preview = false;
            static bool preview_inited = false;
            if (!preview_inited) { enable_preview = g_CampathDrawer.Draw_get(); preview_inited = true; }
            if (ImGui::Checkbox("Enable camera path preview", &enable_preview)) {
                g_CampathDrawer.Draw_set(enable_preview);
                advancedfx::Message("Overlay: mirv_campath draw enabled %d\n", enable_preview ? 1 : 0);
            }

            // Campath information (if any)
            extern CamPath g_CamPath;
            size_t cpCount = g_CamPath.GetSize();
            if (cpCount > 0) {
                double seconds = 0.0;
                if (cpCount >= 2) seconds = g_CamPath.GetDuration();
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
            {
                static bool s_interpCubic = true;
                const char* interpLabel = s_interpCubic ? "Interp: Cubic" : "Interp: Linear";
                if (ImGui::Button(interpLabel)) {
                    s_interpCubic = !s_interpCubic;
                    if (s_interpCubic) {
                        Afx_ExecClientCmd("mirv_campath edit interp position default; mirv_campath edit interp rotation default; mirv_campath edit interp fov default");
                    } else {
                        Afx_ExecClientCmd("mirv_campath edit interp position linear; mirv_campath edit interp rotation sLinear; mirv_campath edit interp fov linear");
                    }
                }
                // File operations next to Interp button
                ImGui::SameLine();
                {
                    // Persistent file dialogs for load/save
                    static ImGui::FileBrowser s_campathOpenDialog(
                        ImGuiFileBrowserFlags_CloseOnEsc |
                        ImGuiFileBrowserFlags_EditPathString |
                        ImGuiFileBrowserFlags_CreateNewDir
                    );
                    static ImGui::FileBrowser s_campathSaveDialog(
                        ImGuiFileBrowserFlags_CloseOnEsc |
                        ImGuiFileBrowserFlags_EditPathString |
                        ImGuiFileBrowserFlags_CreateNewDir |
                        ImGuiFileBrowserFlags_EnterNewFilename
                    );
                    static bool s_dialogsInit = false;
                    if (!s_dialogsInit) {
                        s_campathOpenDialog.SetTitle("Load Campath");
                        s_campathSaveDialog.SetTitle("Save Campath");
                        s_dialogsInit = true;
                    }

                    // Load Campath button
                    if (ImGui::Button("Load Campath")) {
                        if (!g_OverlayPaths.campathDir.empty())
                            s_campathOpenDialog.SetDirectory(g_OverlayPaths.campathDir);
                        s_campathOpenDialog.Open();
                    }
                    ImGui::SameLine();
                    // Save Campath button
                    if (ImGui::Button("Save Campath")) {
                        if (!g_OverlayPaths.campathDir.empty())
                            s_campathSaveDialog.SetDirectory(g_OverlayPaths.campathDir);
                        s_campathSaveDialog.Open();
                    }

                    // Render dialogs
                    s_campathOpenDialog.Display();
                    s_campathSaveDialog.Display();

                    // Handle selections
                    if (s_campathOpenDialog.HasSelected()) {
                        const std::string path = s_campathOpenDialog.GetSelected().string();
                        char cmd[2048];
                        snprintf(cmd, sizeof(cmd), "mirv_campath load \"%s\"", path.c_str());
                        Afx_ExecClientCmd(cmd);
                        // remember directory
                        try { g_OverlayPaths.campathDir = std::filesystem::path(path).parent_path().string(); } catch(...) {}
                        ImGui::MarkIniSettingsDirty();
                        s_campathOpenDialog.ClearSelected();
                    }
                    if (s_campathSaveDialog.HasSelected()) {
                        const std::string path = s_campathSaveDialog.GetSelected().string();
                        char cmd[2048];
                        snprintf(cmd, sizeof(cmd), "mirv_campath save \"%s\"", path.c_str());
                        Afx_ExecClientCmd(cmd);
                        // remember directory
                        try { g_OverlayPaths.campathDir = std::filesystem::path(path).parent_path().string(); } catch(...) {}
                        ImGui::MarkIniSettingsDirty();
                        s_campathSaveDialog.ClearSelected();
                    }
                }
            }

            // Sequencer toggle
            ImGui::Checkbox("Show Sequencer", &g_ShowSequencer);

            ImGui::EndTabItem();
        }

        // Recording
        if (ImGui::BeginTabItem("Recording")) {
            // Record name (path) and Open recordings folder
            {
                static char s_recName[512] = {0};
                static bool recInit = false;
                static ImGui::FileBrowser dirDialog(
                    ImGuiFileBrowserFlags_SelectDirectory |
                    ImGuiFileBrowserFlags_CloseOnEsc |
                    ImGuiFileBrowserFlags_EditPathString |
                    ImGuiFileBrowserFlags_CreateNewDir
                );
                static bool dialogInit = false;
                if (!dialogInit) { dirDialog.SetTitle("Select record folder"); dialogInit = true; }
                if (!recInit) { const char* rn = AfxStreams_GetRecordNameUtf8(); if (rn) strncpy_s(s_recName, rn, _TRUNCATE); recInit = true; }

                ImGuiStyle& st = ImGui::GetStyle();
                ImGui::TextUnformatted("Record path");
                ImGui::SameLine(0.0f, st.ItemInnerSpacing.x);
                float avail = ImGui::GetContentRegionAvail().x;
                float btnBrowseW = ImGui::CalcTextSize("Browse...").x + st.FramePadding.x * 2.0f;
                float btnSetW    = ImGui::CalcTextSize("Set").x       + st.FramePadding.x * 2.0f;
                float gap1 = st.ItemInnerSpacing.x * 2.0f + 8.0f;
                float gap2 = st.ItemInnerSpacing.x;
                float boxW = avail - (btnBrowseW + btnSetW + gap1 + gap2);
                if (boxW < 150.0f) boxW = 150.0f;
                if (boxW > avail - (btnBrowseW + btnSetW + gap1 + gap2)) boxW = avail - (btnBrowseW + btnSetW + gap1 + gap2);
                if (boxW < 50.0f) boxW = 50.0f;
                ImGui::SetNextItemWidth(boxW);
                ImGui::InputText("##recname", s_recName, sizeof(s_recName));
                ImGui::SameLine(0.0f, gap1);
                if (ImGui::Button("Browse...##recname")) {
                    if (!g_OverlayPaths.recordBrowseDir.empty())
                        dirDialog.SetDirectory(g_OverlayPaths.recordBrowseDir);
                    dirDialog.Open();
                }
                ImGui::SameLine(0.0f, gap2);
                if (ImGui::Button("Set##recname")) {
                    AfxStreams_SetRecordNameUtf8(s_recName);
                    advancedfx::Message("Overlay: mirv_streams record name set.\n");
                }
                dirDialog.Display();
                if (dirDialog.HasSelected()) {
                    const std::string selected = dirDialog.GetSelected().string();
                    strncpy_s(s_recName, selected.c_str(), _TRUNCATE);
                    AfxStreams_SetRecordNameUtf8(s_recName);
                    advancedfx::Message("Overlay: mirv_streams record name set via browser.\n");
                    // remember directory
                    g_OverlayPaths.recordBrowseDir = s_recName;
                    ImGui::MarkIniSettingsDirty();
                    dirDialog.ClearSelected();
                }
            }

            if (ImGui::Button("Open recordings folder")) {
                const char* recUtf8 = AfxStreams_GetRecordNameUtf8();
                std::wstring wPath;
                if (recUtf8 && recUtf8[0]) {
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, recUtf8, -1, nullptr, 0);
                    if (wlen > 0) { wPath.resize((size_t)wlen - 1); MultiByteToWideChar(CP_UTF8, 0, recUtf8, -1, &wPath[0], wlen); }
                }
                if (wPath.empty()) {
                    const wchar_t* takeDir = AfxStreams_GetTakeDir();
                    if (takeDir && takeDir[0]) wPath = takeDir;
                }
                if (!wPath.empty()) {
                    HINSTANCE r = ShellExecuteW(nullptr, L"open", wPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    if ((INT_PTR)r <= 32) { ShellExecuteW(nullptr, L"open", L"explorer.exe", wPath.c_str(), nullptr, SW_SHOWNORMAL); }
                } else {
                    advancedfx::Warning("Overlay: No recordings folder set. Use 'mirv_streams record name <path>'.\n");
                }
            }

            // Recording FPS override and screen enable
            {
                static char s_fpsText[64] = {0};
                static bool fpsInit = false;
                if (!fpsInit) {
                    if (AfxStreams_GetOverrideFps()) snprintf(s_fpsText, sizeof(s_fpsText), "%.2f", AfxStreams_GetOverrideFpsValue());
                    else s_fpsText[0] = 0; fpsInit = true;
                }
                ImGuiStyle& st2 = ImGui::GetStyle();
                float fiveChars = ImGui::CalcTextSize("00000").x + st2.FramePadding.x * 2.0f + 2.0f;
                ImGui::SetNextItemWidth(fiveChars);
                ImGui::InputText("Record FPS", s_fpsText, sizeof(s_fpsText));
                ImGui::SameLine(0.0f, st2.ItemInnerSpacing.x);
                if (ImGui::Button("Apply FPS")) {
                    if (s_fpsText[0] == 0) { AfxStreams_SetOverrideFpsDefault(); advancedfx::Message("Overlay: mirv_streams record fps default\n"); }
                    else {
                        float v = (float)atof(s_fpsText);
                        if (v > 0.0f) { AfxStreams_SetOverrideFpsValue(v); advancedfx::Message("Overlay: mirv_streams record fps %.2f\n", v); }
                        else { advancedfx::Warning("Overlay: Invalid FPS value.\n"); }
                    }
                }
                bool screenEnabled = AfxStreams_GetRecordScreenEnabled();
                if (ImGui::Checkbox("Record screen enabled", &screenEnabled)) { AfxStreams_SetRecordScreenEnabled(screenEnabled); }
                ImGui::SameLine();
                {
                    static bool s_toggleXray = true;
                    const char* xrayLabel = s_toggleXray ? "Disable X-Ray" : "Enable X-Ray";
                    if (ImGui::Button(xrayLabel)) {
                        s_toggleXray = !s_toggleXray;
                        if (s_toggleXray) {
                            Afx_ExecClientCmd("spec_show_xray 1");
                        } else {
                            Afx_ExecClientCmd("spec_show_xray 0");
                        }
                    }
                }
            }

            static bool s_hideHud = false;
            if (ImGui::Checkbox("Hide HUD", &s_hideHud)) {
                if (s_hideHud) Afx_ExecClientCmd("cl_drawhud 0"); else Afx_ExecClientCmd("cl_drawhud 1");
            }
            ImGui::SameLine();
            static bool s_onlyDeathnotices = false;
            if (ImGui::Checkbox("Only Deathnotices", &s_onlyDeathnotices)) {
                if (s_onlyDeathnotices) Afx_ExecClientCmd("cl_draw_only_deathnotices 1"); else Afx_ExecClientCmd("cl_draw_only_deathnotices 0");
            }

            ImGui::Separator();
            ImGui::TextUnformatted("FFmpeg profiles");
            ImGui::SameLine();
            static const char* s_selectedProfile = "Select profile...";
            if (ImGui::BeginCombo("##ffmpeg_profiles", s_selectedProfile)) {
                if (ImGui::Selectable("TGA Sequence")) { s_selectedProfile = "TGA Sequence"; Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings afxClassic;echo [Current Record Setting];echo afxClassic - �-��?Y .tga �>�%Ά�?�^-)" ); }
                if (ImGui::Selectable("ProRes 4444")) { s_selectedProfile = "ProRes 4444"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg p0  "-c:v prores  -profile:v 4 {QUOTE}{AFX_STREAM_PATH}\video.mov{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings p0  ;echo [Current Record Setting];echo p0 - ProRes 4444)" ); }
                if (ImGui::Selectable("ProRes 422 HQ")) { s_selectedProfile = "ProRes 422 HQ"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg phq "-c:v prores  -profile:v 3 -pix_fmt yuv422p10le {QUOTE}{AFX_STREAM_PATH}\video.mov{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings phq ;echo [Current Record Setting];echo phq - ProRes 422 HQ)" ); }
                if (ImGui::Selectable("ProRes 422")) { s_selectedProfile = "ProRes 422"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg p1  "-c:v prores  -profile:v 2 -pix_fmt yuv422p10le {QUOTE}{AFX_STREAM_PATH}\video.mov{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings p1  ;echo [Current Record Setting];echo p1 - ProRes 422)" ); }
                if (ImGui::Selectable("x264 Lossless")) { s_selectedProfile = "x264 Lossless"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg c0  "-c:v libx264 -preset 0 -qp  0  -g 120 -keyint_min 1 -pix_fmt yuv422p10le {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings c0  ;echo [Current Record Setting];echo c0 - x264 �-��?Y)" ); }
                if (ImGui::Selectable("x264 HQ")) { s_selectedProfile = "x264 HQ"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg c1  "-c:v libx264 -preset 1 -crf 4  -qmax 20 -g 120 -keyint_min 1 -pix_fmt yuv420p -x264-params ref=3:me=hex:subme=3:merange=12:b-adapt=1:aq-mode=2:aq-strength=0.9:no-fast-pskip=1 {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings c1  ;echo [Current Record Setting];echo c1 - x264 ��~�"����)" ); }
                if (ImGui::Selectable("x265 Lossless")) { s_selectedProfile = "x265 Lossless"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg he0  "-c:v libx265 -x265-params no-sao=1 -preset 0 -lossless -g 120 -keyint_min 1 -pix_fmt yuv422p {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings he0 ;echo [Current Record Setting];echo he0 - x265 �-��?Y)" ); }
                if (ImGui::Selectable("x265 HQ")) { s_selectedProfile = "x265 HQ"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg he1  "-c:v libx265 -x265-params no-sao=1 -preset 1 -crf 8  -qmax 20 -g 120 -keyint_min 1 -pix_fmt yuv422p {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings he1 ;echo [Current Record Setting];echo he1 - x265 ��~�"����)" ); }
                ImGui::EndCombo();
            }
            ImGui::TextUnformatted("Depth Stream");
            ImGui::SameLine();
            static const char* s_selectedDepthProfile = "Disabled";
            if (ImGui::BeginCombo("##depth_profiles", s_selectedDepthProfile)) {
                if (ImGui::Selectable("Disabled")) { s_selectedDepthProfile = "Disabled"; Afx_ExecClientCmd(R"(mirv_streams remove ddepth; mirv_streams remove exr)" ); }
                if (ImGui::Selectable("Default")) { s_selectedDepthProfile = "Default"; Afx_ExecClientCmd(R"(mirv_streams remove ddepth; mirv_streams remove exr; mirv_streams add depth ddepth)" ); }
                if (ImGui::Selectable("EXR")) { s_selectedDepthProfile = "EXR"; Afx_ExecClientCmd(R"(mirv_streams remove ddepth; mirv_streams remove exr; mirv_streams add depth exr; mirv_streams edit exr depthMode linear; mirv_streams edit exr depthVal 0; mirv_streams edit exr depthValMax 0; mirv_streams edit exr depthChannels gray; mirv_streams edit exr captureType depthF; mirv_streams edit exr settings afxClassic);echo [Current Depth Setting];echo EXR)" ); }
                if (ImGui::Selectable("ProRes 4444")) { s_selectedDepthProfile = "ProRes 4444"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg p0  "-c:v prores  -profile:v 4 {QUOTE}{AFX_STREAM_PATH}\video.mov{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams remove ddepth; mirv_streams remove exr;mirv_streams add depth ddepth; mirv_streams edit ddepth settings p0  ;echo [Current Depth Setting];echo p0 - ProRes 4444)" ); }
                if (ImGui::Selectable("ProRes 422 HQ")) { s_selectedDepthProfile = "ProRes 422 HQ"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg phq "-c:v prores  -profile:v 3 -pix_fmt yuv422p10le {QUOTE}{AFX_STREAM_PATH}\video.mov{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams remove ddepth; mirv_streams remove exr;mirv_streams add depth ddepth; mirv_streams edit ddepth settings phq ;echo [Current Depth Setting];echo phq - ProRes 422 HQ)" ); }
                if (ImGui::Selectable("ProRes 422")) { s_selectedDepthProfile = "ProRes 422"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg p1  "-c:v prores  -profile:v 2 -pix_fmt yuv422p10le {QUOTE}{AFX_STREAM_PATH}\video.mov{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams remove ddepth; mirv_streams remove exr;mirv_streams add depth ddepth; mirv_streams edit ddepth settings p1  ;echo [Current Depth Setting];echo p1 - ProRes 422)" ); }
                if (ImGui::Selectable("x264 Lossless")) { s_selectedDepthProfile = "x264 Lossless"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg c0  "-c:v libx264 -preset 0 -qp  0  -g 120 -keyint_min 1 -pix_fmt yuv422p10le {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams remove ddepth; mirv_streams remove exr;mirv_streams add depth ddepth; mirv_streams edit ddepth settings c0  ;echo [Current Depth Setting];echo c0 - x264 �-��?Y)" ); }
                if (ImGui::Selectable("x264 HQ")) { s_selectedDepthProfile = "x264 HQ"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg c1  "-c:v libx264 -preset 1 -crf 4  -qmax 20 -g 120 -keyint_min 1 -pix_fmt yuv420p -x264-params ref=3:me=hex:subme=3:merange=12:b-adapt=1:aq-mode=2:aq-strength=0.9:no-fast-pskip=1 {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams remove ddepth; mirv_streams remove exr;mirv_streams add depth ddepth; mirv_streams edit ddepth settings c1  ;echo [Current Depth Setting];echo c1 - x264 ��~�"����)" ); }
                if (ImGui::Selectable("x265 Lossless")) { s_selectedDepthProfile = "x265 Lossless"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg he0  "-c:v libx265 -x265-params no-sao=1 -preset 0 -lossless -g 120 -keyint_min 1 -pix_fmt yuv422p {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams remove ddepth; mirv_streams remove exr;mirv_streams add depth ddepth; mirv_streams edit ddepth settings he0 ;echo [Current Depth Setting];echo he0 - x265 �-��?Y)" ); }
                if (ImGui::Selectable("x265 HQ")) { s_selectedDepthProfile = "x265 HQ"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg he1  "-c:v libx265 -x265-params no-sao=1 -preset 1 -crf 8  -qmax 20 -g 120 -keyint_min 1 -pix_fmt yuv422p {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams remove ddepth; mirv_streams remove exr;mirv_streams add depth ddepth; mirv_streams edit ddepth settings he1 ;echo [Current Depth Setting];echo he1 - x265 ��~�"����)" ); }
                ImGui::EndCombo();
            }

            if (ImGui::Button("Start Recording")) { Afx_ExecClientCmd("demo_resume"); Afx_ExecClientCmd("mirv_streams record start"); }
            ImGui::SameLine();
            if (ImGui::Button("End Recording")) {
                Afx_ExecClientCmd("demo_pause");
                Afx_ExecClientCmd("mirv_streams record end");
                //AfxStreams_RecordEnd();
            }

            ImGui::EndTabItem();
        }

        // Settings
        if (ImGui::BeginTabItem("Settings")) {
            // FPS readout
            ImGuiIO& io = ImGui::GetIO();
            ImGui::Text("FPS: %.1f", io.Framerate);

            // UI scale (DPI) controls
            float uiScaleTmp = g_UiScale;
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("000.00x").x + ImGui::GetStyle().FramePadding.x * 6.0f);
            if (ImGui::SliderFloat("UI scale", &uiScaleTmp, 0.50f, 2.00f, "%.2fx", ImGuiSliderFlags_AlwaysClamp)) g_UiScale = uiScaleTmp;
            ImGui::SameLine(); if (ImGui::SmallButton("90%"))  g_UiScale = 0.90f;
            ImGui::SameLine(); if (ImGui::SmallButton("100%")) g_UiScale = 1.00f;
            ImGui::SameLine(); if (ImGui::SmallButton("125%")) g_UiScale = 1.25f;
            ImGui::SameLine(); if (ImGui::SmallButton("150%")) g_UiScale = 1.50f;
            ImGui::SameLine(); if (ImGui::SmallButton("200%")) g_UiScale = 2.00f;
            io.FontGlobalScale = g_UiScale;

            // Toggle overlay console
            ImGui::Checkbox("Show Console", &g_ShowOverlayConsole);

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }


    ImGui::End();

    // Sequencer window (ImGui Neo Sequencer)
    if (g_ShowSequencer) {
        // Sequencer: horizontally resizable; adjust height to content each frame
        ImGui::SetNextWindowSize(ImVec2(720, 260), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("HLAE Sequencer", &g_ShowSequencer, ImGuiWindowFlags_NoCollapse)) {
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

            // Add extra space for per-keyframe labels drawn below the markers.
            // Account for font global scale since layout metrics don't grow with it.
            float labelExtra   = ImGui::GetTextLineHeightWithSpacing();
            float fontScale    = ImMax(1.0f, ImGui::GetIO().FontGlobalScale);
            float desiredSeqH  = headerH + trackCount * trackRowH + margins + labelExtra * fontScale;

            // Clamp: at least 100 px, at most what’s left after the controls row
            float minSeqH      = 85.0f * fontScale; // ensure room for labels at higher DPI
            float maxSeqHAvail = ImMax(minSeqH, ImMax(55.0f, availY - btnRowH));
            float seqH         = ImClamp(desiredSeqH, minSeqH, maxSeqHAvail);

            // Use dynamic sequencer height so labels have space across DPI scales
            ImGui::BeginChild("seq_child", ImVec2(0.0f, seqH), false); // scrolling lives inside this child
            {
                const ImVec2 seqSize(ImGui::GetContentRegionAvail().x, 0.0f);
                const ImGuiNeoSequencerFlags seqFlags =
                    ImGuiNeoSequencerFlags_AlwaysShowHeader |
                    ImGuiNeoSequencerFlags_EnableSelection |
                    ImGuiNeoSequencerFlags_Selection_EnableDragging;
                if (ImGui::BeginNeoSequencer("##demo_seq", &s_seqFrame, &s_seqStart, &s_seqEnd, seqSize, seqFlags)) {
                    // Campath timeline: populate from g_CamPath keyframes
                    
                    extern CamPath g_CamPath;
                    struct KfCache {
                        bool valid = false;
                        std::vector<int32_t> ticks;   // demo ticks
                        std::vector<double> times;    // relative seconds
                        std::vector<CamPathValue> values;
                    };
                    static KfCache s_cache;
                    static bool s_wasDragging = false;

                    double curTime = g_MirvTime.curtime_get();
                    double currentDemoTime = 0.0;
                    bool haveDemoTime = g_MirvTime.GetCurrentDemoTime(currentDemoTime);
                    float ipt = g_MirvTime.interval_per_tick_get();
                    if (ipt <= 0.0f) ipt = 1.0f / 64.0f;

                    auto rebuild_cache_from_campath = [&]() {
                        s_cache.valid = true;
                        s_cache.ticks.clear();
                        s_cache.times.clear();
                        s_cache.values.clear();
                        s_cache.ticks.reserve(g_CamPath.GetSize());
                        s_cache.times.reserve(g_CamPath.GetSize());
                        s_cache.values.reserve(g_CamPath.GetSize());
                        for (CamPathIterator it = g_CamPath.GetBegin(); it != g_CamPath.GetEnd(); ++it) {
                            double tRel = it.GetTime();
                            s_cache.times.push_back(tRel);
                            s_cache.values.push_back(it.GetValue());
                            double clientTarget = g_CamPath.GetOffset() + tRel;
                            int tick = 0;
                            if (haveDemoTime) {
                                double demoTarget = clientTarget - (curTime - currentDemoTime);
                                tick = (int)llround(demoTarget / (double)ipt);
                            } else {
                                tick = (int)llround(clientTarget / (double)ipt);
                            }
                            if (tick < 0) tick = 0;
                            s_cache.ticks.push_back(tick);
                        }
                    };
                    if (g_SequencerNeedsRefresh) {
                        rebuild_cache_from_campath();
                        g_SequencerNeedsRefresh = false;
                        s_cache.valid = true;
                        // Also reset the drag sentinel so the next block doesn't think a user drag just ended
                        s_wasDragging = ImGui::NeoIsDraggingSelection();
                    }

                    // When no refresh is pending, do the lazy refresh (only when not dragging)
                    if (!ImGui::NeoIsDraggingSelection()) {
                        if (!s_cache.valid || s_cache.ticks.size() != g_CamPath.GetSize())
                            rebuild_cache_from_campath();
                    }

                    bool open = true;
                    if (ImGui::BeginNeoTimelineEx("Campath", &open, ImGuiNeoTimelineFlags_None)) {

                        int rightClickedIndex = -1;
                        static CampathCtx s_ctxMenuPending; // holds context menu target until action chosen
                        // Collect selection while rendering keyframes
                        static std::vector<int> s_lastUiSelection; // indices from UI last frame
                        std::vector<int> curUiSelection;
                        curUiSelection.reserve(s_cache.ticks.size());
                        for (int i = 0; i < (int)s_cache.ticks.size(); ++i) {
                            ImGui::NeoKeyframe(&s_cache.ticks[i]);
                            if (ImGui::IsNeoKeyframeSelected()) curUiSelection.push_back(i);
                            if (ImGui::IsNeoKeyframeRightClicked())
                                rightClickedIndex = i;
                        }
                        // Sync UI selection to mirv_campath selection when selection state stabilizes
                        if (!ImGui::NeoIsSelecting() && !ImGui::NeoIsDraggingSelection()) {
                            auto normalized = [](std::vector<int>& v){ std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end()); };
                            normalized(curUiSelection);
                            std::vector<int> last = s_lastUiSelection; normalized(last);
                            if (last != curUiSelection) {
                                // Update in-memory selection immediately and mirror to engine
                                g_CamPath.SelectNone();
                                std::string cmd = "mirv_campath select none;";
                                int rangeStart = -1, prev = -1000000;
                                for (int idx : curUiSelection) {
                                    if (rangeStart < 0) { rangeStart = prev = idx; continue; }
                                    if (idx == prev + 1) { prev = idx; continue; }
                                    g_CamPath.SelectAdd((size_t)rangeStart, (size_t)prev);
                                    char tmp[128];
                                    snprintf(tmp, sizeof(tmp), " mirv_campath select add #%d #%d;", rangeStart, prev);
                                    cmd += tmp;
                                    rangeStart = prev = idx;
                                }
                                if (rangeStart >= 0) {
                                    g_CamPath.SelectAdd((size_t)rangeStart, (size_t)prev);
                                    char tmp[128];
                                    snprintf(tmp, sizeof(tmp), " mirv_campath select add #%d #%d;", rangeStart, prev);
                                    cmd += tmp;
                                }
                                Afx_ExecClientCmd(cmd.c_str());
                                g_SequencerNeedsRefresh = true;
                                s_lastUiSelection = curUiSelection;
                            }
                        }
                        if (rightClickedIndex >= 0) {
                            // Do not immediately activate gizmo; defer to context menu action
                            s_ctxMenuPending.active = true;
                            s_ctxMenuPending.time = s_cache.times[(size_t)rightClickedIndex];
                            s_ctxMenuPending.value = s_cache.values[(size_t)rightClickedIndex];
                            ImGui::OpenPopup("campath_kf_ctx");
                            ImGui::SetNextWindowPos(ImGui::GetMousePos());
                            ImGui::SetNextWindowSizeConstraints(ImVec2(120,0), ImVec2(300,FLT_MAX));
                        }
                        if (ImGui::BeginPopup("campath_kf_ctx")) {
                            bool doRemove = false, doGet = false, doSet = false;
                            if (ImGui::MenuItem("Remove")) doRemove = true;
                            if (ImGui::MenuItem("Get")) doGet = true;
                            if (ImGui::MenuItem("Set")) doSet = true;
                            if (ImGui::MenuItem("Edit")) {
                                if (s_ctxMenuPending.active) {
                                    // Activate gizmo edit for the pending keyframe
                                    g_LastCampathCtx = s_ctxMenuPending;
                                }
                                // Close the popup
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();

                            if (s_ctxMenuPending.active) {
                                extern CCampathDrawer g_CampathDrawer;
                                bool prevDraw = g_CampathDrawer.Draw_get();
                                if (doRemove) {
                                    g_CampathDrawer.Draw_set(false);
                                    g_CamPath.Remove(s_ctxMenuPending.time);
                                    g_CampathDrawer.Draw_set(prevDraw);
                                    s_ctxMenuPending.active = false;
                                    g_SequencerNeedsRefresh = true;
                                    const double epsTime = 1e-9;
                                    if (g_LastCampathCtx.active && fabs(g_LastCampathCtx.time - s_ctxMenuPending.time) < epsTime) {
                                        g_LastCampathCtx.active = false;
                                    }
                                }
                                if (doGet) {
                                    if (MirvInput* pMirv = Afx_GetMirvInput()) {
                                        pMirv->SetCameraControlMode(true);
                                        const CamPathValue& v = s_ctxMenuPending.value;
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
                                    s_ctxMenuPending.active = false;
                                }
                                if (doSet) {
                                    double cx, cy, cz, rX, rY, rZ; float cfov;
                                    Afx_GetLastCameraData(cx, cy, cz, rX, rY, rZ, cfov);
                                    CamPathValue newVal(cx, cy, cz, rX, rY, rZ, cfov);
                                    g_CampathDrawer.Draw_set(false);
                                    g_CamPath.Remove(s_ctxMenuPending.time);
                                    g_CamPath.Add(s_ctxMenuPending.time, newVal);
                                    g_CampathDrawer.Draw_set(prevDraw);
                                    s_ctxMenuPending.active = false;
                                    g_SequencerNeedsRefresh = true;
                                }
                            }
                        }
                        // Commit moved keys on drag end (transaction-style: rebuild the whole path)
                        bool draggingNow = ImGui::NeoIsDraggingSelection();
                        if (s_wasDragging && !draggingNow) {
                            // 1) Snapshot authoritative values (in current index order) directly from g_CamPath
                            std::vector<CamPathValue> liveValues;
                            liveValues.reserve(g_CamPath.GetSize());
                            // Also capture current selection indices and gizmo-target index (by time) for remap
                            std::vector<int> prevSelectedIdx;
                            int prevGizmoIdx = -1;
                            int curIdx = 0;
                            const double timeEps = 1e-9;
                            for (CamPathIterator it = g_CamPath.GetBegin(); it != g_CamPath.GetEnd(); ++it, ++curIdx) {
                                liveValues.push_back(it.GetValue());
                                if (it.GetValue().Selected) prevSelectedIdx.push_back(curIdx);
                                if (g_LastCampathCtx.active && fabs(it.GetTime() - g_LastCampathCtx.time) < timeEps)
                                    prevGizmoIdx = curIdx;
                            }

                            // 2) Convert cached ticks to target times (seconds), keep original index for remapping
                            struct TmpKey { double t; CamPathValue v; int origIndex; };
                            std::vector<TmpKey> newKeys;
                            newKeys.reserve(s_cache.ticks.size());
                            for (size_t i = 0; i < s_cache.ticks.size(); ++i) {
                                const double newDemoTime  = (double)s_cache.ticks[i] * (double)ipt;
                                const double clientTarget = haveDemoTime
                                    ? newDemoTime + (curTime - currentDemoTime)
                                    : newDemoTime;
                                double newRelTime = clientTarget - g_CamPath.GetOffset();

                                // Use authoritative value (aligned by index) rather than stale s_cache.values[i]
                                CamPathValue val = (i < liveValues.size()) ? liveValues[i] : s_cache.values[i];
                                newKeys.push_back({ newRelTime, val, (int)i });
                            }

                            // 3) Sort + nudge identical times
                            std::sort(newKeys.begin(), newKeys.end(),
                                [](const TmpKey& a, const TmpKey& b){ return a.t < b.t; });
                            const double eps = 1e-6;
                            for (size_t i = 1; i < newKeys.size(); ++i) {
                                if (fabs(newKeys[i].t - newKeys[i-1].t) < eps)
                                    newKeys[i].t = newKeys[i-1].t + eps;
                            }

                            // 4) Rebuild atomically – force a true full-clear independent of current selection
                            extern CCampathDrawer g_CampathDrawer;
                            const bool prevDraw = g_CampathDrawer.Draw_get();
                            g_CampathDrawer.Draw_set(false);
                            const bool prevEnabled = g_CamPath.Enabled_get();
                            g_CamPath.Enabled_set(false);

                            // Ensure Clear() path clears all by neutralizing selection first
                            g_CamPath.SelectNone();
                            g_CamPath.Clear();
                            for (const auto& k : newKeys) g_CamPath.Add(k.t, k.v);

                            // Restore selection (map old indices -> new indices via origIndex)
                            if (!prevSelectedIdx.empty()) {
                                // Build mapping origIndex -> newIndex
                                std::vector<int> newSelected;
                                newSelected.reserve(prevSelectedIdx.size());
                                for (int oldIdx : prevSelectedIdx) {
                                    for (size_t ni = 0; ni < newKeys.size(); ++ni) {
                                        if (newKeys[ni].origIndex == oldIdx) { newSelected.push_back((int)ni); break; }
                                    }
                                }
                                // Coalesce contiguous ranges and select
                                std::sort(newSelected.begin(), newSelected.end());
                                newSelected.erase(std::unique(newSelected.begin(), newSelected.end()), newSelected.end());
                                int rangeStart = -1, prev = -1000000;
                                // Update in-memory selection immediately
                                g_CamPath.SelectNone();
                                // Mirror selection to engine via mirv_campath commands
                                std::string cmd = "mirv_campath select none;";
                                for (int idx : newSelected) {
                                    if (rangeStart < 0) { rangeStart = prev = idx; continue; }
                                    if (idx == prev + 1) { prev = idx; continue; }
                                    g_CamPath.SelectAdd((size_t)rangeStart, (size_t)prev);
                                    char tmp[128];
                                    snprintf(tmp, sizeof(tmp), " mirv_campath select add #%d #%d;", rangeStart, prev);
                                    cmd += tmp;
                                    rangeStart = prev = idx;
                                }
                                if (rangeStart >= 0) {
                                    g_CamPath.SelectAdd((size_t)rangeStart, (size_t)prev);
                                    char tmp[128];
                                    snprintf(tmp, sizeof(tmp), " mirv_campath select add #%d #%d;", rangeStart, prev);
                                    cmd += tmp;
                                }
                                Afx_ExecClientCmd(cmd.c_str());
                            }

                            g_CamPath.Enabled_set(prevEnabled);
                            g_CampathDrawer.Draw_set(prevDraw);

                            // 5) Update cache times and mark invalid so the next frame refreshes from the authoritative state
                            for (size_t i = 0; i < newKeys.size(); ++i) s_cache.times[i] = newKeys[i].t;
                            s_cache.valid = false;

                            // 6) Keep ImGuizmo target alive across the move by remapping to its new index
                            if (g_LastCampathCtx.active && prevGizmoIdx >= 0) {
                                int newIdx = -1;
                                for (size_t ni = 0; ni < newKeys.size(); ++ni) {
                                    if (newKeys[ni].origIndex == prevGizmoIdx) { newIdx = (int)ni; break; }
                                }
                                if (newIdx >= 0) {
                                    // Fetch updated time/value from authoritative path
                                    int idx = 0;
                                    for (CamPathIterator it = g_CamPath.GetBegin(); it != g_CamPath.GetEnd(); ++it, ++idx) {
                                        if (idx == newIdx) {
                                            g_LastCampathCtx.time  = it.GetTime();
                                            g_LastCampathCtx.value = it.GetValue();
                                            break;
                                        }
                                    }
                                } else {
                                    // Could not remap – deactivate to avoid editing a stale key
                                    g_LastCampathCtx.active = false;
                                }
                            }
                        }
                        s_wasDragging = draggingNow;
                        ImGui::EndNeoTimeLine();
                    }
                    
                    ImGui::EndNeoSequencer();
                }
            }
            ImGui::EndChild();
            if (ImGui::SmallButton("Toggle Pause")) {
                Afx_ExecClientCmd("demo_togglepause");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("0.2x")) {
                Afx_ExecClientCmd("demo_timescale 0.2");
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
            if (ImGui::SmallButton("5x")) {
                Afx_ExecClientCmd("demo_timescale 5");
            }
            ImGui::SameLine();
            ImGui::Text("Start: %d  End: %d  Current: %d",
                        (int)s_seqStart, (int)s_seqEnd, (int)s_seqFrame);
            ImGui::SameLine();
            if (ImGui::SmallButton(g_PreviewFollow ? "Detach Preview" : "Follow Preview")) {
                g_PreviewFollow = !g_PreviewFollow;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Add keyframe at preview")) {
                // Compute preview time based on normalized slider and current campath bounds
                if (g_CamPath.GetSize() >= 2 && g_CamPath.CanEval()) {
                    double tMin = g_CamPath.GetLowerBound();
                    double tMax = g_CamPath.GetUpperBound();
                    if (tMax <= tMin) tMax = tMin + 1.0;
                    double tEval = tMin + (double)g_PreviewNorm * (tMax - tMin);
                    double cx, cy, cz, rX, rY, rZ; float cfov;
                    Afx_GetLastCameraData(cx, cy, cz, rX, rY, rZ, cfov);
                    CamPathValue nv(cx, cy, cz, rX, rY, rZ, cfov);
                    extern CCampathDrawer g_CampathDrawer;
                    bool prevDraw = g_CampathDrawer.Draw_get();
                    g_CampathDrawer.Draw_set(false);
                    g_CamPath.Add(tEval, nv);
                    g_CampathDrawer.Draw_set(prevDraw);
                    g_SequencerNeedsRefresh = true;
                    advancedfx::Message("Overlay: mirv_campath add at preview (t=%.3f)\n", tEval);
                } else {
                    advancedfx::Warning("Overlay: Not enough campath keyframes to add at preview.\n");
                }
            }
            // Preview slider (scrub camera along campath without playing demo)
            {
                size_t cpCountPrev = g_CamPath.GetSize();
                if (cpCountPrev >= 2 && g_CamPath.CanEval()) {
                    // Normalized position [0..1] across first..last key time
                    static bool  s_previewActive = false;
                    static bool  s_prevCamEnabled = false;
                    static bool  s_havePrevCamPose = false;
                    static double s_prevX = 0.0, s_prevY = 0.0, s_prevZ = 0.0,
                                  s_prevRx = 0.0, s_prevRy = 0.0, s_prevRz = 0.0;
                    static float  s_prevFov = 90.0f;

                    double tMin = g_CamPath.GetLowerBound();
                    double tMax = g_CamPath.GetUpperBound();
                    if (tMax <= tMin) tMax = tMin + 1.0;

                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if (ImGui::SliderFloat("##previewcampath", &g_PreviewNorm, 0.0f, 1.0f, "%.3f")) {
                        // value changed; if actively dragging we'll update camera below
                    }

                    bool activeNow = ImGui::IsItemActive();
                    bool activated = ImGui::IsItemActivated();
                    bool deactivated = ImGui::IsItemDeactivated();

                    if (activated) {
                        s_previewActive = true;
                        if (MirvInput* pMirv = Afx_GetMirvInput()) {
                            s_prevCamEnabled = pMirv->GetCameraControlMode();
                            // If user was already in Mirv Camera mode, snapshot current pose to restore later
                            s_havePrevCamPose = false;
                            if (s_prevCamEnabled) {
                                Afx_GetLastCameraData(s_prevX, s_prevY, s_prevZ, s_prevRx, s_prevRy, s_prevRz, s_prevFov);
                                s_havePrevCamPose = true;
                            }
                            pMirv->SetCameraControlMode(true);
                        }
                    }

                    if (activeNow) {
                        double tEval = tMin + (double)g_PreviewNorm * (tMax - tMin);
                        CamPathValue v = g_CamPath.Eval(tEval);
                        if (MirvInput* pMirv = Afx_GetMirvInput()) {
                            pMirv->SetTx((float)v.X);
                            pMirv->SetTy((float)v.Y);
                            pMirv->SetTz((float)v.Z);
                            using namespace Afx::Math;
                            QEulerAngles ea = v.R.ToQREulerAngles().ToQEulerAngles();
                            pMirv->SetRx((float)ea.Pitch);
                            pMirv->SetRy((float)ea.Yaw);
                            pMirv->SetRz((float)ea.Roll);
                            pMirv->SetFov((float)v.Fov);
                        }
                    }

                    if (deactivated && s_previewActive) {
                        if (MirvInput* pMirv = Afx_GetMirvInput()) {
                            // If we started in Mirv Camera mode and user chose Detach, restore the previous pose
                            if (!g_PreviewFollow && s_prevCamEnabled && s_havePrevCamPose) {
                                pMirv->SetTx((float)s_prevX);
                                pMirv->SetTy((float)s_prevY);
                                pMirv->SetTz((float)s_prevZ);
                                pMirv->SetRx((float)s_prevRx);
                                pMirv->SetRy((float)s_prevRy);
                                pMirv->SetRz((float)s_prevRz);
                                pMirv->SetFov((float)s_prevFov);
                            }
                            pMirv->SetCameraControlMode(s_prevCamEnabled);
                        }
                        s_previewActive = false;
                    }
                }
            }

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



            // Auto-height: shrink/grow to fit current content while preserving user-set width
            {
                ImVec2 cur = ImGui::GetWindowSize();
                float remain = ImGui::GetContentRegionAvail().y; // remaining vertical space
                float desired = cur.y - remain;
                // Optional: clamp to a sensible minimum
                float min_h = ImGui::GetFrameHeightWithSpacing() * 6.0f;
                if (desired < min_h) desired = min_h;
                ImGui::SetWindowSize(ImVec2(cur.x, desired));
            }
            ImGui::End();
        }
    }

    // Mirv input camera controls/indicator
    if (MirvInput* pMirv = Afx_GetMirvInput()) {
        // Mirv Camera: horizontally resizable; adjust height to content each frame
        ImGui::Begin("Mirv Camera");
        bool camEnabled = pMirv->GetCameraControlMode();
        ImGui::Text("Mirv Camera (C): %s", camEnabled ? "enabled" : "disabled");
        ImGui::SameLine();
        if (ImGui::Button(camEnabled ? "Disable" : "Enable")) {
            pMirv->SetCameraControlMode(!camEnabled);
        }
        if (ImGui::GetActiveID() == 0) {
            if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
                pMirv->SetCameraControlMode(!camEnabled);
            }
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
        // FOV slider (synced with RMB+wheel in passthrough)
        if (!g_uiFovInit) { g_uiFov = GetLastCameraFov(); g_uiFovDefault = g_uiFov; g_uiFovInit = true; }
        {
            float tmp = g_uiFov;
            bool changed = ImGui::SliderFloat("FOV", &tmp, 1.0f, 179.0f, "%.1f deg");
            bool reset = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
            if (reset) {
                g_uiFov = g_uiFovDefault;
                ImGui::ClearActiveID();
                pMirv->SetFov(g_uiFov);
            } else if (changed) {
                g_uiFov = tmp;
                pMirv->SetFov(g_uiFov);
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
        if (!g_uiRollInit) { g_uiRoll = GetLastCameraRoll(); g_uiRollDefault = g_uiRoll; g_uiRollInit = true; }
        {
            float tmp = g_uiRoll;
            bool changed = ImGui::SliderFloat("Roll", &tmp, -180.0f, 180.0f, "%.1f deg");
            bool reset = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
            if (reset) {
                g_uiRoll = g_uiRollDefault;
                ImGui::ClearActiveID();
                pMirv->SetRz(g_uiRoll);
            } else if (changed) {
                g_uiRoll = tmp;
                pMirv->SetRz(g_uiRoll);
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
        if (!g_uiKsensInit) { g_uiKsens = (float)pMirv->GetKeyboardSensitivty(); g_uiKsensDefault = g_uiKsens; g_uiKsensInit = true; }
        {
            float tmp = g_uiKsens;
            bool changed = ImGui::SliderFloat("ksens", &tmp, 0.01f, 10.0f, "%.2f");
            bool reset = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
            if (reset) {
                g_uiKsens = g_uiKsensDefault;
                ImGui::ClearActiveID();
                pMirv->SetKeyboardSensitivity(g_uiKsens);
            } else if (changed) {
                g_uiKsens = tmp;
                pMirv->SetKeyboardSensitivity(g_uiKsens);
            }
        }
        ImGui::Text("Scroll: ksens | +Shift: FOV | +Ctrl: Roll");

        // Auto-height: shrink/grow to fit current content while preserving user-set width
        {
            ImVec2 cur = ImGui::GetWindowSize();
            float remain = ImGui::GetContentRegionAvail().y;
            float desired = cur.y - remain;
            float min_h = ImGui::GetFrameHeightWithSpacing() * 6.0f; // camera panel needs a bit more
            if (desired < min_h) desired = min_h;
            ImGui::SetWindowSize(ImVec2(cur.x, desired));
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
    // ---------- ImGuizmo overlay + small control panel ----------
    ImGuizmo::BeginFrame();

    // Global hotkeys for gizmo mode (avoid triggering while editing a text field)
    if (ImGui::GetActiveID() == 0) {
        if (ImGui::IsKeyPressed(ImGuiKey_G, false)) {
            g_GizmoOp = ImGuizmo::TRANSLATE;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
            g_GizmoOp = ImGuizmo::ROTATE;
        }
    }

    // Tiny panel to pick operation/mode
    ImGui::Begin("Gizmo", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    if (ImGui::RadioButton("Translate (G)", g_GizmoOp == ImGuizmo::TRANSLATE)) g_GizmoOp = ImGuizmo::TRANSLATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate (R)",    g_GizmoOp == ImGuizmo::ROTATE))    g_GizmoOp = ImGuizmo::ROTATE;
    if (ImGui::RadioButton("Local",     g_GizmoMode == ImGuizmo::LOCAL))   g_GizmoMode = ImGuizmo::LOCAL;
    ImGui::SameLine();
    if (ImGui::RadioButton("World",     g_GizmoMode == ImGuizmo::WORLD))   g_GizmoMode = ImGuizmo::WORLD;
    ImGui::TextUnformatted("Hold CTRL to snap");
    ImGui::End();

    // Only draw a gizmo if we have a selected keyframe
    using advancedfx::overlay::g_LastCampathCtx;
    if (g_LastCampathCtx.active)
    {
        // Where to draw (full screen / swapchain area)
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        // While right mouse button passthrough is active, make gizmo intangible
        // so it doesn't steal focus or reveal the cursor while the camera is controlled.
        ImGuizmo::Enable(!Overlay::Get().IsRmbPassthroughActive());
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList()); // draw on top
        // Use main viewport rect for ImGuizmo normalization (accounts for viewport offset/DPI)
        ImGuizmo::SetRect(0.0f, 0.0f, ds.x, ds.y);

        // Build view & projection from current camera
        double cx, cy, cz, rX, rY, rZ; float cfovDeg;
        Afx_GetLastCameraData(cx, cy, cz, rX, rY, rZ, cfovDeg);

        auto DegToRad = [](double d){ return (float)(d * 3.14159265358979323846 / 180.0); };

        // Helper to convert Source (X=fwd,Y=left,Z=up) vectors into DirectX style (X=right,Y=up,Z=fwd)
        auto SrcToDx = [](float x, float y, float z, float out[3]) {
            out[0] = -y; // left to right
            out[1] =  z; // up stays up
            out[2] =  x; // forward to forward (z)
        };

        // Build a column-major MODEL matrix from Source2 position / angles
        auto BuildModel = [&](float px, float py, float pz,
                              float pitchDeg, float yawDeg, float rollDeg,
                              float out[16])
        {
            // Convert Euler to basis vectors using shared math util (roll,pitch,yaw order)
            double fwdS[3], rightS[3], upS[3];
            Afx::Math::MakeVectors(rollDeg, pitchDeg, yawDeg, fwdS, rightS, upS);

            float fwd[3], right[3], up[3], pos[3];
            SrcToDx((float)rightS[0], (float)rightS[1], (float)rightS[2], right);
            SrcToDx((float)upS[0],    (float)upS[1],    (float)upS[2],    up);
            SrcToDx((float)fwdS[0],   (float)fwdS[1],   (float)fwdS[2],   fwd);
            SrcToDx(px, py, pz, pos);

            // Row-major 4x4, row-vector math:
            //   row0 = camera right
            //   row1 = camera up
            //   row2 = camera forward
            //   row3 = translation (position)
            out[0]=right[0]; out[1]=right[1]; out[2]=right[2]; out[3]=0.0f;
            out[4]=up[0];    out[5]=up[1];    out[6]=up[2];    out[7]=0.0f;
            out[8]=fwd[0];   out[9]=fwd[1];   out[10]=fwd[2];  out[11]=0.0f;
            out[12]=pos[0];  out[13]=pos[1];  out[14]=pos[2];  out[15]=1.0f;
        };

        // Build VIEW = inverse(model) for a rigid transform (row-major, row-vector math)
        auto BuildView = [&](float eyeX, float eyeY, float eyeZ,
                              float pitchDeg, float yawDeg, float rollDeg,
                              float out[16])
        {
            float M[16];
            BuildModel(eyeX, eyeY, eyeZ, pitchDeg, yawDeg, rollDeg, M);

            // Extract rotation rows (row-major):
            const float r00=M[0], r01=M[1], r02=M[2];   // row 0
            const float r10=M[4], r11=M[5], r12=M[6];   // row 1
            const float r20=M[8], r21=M[9], r22=M[10];  // row 2
            const float tx = M[12], ty = M[13], tz = M[14]; // translation (row 3)

            // View rotation = R^{-1} = R^T (row-vector math => rows become columns)
            out[0]=r00; out[1]=r10; out[2]=r20; out[3]=0.0f; // row 0 = column 0 of R
            out[4]=r01; out[5]=r11; out[6]=r21; out[7]=0.0f; // row 1 = column 1 of R
            out[8]=r02; out[9]=r12; out[10]=r22; out[11]=0.0f; // row 2 = column 2 of R
            // Translation = -t * R^{-1} = -t * R^T (row-vector math)
            out[12]=-(tx*r00 + ty*r01 + tz*r02);
            out[13]=-(tx*r10 + ty*r11 + tz*r12);
            out[14]=-(tx*r20 + ty*r21 + tz*r22);
            out[15]=1.0f;
        };

        // Matrix conventions for ImGuizmo interop:
        // - All matrices below are row-major and use row-vector math (v' = v * M).
        // - World axes mapping follows Source: input angles build X=fwd, Y=left, Z=up, then we remap
        //   to overlay/Im space as X=right, Y=up, Z=forward (see SrcToDx above).
        // - View matrix is the rigid inverse of the model in row-major form: R^T in the top-left 3x3,
        //   and translation = -t * R^T (note: multiply on the right due to row-vectors).
        // - We pre-apply a Z flip (negate 3rd row) to match ImGuizmo's expected camera convention.
        // - Projection is OpenGL-style right-handed with z in [-1,1] (m[2][3] = -1), using a vertical FOV.
        //   Since the engine FOV is horizontal (scaled from a 4:3 default), we convert hfov->vfov
        //   based on the current swapchain aspect so gizmo projection matches the game.

        float view[16], proj[16];
        // Build a view from camera rigid transform (same mapping as model)
        BuildView((float)cx,(float)cy,(float)cz,(float)rX,(float)rY,(float)rZ, view);

        // Build GL-style RH projection (z in [-1,1]) expected by ImGuizmo.
        auto PerspectiveGlRh = [&](float fovyDeg, float aspect, float znear, float zfar, float* m16)
        {
            const float f = 1.0f / tanf((float)(fovyDeg * 3.14159265358979323846 / 180.0 * 0.5));
            m16[0] = f/aspect; m16[1]=0; m16[2]=0;               m16[3]=0;
            m16[4] = 0;        m16[5]=f; m16[6]=0;               m16[7]=0;
            m16[8] = 0;        m16[9]=0; m16[10]=-(zfar+znear)/(zfar-znear); m16[11]=-1.0f;
            m16[12]=0;         m16[13]=0; m16[14]=-(2.0f*zfar*znear)/(zfar-znear); m16[15]=0;
        };
        // Apply Z-flip as pre-multiply (negate 3rd row) – matches ImGuizmo's expectations here
        view[2]  *= -1.0f; view[6]  *= -1.0f; view[10] *= -1.0f; view[14] *= -1.0f;

        // Prefer swapchain aspect if known
        float aspect = 0.0f;
        if (m_Rtv.height > 0) aspect = (float)m_Rtv.width / (float)m_Rtv.height;
        else if (ds.y > 0.0f) aspect = ds.x / ds.y;
        if (aspect <= 0.0f) aspect = 16.0f/9.0f;

        // Convert engine-reported horizontal FOV (scaled from 4:3) to vertical FOV for GL projection
        float fovyDeg = cfovDeg;
        {
            const float defaultAspect = 4.0f/3.0f;
            const float ratio = aspect / defaultAspect;
            const float fovRad = (float)(cfovDeg * 3.14159265358979323846/180.0);
            const float half = 0.5f * fovRad;
            const float halfVert = atanf(tanf(half) / ratio);
            fovyDeg = (float)(2.0f * (halfVert) * 180.0 / 3.14159265358979323846);
        }
        PerspectiveGlRh(fovyDeg, aspect, 0.1f, 100000.0f, proj);

        // Model from selected key
        float model[16];
        {
            const CamPathValue& v = g_LastCampathCtx.value;
            using namespace Afx::Math;
            QEulerAngles ea = v.R.ToQREulerAngles().ToQEulerAngles();
            BuildModel((float)v.X, (float)v.Y, (float)v.Z,
                    (float)ea.Pitch, (float)ea.Yaw, (float)ea.Roll, model);
        }

        // Matrices ready – call ImGuizmo

        // Optional snap
        bool snap = ImGui::GetIO().KeyCtrl;
        float snapTranslate[3] = {1.0f,1.0f,1.0f};
        float snapRotate = 5.0f;

        // Keep model stable across frames while dragging: don't rebuild from campath every frame.
        // Use a per-selection temporary matrix that we feed back into ImGuizmo while IsUsing().
        static bool   s_hasModelOverride = false;
        static double s_overrideKeyTime = 0.0;
        static float  s_modelOverride[16];
        if (s_hasModelOverride) {
            // Only keep override while operating on the same selected key/time.
            if (fabs(s_overrideKeyTime - g_LastCampathCtx.time) < 1e-9) {
                for (int i=0;i<16;++i) model[i]=s_modelOverride[i];
            } else {
                s_hasModelOverride = false;
            }
        }

        bool changed = ImGuizmo::Manipulate(
            view, proj,
            g_GizmoOp, g_GizmoMode,
            model, nullptr,
            snap ? (g_GizmoOp == ImGuizmo::ROTATE ? &snapRotate : snapTranslate) : nullptr
        );

        static bool wasUsing = false;
        static bool s_dragChanged = false;
        bool usingNow = ImGuizmo::IsUsing();
        if (usingNow) {
            // Persist the live-updated model so next frame starts from here.
            for (int i=0;i<16;++i) s_modelOverride[i]=model[i];
            s_hasModelOverride = true;
            s_overrideKeyTime = g_LastCampathCtx.time;
            if (changed) s_dragChanged = true;
        }

        // Commit on release (only if it changed)
        if (wasUsing && !usingNow && (s_dragChanged || s_hasModelOverride)) {
            float t[3], r_dummy[3], s_dummy[3];
            // Use the last live model (override) if available
            const float* finalModel = s_hasModelOverride ? s_modelOverride : model;
            ImGuizmo::DecomposeMatrixToComponents(finalModel, t, r_dummy, s_dummy);
            // Translation back to Source2 space (X=fwd,Y=left,Z=up)
            double sx = (double)t[2];
            double sy = (double)(-t[0]);
            double sz = (double)t[1];

            // Robust rotation extraction: derive Quake (pitch,yaw,roll) from basis vectors
            // 1) Extract Im-space basis (row-major rows): right=R, up=U, forward=F
            float Rx = finalModel[0],  Ry = finalModel[1],  Rz = finalModel[2];
            float Ux = finalModel[4],  Uy = finalModel[5],  Uz = finalModel[6];
            float Fx = finalModel[8],  Fy = finalModel[9],  Fz = finalModel[10];
            // 2) Map Im -> Source (X=fwd,Y=left,Z=up): S = {X=iz, Y=-ix, Z=iy}
            auto im_to_src = [](float ix, float iy, float iz, double out[3]){
                out[0] = (double)iz;   // X (forward)
                out[1] = (double)(-ix); // Y (left)
                out[2] = (double)iy;   // Z (up)
            };
            double R_s[3], U_s[3], F_s[3];
            im_to_src(Rx,Ry,Rz,R_s);
            im_to_src(Ux,Uy,Uz,U_s);
            im_to_src(Fx,Fy,Fz,F_s);
            // Normalize just in case
            auto norm3 = [](double v[3]){
                double l = sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); if(l>1e-12){ v[0]/=l; v[1]/=l; v[2]/=l; }
            };
            norm3(R_s); norm3(U_s); norm3(F_s);
            // 3) Quake angles from F_s and U_s
            // Forward from (pitch,yaw): F = (cp*cy, cp*sy, -sp)
            double sp = -F_s[2];
            if (sp < -1.0) sp = -1.0; else if (sp > 1.0) sp = 1.0;
            double pitch = asin(sp) * (180.0 / 3.14159265358979323846);
            double cp = cos(pitch * (3.14159265358979323846/180.0));
            double yaw = 0.0;
            if (fabs(cp) > 1e-6) {
                yaw = atan2(F_s[1], F_s[0]) * (180.0 / 3.14159265358979323846);
            }
            // 4) Compute roll by comparing actual Up to the theoretical Up with roll=0
            double fwd0[3], right0[3], up0[3];
            {
                // Use MakeVectors with roll=0, pitch,yaw to get base axes in Source space
                double fwdQ[3], rightQ[3], upQ[3];
                Afx::Math::MakeVectors(/*roll*/0.0, /*pitch*/pitch, /*yaw*/yaw, fwdQ, rightQ, upQ);
                // MakeVectors returns right (to the right, not left). Our R_s is "right" already, consistent.
                fwd0[0]=fwdQ[0]; fwd0[1]=fwdQ[1]; fwd0[2]=fwdQ[2];
                right0[0]=rightQ[0]; right0[1]=rightQ[1]; right0[2]=rightQ[2];
                up0[0]=upQ[0]; up0[1]=upQ[1]; up0[2]=upQ[2];
            }
            // roll = atan2( dot(U, right0), dot(U, up0) )
            double dot_ur = U_s[0]*right0[0] + U_s[1]*right0[1] + U_s[2]*right0[2];
            double dot_uu = U_s[0]*up0[0]    + U_s[1]*up0[1]    + U_s[2]*up0[2];
            double roll = atan2(dot_ur, dot_uu) * (180.0 / 3.14159265358979323846);

            CamPathValue newVal(
                sx, sy, sz,
                pitch, yaw, roll,
                g_LastCampathCtx.value.Fov
            );

            // Prefer updating via console commands on the engine thread to avoid races with the drawer.
            // Try to select the exact keyframe by index, then apply per-component edits.
            int foundIndex = -1;
            {
                int idx = 0;
                const double eps = 1e-9;
                for (CamPathIterator it = g_CamPath.GetBegin(); it != g_CamPath.GetEnd(); ++it, ++idx) {
                    if (fabs(it.GetTime() - g_LastCampathCtx.time) < eps) { foundIndex = idx; break; }
                }
            }
            if (foundIndex >= 0) {
                // Count currently selected keyframes
                int selCount = 0;
                for (CamPathIterator it = g_CamPath.GetBegin(); it != g_CamPath.GetEnd(); ++it) {
                    if (it.GetValue().Selected) ++selCount;
                }

                char buf[1024];
                if (selCount > 1) {
                    // Bulk edit via anchor transform: apply same translation/rotation delta to all selected
                    const auto e = newVal.R.ToQREulerAngles().ToQEulerAngles();
                    snprintf(buf, sizeof(buf), "mirv_campath edit anchor #%d %.*f %.*f %.*f %.*f %.*f %.*f",
                        foundIndex,
                        6, (float)newVal.X,
                        6, (float)newVal.Y,
                        6, (float)newVal.Z,
                        6, (float)e.Pitch,
                        6, (float)e.Yaw,
                        6, (float)e.Roll);
                    Afx_ExecClientCmd(buf);
                    // Note: FOV intentionally not changed for bulk edit
                    g_SequencerNeedsRefresh = true;
                } else {
                    // Single-key edit: keep previous behavior and update FOV
                    snprintf(buf, sizeof(buf), "mirv_campath select none; mirv_campath select #%d #%d", foundIndex, foundIndex);
                    Afx_ExecClientCmd(buf);
                    snprintf(buf, sizeof(buf), "mirv_campath edit position %.*f %.*f %.*f",
                            6, (float)newVal.X, 6, (float)newVal.Y, 6, (float)newVal.Z);
                    Afx_ExecClientCmd(buf);
                    snprintf(buf, sizeof(buf), "mirv_campath edit angles %.*f %.*f %.*f",
                            6, (float)newVal.R.ToQREulerAngles().ToQEulerAngles().Pitch,
                            6, (float)newVal.R.ToQREulerAngles().ToQEulerAngles().Yaw,
                            6, (float)newVal.R.ToQREulerAngles().ToQEulerAngles().Roll);
                    Afx_ExecClientCmd(buf);
                    snprintf(buf, sizeof(buf), "mirv_campath edit fov %.*f", 6, (float)newVal.Fov);
                    Afx_ExecClientCmd(buf);
                    Afx_ExecClientCmd("mirv_campath select none");
                    g_SequencerNeedsRefresh = true;
                }
            }

            g_LastCampathCtx.value = newVal;
            s_hasModelOverride = false;
            s_dragChanged = false;
        }
        wasUsing = usingNow;
    }

#else
    (void)dtSeconds;
#endif
}

void OverlayDx11::Render() {
#ifdef _WIN32
    if (!m_Initialized) return;
    // Update backbuffer size cache
    UpdateBackbufferSize();

    // Recreate ImGui device objects once after a resize/device-loss
    if (m_ImGuiNeedRecreate) {
        ImGui_ImplDX11_CreateDeviceObjects();
        m_ImGuiNeedRecreate = false;
    }

    // Backup current state we might disturb (render targets + viewport)
    ID3D11RenderTargetView* prevRtv = nullptr;
    ID3D11DepthStencilView* prevDsv = nullptr;
    m_Context->OMGetRenderTargets(1, &prevRtv, &prevDsv);

    UINT prevVpCount = 0;
    m_Context->RSGetViewports(&prevVpCount, nullptr);
    D3D11_VIEWPORT prevVp = {};
    if (prevVpCount > 0) {
        prevVpCount = 1;
        m_Context->RSGetViewports(&prevVpCount, &prevVp);
    }

    // Create ephemeral RTV this frame, render, then release
    ID3D11Texture2D* backbuffer = nullptr;
    if (SUCCEEDED(m_Swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer)) && backbuffer) {
        ID3D11RenderTargetView* rtv = nullptr;
        if (SUCCEEDED(m_Device->CreateRenderTargetView(backbuffer, nullptr, &rtv)) && rtv) {
            ID3D11RenderTargetView* rtvs[1] = { rtv };
            m_Context->OMSetRenderTargets(1, rtvs, nullptr);
            ImGui::Render();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            rtv->Release();
        }
        backbuffer->Release();
    }

    // Restore viewport
    if (prevVpCount > 0)
        m_Context->RSSetViewports(1, &prevVp);

    // Restore previous render targets
    if (prevRtv || prevDsv) {
        if (prevRtv)
            m_Context->OMSetRenderTargets(1, &prevRtv, prevDsv);
        else
            m_Context->OMSetRenderTargets(0, nullptr, prevDsv);
    }
    if (prevRtv) prevRtv->Release();
    if (prevDsv) prevDsv->Release();
#endif
}

void OverlayDx11::EndFrame() {
    // Nothing specific for DX11 backend
}

void OverlayDx11::OnDeviceLost() {
#ifdef _WIN32
    if (!m_Initialized) return;
    m_Rtv.width = m_Rtv.height = 0;
    ImGui_ImplDX11_InvalidateDeviceObjects();
    m_ImGuiNeedRecreate = true;
#endif
}

void OverlayDx11::OnResize(uint32_t, uint32_t) {
#ifdef _WIN32
    if (!m_Initialized) return;
    m_Rtv.width = m_Rtv.height = 0;
    ImGui_ImplDX11_InvalidateDeviceObjects();
    // Defer device object and RTV recreation to the next Render() after resize has completed
    m_ImGuiNeedRecreate = true;
#endif
}

}} // namespace

#ifdef _WIN32
namespace advancedfx { namespace overlay {

void OverlayDx11::UpdateBackbufferSize() {
    if (!m_Swapchain || !m_Device) return;
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(m_Swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer)) || !backbuffer)
        return;
    D3D11_TEXTURE2D_DESC bbDesc = {};
    backbuffer->GetDesc(&bbDesc);
    m_Rtv.width = bbDesc.Width;
    m_Rtv.height = bbDesc.Height;
    backbuffer->Release();
}

}} // namespace
#endif
