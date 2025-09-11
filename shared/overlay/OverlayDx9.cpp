// NOTE: This is a DX9 port of the DX11 overlay implementation.
// It mirrors the same UI (campath, recording, settings) with D3D9 backend calls.

#include "OverlayDx9.h"
#include "Overlay.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shellapi.h>

// Dear ImGui
#include "third_party/imgui/imgui.h"
#include "third_party/imgui/imgui_internal.h"
#include "third_party/imgui/backends/imgui_impl_win32.h"
#include "third_party/imgui/backends/imgui_impl_dx9.h"
#include "third_party/imgui_filebrowser/imfilebrowser.h"
#include "third_party/imgui_neo_sequencer/imgui_neo_sequencer.h"
#include "third_party/imguizmo/ImGuizmo.h"

// Forward decl from backend header to avoid including it elsewhere
LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Shared HLAE headers
#include "../AfxConsole.h"
#include "../AfxMath.h"
#include "../CamPath.h"

// Source 1 integration points
#include "../../AfxHookSource/MirvTime.h"
#include "../../AfxHookSource/CampathDrawer.h"
#include "../../AfxHookSource/RenderView.h"
#include "../../AfxHookSource/WrpVEngineClient.h"
#include "../../AfxHookSource/hlaeFolder.h"

#include <math.h>
#include <cmath>
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>

#pragma comment(lib, "shell32.lib")

// Externs from Source 1 side:
extern WrpVEngineClient* g_VEngineClient;
extern Hook_VClient_RenderView g_Hook_VClient_RenderView;
extern CCampathDrawer g_CampathDrawer;
// Demo time helpers (Source 1)
extern bool GetCurrentDemoTick(int& outTick);
extern bool GetCurrentDemoTime(double& outDemoTime);
extern bool GetDemoTickFromClientTime(double curTime, double targetTime, int& outTick);

// Provide CS1 equivalents for DX11 overlay helpers
static inline void Afx_GetLastCameraData(double &x,double &y,double &z,double &rX,double &rY,double &rZ,float &fov)
{
    x = g_Hook_VClient_RenderView.LastCameraOrigin[0];
    y = g_Hook_VClient_RenderView.LastCameraOrigin[1];
    z = g_Hook_VClient_RenderView.LastCameraOrigin[2];
    rX = g_Hook_VClient_RenderView.LastCameraAngles[0];
    rY = g_Hook_VClient_RenderView.LastCameraAngles[1];
    rZ = g_Hook_VClient_RenderView.LastCameraAngles[2];
    fov = (float)g_Hook_VClient_RenderView.LastCameraFov;
}

// Helpers mirroring the DX11 overlay API (Source 1 equivalents)
static inline float GetLastCameraFov()
{
    return (float)g_Hook_VClient_RenderView.LastCameraFov;
}
static inline float GetLastCameraRoll()
{
    return (float)g_Hook_VClient_RenderView.LastCameraAngles[2];
}

static inline void Afx_ExecClientCmd(const char* cmd)
{
    if (g_VEngineClient && cmd)
        g_VEngineClient->ClientCmd_Unrestricted(cmd);
}

static inline void Afx_GotoDemoTick(int tick)
{
    if (!g_VEngineClient) return;
    char buf[64];
    sprintf_s(buf, "demo_gototick %d", tick);
    g_VEngineClient->ClientCmd_Unrestricted(buf);
}

// Optional info from streams (not available on Source 1 here): provide safe stubs.
static inline const wchar_t* AfxStreams_GetTakeDir() { return nullptr; }
static inline const char*   AfxStreams_GetRecordNameUtf8() { return nullptr; }

// Execute common mirv_streams controls via client commands (Source 1 shim)
static inline void AfxStreams_SetRecordNameUtf8(const char* name) {
    if (!name) return;
    char cmd[2048];
    // Quote the path so spaces are handled correctly.
    // Note: We don't expect embedded quotes from the file browser.
    sprintf_s(cmd, "mirv_streams record name \"%s\"", name);
    Afx_ExecClientCmd(cmd);
}
static inline bool AfxStreams_GetRecordScreenEnabled() { return false; }
static inline void AfxStreams_SetRecordScreenEnabled(bool en) {
    Afx_ExecClientCmd(en ? "mirv_streams record screen enabled 1" : "mirv_streams record screen enabled 0");
}
static inline bool AfxStreams_GetOverrideFps() { return false; }
static inline float AfxStreams_GetOverrideFpsValue() { return 0.0f; }
static inline void AfxStreams_SetOverrideFpsDefault() { Afx_ExecClientCmd("mirv_streams record fps default"); }
static inline void AfxStreams_SetOverrideFpsValue(float v) {
    char cmd[256]; sprintf_s(cmd, "mirv_streams record fps %.3f", v); Afx_ExecClientCmd(cmd);
}

#endif // _WIN32

namespace advancedfx { namespace overlay {

// Apply a sleek dark style for the HLAE overlay.
static void ApplyHlaeDarkStyle()
{
#ifdef _WIN32
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding     = ImVec2(10.0f, 10.0f);
    style.FramePadding      = ImVec2(8.0f, 6.0f);
    style.ItemSpacing       = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
    style.CellPadding       = ImVec2(6.0f, 6.0f);
    style.ScrollbarSize     = 14.0f;
    style.GrabMinSize       = 10.0f;
    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 6.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 12.0f;
    style.GrabRounding      = 6.0f;
    style.TabRounding       = 6.0f;
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.TabBorderSize     = 0.0f;
    style.WindowTitleAlign  = ImVec2(0.0f, 0.5f);

    auto C = [](int r, int g, int b, int a = 255) {
        return ImVec4(r/255.0f, g/255.0f, b/255.0f, a/255.0f);
    };

    const ImVec4 BG0 = C(17, 19, 24);
    const ImVec4 BG1 = C(24, 27, 33);
    const ImVec4 BG2 = C(28, 32, 39);
    const ImVec4 BG3 = C(36, 41, 49);
    const ImVec4 BG4 = C(45, 51, 61);
    const ImVec4 FG0 = C(224, 224, 224);
    const ImVec4 FG1 = C(136, 136, 136);
    const ImVec4 BRD = C(58, 64, 74);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = FG0;
    colors[ImGuiCol_TextDisabled]          = FG1;
    colors[ImGuiCol_WindowBg]              = BG0;
    colors[ImGuiCol_ChildBg]               = ImVec4(0,0,0,0);
    colors[ImGuiCol_PopupBg]               = ImVec4(BG1.x, BG1.y, BG1.z, 0.98f);
    colors[ImGuiCol_Border]                = BRD;
    colors[ImGuiCol_BorderShadow]          = ImVec4(0,0,0,0);
    colors[ImGuiCol_FrameBg]               = BG2;
    colors[ImGuiCol_FrameBgHovered]        = BG3;
    colors[ImGuiCol_FrameBgActive]         = BG4;
    colors[ImGuiCol_TitleBg]               = C(14, 16, 20);
    colors[ImGuiCol_TitleBgActive]         = C(20, 24, 28);
    colors[ImGuiCol_TitleBgCollapsed]      = C(14, 16, 20);
    colors[ImGuiCol_MenuBarBg]             = C(22, 25, 31);
#endif
}


// Overlay UI state (parity with DX11)
struct CampathCtx { bool active=false; double time=0.0; CamPathValue value{}; };
static CampathCtx g_LastCampathCtx;
static bool g_ShowSequencer = false;
static bool g_ShowCameraControl = false;
static bool g_ShowGizmo = false;
static ImGuizmo::OPERATION g_GizmoOp = ImGuizmo::TRANSLATE;
static ImGuizmo::MODE      g_GizmoMode = ImGuizmo::LOCAL;

// Sequencer/curve editor shared state (parity with DX11)
static bool g_SequencerNeedsRefresh = false;
static bool g_ShowCurveEditor = false;
static float g_CurvePadding = 12.0f;
static float g_CurveValueScale = 1.0f;
static float g_CurveValueOffset = 0.0f;
static std::vector<int> g_CurveSelection;
static bool g_CurveShow[7] = { false, false, false, false, false, false, false };
static int g_CurveFocusChannel = -1; // -1 = none
struct CurveCache { bool valid = false; std::vector<double> times; std::vector<CamPathValue> values; } g_CurveCache;
static int g_CurveCtxKeyIndex = -1;
static int g_CurveCtxChannel  = -1;
static bool g_PreviewFollow = false;
static float g_PreviewNorm = 0.0f;
static bool g_CurveNormalize = true;             // normalize each channel to its own range

//UI SCALE
static float g_UiScale = 1.0f;

// Fonts
// Pointers to fonts we load at startup. We always have Default; the others
// may be null if the system TTF file isn't found.
static ImFont* g_FontDefault = nullptr;
static ImFont* g_FontMono    = nullptr;
static ImFont* g_FontSans    = nullptr;
static ImFont* g_FontSilly   = nullptr;

// Current choice in Settings tab: 0=Default, 1=Monospace, 2=Sans Serif, 3=Silly.
static int g_FontChoice = 0;

// Guard to run font loading once.
static bool g_FontsLoaded = false;

// Persistent demo file dialog (Settings → Open demo)
static ImGui::FileBrowser g_DemoOpenDialog(
    ImGuiFileBrowserFlags_CloseOnEsc |
    ImGuiFileBrowserFlags_EditPathString |
    ImGuiFileBrowserFlags_CreateNewDir
);
static bool g_DemoDialogInit = false;

// UI state mirrored for Mirv camera sliders so external changes (e.g., wheel in passthrough)
// stay in sync with the controls below.
static bool  g_uiFovInit   = false; static float  g_uiFov = 90.0f;   static float g_uiFovDefault = 90.0f;
static bool  g_uiRollInit  = false; static float  g_uiRoll = 0.0f;   static float g_uiRollDefault = 0.0f;
static bool  g_uiKsensInit = false; static float  g_uiKsens = 1.0f;  static float g_uiKsensDefault = 1.0f;

static void CurveCache_Rebuild()
{
    g_CurveCache.valid = true;
    g_CurveCache.times.clear();
    g_CurveCache.values.clear();
    g_CurveCache.times.reserve(g_Hook_VClient_RenderView.m_CamPath.GetSize());
    g_CurveCache.values.reserve(g_Hook_VClient_RenderView.m_CamPath.GetSize());
    for (CamPathIterator it = g_Hook_VClient_RenderView.m_CamPath.GetBegin(); it != g_Hook_VClient_RenderView.m_CamPath.GetEnd(); ++it) {
        g_CurveCache.times.push_back(it.GetTime());
        g_CurveCache.values.push_back(it.GetValue());
    }
}

// File dialogs for campath open/save
static ImGui::FileBrowser s_campathOpenDialog(ImGuiFileBrowserFlags_CloseOnEsc | ImGuiFileBrowserFlags_EditPathString | ImGuiFileBrowserFlags_CreateNewDir);
static ImGui::FileBrowser s_campathSaveDialog(ImGuiFileBrowserFlags_EnterNewFilename | ImGuiFileBrowserFlags_CloseOnEsc | ImGuiFileBrowserFlags_EditPathString | ImGuiFileBrowserFlags_CreateNewDir);
static bool s_campathDialogsInit = false;

// Persisted overlay paths in imgui.ini
struct OverlayPathsSettings { std::string campathDir, recordBrowseDir, demoDir; };
static OverlayPathsSettings g_OverlayPaths;
static void* OverlayPaths_ReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
    return (0 == strcmp(name, "Paths")) ? (void*)&g_OverlayPaths : nullptr;
}
static void OverlayPaths_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    OverlayPathsSettings* s = (OverlayPathsSettings*)entry;
    const char* eq = strchr(line, '='); if (!eq) return;
    std::string key(line, eq - line); std::string val(eq + 1);
    if (key == "CampathDir") s->campathDir = val;
    else if (key == "RecordBrowseDir") s->recordBrowseDir = val;
    else if (key == "DemoDir") s->demoDir = val;
}
static void OverlayPaths_WriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* out_buf) {
    out_buf->appendf("[%s][%s]\n", handler->TypeName, "Paths");
    if (!g_OverlayPaths.campathDir.empty()) out_buf->appendf("CampathDir=%s\n", g_OverlayPaths.campathDir.c_str());
    if (!g_OverlayPaths.recordBrowseDir.empty()) out_buf->appendf("RecordBrowseDir=%s\n", g_OverlayPaths.recordBrowseDir.c_str());
    if (!g_OverlayPaths.demoDir.empty()) out_buf->appendf("DemoDir=%s\n", g_OverlayPaths.demoDir.c_str());
    out_buf->append("\n");
}

OverlayDx9::OverlayDx9(IDirect3DDevice9* device, HWND hwnd)
    : m_Device(device), m_Hwnd(hwnd) {}

OverlayDx9::~OverlayDx9() { Shutdown(); }

bool OverlayDx9::Initialize() {
#ifdef _WIN32
    if (m_Initialized) return true;
    if (!m_Device || !m_Hwnd) return false;

    if (!ImGui::GetCurrentContext()) ImGui::CreateContext();
    ApplyHlaeDarkStyle();
    // Match DX11 behavior: only drag windows via title bar
    {
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigWindowsMoveFromTitleBarOnly = true;
    }

    // Load fonts once
    if (!g_FontsLoaded) {
        ImGuiIO& io = ImGui::GetIO();
        ImFontAtlas* atlas = io.Fonts;
        g_FontDefault = atlas->AddFontDefault();
        io.FontDefault = g_FontDefault;
        g_FontsLoaded = true;
    }

    if (!ImGui_ImplWin32_Init(m_Hwnd)) return false;
    if (!ImGui_ImplDX9_Init(m_Device)) return false;
    m_DeviceObjectsValid = true;
    m_Initialized = true;
    advancedfx::Message("Overlay: renderer=DX9\n");

    // Register custom ini handler (paths)
    if (!ImGui::FindSettingsHandler("HLAEOverlayPaths")) {
        ImGuiSettingsHandler ini; ini.TypeName = "HLAEOverlayPaths"; ini.TypeHash = ImHashStr(ini.TypeName);
        ini.ReadOpenFn = OverlayPaths_ReadOpen; ini.ReadLineFn = OverlayPaths_ReadLine; ini.WriteAllFn = OverlayPaths_WriteAll;
        ImGui::AddSettingsHandler(&ini);
        ImGuiIO& io2 = ImGui::GetIO();
        if (io2.IniFilename && io2.IniFilename[0]) ImGui::LoadIniSettingsFromDisk(io2.IniFilename);
    }

    return true;
#else
    return false;
#endif
}

void OverlayDx9::Shutdown() {
#ifdef _WIN32
    if (!m_Initialized) return;
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    m_DeviceObjectsValid = false;
    m_Initialized = false;
#endif
}

void OverlayDx9::BeginFrame(float dtSeconds) {
#ifdef _WIN32
    if (!m_Initialized) return;
    ImGui::GetIO().DeltaTime = dtSeconds > 0.0f ? dtSeconds : ImGui::GetIO().DeltaTime;
    ImGui_ImplWin32_NewFrame();
    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();
#endif
}

// Small helpers for time/tick on Source 1
static inline double S1_ClientTime() { return g_MirvTime.GetTime(); }

void OverlayDx9::Render() {
#ifdef _WIN32
    if (!m_Initialized) return;

    // Diagnostic watermark
    ImGui::GetForegroundDrawList()->AddText(ImVec2(8,8), IM_COL32(255,255,255,255), "HLAE Overlay - Press F8 to toggle", nullptr);

    if (!s_campathDialogsInit) {
        s_campathOpenDialog.SetTitle("Open Campath");
        s_campathOpenDialog.SetTypeFilters({ ".json", ".txt" , ".cfg" });
        s_campathSaveDialog.SetTitle("Save Campath");
        s_campathSaveDialog.SetTypeFilters({ ".json", ".txt" , ".cfg" });
        s_campathDialogsInit = true;
    }

    ImGui::Begin("HLAE Overlay", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
    if (ImGui::BeginTabBar("##hlae_tabs")) {
        // Campath tab
        if (ImGui::BeginTabItem("Campath")) {
            // Preview toggle
            static bool preview = false; static bool previewInit = false;
            if (!previewInit) { preview = g_CampathDrawer.Draw_get(); previewInit = true; }
            if (ImGui::Checkbox("Enable camera path preview", &preview)) {
                g_CampathDrawer.Draw_set(preview);
                advancedfx::Message("Overlay: mirv_campath draw enabled %d\n", preview ? 1 : 0);
            }

            // Info
            size_t cpCount = g_Hook_VClient_RenderView.m_CamPath.GetSize();
            if (cpCount > 0) {
                double seconds = (cpCount >= 2) ? g_Hook_VClient_RenderView.m_CamPath.GetDuration() : 0.0;
                int ticks = 0; double demoNow = 0.0;
                if (GetCurrentDemoTime(demoNow) && seconds > 0.0) {
                    int curTick = 0, endTick = 0;
                    double curTime = S1_ClientTime();
                    double startClient = g_Hook_VClient_RenderView.m_CamPath.GetOffset() + g_Hook_VClient_RenderView.m_CamPath.GetBegin().GetTime();
                    double endClient   = startClient + seconds;
                    if (GetDemoTickFromClientTime(curTime, startClient, curTick) && GetDemoTickFromClientTime(curTime, endClient, endTick))
                        ticks = endTick - curTick;
                }
                ImGui::Separator();
                ImGui::Text("Campath: points=%u, length=%d ticks (%.2f s)", (unsigned)cpCount, ticks, seconds);
            }

            // Basic controls
            bool campathEnabled = g_Hook_VClient_RenderView.m_CamPath.Enabled_get();
            if (ImGui::Checkbox("Campath enabled", &campathEnabled)) {
                g_Hook_VClient_RenderView.m_CamPath.Enabled_set(campathEnabled);
            }
            ImGui::SameLine();
            if (ImGui::Button("Add point")) {
                double x,y,z,rx,ry,rz; float fov;
                Afx_GetLastCameraData(x,y,z,rx,ry,rz,fov);
                double t = S1_ClientTime() - g_Hook_VClient_RenderView.m_CamPath.GetOffset();
                g_Hook_VClient_RenderView.m_CamPath.Add(t, CamPathValue(x,y,z, rx, ry, rz, fov));
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear")) g_Hook_VClient_RenderView.m_CamPath.Clear();
            ImGui::SameLine();
            if (ImGui::Button("Goto Start")) {
                if (g_Hook_VClient_RenderView.m_CamPath.GetSize() > 0) {
                    double firstT = g_Hook_VClient_RenderView.m_CamPath.GetBegin().GetTime();
                    double targetClientTime = g_Hook_VClient_RenderView.m_CamPath.GetOffset() + firstT;
                    int targetTick = 0;
                    if (GetDemoTickFromClientTime(S1_ClientTime(), targetClientTime, targetTick)) Afx_GotoDemoTick(targetTick);
                    else advancedfx::Warning("Overlay: Failed to compute demo tick for campath start.\n");
                }
            }

            // Interpolation mode toggle
            static bool s_interpCubic = true;
            const char* interpLabel = s_interpCubic ? "Interp: Cubic" : "Interp: Linear";
            if (ImGui::Button(interpLabel)) {
                s_interpCubic = !s_interpCubic;
                if (s_interpCubic)
                    Afx_ExecClientCmd("mirv_campath edit interp position default; mirv_campath edit interp rotation default; mirv_campath edit interp fov default");
                else
                    Afx_ExecClientCmd("mirv_campath edit interp position linear; mirv_campath edit interp rotation sLinear; mirv_campath edit interp fov linear");
            }

            // Open/Save campath
            ImGui::Separator();
            if (ImGui::Button("Open campath")) {
                if (!g_OverlayPaths.campathDir.empty()) s_campathOpenDialog.SetDirectory(g_OverlayPaths.campathDir);
                s_campathOpenDialog.Open();
            }
            ImGui::SameLine();
            if (ImGui::Button("Save campath")) {
                if (!g_OverlayPaths.campathDir.empty()) s_campathSaveDialog.SetDirectory(g_OverlayPaths.campathDir);
                s_campathSaveDialog.Open();
            }
            s_campathOpenDialog.Display();
            s_campathSaveDialog.Display();
            if (s_campathOpenDialog.HasSelected()) {
                const std::string path = s_campathOpenDialog.GetSelected().string();
                char cmd[2048]; sprintf_s(cmd, "mirv_campath load \"%s\"", path.c_str()); Afx_ExecClientCmd(cmd);
                try { g_OverlayPaths.campathDir = std::filesystem::path(path).parent_path().string(); } catch(...) {}
                ImGui::MarkIniSettingsDirty();
                s_campathOpenDialog.ClearSelected();
            }
            if (s_campathSaveDialog.HasSelected()) {
                const std::string path = s_campathSaveDialog.GetSelected().string();
                char cmd[2048]; sprintf_s(cmd, "mirv_campath save \"%s\"", path.c_str()); Afx_ExecClientCmd(cmd);
                try { g_OverlayPaths.campathDir = std::filesystem::path(path).parent_path().string(); } catch(...) {}
                ImGui::MarkIniSettingsDirty();
                s_campathSaveDialog.ClearSelected();
            }

            // Sequencer / Camera Control / Gizmo toggles
            ImGui::Separator();
            ImGui::Checkbox("Show Sequencer", &g_ShowSequencer);
            ImGui::SameLine();
            ImGui::Checkbox("Camera Control", &g_ShowCameraControl);
            ImGui::SameLine();
            ImGui::Checkbox("Show Gizmo", &g_ShowGizmo);

            ImGui::EndTabItem();
        }

        // Recording tab (Source 1 specific)
        if (ImGui::BeginTabItem("Recording")) {
            // Shared: record path chooser
            static char s_recName[512] = {0}; static bool recInit = false;
            static ImGui::FileBrowser dirDialog(
                ImGuiFileBrowserFlags_SelectDirectory |
                ImGuiFileBrowserFlags_CloseOnEsc |
                ImGuiFileBrowserFlags_EditPathString |
                ImGuiFileBrowserFlags_CreateNewDir);
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
            if (boxW < 50.0f) boxW = 50.0f;
            ImGui::SetNextItemWidth(boxW);
            ImGui::InputText("##recname", s_recName, sizeof(s_recName));
            ImGui::SameLine(0.0f, gap1);
            if (ImGui::Button("Browse...##recname")) {
                if (!g_OverlayPaths.recordBrowseDir.empty()) dirDialog.SetDirectory(g_OverlayPaths.recordBrowseDir);
                dirDialog.Open();
            }
            ImGui::SameLine(0.0f, gap2);
            if (ImGui::Button("Set##recname")) { AfxStreams_SetRecordNameUtf8(s_recName); }
            dirDialog.Display();
            if (dirDialog.HasSelected()) {
                const std::string selected = dirDialog.GetSelected().string();
                strncpy_s(s_recName, selected.c_str(), _TRUNCATE);
                AfxStreams_SetRecordNameUtf8(s_recName);
                g_OverlayPaths.recordBrowseDir = s_recName;
                ImGui::MarkIniSettingsDirty();
                dirDialog.ClearSelected();
            }

            if (ImGui::Button("Open recordings folder")) {
                std::wstring wPath;
                if (s_recName[0]) {
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, s_recName, -1, nullptr, 0);
                    if (wlen > 0) { wPath.resize((size_t)wlen - 1); MultiByteToWideChar(CP_UTF8, 0, s_recName, -1, &wPath[0], wlen); }
                }
                if (!wPath.empty()) ShellExecuteW(nullptr, L"open", wPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }

            ImGui::Separator();
            // Game mode toggle: CS:GO vs Other Source 1
            static bool s_isCsgo = true;
            ImGui::TextUnformatted("Game mode:");
            ImGui::SameLine();
            if (ImGui::RadioButton("CS:GO", s_isCsgo)) s_isCsgo = true;
            ImGui::SameLine();
            if (ImGui::RadioButton("Other Source 1", !s_isCsgo)) s_isCsgo = false;

            // FPS input (interpreted differently depending on mode)
            static char s_fpsText[64] = {0}; static bool fpsInit = false;
            if (!fpsInit) { strncpy_s(s_fpsText, "60", _TRUNCATE); fpsInit = true; }
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000").x + st.FramePadding.x * 3.0f);
            ImGui::InputText(s_isCsgo ? "host_framerate" : "record FPS", s_fpsText, sizeof(s_fpsText));

            // Common HUD toggles
            ImGui::Separator();
            static bool s_hideHud = false; if (ImGui::Checkbox("Hide HUD", &s_hideHud)) Afx_ExecClientCmd(s_hideHud?"cl_drawhud 0":"cl_drawhud 1");
            ImGui::SameLine(); static bool s_onlyDeathnotices = false; if (ImGui::Checkbox("Only Deathnotices", &s_onlyDeathnotices)) Afx_ExecClientCmd(s_onlyDeathnotices?"cl_draw_only_deathnotices 1":"cl_draw_only_deathnotices 0");

            if (s_isCsgo) {
                // CS:GO workflow: select streams to record
                ImGui::Separator();
                ImGui::TextUnformatted("Streams to record (CS:GO)");
                static bool s_recNorm = true, s_recMatte = false, s_recDepth = false;
                ImGui::Checkbox("normal (hlae_norm)", &s_recNorm);
                ImGui::SameLine();
                ImGui::Checkbox("matte entity (hlae_matte)", &s_recMatte);
                ImGui::SameLine();
                ImGui::Checkbox("depth (hlae_depth)", &s_recDepth);
                if (ImGui::SmallButton("Apply Streams")) {
                    // Ensure streams exist, then set record flags
                    if (s_recNorm) Afx_ExecClientCmd("mirv_streams add normal hlaenorm");
                    if (s_recMatte) Afx_ExecClientCmd("mirv_streams add matteEntity hlaematte");
                    if (s_recDepth) Afx_ExecClientCmd("mirv_streams add depth hlaedepth");
                    Afx_ExecClientCmd(s_recNorm ? "mirv_streams edit hlaenorm record 1" : "mirv_streams edit hlaenorm record 0");
                    Afx_ExecClientCmd(s_recMatte ? "mirv_streams edit hlaematte record 1" : "mirv_streams edit hlaematte record 0");
                    Afx_ExecClientCmd(s_recDepth ? "mirv_streams edit hlaedepth record 1" : "mirv_streams edit hlaedepth record 0");
                }

                // Optional: quick FFmpeg settings for streams (sets afxDefault)
                ImGui::TextUnformatted("Stream profile"); ImGui::SameLine();
                static const char* s_selectedProfile = "Select profile...";
                if (ImGui::BeginCombo("##ffmpeg_profiles", s_selectedProfile)) {
                    if (ImGui::Selectable("TGA Sequence")) { s_selectedProfile = "TGA Sequence"; Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings afxClassic)"); }
                    if (ImGui::Selectable("ProRes 4444")) { s_selectedProfile = "ProRes 4444"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg p0  "-c:v prores  -profile:v 4 {QUOTE}{AFX_STREAM_PATH}\video.mov{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings p0)"); }
                    if (ImGui::Selectable("ProRes 422 HQ")) { s_selectedProfile = "ProRes 422 HQ"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg phq "-c:v prores  -profile:v 3 -pix_fmt yuv422p10le {QUOTE}{AFX_STREAM_PATH}\video.mov{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings phq)"); }
                    if (ImGui::Selectable("ProRes 422")) { s_selectedProfile = "ProRes 422"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg p1  "-c:v prores  -profile:v 2 -pix_fmt yuv422p10le {QUOTE}{AFX_STREAM_PATH}\video.mov{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings p1)"); }
                    if (ImGui::Selectable("x264 Lossless")) { s_selectedProfile = "x264 Lossless"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg c0  "-c:v libx264 -preset 0 -qp  0  -g 120 -keyint_min 1 -pix_fmt yuv422p10le {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings c0)"); }
                    if (ImGui::Selectable("x264 HQ")) { s_selectedProfile = "x264 HQ"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg c1  "-c:v libx264 -preset 1 -crf 4  -qmax 20 -g 120 -keyint_min 1 -pix_fmt yuv420p {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings c1)"); }
                    if (ImGui::Selectable("x265 Lossless")) { s_selectedProfile = "x265 Lossless"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg he0  "-c:v libx265 -x265-params no-sao=1 -preset 0 -lossless -g 120 -keyint_min 1 -pix_fmt yuv422p {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings he0)"); }
                    if (ImGui::Selectable("x265 HQ")) { s_selectedProfile = "x265 HQ"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg he1  "-c:v libx265 -x265-params no-sao=1 -preset 1 -crf 8  -qmax 20 -g 120 -keyint_min 1 -pix_fmt yuv422p {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings he1)"); }
                    ImGui::EndCombo();
                }

                // Depth helper profiles (optional)
                ImGui::TextUnformatted("Depth helper"); ImGui::SameLine();
                static const char* s_selectedDepthProfile = "Select...";
                if (ImGui::BeginCombo("##depth_profiles", s_selectedDepthProfile)) {
                    if (ImGui::Selectable("Disabled")) { s_selectedDepthProfile = "Disabled"; Afx_ExecClientCmd(R"(mirv_streams edit hlaedepth record 0)"); }
                    if (ImGui::Selectable("Default")) { s_selectedDepthProfile = "Default"; Afx_ExecClientCmd(R"(mirv_streams add depth hlaedepth)"); }
                    if (ImGui::Selectable("EXR")) { s_selectedDepthProfile = "EXR"; Afx_ExecClientCmd(R"(mirv_streams add depth hlaedepth; mirv_streams edit hlaedepth depthVal 7; mirv_streams edit hlaedepth depthValMax 8192; mirv_streams edit hlaedepth drawZ rgb; mirv_streams edit hlaedepth drawZMode linear; mirv_streams edit hlaedepth captureType depth24)"); }
                    ImGui::EndCombo();
                }

                ImGui::Separator();
                if (ImGui::Button("Start Recording")) {
                    // host_framerate X; host_timescale 0; mirv_snd_timescale 1; mirv_streams record start
                    float fps = (float)atof(s_fpsText);
                    if (fps <= 0.0f) fps = 60.0f;
                    char cmd[256];
                    sprintf_s(cmd, "host_framerate %.0f; host_timescale 0; mirv_snd_timescale 1; mirv_streams record start", fps);
                    Afx_ExecClientCmd(cmd);
                }
                ImGui::SameLine();
                if (ImGui::Button("Stop")) {
                    Afx_ExecClientCmd("mirv_streams record end; host_framerate 0");
                }
            } else {
                // Other Source 1 games workflow (screen recording)
                ImGui::Separator();
                if (ImGui::SmallButton("Apply pre-load tweaks")) {
                    Afx_ExecClientCmd("engine_no_focus_sleep 0; snd_mute_losefocus 0");
                }

                // Record screen settings
                static bool screenEnabled = false;
                if (ImGui::Checkbox("Record screen enabled", &screenEnabled)) {
                    Afx_ExecClientCmd(screenEnabled ? "mirv_streams record screen enabled 1" : "mirv_streams record screen enabled 0");
                }
                ImGui::TextUnformatted("Record screen profile"); ImGui::SameLine();
                static const char* s_screenProfile = "Select profile...";
                if (ImGui::BeginCombo("##screen_profiles", s_screenProfile)) {
                    if (ImGui::Selectable("TGA Sequence")) { s_screenProfile = "TGA Sequence"; Afx_ExecClientCmd(R"(mirv_streams record screen settings afxClassic)"); }
                    if (ImGui::Selectable("FFmpeg YUV420p")) { s_screenProfile = "FFMPEG YUV420p"; Afx_ExecClientCmd(R"(mirv_streams record screen settings afxFfmpegYuv420p)"); }
                    if (ImGui::Selectable("FFmpeg Lossless Best")) { s_screenProfile = "FFMPEG Lossless Best"; Afx_ExecClientCmd(R"(mirv_streams record screen settings afxFfmpegLosslessBest)"); }
                    if (ImGui::Selectable("ProRes 4444")) { s_screenProfile = "ProRes 4444"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg p0  "-c:v prores  -profile:v 4 {QUOTE}{AFX_STREAM_PATH}\video.mov{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings p0  ;echo [Current Record Setting];echo p0 - ProRes 4444)" ); }
                    if (ImGui::Selectable("ProRes 422 HQ")) { s_screenProfile = "ProRes 422 HQ"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg phq "-c:v prores  -profile:v 3 -pix_fmt yuv422p10le {QUOTE}{AFX_STREAM_PATH}\video.mov{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings phq ;echo [Current Record Setting];echo phq - ProRes 422 HQ)" ); }
                    if (ImGui::Selectable("ProRes 422")) { s_screenProfile = "ProRes 422"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg p1  "-c:v prores  -profile:v 2 -pix_fmt yuv422p10le {QUOTE}{AFX_STREAM_PATH}\video.mov{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings p1  ;echo [Current Record Setting];echo p1 - ProRes 422)" ); }
                    if (ImGui::Selectable("x264 Lossless")) { s_screenProfile = "x264 Lossless"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg c0  "-c:v libx264 -preset 0 -qp  0  -g 120 -keyint_min 1 -pix_fmt yuv422p10le {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings c0  ;echo [Current Record Setting];echo c0 - x264 �-��?Y)" ); }
                    if (ImGui::Selectable("x264 HQ")) { s_screenProfile = "x264 HQ"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg c1  "-c:v libx264 -preset 1 -crf 4  -qmax 20 -g 120 -keyint_min 1 -pix_fmt yuv420p -x264-params ref=3:me=hex:subme=3:merange=12:b-adapt=1:aq-mode=2:aq-strength=0.9:no-fast-pskip=1 {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings c1  ;echo [Current Record Setting];echo c1 - x264 ��~�"����)" ); }
                    if (ImGui::Selectable("x265 Lossless")) { s_screenProfile = "x265 Lossless"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg he0  "-c:v libx265 -x265-params no-sao=1 -preset 0 -lossless -g 120 -keyint_min 1 -pix_fmt yuv422p {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings he0 ;echo [Current Record Setting];echo he0 - x265 �-��?Y)" ); }
                    if (ImGui::Selectable("x265 HQ")) { s_screenProfile = "x265 HQ"; Afx_ExecClientCmd(R"(mirv_streams settings add ffmpeg he1  "-c:v libx265 -x265-params no-sao=1 -preset 1 -crf 8  -qmax 20 -g 120 -keyint_min 1 -pix_fmt yuv422p {QUOTE}{AFX_STREAM_PATH}\video.mp4{QUOTE}")"); Afx_ExecClientCmd(R"(mirv_streams settings edit afxDefault settings he1 ;echo [Current Record Setting];echo he1 - x265 ��~�"����)" ); }
                    ImGui::EndCombo();
                }

                ImGui::Separator();
                if (ImGui::Button("Start Recording")) {
                    // host_framerate X; mirv_streams record fps X; mirv_streams record screen enabled 1; mirv_streams record start
                    float fps = (float)atof(s_fpsText);
                    if (fps <= 0.0f) fps = 1200.0f;
                    char cmd[512];
                    sprintf_s(cmd, "host_framerate %.0f; mirv_streams record fps %.0f; mirv_streams record screen enabled 1; mirv_streams record start", fps, fps);
                    Afx_ExecClientCmd(cmd);
                }
                ImGui::SameLine();
                if (ImGui::Button("Stop")) {
                    Afx_ExecClientCmd("mirv_streams record end; host_framerate 0");
                }
            }

            ImGui::EndTabItem();
        }

        // Settings
        if (ImGui::BeginTabItem("Settings")) {
            ImGuiIO& io = ImGui::GetIO();
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            // UI scale (DPI) controls
            float uiScaleTmp = g_UiScale;
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("000.00x").x + ImGui::GetStyle().FramePadding.x * 6.0f);
            if (ImGui::SliderFloat("UI scale", &uiScaleTmp, 0.50f, 2.00f, "%.2fx", ImGuiSliderFlags_AlwaysClamp)) g_UiScale = uiScaleTmp;
            ImGui::SameLine(); if (ImGui::SmallButton("90%"))  g_UiScale = 0.90f;
            ImGui::SameLine(); if (ImGui::SmallButton("100%")) g_UiScale = 1.00f;
            ImGui::SameLine(); if (ImGui::SmallButton("125%")) g_UiScale = 1.25f;
            ImGui::SameLine(); if (ImGui::SmallButton("150%")) g_UiScale = 1.50f;
            ImGui::SameLine(); if (ImGui::SmallButton("200%")) g_UiScale = 2.00f;
            float scale = g_UiScale;

            // apply font-specific correction
            if (g_FontChoice == 2) { // Sans
                scale *= 1.2f;
            }
            else if (g_FontChoice == 3) { // Silly
                scale *= 1.2f;
            }
            float snapped = floorf(scale * 16.0f + 0.5f) / 16.0f;
            io.FontGlobalScale = snapped;

            {
                if (!g_DemoDialogInit) { g_DemoOpenDialog.SetTitle("Open demo"); g_DemoDialogInit = true; }

                if (ImGui::Button("Open demo")) {
                    // Prefer last-used demo directory, otherwise default to game\csgo
                    if (!g_OverlayPaths.demoDir.empty()) {
                        g_DemoOpenDialog.SetDirectory(g_OverlayPaths.demoDir);
                    }
                    
                    g_DemoOpenDialog.Open();
                }

                // Render and handle selection
                g_DemoOpenDialog.Display();
                if (g_DemoOpenDialog.HasSelected()) {
                    const std::string path = g_DemoOpenDialog.GetSelected().string();
                    char cmd[2048];
                    snprintf(cmd, sizeof(cmd), "playdemo \"%s\"", path.c_str());
                    Afx_ExecClientCmd(cmd);
                    // remember directory
                    try { g_OverlayPaths.demoDir = std::filesystem::path(path).parent_path().string(); } catch(...) {}
                    ImGui::MarkIniSettingsDirty();
                    g_DemoOpenDialog.ClearSelected();
                }
            }
            // UI Font picker
            ImGui::SameLine();
            {
                const char* items[] = { "Default (ImGui)", "Monospace", "Sans Serif", "Silly" };
                int prevChoice = g_FontChoice;

                ImGui::SetNextItemWidth(ImGui::CalcTextSize("Default (ImGui)   ").x + ImGui::GetStyle().FramePadding.x * 8.0f);
                if (ImGui::Combo("UI Font", &g_FontChoice, items, (int)(sizeof(items)/sizeof(items[0]))))
                {
                    ImFont* chosen = g_FontDefault;

                    if (g_FontChoice == 1)      chosen = g_FontMono  ? g_FontMono  : g_FontDefault; // Monospace
                    else if (g_FontChoice == 2) chosen = g_FontSans  ? g_FontSans  : g_FontDefault; // Sans Serif
                    else if (g_FontChoice == 3) chosen = g_FontSilly ? g_FontSilly : g_FontDefault; // Silly

                    ImGui::GetIO().FontDefault = chosen ? chosen : g_FontDefault;
                }

                // If the user picked a font that wasn't found, inform them (non-intrusively).
                if ((g_FontChoice == 1 && !g_FontMono)
                || (g_FontChoice == 2 && !g_FontSans)
                || (g_FontChoice == 3 && !g_FontSilly))
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(font not found, using Default)");
                }
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::End();

    // Sequencer window (ImGui Neo Sequencer) – Source1 port of DX11 implementation
    if (g_ShowSequencer) {
        ImGui::SetNextWindowSize(ImVec2(720, 200), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("HLAE Sequencer", &g_ShowSequencer, ImGuiWindowFlags_NoCollapse)) {
            static ImGui::FrameIndexType s_seqFrame = 0;
            static ImGui::FrameIndexType s_seqStart = 0;
            static ImGui::FrameIndexType s_seqEnd = 0;
            static bool s_seqInit = false;

            // Drive end frame with live demo tick
            int curDemoTick = 0;
            if (GetCurrentDemoTick(curDemoTick)) {
                if (curDemoTick > (int)s_seqEnd) s_seqEnd = (ImGui::FrameIndexType)curDemoTick;
            }
            ImGui::FrameIndexType prevFrame = s_seqFrame;

            // Initial content bounds (max campath tick)
            if (!s_seqInit) {
                int contentMaxTick = 0;
                WrpGlobals* gl = g_Hook_VClient_RenderView.GetGlobals();
                double ipt = gl ? gl->interval_per_tick_get() : (1.0 / 64.0);
                double curTime = g_MirvTime.GetTime();
                double demoNow = 0.0; bool haveDemoNow = GetCurrentDemoTime(demoNow);
                auto tick_from_client = [&](double clientTime)->int {
                    int t = 0;
                    if (!GetDemoTickFromClientTime(curTime, clientTime, t))
                        t = (int)llround(clientTime / ipt);
                    return t < 0 ? 0 : t;
                };
                for (CamPathIterator it = g_Hook_VClient_RenderView.m_CamPath.GetBegin(); it != g_Hook_VClient_RenderView.m_CamPath.GetEnd(); ++it) {
                    double tRel = it.GetTime();
                    double clientTarget = g_Hook_VClient_RenderView.m_CamPath.GetOffset() + tRel;
                    int tick = 0;
                    if (haveDemoNow) {
                        double demoTarget = clientTarget - (curTime - demoNow);
                        tick = (int)llround(demoTarget / ipt);
                    } else {
                        tick = tick_from_client(clientTarget);
                    }
                    if (tick > contentMaxTick) contentMaxTick = tick;
                }
                s_seqStart = 0;
                s_seqEnd = (ImGui::FrameIndexType)(contentMaxTick > 0 ? contentMaxTick : 1);
                s_seqInit = true;
            }

            // Layout sizes similar to DX11
            ImGuiStyle& st = ImGui::GetStyle();
            float availY  = ImGui::GetContentRegionAvail().y;
            float btnRowH = ImGui::GetFrameHeight();
            const ImGuiNeoSequencerStyle& neo = ImGui::GetNeoSequencerStyle();
            float topBarH = neo.TopBarHeight > 0.0f ? neo.TopBarHeight : (ImGui::CalcTextSize("100").y + st.FramePadding.y * 2.0f);
            float zoomH   = ImGui::GetFontSize() * neo.ZoomHeightScale + st.FramePadding.y * 2.0f;
            float rowLabelH = ImGui::CalcTextSize("Campath").y + st.FramePadding.y * 2.0f + neo.ItemSpacing.y * 2.0f;
            float desiredSeqH = topBarH + neo.TopBarSpacing + zoomH + rowLabelH;
            float minSeqH = ImMax(60.0f, topBarH + neo.TopBarSpacing + zoomH + rowLabelH);
            float maxSeqHAvail = ImMax(minSeqH, ImMax(55.0f, availY - btnRowH));
            float seqH = ImClamp(desiredSeqH, minSeqH, maxSeqHAvail);

            ImGui::BeginChild("seq_child", ImVec2(0.0f, seqH), false);
            {
                const ImVec2 seqSize(ImVec2(ImGui::GetContentRegionAvail().x, seqH));
                const ImGuiNeoSequencerFlags seqFlags =
                    ImGuiNeoSequencerFlags_AlwaysShowHeader |
                    ImGuiNeoSequencerFlags_EnableSelection |
                    ImGuiNeoSequencerFlags_Selection_EnableDragging;
                if (ImGui::BeginNeoSequencer("##demo_seq", &s_seqFrame, &s_seqStart, &s_seqEnd, seqSize, seqFlags)) {
                    // Cache (ticks <-> times <-> values)
                    struct KfCache { bool valid=false; std::vector<int32_t> ticks; std::vector<double> times; std::vector<CamPathValue> values; };
                    static KfCache s_cache;
                    static bool s_wasDragging = false;

                    WrpGlobals* gl = g_Hook_VClient_RenderView.GetGlobals();
                    double ipt = gl ? gl->interval_per_tick_get() : (1.0/64.0);
                    double curTime = g_MirvTime.GetTime();
                    double demoNow = 0.0; bool haveDemoNow = GetCurrentDemoTime(demoNow);
                    auto tick_from_client = [&](double clientTime)->int {
                        int t = 0;
                        if (!GetDemoTickFromClientTime(curTime, clientTime, t))
                            t = (int)llround(clientTime / ipt);
                        return t < 0 ? 0 : t;
                    };

                    auto rebuild_cache = [&]() {
                        s_cache.valid = true;
                        s_cache.ticks.clear(); s_cache.times.clear(); s_cache.values.clear();
                        s_cache.ticks.reserve(g_Hook_VClient_RenderView.m_CamPath.GetSize());
                        s_cache.times.reserve(g_Hook_VClient_RenderView.m_CamPath.GetSize());
                        s_cache.values.reserve(g_Hook_VClient_RenderView.m_CamPath.GetSize());
                        for (CamPathIterator it = g_Hook_VClient_RenderView.m_CamPath.GetBegin(); it != g_Hook_VClient_RenderView.m_CamPath.GetEnd(); ++it) {
                            double tRel = it.GetTime();
                            s_cache.times.push_back(tRel);
                            s_cache.values.push_back(it.GetValue());
                            double clientTarget = g_Hook_VClient_RenderView.m_CamPath.GetOffset() + tRel;
                            int tick = haveDemoNow ? (int)llround((clientTarget - (curTime - demoNow)) / ipt) : tick_from_client(clientTarget);
                            if (tick < 0) tick = 0;
                            s_cache.ticks.push_back(tick);
                        }
                    };

                    if (g_SequencerNeedsRefresh) {
                        rebuild_cache();
                        g_SequencerNeedsRefresh = false;
                        s_cache.valid = true;
                        g_CurveCache.valid = false;
                        s_wasDragging = ImGui::NeoIsDraggingSelection();
                    }
                    if (!ImGui::NeoIsDraggingSelection()) {
                        if (!s_cache.valid || s_cache.ticks.size() != g_Hook_VClient_RenderView.m_CamPath.GetSize())
                            rebuild_cache();
                    }

                    bool open = true;
                    if (ImGui::BeginNeoTimelineEx("Campath", &open, ImGuiNeoTimelineFlags_None)) {
                        int rightClickedIndex = -1;
                        static CampathCtx s_ctxMenuPending;
                        static std::vector<int> s_lastUiSelection;
                        std::vector<int> curUiSelection; curUiSelection.reserve(s_cache.ticks.size());

                        for (int i = 0; i < (int)s_cache.ticks.size(); ++i) {
                            ImGui::NeoKeyframe(&s_cache.ticks[i]);
                            if (ImGui::IsNeoKeyframeSelected()) curUiSelection.push_back(i);
                            if (ImGui::IsNeoKeyframeRightClicked()) rightClickedIndex = i;
                        }

                        // Sync UI selection -> engine
                        if (!ImGui::NeoIsSelecting() && !ImGui::NeoIsDraggingSelection()) {
                            auto normalized = [](std::vector<int>& v){ std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end()); };
                            normalized(curUiSelection);
                            std::vector<int> last = s_lastUiSelection; normalized(last);
                            if (last != curUiSelection) {
                                g_Hook_VClient_RenderView.m_CamPath.SelectNone();
                                std::string cmd = "mirv_campath select none;";
                                int rangeStart = -1, prev = -1000000;
                                for (int idx : curUiSelection) {
                                    if (rangeStart < 0) { rangeStart = prev = idx; continue; }
                                    if (idx == prev + 1) { prev = idx; continue; }
                                    g_Hook_VClient_RenderView.m_CamPath.SelectAdd((size_t)rangeStart, (size_t)prev);
                                    char tmp[128]; sprintf_s(tmp, " mirv_campath select add #%d #%d;", rangeStart, prev); cmd += tmp;
                                    rangeStart = prev = idx;
                                }
                                if (rangeStart >= 0) {
                                    g_Hook_VClient_RenderView.m_CamPath.SelectAdd((size_t)rangeStart, (size_t)prev);
                                    char tmp[128]; sprintf_s(tmp, " mirv_campath select add #%d #%d;", rangeStart, prev); cmd += tmp;
                                }
                                Afx_ExecClientCmd(cmd.c_str());
                                g_SequencerNeedsRefresh = true;
                                s_lastUiSelection = curUiSelection;
                            }
                            g_CurveSelection = curUiSelection;
                        }

                        // Context menu on keyframe
                        if (rightClickedIndex >= 0) {
                            s_ctxMenuPending.active = true;
                            s_ctxMenuPending.time = s_cache.times[(size_t)rightClickedIndex];
                            s_ctxMenuPending.value = s_cache.values[(size_t)rightClickedIndex];
                            ImGui::OpenPopup("campath_kf_ctx");
                            ImGui::SetNextWindowPos(ImGui::GetMousePos());
                            ImGui::SetNextWindowSizeConstraints(ImVec2(120,0), ImVec2(300,FLT_MAX));
                        }
                        if (ImGui::BeginPopup("campath_kf_ctx")) {
                            bool doRemove=false, doGet=false, doSet=false;
                            if (ImGui::MenuItem("Remove")) doRemove = true;
                            if (ImGui::MenuItem("Get")) doGet = true;
                            if (ImGui::MenuItem("Set")) doSet = true;
                            if (ImGui::MenuItem("Edit")) { if (s_ctxMenuPending.active) g_LastCampathCtx = s_ctxMenuPending; ImGui::CloseCurrentPopup(); }
                            ImGui::EndPopup();

                            if (s_ctxMenuPending.active) {
                                bool prevDraw = g_CampathDrawer.Draw_get();
                                if (doRemove) {
                                    g_CampathDrawer.Draw_set(false);
                                    g_Hook_VClient_RenderView.m_CamPath.Remove(s_ctxMenuPending.time);
                                    g_CampathDrawer.Draw_set(prevDraw);
                                    g_SequencerNeedsRefresh = true;
                                }
                                if (doGet) {
                                    if (MirvInput* pMirv = g_Hook_VClient_RenderView.m_MirvInput) {
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
                                    g_Hook_VClient_RenderView.m_CamPath.Remove(s_ctxMenuPending.time);
                                    g_Hook_VClient_RenderView.m_CamPath.Add(s_ctxMenuPending.time, newVal);
                                    g_CampathDrawer.Draw_set(prevDraw);
                                    s_ctxMenuPending.active = false;
                                    g_SequencerNeedsRefresh = true;
                                }
                            }
                        }

                        // Retime commit on drag end
                        bool draggingNow = ImGui::NeoIsDraggingSelection();
                        if (s_wasDragging && !draggingNow) {
                            // Gather authoritative values
                            std::vector<CamPathValue> liveValues; liveValues.reserve(g_Hook_VClient_RenderView.m_CamPath.GetSize());
                            for (CamPathIterator it = g_Hook_VClient_RenderView.m_CamPath.GetBegin(); it != g_Hook_VClient_RenderView.m_CamPath.GetEnd(); ++it)
                                liveValues.push_back(it.GetValue());
                            // Preserve selection indices
                            std::vector<int> prevSelectedIdx = g_CurveSelection;
                            int prevGizmoIdx = -1; if (g_LastCampathCtx.active) {
                                int idx=0; const double eps=1e-9; for (CamPathIterator it = g_Hook_VClient_RenderView.m_CamPath.GetBegin(); it != g_Hook_VClient_RenderView.m_CamPath.GetEnd(); ++it,++idx) { if (fabs(it.GetTime()-g_LastCampathCtx.time)<eps) { prevGizmoIdx = idx; break; } }
                            }
                            // Convert ticks -> new times
                            struct TmpKey { double t; CamPathValue v; int origIndex; };
                            std::vector<TmpKey> newKeys; newKeys.reserve(s_cache.ticks.size());
                            for (size_t i=0;i<s_cache.ticks.size();++i) {
                                const double newDemoTime = (double)s_cache.ticks[i] * ipt;
                                const double clientTarget = haveDemoNow ? newDemoTime + (curTime - demoNow) : newDemoTime;
                                double newRelTime = clientTarget - g_Hook_VClient_RenderView.m_CamPath.GetOffset();
                                CamPathValue v = (i < liveValues.size()) ? liveValues[i] : s_cache.values[i];
                                newKeys.push_back({ newRelTime, v, (int)i });
                            }
                            // Sort + nudge identicals
                            std::sort(newKeys.begin(), newKeys.end(), [](const TmpKey&a,const TmpKey&b){return a.t<b.t;});
                            const double eps = 1e-6; for (size_t i=1;i<newKeys.size();++i) if (fabs(newKeys[i].t - newKeys[i-1].t) < eps) newKeys[i].t = newKeys[i-1].t + eps;
                            // Rebuild atomically with draw disabled and selection restored
                            bool prevDraw = g_CampathDrawer.Draw_get(); g_CampathDrawer.Draw_set(false);
                            bool prevEnabled = g_Hook_VClient_RenderView.m_CamPath.Enabled_get(); g_Hook_VClient_RenderView.m_CamPath.Enabled_set(false);
                            g_Hook_VClient_RenderView.m_CamPath.SelectNone();
                            g_Hook_VClient_RenderView.m_CamPath.Clear();
                            for (const auto& k : newKeys) g_Hook_VClient_RenderView.m_CamPath.Add(k.t, k.v);
                            if (!prevSelectedIdx.empty()) {
                                std::vector<int> newSelected;
                                newSelected.reserve(prevSelectedIdx.size());
                                for (int oldIdx : prevSelectedIdx) {
                                    for (size_t ni=0; ni<newKeys.size(); ++ni) if (newKeys[ni].origIndex == oldIdx) { newSelected.push_back((int)ni); break; }
                                }
                                std::sort(newSelected.begin(), newSelected.end()); newSelected.erase(std::unique(newSelected.begin(), newSelected.end()), newSelected.end());
                                int rangeStart=-1, prev=-1000000; std::string cmd = "mirv_campath select none;"; g_Hook_VClient_RenderView.m_CamPath.SelectNone();
                                for (int idx : newSelected) {
                                    if (rangeStart < 0) { rangeStart = prev = idx; continue; }
                                    if (idx == prev + 1) { prev = idx; continue; }
                                    g_Hook_VClient_RenderView.m_CamPath.SelectAdd((size_t)rangeStart, (size_t)prev);
                                    char tmp[128]; sprintf_s(tmp, " mirv_campath select add #%d #%d;", rangeStart, prev); cmd += tmp;
                                    rangeStart = prev = idx;
                                }
                                if (rangeStart >= 0) {
                                    g_Hook_VClient_RenderView.m_CamPath.SelectAdd((size_t)rangeStart, (size_t)prev);
                                    char tmp[128]; sprintf_s(tmp, " mirv_campath select add #%d #%d;", rangeStart, prev); cmd += tmp;
                                }
                                Afx_ExecClientCmd(cmd.c_str());
                            }
                            g_Hook_VClient_RenderView.m_CamPath.Enabled_set(prevEnabled);
                            g_CampathDrawer.Draw_set(prevDraw);
                            for (size_t i=0;i<newKeys.size();++i) s_cache.times[i] = newKeys[i].t; s_cache.valid = false; g_CurveCache.valid = false;
                            if (g_LastCampathCtx.active && prevGizmoIdx >= 0) {
                                int newIdx = -1; for (size_t ni=0; ni<newKeys.size(); ++ni) if (newKeys[ni].origIndex == prevGizmoIdx) { newIdx = (int)ni; break; }
                                if (newIdx >= 0) {
                                    int idx=0; for (CamPathIterator it = g_Hook_VClient_RenderView.m_CamPath.GetBegin(); it != g_Hook_VClient_RenderView.m_CamPath.GetEnd(); ++it,++idx) if (idx==newIdx) { g_LastCampathCtx.time = it.GetTime(); g_LastCampathCtx.value = it.GetValue(); break; }
                                } else {
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

            // Controls row
            if (ImGui::SmallButton("Toggle Pause")) { Afx_ExecClientCmd("demo_togglepause"); }
            ImGui::SameLine(); if (ImGui::SmallButton("0.2x")) { Afx_ExecClientCmd("demo_timescale 0.2"); }
            ImGui::SameLine(); if (ImGui::SmallButton("0.5x")) { Afx_ExecClientCmd("demo_timescale 0.5"); }
            ImGui::SameLine(); if (ImGui::SmallButton("1x")) { Afx_ExecClientCmd("demo_timescale 1"); }
            ImGui::SameLine(); if (ImGui::SmallButton("2x")) { Afx_ExecClientCmd("demo_timescale 2"); }
            ImGui::SameLine(); if (ImGui::SmallButton("5x")) { Afx_ExecClientCmd("demo_timescale 5"); }
            ImGui::SameLine(); ImGui::Text("Start: %d  End: %d  Current: %d", (int)s_seqStart, (int)s_seqEnd, (int)s_seqFrame);
            ImGui::SameLine(); if (ImGui::SmallButton(g_PreviewFollow ? "Detach Preview" : "Follow Preview")) { g_PreviewFollow = !g_PreviewFollow; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Add keyframe at preview")) {
                if (g_Hook_VClient_RenderView.m_CamPath.GetSize() >= 2 && g_Hook_VClient_RenderView.m_CamPath.CanEval()) {
                    double tMin = g_Hook_VClient_RenderView.m_CamPath.GetLowerBound();
                    double tMax = g_Hook_VClient_RenderView.m_CamPath.GetUpperBound(); if (tMax <= tMin) tMax = tMin + 1.0;
                    double tEval = tMin + (double)g_PreviewNorm * (tMax - tMin);
                    double cx,cy,cz, rX,rY,rZ; float cfov; Afx_GetLastCameraData(cx,cy,cz,rX,rY,rZ,cfov);
                    CamPathValue nv(cx,cy,cz,rX,rY,rZ,cfov);
                    bool prevDraw = g_CampathDrawer.Draw_get(); g_CampathDrawer.Draw_set(false);
                    g_Hook_VClient_RenderView.m_CamPath.Add(tEval, nv);
                    g_CampathDrawer.Draw_set(prevDraw);
                    g_SequencerNeedsRefresh = true;
                    advancedfx::Message("Overlay: mirv_campath add at preview (t=%.3f)\n", tEval);
                } else {
                    advancedfx::Warning("Overlay: Not enough campath keyframes to add at preview.\n");
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(g_ShowCurveEditor ? "Hide Curve Editor" : "Show Curve Editor"))
                g_ShowCurveEditor = !g_ShowCurveEditor;
            // Standalone Curve Editor (outside Neo timeline)
            if (g_ShowCurveEditor) {
                ImGui::Separator();
                ImGui::SeparatorText("Curve Editor");
                // Removed channel selector; editing is driven by visibility checkboxes and optional focus
                if (ImGui::Button("Enable")) {
                    Afx_ExecClientCmd("mirv_campath edit interp position custom; mirv_campath edit interp rotation custom; mirv_campath edit interp fov custom");
                }
                ImGui::SameLine();
                ImGui::Checkbox("Normalized", &g_CurveNormalize);
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                // Checkboxes with right-click focus toggle
                ImGui::SameLine();
                ImGui::Checkbox("X##show",     &g_CurveShow[0]);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { g_CurveFocusChannel = (g_CurveFocusChannel == 0 ? -1 : 0); if (g_CurveFocusChannel == 0) g_CurveShow[0] = true; }
                if (!g_CurveShow[0] && g_CurveFocusChannel == 0) g_CurveFocusChannel = -1;
                ImGui::SameLine();
                ImGui::Checkbox("Y##show",     &g_CurveShow[1]);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { g_CurveFocusChannel = (g_CurveFocusChannel == 1 ? -1 : 1); if (g_CurveFocusChannel == 1) g_CurveShow[1] = true; }
                if (!g_CurveShow[1] && g_CurveFocusChannel == 1) g_CurveFocusChannel = -1;
                ImGui::SameLine();
                ImGui::Checkbox("Z##show",     &g_CurveShow[2]);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { g_CurveFocusChannel = (g_CurveFocusChannel == 2 ? -1 : 2); if (g_CurveFocusChannel == 2) g_CurveShow[2] = true; }
                if (!g_CurveShow[2] && g_CurveFocusChannel == 2) g_CurveFocusChannel = -1;
                ImGui::SameLine();
                ImGui::Checkbox("FOV##show",   &g_CurveShow[3]);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { g_CurveFocusChannel = (g_CurveFocusChannel == 3 ? -1 : 3); if (g_CurveFocusChannel == 3) g_CurveShow[3] = true; }
                if (!g_CurveShow[3] && g_CurveFocusChannel == 3) g_CurveFocusChannel = -1;
                ImGui::SameLine();
                ImGui::Checkbox("Pitch##show", &g_CurveShow[4]);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { g_CurveFocusChannel = (g_CurveFocusChannel == 4 ? -1 : 4); if (g_CurveFocusChannel == 4) g_CurveShow[4] = true; }
                if (!g_CurveShow[4] && g_CurveFocusChannel == 4) g_CurveFocusChannel = -1;
                ImGui::SameLine();
                ImGui::Checkbox("Yaw##show",   &g_CurveShow[5]);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { g_CurveFocusChannel = (g_CurveFocusChannel == 5 ? -1 : 5); if (g_CurveFocusChannel == 5) g_CurveShow[5] = true; }
                if (!g_CurveShow[5] && g_CurveFocusChannel == 5) g_CurveFocusChannel = -1;
                ImGui::SameLine();
                ImGui::Checkbox("Roll##show",  &g_CurveShow[6]);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { g_CurveFocusChannel = (g_CurveFocusChannel == 6 ? -1 : 6); if (g_CurveFocusChannel == 6) g_CurveShow[6] = true; }
                if (!g_CurveShow[6] && g_CurveFocusChannel == 6) g_CurveFocusChannel = -1;
                // Sliders on same row with reserved right space
                // Two equal-width sliders that grow/shrink with window size
                ImGui::SameLine();
                {
                    const ImGuiStyle& st = ImGui::GetStyle();
                    const float rightGap = 110.0f * g_UiScale; // small reserve at far right to avoid clipping
                    float avail = ImGui::GetContentRegionAvail().x;
                    float per = (avail - rightGap - st.ItemSpacing.x) * 0.5f;
                    //if (per < 100.0f) per = (avail > 0.0f ? avail * 0.45f : 100.0f);

                    ImGui::SetNextItemWidth(per);
                    ImGui::SliderFloat("Zoom Y", &g_CurveValueScale, 0.1f, 10.0f, "%.2fx");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(per);
                    ImGui::SliderFloat("Offset Y", &g_CurveValueOffset, -1000.0f, 1000.0f, "%.1f");
                }

                // Keep cache fresh (standalone)
                if (!g_CurveCache.valid || g_CurveCache.times.size() != g_Hook_VClient_RenderView.m_CamPath.GetSize())
                    CurveCache_Rebuild();

                if (g_CurveCache.times.size() >= 4) {
                    ImVec2 canvasSize(ImGui::GetContentRegionAvail().x, 220.0f);
                    ImGui::BeginChild("##campath_curve2", canvasSize, true, ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImVec2 p0 = ImGui::GetCursorScreenPos();
                    ImVec2 p1 = ImVec2(p0.x + canvasSize.x, p0.y + canvasSize.y);

                    // Background + border
                    dl->AddRectFilled(p0, p1, IM_COL32(18,22,26,255));
                    dl->AddRect(p0, p1, IM_COL32(60,70,80,255));

                    // Screen-space helpers
                    auto toX = [&](double t){
                        double tMin = g_CurveCache.times.front(); double tMax = g_CurveCache.times.back();
                        double u = (t - tMin) / (tMax - tMin + 1e-12);
                        return p0.x + (float)u * (canvasSize.x - 2*g_CurvePadding) + g_CurvePadding;
                    };
                    auto fromX = [&](float px)->double {
                        double tMin = g_CurveCache.times.front();
                        double tMax = g_CurveCache.times.back();
                        double u = (px - (p0.x + g_CurvePadding)) / (canvasSize.x - 2*g_CurvePadding);
                        if (u < 0.0) u = 0.0; if (u > 1.0) u = 1.0;
                        return tMin + u * (tMax - tMin + 1e-12);
                    };

                    // Grid
                    for (int i = 0; i <= 10; ++i) {
                        float x = p0.x + i * (canvasSize.x - 2*g_CurvePadding)/10.0f + g_CurvePadding;
                        dl->AddLine(ImVec2(x, p0.y + g_CurvePadding), ImVec2(x, p1.y - g_CurvePadding), IM_COL32(40,48,56,255));
                    }
                    for (int i = 0; i <= 6; ++i) {
                        float y = p0.y + i * (canvasSize.y - 2*g_CurvePadding)/6.0f + g_CurvePadding;
                        dl->AddLine(ImVec2(p0.x + g_CurvePadding, y), ImVec2(p1.x - g_CurvePadding, y), IM_COL32(40,48,56,255));
                    }

                    // Which channels to draw this frame
                    bool drawMask[7] = { false,false,false,false, false, false, false };
                    for (int c=0;c<7;++c) drawMask[c] = g_CurveShow[c];

                    // Determine anchor channel for non-normalized mapping when multiple channels are shown.
                    int firstVisible = -1;
                    for (int c=0;c<7;++c) { if (g_CurveShow[c]) { firstVisible = c; break; } }
                    int anchorCh = (g_CurveFocusChannel >= 0 ? g_CurveFocusChannel : firstVisible);
                    if (anchorCh < 0) anchorCh = 0; // fallback if none visible

                    // Per-channel colors
                    auto chColor = [&](int ch)->ImU32 {
                        switch (ch) {
                            case 0: return IM_COL32(236, 86, 86, 255);  // X
                            case 1: return IM_COL32(93,  201, 103,255); // Y
                            case 2: return IM_COL32(65,  156, 255,255); // Z
                            case 3: return IM_COL32(255, 200,  80,255); // FOV
                            case 4: return IM_COL32(200, 120, 255,255); // Pitch
                            case 5: return IM_COL32(120, 200, 255,255); // Yaw
                            case 6: return IM_COL32(255, 120, 200,255); // Roll
                            default:return IM_COL32(255, 255, 255,255);
                        }
                    };
                    auto chHandleColor = [&](int ch)->ImU32 {
                        return ImGui::GetColorU32(ImGuiCol_PlotLinesHovered);
                    };

                    double vCenter[7] = {0,0,0,0,0,0,0}, vHalf[7] = {1,1,1,1,1,1,1};
                    for (int ch=0; ch<7; ++ch) {
                        if (!drawMask[ch]) continue;
                        bool first = true;
                        double vMin=0.0, vMax=0.0;
                        double prev = 0.0; bool havePrev = false;
                        for (size_t i=0;i<g_CurveCache.values.size();++i) {
                            const CamPathValue& v = g_CurveCache.values[i];
                            double val;
                            if (ch <= 3) {
                                val = (ch==0? v.X : ch==1? v.Y : ch==2? v.Z : v.Fov);
                            } else {
                                QEulerAngles e = v.R.ToQREulerAngles().ToQEulerAngles();
                                val = (ch==4? e.Pitch : ch==5? e.Yaw : e.Roll);
                                if (havePrev) {
                                    while (val - prev > 180.0) val -= 360.0;
                                    while (val - prev < -180.0) val += 360.0;
                                }
                                prev = val; havePrev = true;
                            }
                            if (first) { vMin=vMax=val; first=false; }
                            else { vMin = (std::min)(vMin,val); vMax = (std::max)(vMax,val); }
                        }
                        if (first) { vMin=0; vMax=1; }
                        if (vMax - vMin < 1e-6) vMax = vMin + 1.0;
                        vCenter[ch] = 0.5*(vMin+vMax) + (double)g_CurveValueOffset;
                        vHalf[ch]   = 0.5*(vMax - vMin);
                    }

                    // Map value -> screen Y for a specific channel
                    auto toY_ch = [&](int ch, double v)->float {
                        if (g_CurveNormalize) {
                            double norm = (v - vCenter[ch]) / (vHalf[ch] > 1e-12 ? vHalf[ch] : 1e-12); // [-1..1]
                            double yNorm = (norm * 0.5 * (double)g_CurveValueScale + 0.5);             // 0..1
                            if (yNorm < 0.0) yNorm = 0.0; if (yNorm > 1.0) yNorm = 1.0;
                            return p1.y - (float)yNorm * (canvasSize.y - 2.0f * g_CurvePadding) - g_CurvePadding;
                        } else {
                            // Fall back to single-channel mapping using anchor channel's scale/center.
                            // Anchor = focused channel if any, else first visible.
                            int ach = anchorCh;
                            double norm = (v - vCenter[ach]) / (vHalf[ach] > 1e-12 ? vHalf[ach] : 1e-12);
                            double yNorm = (norm * 0.5 * (double)g_CurveValueScale + 0.5);
                            if (yNorm < 0.0) yNorm = 0.0; if (yNorm > 1.0) yNorm = 1.0;
                            return p1.y - (float)yNorm * (canvasSize.y - 2.0f * g_CurvePadding) - g_CurvePadding;
                        }
                    };

                    // Helpers to fetch a key by index (stable)
                    auto getValueAt = [&](size_t idx)->CamPathValue {
                        size_t j = 0; CamPathValue out{};
                        for (CamPathIterator it = g_Hook_VClient_RenderView.m_CamPath.GetBegin(); it != g_Hook_VClient_RenderView.m_CamPath.GetEnd(); ++it, ++j) { if (j == idx) { out = it.GetValue(); break; } }
                        return out;
                    };
                    auto compEval = [&](int ch, double t)->double {
                        CamPathValue vv = g_Hook_VClient_RenderView.m_CamPath.Eval(t);
                        if (ch <= 3) return (ch==0? vv.X : ch==1? vv.Y : ch==2? vv.Z : vv.Fov);
                        QEulerAngles e = vv.R.ToQREulerAngles().ToQEulerAngles();
                        return (ch==4? e.Pitch : ch==5? e.Yaw : e.Roll);
                    };

                    // Draw segments
                    // Keep per-channel running continuity for Euler channels so adjacent segments align visually
                    auto unwrapNear = [](double v, double ref)->double {
                        while (v - ref > 180.0) v -= 360.0;
                        while (v - ref < -180.0) v += 360.0;
                        return v;
                    };
                    bool prevValid[7] = {false,false,false,false,false,false,false};
                    double prevYEnd[7] = {0,0,0,0,0,0,0};
                    for (size_t i = 0; i + 1 < g_CurveCache.times.size(); ++i) {
                        double t0 = g_CurveCache.times[i]; double t1 = g_CurveCache.times[i+1];
                        const CamPathValue& cv0 = g_CurveCache.values[i];
                        const CamPathValue& cv1 = g_CurveCache.values[i+1];
                        double h = (t1 - t0);
                        double eps = (std::max)(1e-6, (std::min)(h * 0.01, 0.05));

                        CamPathValue vLeft  = getValueAt(i);
                        CamPathValue vRight = getValueAt(i+1);

                        // Per-channel draw
                        for (int ch=0; ch<7; ++ch) {
                            if (!drawMask[ch]) continue;

                            double y0, y1;
                            if (ch <= 3) {
                                y0 = (ch==0? cv0.X : ch==1? cv0.Y : ch==2? cv0.Z : cv0.Fov);
                                y1 = (ch==0? cv1.X : ch==1? cv1.Y : ch==2? cv1.Z : cv1.Fov);
                            } else {
                                QEulerAngles e0 = cv0.R.ToQREulerAngles().ToQEulerAngles();
                                QEulerAngles e1 = cv1.R.ToQREulerAngles().ToQEulerAngles();
                                double y0raw = (ch==4? e0.Pitch : ch==5? e0.Yaw : e0.Roll);
                                double y1raw = (ch==4? e1.Pitch : ch==5? e1.Yaw : e1.Roll);
                                // Ensure continuity across segments relative to previous segment end
                                if (prevValid[ch]) {
                                    y0 = unwrapNear(y0raw, prevYEnd[ch]);
                                } else {
                                    y0 = y0raw;
                                }
                                y1 = unwrapNear(y1raw, y0);
                            }

                            // Which interpolation is active for this channel?
                            bool isCustom = (ch==3) ? (g_Hook_VClient_RenderView.m_CamPath.FovInterpMethod_get() == CamPath::DI_CUSTOM)
                                                    : (ch<=2 ? (g_Hook_VClient_RenderView.m_CamPath.PositionInterpMethod_get() == CamPath::DI_CUSTOM)
                                                            : (g_Hook_VClient_RenderView.m_CamPath.RotationInterpMethod_get() == CamPath::QI_CUSTOM));

                            // Effective OUT slope at left key
                            double mOut;
                            if (isCustom) {
                                unsigned char modeOut = (ch==0? vLeft.TxModeOut : ch==1? vLeft.TyModeOut : ch==2? vLeft.TzModeOut : ch==3? vLeft.TfovModeOut : (ch==4? vLeft.TRyModeOut : ch==5? vLeft.TRzModeOut : vLeft.TRxModeOut));
                                if (modeOut == (unsigned char)CamPath::TM_FREE) mOut = (ch==0? vLeft.TxOut : ch==1? vLeft.TyOut : ch==2? vLeft.TzOut : ch==3? vLeft.TfovOut : (ch==4? vLeft.TRyOut : ch==5? vLeft.TRzOut : vLeft.TRxOut));
                                else if (modeOut == (unsigned char)CamPath::TM_FLAT) mOut = 0.0;
                                else if (modeOut == (unsigned char)CamPath::TM_LINEAR) mOut = (y1 - y0) / h;
                                else /* AUTO */ {
                                    double v = compEval(ch, t0 + eps);
                                    if (ch >= 4) v = unwrapNear(v, y0);
                                    mOut = (v - y0) / eps;
                                }
                            } else {
                                double v = compEval(ch, t0 + eps);
                                if (ch >= 4) v = unwrapNear(v, y0);
                                mOut = (v - y0) / eps;
                            }

                            // Effective IN slope at right key
                            double mIn;
                            if (isCustom) {
                                unsigned char modeIn = (ch==0? vRight.TxModeIn : ch==1? vRight.TyModeIn : ch==2? vRight.TzModeIn : ch==3? vRight.TfovModeIn : (ch==4? vRight.TRyModeIn : ch==5? vRight.TRzModeIn : vRight.TRxModeIn));
                                if (modeIn == (unsigned char)CamPath::TM_FREE) mIn = (ch==0? vRight.TxIn : ch==1? vRight.TyIn : ch==2? vRight.TzIn : ch==3? vRight.TfovIn : (ch==4? vRight.TRyIn : ch==5? vRight.TRzIn : vRight.TRxIn));
                                else if (modeIn == (unsigned char)CamPath::TM_FLAT) mIn = 0.0;
                                else if (modeIn == (unsigned char)CamPath::TM_LINEAR) mIn = (y1 - y0) / h;
                                else /* AUTO */ {
                                    double v = compEval(ch, t1 - eps);
                                    if (ch >= 4) v = unwrapNear(v, y1);
                                    mIn = (y1 - v) / eps;
                                }
                            } else {
                                double v = compEval(ch, t1 - eps);
                                if (ch >= 4) v = unwrapNear(v, y1);
                                mIn = (y1 - v) / eps;
                            }

                            // Per-side weights
                            double wOut = 1.0, wIn = 1.0;
                            {
                                unsigned char modeOut = (ch==0? vLeft.TxModeOut : ch==1? vLeft.TyModeOut : ch==2? vLeft.TzModeOut : ch==3? vLeft.TfovModeOut : (ch==4? vLeft.TRyModeOut : ch==5? vLeft.TRzModeOut : vLeft.TRxModeOut));
                                unsigned char modeIn  = (ch==0? vRight.TxModeIn  : ch==1? vRight.TyModeIn  : ch==2? vRight.TzModeIn  : ch==3? vRight.TfovModeIn  : (ch==4? vRight.TRyModeIn : ch==5? vRight.TRzModeIn : vRight.TRxModeIn));
                                if (modeOut == CamPath::TM_FREE) wOut = (ch==0? vLeft.TxWOut : ch==1? vLeft.TyWOut : ch==2? vLeft.TzWOut : ch==3? vLeft.TfovWOut : (ch==4? vLeft.TRyWOut : ch==5? vLeft.TRzWOut : vLeft.TRxWOut));
                                if (modeIn  == CamPath::TM_FREE) wIn  = (ch==0? vRight.TxWIn  : ch==1? vRight.TyWIn  : ch==2? vRight.TzWIn  : ch==3? vRight.TfovWIn  : (ch==4? vRight.TRyWIn  : ch==5? vRight.TRzWIn  : vRight.TRxWIn ));
                                if (wOut < 0.0) wOut = 0.0; if (wIn < 0.0) wIn = 0.0;
                            }

                            // Control points for channel ch
                            ImVec2 P0(toX(t0), toY_ch(ch, y0));
                            ImVec2 P3(toX(t1), toY_ch(ch, y1));
                            ImVec2 P1(toX(t0 + wOut * (h/3.0)), toY_ch(ch, y0 + wOut * (h/3.0) * mOut));
                            ImVec2 P2(toX(t1 - wIn  * (h/3.0)), toY_ch(ch, y1 - wIn  * (h/3.0) * mIn));

                            // Curve
                            ImU32 c = chColor(ch);
                            dl->AddBezierCubic(P0, P1, P2, P3, c, 2.0f);

                            // Remember continuity for next segment (Euler channels)
                            if (ch >= 4) { prevValid[ch] = true; prevYEnd[ch] = y1; }

                            // Handles + hit: show handles on all visible by default, or only focused channel if focus is set
                            if ((g_CurveFocusChannel < 0 && drawMask[ch]) || (g_CurveFocusChannel == ch)) {
                                float r = 4.0f;
                                dl->AddCircleFilled(P0, r, IM_COL32(220,220,220,255));
                                dl->AddCircleFilled(P3, r, IM_COL32(220,220,220,255));
                                dl->AddLine(P0,P1,IM_COL32(180,128,64,200)); dl->AddLine(P3,P2,IM_COL32(180,128,64,200));
                                dl->AddCircleFilled(P1, r, chHandleColor(ch)); dl->AddCircleFilled(P2, r, chHandleColor(ch));

                                // Right-click P0/P3 to open context menu (IDs include channel to avoid conflicts)
                                {
                                    float hitR = 6.0f;
                                    ImGui::SetCursorScreenPos(ImVec2(P0.x - hitR, P0.y - hitR));
                                    char idK0[48]; snprintf(idK0, sizeof(idK0), "##curve_key_p0_%zu_ch%d", i, ch);
                                    ImGui::InvisibleButton(idK0, ImVec2(2*hitR, 2*hitR));
                                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { g_CurveCtxKeyIndex = (int)i; g_CurveCtxChannel = ch; ImGui::OpenPopup("campath_curve_kf_ctx"); }

                                    ImGui::SetCursorScreenPos(ImVec2(P3.x - hitR, P3.y - hitR));
                                    char idK3[48]; snprintf(idK3, sizeof(idK3), "##curve_key_p3_%zu_ch%d", i, ch);
                                    ImGui::InvisibleButton(idK3, ImVec2(2*hitR, 2*hitR));
                                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { g_CurveCtxKeyIndex = (int)(i + 1); g_CurveCtxChannel = ch; ImGui::OpenPopup("campath_curve_kf_ctx"); }
                                }

                                // Drag handles (vertical only) – your existing logic, but use channel `ch`
                                // Left handle
                                ImGui::SetCursorScreenPos(ImVec2(P1.x-6,P1.y-6));
                                char id1[48]; snprintf(id1, sizeof(id1), "##h1b_%zu_ch%d", i, ch);
                                ImGui::InvisibleButton(id1, ImVec2(12,12));
                                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                                    ImVec2 mp = ImGui::GetIO().MousePos;
                                    float newY = (std::max)(p0.y+g_CurvePadding, (std::min)(mp.y, p1.y-g_CurvePadding));

                                    // invert mapping helper (per-channel or active mapping)
                                    auto fromYToValue = [&](int chX)->double {
                                        double yNorm = (p1.y - newY - g_CurvePadding) / (canvasSize.y - 2*g_CurvePadding);
                                        double norm = (yNorm - 0.5) / (0.5 * (double)g_CurveValueScale);
                                        if (norm < -10.0) norm = -10.0; if (norm > 10.0) norm = 10.0;
                                        bool perChannel = g_CurveNormalize || (g_CurveFocusChannel < 0);
                                        int ach2 = anchorCh;
                                        return perChannel ? (vCenter[chX] + norm * vHalf[chX])
                                                          : (vCenter[ach2] + norm * vHalf[ach2]);
                                    };

                                    auto compOf = [&](const CamPathValue& v, int chX)->double {
                                        if (chX <= 3) return chX==0? v.X : chX==1? v.Y : chX==2? v.Z : v.Fov;
                                        QEulerAngles e = v.R.ToQREulerAngles().ToQEulerAngles();
                                        return chX==4? e.Pitch : chX==5? e.Yaw : e.Roll;
                                    };

                                    auto mapChToEnum = [&](int chX)->CamPath::Channel {
                                        return (chX==0? CamPath::CH_X : chX==1? CamPath::CH_Y : chX==2? CamPath::CH_Z : chX==3? CamPath::CH_FOV : (chX==4? CamPath::CH_RPITCH : chX==5? CamPath::CH_RYAW : CamPath::CH_RROLL));
                                    };

                                    extern CCampathDrawer g_CampathDrawer; bool prevDraw = g_CampathDrawer.Draw_get(); g_CampathDrawer.Draw_set(false);
                                    g_Hook_VClient_RenderView.m_CamPath.SelectNone(); g_Hook_VClient_RenderView.m_CamPath.SelectAdd((size_t)i,(size_t)i);

                                    ImGuiIO& io = ImGui::GetIO();
                                    bool altHeld = io.KeyAlt || ((io.KeyMods & ImGuiMod_Alt) != 0);
                                    bool shiftHeld = io.KeyShift || ((io.KeyMods & ImGuiMod_Shift) != 0);

                                    CamPath::Channel cch2 = mapChToEnum(ch);
                                    if (shiftHeld) {
                                        double tMouse = fromX(ImGui::GetIO().MousePos.x);
                                        double newW = (tMouse - t0) / (h/3.0);
                                        newW = (std::max)(0.01, (std::min)(newW, 5.0));
                                        g_Hook_VClient_RenderView.m_CamPath.SetTangentMode(cch2, false, true, (unsigned char)CamPath::TM_FREE);
                                        g_Hook_VClient_RenderView.m_CamPath.SetTangentWeight(cch2, false, true, 1.0, newW);
                                    } else {
                                        double baseY0 = y0;
                                        double newV = fromYToValue(ch);
                                        if (ch >= 4) { while (newV - baseY0 > 180.0) newV -= 360.0; while (newV - baseY0 < -180.0) newV += 360.0; }
                                        double newSlope = (newV - baseY0) / (h/3.0);
                                        if (altHeld) {
                                            g_Hook_VClient_RenderView.m_CamPath.SetTangentMode(cch2, false, true, (unsigned char)CamPath::TM_FREE);
                                            g_Hook_VClient_RenderView.m_CamPath.SetTangent(cch2, false, true, 0.0, newSlope);
                                        } else {
                                            g_Hook_VClient_RenderView.m_CamPath.SetTangentMode(cch2, true, true, (unsigned char)CamPath::TM_FREE);
                                            g_Hook_VClient_RenderView.m_CamPath.SetTangent(cch2, true, true, newSlope, newSlope);
                                        }
                                    }
                                    g_CampathDrawer.Draw_set(prevDraw);
                                    g_SequencerNeedsRefresh = true;
                                }

                                // Right handle
                                ImGui::SetCursorScreenPos(ImVec2(P2.x-6,P2.y-6));
                                char id2[48]; snprintf(id2, sizeof(id2), "##h2b_%zu_ch%d", i, ch);
                                ImGui::InvisibleButton(id2, ImVec2(12,12));
                                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                                    ImVec2 mp = ImGui::GetIO().MousePos;
                                    float newY = (std::max)(p0.y+g_CurvePadding, (std::min)(mp.y, p1.y-g_CurvePadding));
                                    auto fromYToValue = [&](int chX)->double {
                                        double yNorm = (p1.y - newY - g_CurvePadding) / (canvasSize.y - 2*g_CurvePadding);
                                        double norm = (yNorm - 0.5) / (0.5 * (double)g_CurveValueScale);
                                        if (norm < -10.0) norm = -10.0; if (norm > 10.0) norm = 10.0;
                                        bool perChannel = g_CurveNormalize || (g_CurveFocusChannel < 0);
                                        int ach2 = anchorCh;
                                        return perChannel ? (vCenter[chX] + norm * vHalf[chX])
                                                          : (vCenter[ach2] + norm * vHalf[ach2]);
                                    };
                                    auto compOf = [&](const CamPathValue& v, int chX)->double {
                                        if (chX <= 3) return chX==0? v.X : chX==1? v.Y : chX==2? v.Z : v.Fov;
                                        QEulerAngles e = v.R.ToQREulerAngles().ToQEulerAngles();
                                        return chX==4? e.Pitch : chX==5? e.Yaw : e.Roll;
                                    };
                                    auto mapChToEnum = [&](int chX)->CamPath::Channel {
                                        return (chX==0? CamPath::CH_X : chX==1? CamPath::CH_Y : chX==2? CamPath::CH_Z : chX==3? CamPath::CH_FOV : (chX==4? CamPath::CH_RPITCH : chX==5? CamPath::CH_RYAW : CamPath::CH_RROLL));
                                    };

                                    extern CCampathDrawer g_CampathDrawer; bool prevDraw = g_CampathDrawer.Draw_get(); g_CampathDrawer.Draw_set(false);
                                    g_Hook_VClient_RenderView.m_CamPath.SelectNone(); g_Hook_VClient_RenderView.m_CamPath.SelectAdd((size_t)(i+1),(size_t)(i+1));

                                    ImGuiIO& io2 = ImGui::GetIO();
                                    bool altHeld2 = io2.KeyAlt || ((io2.KeyMods & ImGuiMod_Alt) != 0);
                                    bool shiftHeld2 = io2.KeyShift || ((io2.KeyMods & ImGuiMod_Shift) != 0);

                                    CamPath::Channel cch2 = mapChToEnum(ch);
                                    if (shiftHeld2) {
                                        double tMouse = fromX(ImGui::GetIO().MousePos.x);
                                        double newW = (t1 - tMouse) / (h/3.0);
                                        newW = (std::max)(0.01, (std::min)(newW, 5.0));
                                        g_Hook_VClient_RenderView.m_CamPath.SetTangentMode(cch2, true, false, (unsigned char)CamPath::TM_FREE);
                                        g_Hook_VClient_RenderView.m_CamPath.SetTangentWeight(cch2, true, false, newW, 1.0);
                                    } else {
                                        double baseY1 = y1;
                                        double newV = fromYToValue(ch);
                                        if (ch >= 4) { while (baseY1 - newV > 180.0) newV += 360.0; while (baseY1 - newV < -180.0) newV -= 360.0; }
                                        double newSlope = (baseY1 - newV) / (h/3.0);
                                        if (altHeld2) {
                                            g_Hook_VClient_RenderView.m_CamPath.SetTangentMode(cch2, true, false, (unsigned char)CamPath::TM_FREE);
                                            g_Hook_VClient_RenderView.m_CamPath.SetTangent(cch2, true, false, newSlope, 0.0);
                                        } else {
                                            g_Hook_VClient_RenderView.m_CamPath.SetTangentMode(cch2, true, true, (unsigned char)CamPath::TM_FREE);
                                            g_Hook_VClient_RenderView.m_CamPath.SetTangent(cch2, true, true, newSlope, newSlope);
                                        }
                                    }
                                    g_CampathDrawer.Draw_set(prevDraw);
                                    g_SequencerNeedsRefresh = true;
                                }
                            } // active-channel handles
                        } // channels
                    } // segments

                    // Draw the curve-editor popup ONCE per frame (outside loops)
                    if (ImGui::BeginPopup("campath_curve_kf_ctx")) {
                        if (g_CurveCtxKeyIndex >= 0 && g_CurveCtxChannel >= 0) {
                            auto mapChToEnum = [&](int chX)->CamPath::Channel { return (chX==0? CamPath::CH_X : chX==1? CamPath::CH_Y : chX==2? CamPath::CH_Z : chX==3? CamPath::CH_FOV : (chX==4? CamPath::CH_RPITCH : chX==5? CamPath::CH_RYAW : CamPath::CH_RROLL)); };

                            auto ApplyMode = [&](bool setIn, bool setOut, unsigned char mode) {
                                extern CCampathDrawer g_CampathDrawer;
                                bool prevDraw = g_CampathDrawer.Draw_get();
                                g_CampathDrawer.Draw_set(false);
                                g_Hook_VClient_RenderView.m_CamPath.SelectNone();
                                g_Hook_VClient_RenderView.m_CamPath.SelectAdd((size_t)g_CurveCtxKeyIndex, (size_t)g_CurveCtxKeyIndex);
                                g_Hook_VClient_RenderView.m_CamPath.SetTangentMode(mapChToEnum(g_CurveCtxChannel), setIn, setOut, mode);
                                g_CampathDrawer.Draw_set(prevDraw);
                                g_SequencerNeedsRefresh = true;
                            };

                            if (ImGui::BeginMenu("Tangent mode")) {
                                if (ImGui::BeginMenu("OUT"))   { if (ImGui::MenuItem("Auto"))   ApplyMode(false,true, CamPath::TM_AUTO);   if (ImGui::MenuItem("Flat"))   ApplyMode(false,true, CamPath::TM_FLAT);   if (ImGui::MenuItem("Linear")) ApplyMode(false,true, CamPath::TM_LINEAR); if (ImGui::MenuItem("Free"))   ApplyMode(false,true, CamPath::TM_FREE);   ImGui::EndMenu(); }
                                if (ImGui::BeginMenu("IN"))    { if (ImGui::MenuItem("Auto"))   ApplyMode(true,false, CamPath::TM_AUTO);   if (ImGui::MenuItem("Flat"))   ApplyMode(true,false, CamPath::TM_FLAT);   if (ImGui::MenuItem("Linear")) ApplyMode(true,false, CamPath::TM_LINEAR); if (ImGui::MenuItem("Free"))   ApplyMode(true,false, CamPath::TM_FREE);   ImGui::EndMenu(); }
                                if (ImGui::BeginMenu("BOTH"))  { if (ImGui::MenuItem("Auto"))   ApplyMode(true,true,  CamPath::TM_AUTO);   if (ImGui::MenuItem("Flat"))   ApplyMode(true,true,  CamPath::TM_FLAT);   if (ImGui::MenuItem("Linear")) ApplyMode(true,true,  CamPath::TM_LINEAR); if (ImGui::MenuItem("Free"))   ApplyMode(true,true,  CamPath::TM_FREE);   ImGui::EndMenu(); }
                                ImGui::EndMenu();
                            }

                            ImGui::Separator();
                            ImGui::MenuItem("Close");
                        }
                        ImGui::EndPopup();
                    }
                    if (!ImGui::IsPopupOpen("campath_curve_kf_ctx")) { g_CurveCtxKeyIndex = -1; g_CurveCtxChannel = -1; }

                    ImGui::EndChild();
                } else {
                    ImGui::TextDisabled("Add at least 4 keyframes to edit curves.");
                }
            }
            // Preview slider driving Mirv camera (Source1 input)
            if (g_Hook_VClient_RenderView.m_CamPath.GetSize() >= 2 && g_Hook_VClient_RenderView.m_CamPath.CanEval()) {
                ImGui::Separator();
                ImGui::TextUnformatted("Preview");
                double tMin = g_Hook_VClient_RenderView.m_CamPath.GetLowerBound();
                double tMax = g_Hook_VClient_RenderView.m_CamPath.GetUpperBound(); if (tMax <= tMin) tMax = tMin + 1.0;
                static bool  s_previewActive = false;
                static bool  s_prevCamEnabled = false;
                static bool  s_havePrevCamPose = false;
                static double s_prevX=0, s_prevY=0, s_prevZ=0, s_prevRx=0, s_prevRy=0, s_prevRz=0; static float s_prevFov=90.0f;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::SliderFloat("##previewcampath", &g_PreviewNorm, 0.0f, 1.0f, "%.3f")) { /* live update below while active */ }
                bool activeNow = ImGui::IsItemActive(); bool activated = ImGui::IsItemActivated(); bool deactivated = ImGui::IsItemDeactivated();
                if (activated) {
                    s_previewActive = true;
                    if (g_Hook_VClient_RenderView.m_MirvInput) {
                        s_prevCamEnabled = g_Hook_VClient_RenderView.m_MirvInput->GetCameraControlMode();
                        s_havePrevCamPose = false;
                        if (s_prevCamEnabled) { Afx_GetLastCameraData(s_prevX,s_prevY,s_prevZ,s_prevRx,s_prevRy,s_prevRz,s_prevFov); s_havePrevCamPose = true; }
                        g_Hook_VClient_RenderView.m_MirvInput->SetCameraControlMode(true);
                    }
                }
                if (activeNow) {
                    double tEval = tMin + (double)g_PreviewNorm * (tMax - tMin);
                    CamPathValue v = g_Hook_VClient_RenderView.m_CamPath.Eval(tEval);
                    if (g_Hook_VClient_RenderView.m_MirvInput) {
                        g_Hook_VClient_RenderView.m_MirvInput->SetTx((float)v.X);
                        g_Hook_VClient_RenderView.m_MirvInput->SetTy((float)v.Y);
                        g_Hook_VClient_RenderView.m_MirvInput->SetTz((float)v.Z);
                        using namespace Afx::Math; QEulerAngles ea = v.R.ToQREulerAngles().ToQEulerAngles();
                        g_Hook_VClient_RenderView.m_MirvInput->SetRx((float)ea.Pitch);
                        g_Hook_VClient_RenderView.m_MirvInput->SetRy((float)ea.Yaw);
                        g_Hook_VClient_RenderView.m_MirvInput->SetRz((float)ea.Roll);
                        g_Hook_VClient_RenderView.m_MirvInput->SetFov((float)v.Fov);
                    }
                }
                if (deactivated && s_previewActive) {
                    if (g_Hook_VClient_RenderView.m_MirvInput) {
                        if (!g_PreviewFollow && s_prevCamEnabled && s_havePrevCamPose) {
                            g_Hook_VClient_RenderView.m_MirvInput->SetTx((float)s_prevX);
                            g_Hook_VClient_RenderView.m_MirvInput->SetTy((float)s_prevY);
                            g_Hook_VClient_RenderView.m_MirvInput->SetTz((float)s_prevZ);
                            g_Hook_VClient_RenderView.m_MirvInput->SetRx((float)s_prevRx);
                            g_Hook_VClient_RenderView.m_MirvInput->SetRy((float)s_prevRy);
                            g_Hook_VClient_RenderView.m_MirvInput->SetRz((float)s_prevRz);
                            g_Hook_VClient_RenderView.m_MirvInput->SetFov((float)s_prevFov);
                        }
                        g_Hook_VClient_RenderView.m_MirvInput->SetCameraControlMode(s_prevCamEnabled);
                    }
                    s_previewActive = false;
                }
            }

            // Frame pointer dragging -> demo_gototick
            static bool s_draggingPointer = false; static ImGui::FrameIndexType s_dragStartFrame = 0;
            const bool pointerMoved = (s_seqFrame != prevFrame);
            const bool lmbDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
            const bool lmbReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
            const bool seqHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
            if (!s_draggingPointer) {
                if (pointerMoved && lmbDown && seqHovered) { s_draggingPointer = true; s_dragStartFrame = prevFrame; }
            }
            if (s_draggingPointer) {
                if (lmbReleased || !lmbDown) { if (s_seqFrame != s_dragStartFrame) Afx_GotoDemoTick((int)s_seqFrame); s_draggingPointer = false; }
            } else if (curDemoTick > 0) {
                s_seqFrame = (ImGui::FrameIndexType)curDemoTick;
            }

            // Auto-height fit
            {
                ImVec2 cur = ImGui::GetWindowSize(); float remain = ImGui::GetContentRegionAvail().y; float desired = cur.y - remain;
                float min_h = ImGui::GetFrameHeightWithSpacing() * 3.0f; if (desired < min_h) desired = min_h; ImGui::SetWindowSize(ImVec2(cur.x, desired));
            }
            ImGui::End();
        }
    }
    //Camera Control Window
    if (g_ShowCameraControl) {
        if (MirvInput* pMirv = g_Hook_VClient_RenderView.m_MirvInput) {
            // Mirv Camera: horizontally resizable; adjust height to content each frame
            ImGui::Begin("Mirv Camera");
            bool camEnabled = pMirv->GetCameraControlMode();
            ImGui::Text("Mirv Camera (C): %s", camEnabled ? "enabled" : "disabled");
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
            // FOV slider (synced with RMB+wheel in passthrough)
            if (!g_uiFovInit) { g_uiFov = GetLastCameraFov(); g_uiFovDefault = g_uiFov; g_uiFovInit = true; }
            // While using RMB+F FOV-modifier, reflect live FOV into the slider value
            if (pMirv->GetMouseFovMode()) {
                g_uiFov = GetLastCameraFov();
                g_uiFovInit = true;
            }
            {
                float tmp = g_uiFov;
                bool changed = ImGui::SliderFloat("FOV (F)", &tmp, 1.0f, 179.0f, "%.1f deg");
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
            // While using RMB+R roll-modifier, reflect live roll into the slider value
            if (pMirv->GetMouseRollMode()) {
                g_uiRoll = GetLastCameraRoll();
                g_uiRollInit = true;
            }
            {
                float tmp = g_uiRoll;
                bool changed = ImGui::SliderFloat("Roll (R)", &tmp, -180.0f, 180.0f, "%.1f deg");
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
                const char* lbl = "ksenss";
                float lblW = ImGui::CalcTextSize(lbl).x * g_UiScale;
                float rightGap = st3.ItemInnerSpacing.x * 3.0f + st3.FramePadding.x * 2.0f + 20.0f;
                float width = avail - (lblW + rightGap);
                if (width < 100.0f) width = avail * 0.6f; // fallback
                ImGui::SetNextItemWidth(width);
            }
            if (!g_uiKsensInit) { g_uiKsens = (float)pMirv->GetKeyboardSensitivty(); g_uiKsensDefault = g_uiKsens; g_uiKsensInit = true; }
            {
                float tmp = g_uiKsens;
                bool changed = ImGui::SliderFloat("ksens (Scroll)", &tmp, 0.01f, 10.0f, "%.2f");
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
            // Auto-height: shrink/grow to fit current content while preserving user-set width
            {
                ImVec2 cur = ImGui::GetWindowSize();
                float remain = ImGui::GetContentRegionAvail().y;
                float desired = cur.y - remain;
                float min_h = ImGui::GetFrameHeightWithSpacing() * 4.0f; // camera panel needs a bit more
                if (desired < min_h) desired = min_h;
                ImGui::SetWindowSize(ImVec2(cur.x, desired));
            }
            ImGui::End();
        }
    }
    // Gizmo overlay + camera control mini panel
    {
        ImGuizmo::BeginFrame();
        // Hotkeys: G = translate, R = rotate, C = toggle camera control
        if (ImGui::GetActiveID() == 0) {
            if (ImGui::IsKeyPressed(ImGuiKey_G, false)) { g_ShowGizmo = true; g_GizmoOp = ImGuizmo::TRANSLATE; }
            if (ImGui::IsKeyPressed(ImGuiKey_R, false)) { g_ShowGizmo = true; g_GizmoOp = ImGuizmo::ROTATE; }
            if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
                if (MirvInput* pMirv = g_Hook_VClient_RenderView.m_MirvInput) {
                    bool camEnabled = pMirv->GetCameraControlMode();
                    pMirv->SetCameraControlMode(!camEnabled);
                }
            }
        }
        if (g_ShowGizmo) {
            ImGui::Begin("Gizmo", &g_ShowGizmo, ImGuiWindowFlags_AlwaysAutoResize);
            if (ImGui::RadioButton("Pos (G)", g_GizmoOp == ImGuizmo::TRANSLATE)) g_GizmoOp = ImGuizmo::TRANSLATE; ImGui::SameLine();
            if (ImGui::RadioButton("Rot (R)", g_GizmoOp == ImGuizmo::ROTATE)) g_GizmoOp = ImGuizmo::ROTATE;
            if (ImGui::RadioButton("Local", g_GizmoMode == ImGuizmo::LOCAL)) g_GizmoMode = ImGuizmo::LOCAL; ImGui::SameLine();
            if (ImGui::RadioButton("World", g_GizmoMode == ImGuizmo::WORLD)) g_GizmoMode = ImGuizmo::WORLD;
            ImGui::End();
        }

        if (g_LastCampathCtx.active) {
            ImVec2 ds = ImGui::GetIO().DisplaySize;
            ImGuizmo::Enable(!Overlay::Get().IsRmbPassthroughActive());
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
            ImGuizmo::SetRect(0.0f, 0.0f, ds.x, ds.y);

            // Build view from current camera
            double cx,cy,cz, rX,rY,rZ; float cfovDeg; Afx_GetLastCameraData(cx,cy,cz,rX,rY,rZ,cfovDeg);
            auto DegToRad = [](double d){ return (float)(d * 3.14159265358979323846 / 180.0); };
            auto SrcToDx = [](float x, float y, float z, float out[3]) { out[0] = -y; out[1] = z; out[2] = x; };
            auto BuildModel = [&](float px, float py, float pz, float pitchDeg, float yawDeg, float rollDeg, float out[16]) {
                double fwdS[3], rightS[3], upS[3];
                Afx::Math::MakeVectors(rollDeg, pitchDeg, yawDeg, fwdS, rightS, upS);
                float fwd[3], right[3], up[3], pos[3];
                SrcToDx((float)rightS[0], (float)rightS[1], (float)rightS[2], right);
                SrcToDx((float)upS[0],    (float)upS[1],    (float)upS[2],    up);
                SrcToDx((float)fwdS[0],   (float)fwdS[1],   (float)fwdS[2],   fwd);
                SrcToDx(px, py, pz, pos);
                out[0]=right[0]; out[1]=right[1]; out[2]=right[2]; out[3]=0.0f;
                out[4]=up[0];    out[5]=up[1];    out[6]=up[2];    out[7]=0.0f;
                out[8]=fwd[0];   out[9]=fwd[1];   out[10]=fwd[2];  out[11]=0.0f;
                out[12]=pos[0];  out[13]=pos[1];  out[14]=pos[2];  out[15]=1.0f;
            };
            auto BuildView = [&](float eyeX, float eyeY, float eyeZ, float pitchDeg, float yawDeg, float rollDeg, float out[16]) {
                float M[16]; BuildModel(eyeX,eyeY,eyeZ,pitchDeg,yawDeg,rollDeg,M);
                const float r00=M[0], r01=M[1], r02=M[2];
                const float r10=M[4], r11=M[5], r12=M[6];
                const float r20=M[8], r21=M[9], r22=M[10];
                const float tx = M[12], ty = M[13], tz = M[14];
                // inverse rigid: R^T and -T * R^T (row-major)
                out[0]=r00; out[1]=r10; out[2]=r20; out[3]=0.0f;
                out[4]=r01; out[5]=r11; out[6]=r21; out[7]=0.0f;
                out[8]=r02; out[9]=r12; out[10]=r22; out[11]=0.0f;
                out[12]=-(tx*r00 + ty*r01 + tz*r02);
                out[13]=-(tx*r10 + ty*r11 + tz*r12);
                out[14]=-(tx*r20 + ty*r21 + tz*r22);
                out[15]=1.0f;
            };
            auto PerspectiveGlRh = [](float fovyDeg, float aspect, float znear, float zfar, float m16[16]) {
                const float f = 1.0f / tanf((float)(fovyDeg * 3.14159265358979323846 / 180.0 * 0.5));
                m16[0] = f/aspect; m16[1]=0; m16[2]=0;               m16[3]=0;
                m16[4] = 0;        m16[5]=f; m16[6]=0;               m16[7]=0;
                m16[8] = 0;        m16[9]=0; m16[10]=-(zfar+znear)/(zfar-znear); m16[11]=-1.0f;
                m16[12]=0;         m16[13]=0; m16[14]=-(2.0f*zfar*znear)/(zfar-znear); m16[15]=0;
            };

            float view[16]; BuildView((float)cx,(float)cy,(float)cz,(float)rX,(float)rY,(float)rZ, view);
            // Z-flip
            view[2]*=-1.0f; view[6]*=-1.0f; view[10]*=-1.0f; view[14]*=-1.0f;
            float aspect = g_Hook_VClient_RenderView.LastHeight > 0 ? (float)g_Hook_VClient_RenderView.LastWidth / (float)g_Hook_VClient_RenderView.LastHeight : (ds.y>0?ds.x/ds.y:16.0f/9.0f);
            // Convert game FOV to a vertical FOV based on 4:3
            float fovyDeg = cfovDeg;
            {
                const float baseAspect = 4.0f/3.0f;
                const float fovRad = (float)(cfovDeg * 3.14159265358979323846/180.0);
                const float half = 0.5f * fovRad;
                const float halfVert = atanf(tanf(half) / baseAspect);
                fovyDeg = (float)(2.0f * (halfVert) * 180.0 / 3.14159265358979323846);
            }
            float proj[16]; PerspectiveGlRh(fovyDeg, aspect, 0.1f, 100000.0f, proj);

            // Model from selected key
            float model[16];
            {
                const CamPathValue& v = g_LastCampathCtx.value;
                using namespace Afx::Math;
                QEulerAngles ea = v.R.ToQREulerAngles().ToQEulerAngles();
                BuildModel((float)v.X, (float)v.Y, (float)v.Z, (float)ea.Pitch, (float)ea.Yaw, (float)ea.Roll, model);
            }

            // Manipulate
            bool snap = ImGui::GetIO().KeyCtrl; float snapTranslate[3]={1,1,1}; float snapRotate=5.0f;
            static bool s_hasModelOverride=false; static double s_overrideKeyTime=0.0; static float s_modelOverride[16];
            if (s_hasModelOverride && fabs(s_overrideKeyTime-g_LastCampathCtx.time) < 1e-9) for(int i=0;i<16;++i) model[i]=s_modelOverride[i]; else s_hasModelOverride=false;
            bool changed = ImGuizmo::Manipulate(view, proj, g_GizmoOp, g_GizmoMode, model, nullptr, snap ? (g_GizmoOp==ImGuizmo::ROTATE? &snapRotate : snapTranslate) : nullptr);
            (void)changed; // suppress warn
            bool usingNow = ImGuizmo::IsUsing(); static bool wasUsing=false; static bool dragChanged=false; if (usingNow) { for(int i=0;i<16;++i) s_modelOverride[i]=model[i]; s_hasModelOverride=true; s_overrideKeyTime=g_LastCampathCtx.time; if (changed) dragChanged=true; }
            if (wasUsing && !usingNow && (dragChanged || s_hasModelOverride)) {
                float t[3], r_dummy[3], s_dummy[3]; const float* finalModel = s_hasModelOverride ? s_modelOverride : model; ImGuizmo::DecomposeMatrixToComponents(finalModel, t, r_dummy, s_dummy);
                // Back to Source axes
                double sx = (double)t[2]; double sy = (double)(-t[0]); double sz = (double)t[1];
                // Extract basis
                float Rx = finalModel[0], Ry=finalModel[1], Rz=finalModel[2]; float Ux = finalModel[4], Uy=finalModel[5], Uz=finalModel[6]; float Fx=finalModel[8], Fy=finalModel[9], Fz=finalModel[10];
                auto im_to_src = [](float ix, float iy, float iz, double out[3]){ out[0]= (double)iz; out[1]=(double)(-ix); out[2]=(double)iy; };
                double R_s[3], U_s[3], F_s[3]; im_to_src(Rx,Ry,Rz,R_s); im_to_src(Ux,Uy,Uz,U_s); im_to_src(Fx,Fy,Fz,F_s);
                auto norm3=[](double v[3]){ double l=sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); if(l>1e-12){ v[0]/=l; v[1]/=l; v[2]/=l; } };
                norm3(R_s); norm3(U_s); norm3(F_s);
                double sp = -F_s[2]; sp = sp<-1.0? -1.0 : (sp>1.0? 1.0: sp); double pitch = asin(sp) * (180.0 / 3.14159265358979323846);
                double cp = cos(pitch * (3.14159265358979323846/180.0)); double yaw=0.0; if (fabs(cp) > 1e-6) yaw = atan2(F_s[1], F_s[0]) * (180.0 / 3.14159265358979323846);
                double fwd0[3], right0[3], up0[3]; {
                    double fwdQ[3], rightQ[3], upQ[3]; Afx::Math::MakeVectors(0.0, pitch, yaw, fwdQ, rightQ, upQ);
                    fwd0[0]=fwdQ[0]; fwd0[1]=fwdQ[1]; fwd0[2]=fwdQ[2]; right0[0]=rightQ[0]; right0[1]=rightQ[1]; right0[2]=rightQ[2]; up0[0]=upQ[0]; up0[1]=upQ[1]; up0[2]=upQ[2]; }
                double dot_ur = U_s[0]*right0[0] + U_s[1]*right0[1] + U_s[2]*right0[2]; double dot_uu = U_s[0]*up0[0] + U_s[1]*up0[1] + U_s[2]*up0[2]; double roll = atan2(dot_ur, dot_uu) * (180.0 / 3.14159265358979323846);
                CamPathValue newVal(sx,sy,sz,pitch,yaw,roll,g_LastCampathCtx.value.Fov);
                // Apply to nearest key at g_LastCampathCtx.time
                char buf[1024];
                // Select only target key, update pos/angles/fov via commands
                int idx=0, foundIndex=-1; const double eps=1e-9; for (CamPathIterator it = g_Hook_VClient_RenderView.m_CamPath.GetBegin(); it != g_Hook_VClient_RenderView.m_CamPath.GetEnd(); ++it, ++idx) { if (fabs(it.GetTime()-g_LastCampathCtx.time) < eps) { foundIndex=idx; break; } }
                if (foundIndex>=0) {
                    sprintf_s(buf, "mirv_campath select none; mirv_campath select #%d #%d", foundIndex, foundIndex); Afx_ExecClientCmd(buf);
                    sprintf_s(buf, "mirv_campath edit position %.*f %.*f %.*f", 6,(float)newVal.X,6,(float)newVal.Y,6,(float)newVal.Z); Afx_ExecClientCmd(buf);
                    auto e=newVal.R.ToQREulerAngles().ToQEulerAngles(); sprintf_s(buf, "mirv_campath edit angles %.*f %.*f %.*f",6,(float)e.Pitch,6,(float)e.Yaw,6,(float)e.Roll); Afx_ExecClientCmd(buf);
                    sprintf_s(buf, "mirv_campath edit fov %.*f", 6,(float)newVal.Fov); Afx_ExecClientCmd(buf);
                    Afx_ExecClientCmd("mirv_campath select none");
                }
                g_LastCampathCtx.value = newVal; dragChanged=false; s_hasModelOverride=false;
            }
            wasUsing = usingNow;
        }
    }

    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
#endif
}

void OverlayDx9::EndFrame() {
    // Nothing special for DX9
}

void OverlayDx9::OnDeviceLost() {
#ifdef _WIN32
    if (!m_Initialized) return;
    if (m_DeviceObjectsValid) { ImGui_ImplDX9_InvalidateDeviceObjects(); m_DeviceObjectsValid = false; }
#endif
}

void OverlayDx9::OnResize(uint32_t, uint32_t) {
#ifdef _WIN32
    if (!m_Initialized) return;
    if (!m_DeviceObjectsValid) { ImGui_ImplDX9_CreateDeviceObjects(); m_DeviceObjectsValid = true; }
#endif
}

}} // namespace
