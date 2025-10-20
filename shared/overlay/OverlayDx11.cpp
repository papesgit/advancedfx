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
#include "AttachCameraState.h"
#include "BirdCamera.h"
#include "CameraOverride.h"
#ifdef IMGUI_ENABLE_FREETYPE
#include "third_party/imgui/misc/freetype/imgui_freetype.h"
#endif

// Campath info (points and duration): access global campath and time.
#include "../CamPath.h"
#include "../../AfxHookSource2/MirvTime.h"
#include "../../AfxHookSource2/CampathDrawer.h"
#include "../MirvInput.h"
#include "../../AfxHookSource2/RenderSystemDX11Hooks.h"
#include "../../AfxHookSource2/ClientEntitySystem.h"
// mirv_cmd integration
#include "../CommandSystem.h"
// For default game folder (…/game/) resolution
#include "../../AfxHookSource2/hlaeFolder.h"

#include <d3d11.h>
#include <dxgi.h>
#include <mutex>
#include <deque>
#include <wincodec.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <vector>
#include <string>
#include <sstream>
#include <string.h>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <set>
#include <thread>
#include <limits.h>

// cs-hud radar integration
#include "radar/Radar.h"
#include "Hud.h"
#include "GsiHttpServer.h"

// NanoSVG for grenade icon loading (implementation already in Hud.cpp)
#include "third_party/nanosvg/nanosvg.h"
#include "third_party/nanosvg/nanosvgrast.h"
// The official backend header intentionally comments out the WndProc declaration to avoid pulling in windows.h.
// Forward declare it here with C++ linkage so it matches the backend definition.
LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Recording/Take folder accessor from streams module
extern const wchar_t* AfxStreams_GetTakeDir();
extern const char* AfxStreams_GetRecordNameUtf8();
// Global campath instance (provided by shared CamPath module)
extern CamPath g_CamPath;
extern class CommandSystem g_CommandSystem;
#endif

namespace advancedfx { namespace overlay {

#ifdef _WIN32
// Defensive wrappers around entity queries to avoid hard crashes if an entity is invalid
// or a vtable slot is not present momentarily. Prefer skipping bad entities over crashing.
static bool SafeIsPlayerPawn(CEntityInstance* ent) {
    if (!ent) return false;
    __try { return ent->IsPlayerPawn(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeIsPlayerController(CEntityInstance* ent) {
    if (!ent) return false;
    __try { return ent->IsPlayerController(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static int SafeGetTeam(CEntityInstance* ent) {
    if (!ent) return 0;
    __try { return ent->GetTeam(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static uint8_t SafeGetObserverMode(CEntityInstance* ent) {
    if (!ent) return 0;
    __try { return ent->GetObserverMode(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int SafeGetHandleInt(CEntityInstance* ent) {
    if (!ent) return 0;
    __try { return ent->GetHandle().ToInt(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int SafeGetPlayerControllerEntryIndex(CEntityInstance* ent) {
    if (!ent) return -1;
    __try {
        auto h = ent->GetPlayerControllerHandle();
        if (h.ToInt() == 0) return -1;
        return h.GetEntryIndex();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static const char* SafeGetSanitizedPlayerName(CEntityInstance* ent) {
    if (!ent) return nullptr;
    __try { return ent->GetSanitizedPlayerName(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static const char* SafeGetPlayerName(CEntityInstance* ent) {
    if (!ent) return nullptr;
    __try { return ent->GetPlayerName(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static const char* SafeGetDebugName(CEntityInstance* ent) {
    if (!ent) return nullptr;
    __try { return ent->GetDebugName(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static const char* SafeGetClassName(CEntityInstance* ent) {
    if (!ent) return nullptr;
    __try { return ent->GetClassName(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static const char* SafeGetClientClassName(CEntityInstance* ent) {
    if (!ent) return nullptr;
    __try { return ent->GetClientClassName(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static uint64_t SafeGetSteamId(CEntityInstance* ent) {
    if (!ent) return 0ULL;
    __try { return ent->GetSteamId(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0ULL; }
}
#endif

// Persistent radar number assignment per controller index so labels don't change when players die.
// - CT (team=3): uses digits '1','2','3','4','5'
// - T  (team=2): uses digits '6','7','8','9','0'
static std::unordered_map<int, char> g_RadarLabelByController; // controllerIndex -> digit
static std::unordered_map<int, int>  g_RadarLabelTeam;         // controllerIndex -> team (2 or 3)

static char Radar_AssignDigit(int team)
{
    static const char kCtDigits[5] = { '1','2','3','4','5' };
    static const char kTDigits[5]  = { '6','7','8','9','0' };
    const char* list = (team == 3) ? kCtDigits : ((team == 2) ? kTDigits : nullptr);
    if (!list) return 0;
    // Build used set for this team
    std::unordered_set<char> used;
    for (const auto &kv : g_RadarLabelByController) {
        int ctrl = kv.first; char dig = kv.second;
        auto itT = g_RadarLabelTeam.find(ctrl);
        if (itT != g_RadarLabelTeam.end() && itT->second == team) used.insert(dig);
    }
    for (int i = 0; i < 5; ++i) if (!used.count(list[i])) return list[i];
    return 0; // none available (shouldn't happen for 5v5)
}

static char Radar_GetOrAssignDigitForController(int controllerIndex, int team)
{
    if (controllerIndex < 0 || !(team == 2 || team == 3)) return 0;
    auto it = g_RadarLabelByController.find(controllerIndex);
    auto itTeam = g_RadarLabelTeam.find(controllerIndex);
    if (it != g_RadarLabelByController.end() && itTeam != g_RadarLabelTeam.end()) {
        if (itTeam->second == team) return it->second;
        // Team changed: release old and reassign on new team
        g_RadarLabelByController.erase(it);
        g_RadarLabelTeam.erase(itTeam);
    }
    char d = Radar_AssignDigit(team);
    if (d != 0) {
        g_RadarLabelByController[controllerIndex] = d;
        g_RadarLabelTeam[controllerIndex] = team;
    }
    return d;
}



// Apply a sleek dark style for the HLAE overlay.
static void ApplyHlaeDarkStyle()
{
#ifdef _WIN32
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    // Layout and rounding for a modern look
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

    // Accent color (teal)
    const ImVec4 ACCENT      = C(32, 208, 194);
    const ImVec4 ACCENT_HOV  = C(42, 223, 208);
    const ImVec4 ACCENT_ACT  = C(26, 178, 165);

    // Neutral palette
    const ImVec4 BG0 = C(17, 19, 24);   // window background
    const ImVec4 BG1 = C(24, 27, 33);   // child/popup background
    const ImVec4 BG2 = C(28, 32, 39);   // header/active frame
    const ImVec4 BG3 = C(36, 41, 49);   // hovered frame/header
    const ImVec4 BG4 = C(45, 51, 61);   // buttons hovered
    const ImVec4 FG0 = C(224, 224, 224);// text
    const ImVec4 FG1 = C(136, 136, 136);// text disabled
    const ImVec4 BRD = C(58, 64, 74);   // border

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = FG0;
    colors[ImGuiCol_TextDisabled]          = FG1;
    colors[ImGuiCol_WindowBg]              = BG0;
    colors[ImGuiCol_ChildBg]               = C(0,0,0,0);
    colors[ImGuiCol_PopupBg]               = ImVec4(BG1.x, BG1.y, BG1.z, 0.98f);
    colors[ImGuiCol_Border]                = BRD;
    colors[ImGuiCol_BorderShadow]          = C(0,0,0,0);

    colors[ImGuiCol_FrameBg]               = BG2;
    colors[ImGuiCol_FrameBgHovered]        = BG3;
    colors[ImGuiCol_FrameBgActive]         = BG4;

    colors[ImGuiCol_TitleBg]               = C(14, 16, 20);
    colors[ImGuiCol_TitleBgActive]         = C(20, 24, 28);
    colors[ImGuiCol_TitleBgCollapsed]      = C(14, 16, 20);

    colors[ImGuiCol_MenuBarBg]             = C(22, 25, 31);

    colors[ImGuiCol_ScrollbarBg]           = ImVec4(BG0.x, BG0.y, BG0.z, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]         = C(51, 56, 65);
    colors[ImGuiCol_ScrollbarGrabHovered]  = C(62, 68, 78);
    colors[ImGuiCol_ScrollbarGrabActive]   = C(76, 84, 96);

    colors[ImGuiCol_CheckMark]             = ACCENT;
    colors[ImGuiCol_SliderGrab]            = ACCENT;
    colors[ImGuiCol_SliderGrabActive]      = ACCENT_ACT;

    colors[ImGuiCol_Button]                = C(45, 50, 59);
    colors[ImGuiCol_ButtonHovered]         = BG4;
    colors[ImGuiCol_ButtonActive]          = C(54, 60, 70);

    colors[ImGuiCol_Header]                = BG3;
    colors[ImGuiCol_HeaderHovered]         = BG4;
    colors[ImGuiCol_HeaderActive]          = BG2;

    colors[ImGuiCol_Separator]             = BRD;
    colors[ImGuiCol_SeparatorHovered]      = C(72, 80, 92);
    colors[ImGuiCol_SeparatorActive]       = C(72, 80, 92);

    colors[ImGuiCol_ResizeGrip]            = C(51, 56, 65);
    colors[ImGuiCol_ResizeGripHovered]     = C(62, 68, 78);
    colors[ImGuiCol_ResizeGripActive]      = C(76, 84, 96);

    colors[ImGuiCol_Tab]                   = C(27, 31, 38);
    colors[ImGuiCol_TabHovered]            = C(40, 45, 53);
    colors[ImGuiCol_TabActive]             = C(32, 36, 44);
    colors[ImGuiCol_TabUnfocused]          = C(27, 31, 38);
    colors[ImGuiCol_TabUnfocusedActive]    = C(32, 36, 44);

    colors[ImGuiCol_PlotLines]             = C(156, 156, 156);
    colors[ImGuiCol_PlotLinesHovered]      = ACCENT_HOV;
    colors[ImGuiCol_PlotHistogram]         = C(156, 135, 0);
    colors[ImGuiCol_PlotHistogramHovered]  = C(196, 175, 40);

    colors[ImGuiCol_TableHeaderBg]         = C(28, 32, 39);
    colors[ImGuiCol_TableBorderStrong]     = C(52, 58, 70);
    colors[ImGuiCol_TableBorderLight]      = C(46, 52, 62);
    colors[ImGuiCol_TableRowBg]            = ImVec4(1,1,1,0.00f);
    colors[ImGuiCol_TableRowBgAlt]         = ImVec4(1,1,1,0.06f);

    colors[ImGuiCol_TextSelectedBg]        = ImVec4(ACCENT.x, ACCENT.y, ACCENT.z, 0.35f);
    colors[ImGuiCol_DragDropTarget]        = ImVec4(ACCENT.x, ACCENT.y, ACCENT.z, 0.90f);
    colors[ImGuiCol_NavHighlight]          = ImVec4(ACCENT.x, ACCENT.y, ACCENT.z, 0.80f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1,1,1,0.70f);
    colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0,0,0,0.60f);
    colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0,0,0,0.50f);
#endif
}

struct CampathCtx {
    bool active = false;
    double time = 0.0;
    CamPathValue value{};
};

//Cvar Unhide
static bool g_cvarsUnhidden = false;
// Window activation state
static bool g_windowActive = true;
// Used to drop the first noisy mouse event right after we re-activate
static bool g_dropFirstMouseAfterActivate = false;

static std::mutex g_imguiInputMutex;
// Queue Win32 messages coming from WndProc to feed ImGui on the render thread.
// This avoids races/drops when WndProc fires while the render thread is inside NewFrame/Render.
struct ImGuiQueuedMsg { HWND hwnd; UINT msg; WPARAM wParam; LPARAM lParam; };
static std::mutex g_imguiMsgQueueMutex;
static std::deque<ImGuiQueuedMsg> g_imguiMsgQueue;
static void ImGui_EnqueueWin32Msg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    std::lock_guard<std::mutex> lk(g_imguiMsgQueueMutex);
    if (g_imguiMsgQueue.size() > 2048) g_imguiMsgQueue.pop_front();
    g_imguiMsgQueue.push_back(ImGuiQueuedMsg{ hwnd, msg, wParam, lParam });
}
static void ImGui_DrainQueuedWin32Msgs()
{
    // Move to local to minimize time holding the queue mutex
    std::deque<ImGuiQueuedMsg> local;
    {
        std::lock_guard<std::mutex> lk(g_imguiMsgQueueMutex);
        if (!g_imguiMsgQueue.empty()) { local.swap(g_imguiMsgQueue); }
    }
    for (const auto &m : local)
        ImGui_ImplWin32_WndProcHandler(m.hwnd, m.msg, m.wParam, m.lParam);
}

// Optional: group all overlay windows inside a single ImGui window with a DockSpace
static bool   g_GroupIntoWorkspace = false;         // user toggle
static bool   g_WorkspaceNeedsLayout = false;       // request (re)build of docking layout
static bool   g_WorkspaceForceRedock = false;       // force rebuild even if .ini exists (from "Redock all" button)
static bool   g_WorkspaceLayoutInitialized = false; // true once layout has been built at least once
static bool   g_WorkspaceOpen = true;               // visibility of the workspace window
static ImGuiID g_WorkspaceDockspaceId = 0;          // ID of the DockSpace inside the workspace window
static ImGuiID g_WorkspaceViewportId = 0;           // Platform viewport ID hosting the workspace window

// Separate .ini files for normal and workspace modes
static std::string g_IniFileNormal = "imgui.ini";
static std::string g_IniFileWorkspace = "imgui_workspace.ini";

// Store window visibility settings from .ini to apply after workspace is created
struct WorkspaceWindowSettings {
    bool hasSettings = false;
    bool showRadar = false;
    bool showRadarSettings = false;
    bool showAttachmentControl = false;
    bool showDofWindow = false;
    bool showObservingBindings = false;
    bool showObservingCameras = false;
    bool showGroupViewWindow = false;
    bool showMultikillWindow = false;
    bool showBackbufferWindow = false;
    bool showSequencer = false;
    bool showCameraControl = false;
    bool showOverlayConsole = false;
    bool showGizmo = false;
};
static WorkspaceWindowSettings g_WorkspaceWindowSettings;

static bool  g_hasPendingWarp = false;
static POINT g_pendingWarpPt  = {0, 0};

static bool  g_EnableDofTimeline = false;

// Global (file-scope) context for “last selected/edited campath key”
static CampathCtx g_LastCampathCtx;
static ImGuizmo::OPERATION g_GizmoOp   = ImGuizmo::TRANSLATE;
static ImGuizmo::MODE      g_GizmoMode = ImGuizmo::LOCAL;
// Signal to sequencer to refresh its cached keyframe values after edits
static bool g_SequencerNeedsRefresh = false;

// Simple in-overlay console state
static bool g_ShowOverlayConsole = false;
static bool g_ShowCameraControl = false;
static bool g_ShowGizmo = false;
static bool g_ShowBackbufferWindow = false;
// Viewport source: 0=Backbuffer (with UI), 1=BeforeUi (no UI)
static int  g_ViewportSourceMode = 0;
static bool g_ViewportEnableRmbControl = true;  // Enable RMB camera control in viewport by default
static bool g_ViewportSmoothMode = false;       // Enable smooth camera movements with acceleration/deceleration
static float g_ViewportSmoothHalftimePos = 0.15f;   // Halftime for smooth position movements (in seconds)
static float g_ViewportSmoothHalftimeAngle = 0.10f; // Halftime for smooth angle movements (in seconds)
static float g_ViewportSmoothHalftimeFov = 0.20f;   // Halftime for smooth FOV movements (in seconds)
static bool g_ViewportSmoothSettingsOpen = false;   // Settings window open state
static float g_ViewportSmoothScrollSpeedIncrement = 0.10f;  // Multiplier for scroll speed adjustment (1.0 + this value)
static float g_ViewportSmoothScrollFovIncrement = 2.0f;     // FOV change per scroll click
static bool g_ViewportSmoothAnalogInput = false;    // Enable analog keyboard input (Wooting, etc.)
static bool g_GetSmoothPass = false;
static int g_GetSmoothIndex = -1;
static float g_GetSmoothTime = 0.0f;
static bool g_GetSmoothFirstFrame = false;
static float g_GetSmoothFirstPos[3] = {0, 0, 0};
// ImGui per-draw opaque blend override for viewport image
static ID3D11BlendState* g_ViewportOpaqueBlend = nullptr;
static ID3D11DeviceContext* g_ImguiD3DContext = nullptr;
static void ImGui_SetOpaqueBlendCallback(const ImDrawList*, const ImDrawCmd*)
{
#ifdef _WIN32
    if (g_ImguiD3DContext && g_ViewportOpaqueBlend) {
        float blendFactor[4] = {0,0,0,0};
        g_ImguiD3DContext->OMSetBlendState(g_ViewportOpaqueBlend, blendFactor, 0xffffffff);
    }
#endif
}
static float g_PreviewLookScale = 0.01f; // baseline so 1.00x feels like mirv camera
static float g_PreviewLookMultiplier = 1.0f; // user-tunable multiplier (via Settings)
// Gizmo-on-preview helpers (updated each frame when preview window renders)
static bool   g_PreviewRectValid = false;
static ImVec2 g_PreviewRectMin  = ImVec2(0,0);
static ImVec2 g_PreviewRectSize = ImVec2(0,0);
static ImDrawList* g_PreviewDrawList = nullptr;
static std::vector<std::string> g_OverlayConsoleLog;
static char g_OverlayConsoleInput[512] = {0};
static bool g_OverlayConsoleScrollToBottom = false;
// Sequencer toggle state (shared between tabs and window)
static bool g_ShowSequencer = false;
// Curve editor state (Campath)
static bool g_ShowCurveEditor = false;
static float g_CurvePadding = 12.0f;
static float g_CurveValueScale = 1.0f;
static float g_CurveValueOffset = 0.0f;
static std::vector<int> g_CurveSelection;
// Multi-curve view state:
static bool g_CurveNormalize = true;             // normalize each channel to its own range
static bool g_CurveShow[7] = { false, false, false, false, false, false, false }; // start with no curves visible by default
// Focused channel for editing handles (-1 = all visible channels editable)
static int g_CurveFocusChannel = -1;

// Multikill Browser (demo parser integration)
static bool g_ShowMultikillWindow = false;
// Event browser view mode: 0=Multikills, 1=Noscope, 2=Wallbang, 3=Jumpshot
static int g_MkViewMode = 0;
struct MultikillEvent {
    uint64_t steamId = 0;
    std::string player;
    int round = 0;
    int count = 0;
    int startTick = 0;
    int endTick = 0;
    std::vector<std::string> victims;
};
static std::vector<MultikillEvent> g_MkEvents;
// Flat kill rows for alternate event views
struct Mk_EventKill {
    int round = 0;
    int tick = 0;
    uint64_t attackerSid = 0;
    std::string attackerName;
    std::string victimName;
    std::string attackerTeam;
    std::string victimTeam;
    std::string weapon;
    bool noscope = false;
    bool inAir = false; // Attackerinair
    bool blind = false; // Attackerblind
    int penetrated = 0; // >0 means wallbang
    bool smoke = false;
};
static std::vector<Mk_EventKill> g_MkAllKills;
static bool g_MkParsing = false;
static std::string g_MkParseError;
static char g_MkParserPath[1024] = "x64/DemoParser.exe";  // defaults to PATH lookup
static char g_MkDemoPath[2048] = "";
static std::thread g_MkWorker;
static std::mutex g_MkMutex; // protects g_MkEvents / g_MkParseError while worker updates

// Context menu target (you already added this var earlier)
static int g_CurveCtxKeyIndex = -1;
static int g_CurveCtxChannel  = -1; // channel for context popup target

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

// Grey matte between game and overlay windows (shown while Viewport is open)
static bool  g_DimGameWhileViewport = false; // UI toggle
static float g_DimOpacity           = 1.00f; // 0..1, 100% by default

// Standalone curve editor cache (times and values only; relative time from campath)
struct CurveCache {
    bool valid = false;
    std::vector<double> times;
    std::vector<CamPathValue> values;
} g_CurveCache;

static void CurveCache_Rebuild()
{
    g_CurveCache.valid = true;
    g_CurveCache.times.clear();
    g_CurveCache.values.clear();
    g_CurveCache.times.reserve(g_CamPath.GetSize());
    g_CurveCache.values.reserve(g_CamPath.GetSize());
    for (CamPathIterator it = g_CamPath.GetBegin(); it != g_CamPath.GetEnd(); ++it) {
        g_CurveCache.times.push_back(it.GetTime());
        g_CurveCache.values.push_back(it.GetValue());
    }
}

//Misc

static bool g_ShowAttachmentControl = false;

// Camera attach-to-attachment state
struct AttachEntityEntry {
    int index = -1;      // entity list index
    int handle = -1;     // stable handle
    std::string name;    // display name
    bool isPawn = false; // player pawn
    bool isWeapon = false; // weapon-ish client classes
};

static std::vector<AttachEntityEntry> g_AttachEntityCache;
static bool g_AttachEntityCacheValid = false;

static bool  g_AttachCamEnabled = false;
static int   g_AttachSelectedHandle = -1; // currently selected (combobox) entity handle
static int   g_AttachSelectedIndex  = -1; // entity index (best effort, may go stale)
static int   g_AttachSelectedAttachmentIdx = 1; // 1-based, 0 is invalid per engine semantics
static float g_AttachOffsetPos[3] = { 0.0f, 0.0f, 0.0f };    // forward, right, up (Source axes)
static float g_AttachOffsetRot[3] = { 0.0f, 0.0f, 0.0f };    // pitch, yaw, roll (degrees)

static void UpdateAttachEntityCache()
{
    g_AttachEntityCache.clear();

    if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
        return;

    const int highest = GetHighestEntityIndex();
    if (highest < 0) return;

    for (int idx = 0; idx <= highest; ++idx) {
        if (auto* ent = static_cast<CEntityInstance*>(g_GetEntityFromIndex(*g_pEntityList, idx))) {
            const bool isPawn = SafeIsPlayerPawn(ent);
            // Controllers don't hold attachments we want; prefer pawn over controller
            const bool isController = SafeIsPlayerController(ent);

            // Heuristic weapon detection: client class name contains "Weapon"
            bool isWeapon = false;
            if (const char* ccn = SafeGetClassName(ent)) {
                if (ccn && *ccn && nullptr != strstr(ccn, "weapon")) isWeapon = true;
            }

            if (!(isPawn || isWeapon)) continue;

            const char* display = nullptr;
            std::string name;
            if (isPawn) {
                // Resolve name from the corresponding PlayerController
                display = nullptr;
                int ctrlIdx = SafeGetPlayerControllerEntryIndex(ent);
                if (ctrlIdx >= 0 && ctrlIdx <= highest) {
                    if (auto* ctrl = static_cast<CEntityInstance*>(g_GetEntityFromIndex(*g_pEntityList, ctrlIdx))) {
                        const char* nm = SafeGetSanitizedPlayerName(ctrl);
                        if (!nm || !*nm) nm = SafeGetPlayerName(ctrl);
                        if (!nm || !*nm) nm = SafeGetDebugName(ctrl);
                        display = nm;
                    }
                }
                if (!display || !*display) display = SafeGetDebugName(ent);
                name = display && *display ? display : "Player";
            } else {
                const char* c1 = SafeGetClientClassName(ent);
                const char* c2 = SafeGetClassName(ent);
                name = c1 && *c1 ? c1 : (c2 && *c2 ? c2 : "Weapon");
            }

            AttachEntityEntry e;
            e.index = idx;
            e.handle = SafeGetHandleInt(ent);
            e.isPawn = isPawn && !isController; // prefer pawn flag only when it's actually a pawn
            e.isWeapon = isWeapon && !isPawn;
            e.name = std::move(name);
            g_AttachEntityCache.emplace_back(std::move(e));
        }
    }
}

static CEntityInstance* FindEntityByHandle(int handle, int* outIndex = nullptr)
{
    if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) return nullptr;
    const int highest = GetHighestEntityIndex();
    if (highest < 0) return nullptr;
    for (int idx = 0; idx <= highest; ++idx) {
        if (auto* ent = static_cast<CEntityInstance*>(g_GetEntityFromIndex(*g_pEntityList, idx))) {
            if (ent->GetHandle().ToInt() == handle) {
                if (outIndex) *outIndex = idx;
                return ent;
            }
        }
    }
    return nullptr;
}

// Attachment name/index cache for current selection
struct AttachListItem { int idx; const char* name; };
static std::unordered_map<int, std::string> g_AttachIdxToName; // idx -> name (empty string if unnamed)
static std::vector<AttachListItem> g_AttachItems;              // sorted by idx, pointers into map strings
static int g_AttachCacheHandle = -1;
static bool g_AttachCacheValid = false;

static void InvalidateAttachmentCache() {
    g_AttachIdxToName.clear();
    g_AttachItems.clear();
    g_AttachCacheHandle = -1;
    g_AttachCacheValid = false;
}

static void RebuildAttachmentCacheForSelected() {
    InvalidateAttachmentCache();
    if (g_AttachSelectedHandle <= 0) return;
    int idxTmp = -1;
    CEntityInstance* ent = FindEntityByHandle(g_AttachSelectedHandle, &idxTmp);
    if (!ent) return;

    // Known names to probe
    static const char* kAttachNames[] = {
        "knife","eholster","pistol","leg_l_iktarget","leg_r_iktarget","defusekit","grenade0","grenade1",
        "grenade2","grenade3","grenade4","primary","primary_smg","c4","look_straight_ahead_stand",
        "clip_limit","weapon_hand_l","weapon_hand_r","gun_accurate","weaponhier_l_iktarget",
        "weaponhier_r_iktarget","look_straight_ahead_crouch","axis_of_intent","muzzle_flash","muzzle_flash2",
        "camera_inventory","shell_eject","stattrak","weapon_holster_center","stattrak_legacy","nametag",
        "nametag_legacy","keychain","keychain_legacy"
    };

    std::unordered_set<int> seen;
    for (const char* an : kAttachNames) {
        uint8_t idx = ent->LookupAttachment(an);
        if (idx != 0 && seen.insert((int)idx).second) {
            g_AttachIdxToName[(int)idx] = an;
        }
    }
    SOURCESDK::Vector o; SOURCESDK::Quaternion q;
    for (int i = 1; i <= 64; ++i) {
        if (ent->GetAttachment((uint8_t)i, o, q)) {
            if (!seen.count(i)) {
                g_AttachIdxToName[i] = std::string();
                seen.insert(i);
            }
        }
    }
    g_AttachItems.reserve(g_AttachIdxToName.size());
    for (auto& kv : g_AttachIdxToName) {
        g_AttachItems.push_back({kv.first, kv.second.empty() ? nullptr : kv.second.c_str()});
    }
    std::sort(g_AttachItems.begin(), g_AttachItems.end(), [](const AttachListItem& a, const AttachListItem& b){ return a.idx < b.idx; });
    g_AttachCacheHandle = g_AttachSelectedHandle;
    g_AttachCacheValid = true;
}

//DOF
static bool g_ShowDofWindow = false;

// Observing mode state
static bool g_ObservingEnabled = false;
static bool g_ShowObservingBindings = false;
static std::map<int, int> g_ObservingHotkeyBindings; // Map from key (0-9) to controller index

// Radar state for Observing tab
static bool g_ShowRadar = false;
static bool g_ShowRadarSettings = false;

// cs-hud radar integration settings
static char g_RadarsJsonPath[1024] = "";             // e.g. path to cs-hud src/themes/fennec/radars.json
static char g_RadarImagePath[1024] = "";              // e.g. .../img/radars/ingame/de_mirage.png
// On-screen placement and size
static float g_RadarUiPosX = 50.0f;
static float g_RadarUiPosY = 50.0f;
static float g_RadarUiSize = 300.0f; // width==height in pixels
static float g_RadarDotScale = 1.0f;
static char g_RadarMapName[128] = "de_mirage";        // default example
static bool g_RadarAssetsLoaded = false;
static Radar::Context g_RadarCtx;                     // holds cfg, texture SRV, smoothers
static bool g_ShowHud = false;                        // draw native HUD in viewport
static int  g_GsiPort = 31983;                        // must match CS2 cfg
static GsiHttpServer g_GsiServer;
static char g_FilteredPlayers[512] = "";              // comma-separated player names to filter out (e.g., coaches)

// Smoke detonation tracking (detect when smokes become stationary)
struct SmokeTracker {
    float pos[3];
    float lastPos[3];
    int ownerSide;
    bool isDetonated;
    uint64_t lastHeartbeat; // Last GSI heartbeat when position was updated
    int stationaryUpdates; // Count GSI updates smoke hasn't moved
    double detonationTime; // ImGui::GetTime() when smoke was detonated
};
static std::map<int, SmokeTracker> g_SmokeTrackers; // Key: hash of position for identification

// Grenade icon cache
struct GrenadeIconTex {
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0;
    int h = 0;
};
static std::unordered_map<std::string, GrenadeIconTex> g_GrenadeIconCache;

// Bird camera context menu
static bool g_BirdContextMenuOpen = false;
static int g_BirdContextTargetObserverSlot = -1;
static int g_BirdContextTargetPlayerId = -1;
static std::vector<Hud::PlayerRowRect> g_HudPlayerRects;
// Built-in radar assets
static bool g_RadarMapsLoaded = false;
static std::unordered_map<std::string, Radar::RadarConfig> g_RadarAllConfigs;
static std::vector<std::string> g_RadarMapList;

// Observing cameras state
static bool g_ShowObservingCameras = false;

struct CameraItem {
    std::string name;
    std::string camPathFile;
    std::string imagePath;
};

struct CameraProfile {
    std::string name;
    std::vector<CameraItem> cameras;
};

enum class GroupPlayMode {
    Random = 0,
    Sequentially = 1,
    FromStart = 2
};

struct CameraGroup {
    std::string name;
    std::string profileName;
    std::vector<int> cameraIndices; // Indices into the current profile's camera list
    GroupPlayMode playMode = GroupPlayMode::Random;
    int sequentialIndex = 0; // Current index for Sequential/FromStart modes
};

static std::vector<CameraProfile> g_CameraProfiles;
static int g_SelectedProfileIndex = -1;
static float g_CameraRectScale = 1.0f; // UI scale for camera rectangles
// Track which camera was loaded via rectangle click to visualize progress
static std::string g_ActiveCameraCampathPath;

// Camera groups (per profile)
static std::vector<CameraGroup> g_CameraGroups;
static bool g_ShowGroupsSidebar = false;
static int g_SelectedGroupForView = -1; // -1 = none selected
static bool g_ShowGroupViewWindow = false;
static std::string g_LastPlayedGroupOrCamera; // Track last played to reset FromStart mode

// Texture cache for camera images
struct CameraTexture {
    ID3D11ShaderResourceView* srv = nullptr;
    int width = 0;
    int height = 0;
};
static std::map<std::string, CameraTexture> g_CameraTextures;

static float g_FarBlurry = 2000.0f;
static float g_FarCrisp = 180.0f;
static float g_NearBlurry = -100.0f;
static float g_NearCrisp = 0.0f;

static float g_FarBlurryDefault = 2000.0f;
static float g_FarCrispDefault = 180.0f;
static float g_NearBlurryDefault = -100.0f;
static float g_NearCrispDefault = 0.0f;

static float g_MaxBlur = 5.0f;
static float g_DofRadiusScale = 0.25f;
static float g_DofTilt = 0.5f;

static float g_MaxBlurDefault = 5.0f;
static float g_DofRadiusScaleDefault = 0.25f;
static float g_DofTiltDefault = 0.5f;
// DOF Timeline state
static bool  g_DofInterpCubic = false; // apply to all curves when >=4 keys

struct DofKeyframe {
    int   tick = 0;
    float farBlurry = 2000.0f;
    float farCrisp = 180.0f;
    float nearBlurry = -100.0f;
    float nearCrisp = 0.0f;
    float maxBlur = 5.0f;
    float radiusScale = 0.25f;
    float tilt = 0.5f;
};

static std::vector<DofKeyframe> g_DofKeys; // authoritative order matches UI order; kept sorted by tick
static std::vector<ImGui::FrameIndexType> g_DofTicks; // mirrors g_DofKeys.tick for ImGui Neo

static const char* kDofCurveTag  = "overlay_dof_curve";
static const char* kDofOnceTags[7] = {
    "overlay_dof_once_far_blurry",
    "overlay_dof_once_far_crisp",
    "overlay_dof_once_near_blurry",
    "overlay_dof_once_near_crisp",
    "overlay_dof_once_max_blur",
    "overlay_dof_once_radius_scale",
    "overlay_dof_once_tilt"
};

// Build observing hotkey bindings based on current entities.
// CT team (3) gets keys 1..5 in ascending controller index order.
// T team (2) gets keys 6,7,8,9,0 in ascending controller index order.
static void AutoPopulateObservingBindingsFromEntities()
{
#ifdef _WIN32
    if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) return;

    const int highest = GetHighestEntityIndex();
    if (highest < 0) return;

    std::vector<int> ctIdx;
    std::vector<int> tIdx;
    ctIdx.reserve(8);
    tIdx.reserve(8);
    std::unordered_set<int> seenCt;
    std::unordered_set<int> seenT;

    for (int idx = 0; idx <= highest; ++idx) {
        if (auto* ent = static_cast<CEntityInstance*>(g_GetEntityFromIndex(*g_pEntityList, idx))) {
            if (!SafeIsPlayerPawn(ent)) continue;
            if (SafeGetObserverMode(ent) != 0) continue; // skip observers
            const int team = SafeGetTeam(ent);
            if (!(team == 2 || team == 3)) continue;
            const int ctrl = SafeGetPlayerControllerEntryIndex(ent);
            if (ctrl < 0) continue;
            if (team == 3) { if (seenCt.insert(ctrl).second) ctIdx.push_back(ctrl); }
            else if (team == 2) { if (seenT.insert(ctrl).second) tIdx.push_back(ctrl); }
        }
    }

    std::sort(ctIdx.begin(), ctIdx.end());
    std::sort(tIdx.begin(),  tIdx.end());

    // Clear existing assignments then assign new ones
    g_ObservingHotkeyBindings.clear();

    // CT: keys 1..5
    for (int i = 0; i < (int)ctIdx.size() && i < 5; ++i) {
        g_ObservingHotkeyBindings[1 + i] = ctIdx[i];
    }
    // T: keys 6,7,8,9,0
    for (int i = 0; i < (int)tIdx.size() && i < 5; ++i) {
        int key = (i < 4) ? (6 + i) : 0;
        g_ObservingHotkeyBindings[key] = tIdx[i];
    }

    advancedfx::Message("Overlay: Auto-populated observing bindings (CT=%d, T=%d)\n", (int)ctIdx.size(), (int)tIdx.size());
#else
    (void)0;
#endif
}

// Get controller index for focused player by comparing HUD player name to entity names
static int GetFocusedPlayerControllerIndex() {
    // Get HUD state to find focused player name
    auto hudOpt = g_GsiServer.TryGetHudState();
    if (!hudOpt.has_value()) {
        return -1;
    }

    // Find the focused player
    const Hud::Player* focusedPlayer = nullptr;
    for (const auto& p : hudOpt->leftPlayers) {
        if (p.isFocused) {
            focusedPlayer = &p;
            break;
        }
    }
    if (!focusedPlayer) {
        for (const auto& p : hudOpt->rightPlayers) {
            if (p.isFocused) {
                focusedPlayer = &p;
                break;
            }
        }
    }

    if (!focusedPlayer) {
        return -1;
    }

    // Get the focused player's name
    std::string focusedName = focusedPlayer->name;
    if (focusedName.empty()) {
        return -1;
    }

    // Search through entities to find matching controller by name
    if (g_pEntityList && *g_pEntityList && g_GetEntityFromIndex) {
        int highest = GetHighestEntityIndex();
        for (int i = 0; i <= highest; i++) {
            if (auto* ent = static_cast<CEntityInstance*>(g_GetEntityFromIndex(*g_pEntityList, i))) {
                if (SafeIsPlayerController(ent)) {
                    const char* name = SafeGetSanitizedPlayerName(ent);
                    if (!name || !*name) name = SafeGetPlayerName(ent);

                    if (name && focusedName == name) {
                        return i; // Found matching controller
                    }
                }
            }
        }
    }

    return -1;
}

static void Dof_SyncTicksFromKeys()
{
    g_DofTicks.clear();
    g_DofTicks.reserve(g_DofKeys.size());
    for (const auto& k : g_DofKeys) g_DofTicks.push_back((ImGui::FrameIndexType)k.tick);
}

static void Dof_RemoveScheduled()
{
    // Remove curves entry, if any
    g_CommandSystem.RemoveByTag(kDofCurveTag);
    // Remove one-shot entries, if any
    for (int i = 0; i < 7; ++i) g_CommandSystem.RemoveByTag(kDofOnceTags[i]);
}

static void Dof_AddOneShotsAtTick(int tick, const DofKeyframe& k)
{
    using namespace advancedfx;
    char buf[64];

    // far_blurry
    {
        CFakeCommandArgs a("mirv_cmd"); a.AddArg("addAtTick");
        _snprintf_s(buf, _TRUNCATE, "%d", tick); a.AddArg(buf);
        a.AddArg("r_dof_override_far_blurry"); _snprintf_s(buf, _TRUNCATE, "%f", (double)k.farBlurry); a.AddArg(buf);
        g_CommandSystem.Console_Command(&a);
        g_CommandSystem.TagLastAdded(kDofOnceTags[0]);
    }
    // far_crisp
    {
        CFakeCommandArgs a("mirv_cmd"); a.AddArg("addAtTick");
        _snprintf_s(buf, _TRUNCATE, "%d", tick); a.AddArg(buf);
        a.AddArg("r_dof_override_far_crisp"); _snprintf_s(buf, _TRUNCATE, "%f", (double)k.farCrisp); a.AddArg(buf);
        g_CommandSystem.Console_Command(&a);
        g_CommandSystem.TagLastAdded(kDofOnceTags[1]);
    }
    // near_blurry
    {
        CFakeCommandArgs a("mirv_cmd"); a.AddArg("addAtTick");
        _snprintf_s(buf, _TRUNCATE, "%d", tick); a.AddArg(buf);
        a.AddArg("r_dof_override_near_blurry"); _snprintf_s(buf, _TRUNCATE, "%f", (double)k.nearBlurry); a.AddArg(buf);
        g_CommandSystem.Console_Command(&a);
        g_CommandSystem.TagLastAdded(kDofOnceTags[2]);
    }
    // near_crisp
    {
        CFakeCommandArgs a("mirv_cmd"); a.AddArg("addAtTick");
        _snprintf_s(buf, _TRUNCATE, "%d", tick); a.AddArg(buf);
        a.AddArg("r_dof_override_near_crisp"); _snprintf_s(buf, _TRUNCATE, "%f", (double)k.nearCrisp); a.AddArg(buf);
        g_CommandSystem.Console_Command(&a);
        g_CommandSystem.TagLastAdded(kDofOnceTags[3]);
    }
    // max blur size
    {
        CFakeCommandArgs a("mirv_cmd"); a.AddArg("addAtTick");
        _snprintf_s(buf, _TRUNCATE, "%d", tick); a.AddArg(buf);
        a.AddArg("r_dof2_maxblursize"); _snprintf_s(buf, _TRUNCATE, "%f", (double)k.maxBlur); a.AddArg(buf);
        g_CommandSystem.Console_Command(&a);
        g_CommandSystem.TagLastAdded(kDofOnceTags[4]);
    }
    // radius scale
    {
        CFakeCommandArgs a("mirv_cmd"); a.AddArg("addAtTick");
        _snprintf_s(buf, _TRUNCATE, "%d", tick); a.AddArg(buf);
        a.AddArg("r_dof2_radiusscale"); _snprintf_s(buf, _TRUNCATE, "%f", (double)k.radiusScale); a.AddArg(buf);
        g_CommandSystem.Console_Command(&a);
        g_CommandSystem.TagLastAdded(kDofOnceTags[5]);
    }
    // tilt to ground
    {
        CFakeCommandArgs a("mirv_cmd"); a.AddArg("addAtTick");
        _snprintf_s(buf, _TRUNCATE, "%d", tick); a.AddArg(buf);
        a.AddArg("r_dof_override_tilt_to_ground"); _snprintf_s(buf, _TRUNCATE, "%f", (double)k.tilt); a.AddArg(buf);
        g_CommandSystem.Console_Command(&a);
        g_CommandSystem.TagLastAdded(kDofOnceTags[6]);
    }
}

static void Dof_AddCurves()
{
    if (g_DofKeys.size() < 2) return;
    using namespace advancedfx;
    char b[64];

    const int startTick = g_DofKeys.front().tick;
    const int endTick   = g_DofKeys.back().tick;

    CFakeCommandArgs a("mirv_cmd");
    a.AddArg("addCurves");
    a.AddArg("tick");
    _snprintf_s(b, _TRUNCATE, "%d", startTick); a.AddArg(b);
    _snprintf_s(b, _TRUNCATE, "%d", endTick);   a.AddArg(b);

    const bool useCubic = g_DofInterpCubic && g_DofKeys.size() >= 4;

    auto addCurve = [&](int curveIdx, const char* interp, auto valueOf) {
        a.AddArg("-");
        a.AddArg(interp);
        a.AddArg("space=abs");
        for (const auto& k : g_DofKeys) {
            _snprintf_s(b, _TRUNCATE, "%d", k.tick); a.AddArg(b);
            _snprintf_s(b, _TRUNCATE, "%f", (double)valueOf(k)); a.AddArg(b);
        }
    };

    const char* interpArg = useCubic ? "interp=cubic" : "interp=linear";
    addCurve(0, interpArg, [](const DofKeyframe& k){ return k.farBlurry; });
    addCurve(1, interpArg, [](const DofKeyframe& k){ return k.farCrisp; });
    addCurve(2, interpArg, [](const DofKeyframe& k){ return k.nearBlurry; });
    addCurve(3, interpArg, [](const DofKeyframe& k){ return k.nearCrisp; });
    addCurve(4, interpArg, [](const DofKeyframe& k){ return k.maxBlur; });
    addCurve(5, interpArg, [](const DofKeyframe& k){ return k.radiusScale; });
    addCurve(6, interpArg, [](const DofKeyframe& k){ return k.tilt; });

    a.AddArg("--");
    {
        std::string body =
            std::string("r_dof_override_far_blurry {0}; ")
            + "r_dof_override_far_crisp {1}; "
            + "r_dof_override_near_blurry {2}; "
            + "r_dof_override_near_crisp {3}; "
            + "r_dof2_maxblursize {4}; "
            + "r_dof2_radiusscale {5}; "
            + "r_dof_override_tilt_to_ground {6}";
        a.AddArg(body.c_str());
    }

    g_CommandSystem.Console_Command(&a);
    g_CommandSystem.TagLastAdded(kDofCurveTag);
}

static void Dof_RebuildScheduled()
{
    // Always remove previously scheduled DOF commands first
    Dof_RemoveScheduled();
    if (!g_EnableDofTimeline || g_DofKeys.empty()) return;
    if (g_DofKeys.size() == 1) {
        Dof_AddOneShotsAtTick(g_DofKeys[0].tick, g_DofKeys[0]);
        return;
    }
    Dof_AddCurves();
}
//Viewport stuff

struct ViewportPlayerEntry {
    std::string displayName;
    std::string command;
    uint64_t steamId = 0;
};

static std::vector<ViewportPlayerEntry> g_ViewportPlayerCache;
static std::chrono::steady_clock::time_point g_ViewportPlayerCacheNextRefresh{};
static bool g_ViewportPlayersMenuOpen = false;

static void UpdateViewportPlayerCache()
{
    using clock = std::chrono::steady_clock;
    const clock::time_point now = clock::now();
    if (now < g_ViewportPlayerCacheNextRefresh) {
        return;
    }

    g_ViewportPlayerCacheNextRefresh = now + std::chrono::milliseconds(500);
    g_ViewportPlayerCache.clear();

    if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) {
        g_ViewportPlayersMenuOpen = false;
        return;
    }

    const int highestIndex = GetHighestEntityIndex();
    if (highestIndex < 0) {
        g_ViewportPlayersMenuOpen = false;
        return;
    }

    std::unordered_set<uint64_t> seenSteamIds;
    seenSteamIds.reserve(static_cast<size_t>(highestIndex) + 1);

    for (int index = 0; index <= highestIndex; ++index) {
        if (auto* instance = static_cast<CEntityInstance*>(g_GetEntityFromIndex(*g_pEntityList, index))) {
            if (!SafeIsPlayerController(instance)) {
                continue;
            }

            const uint64_t steamId = SafeGetSteamId(instance);
            if (!seenSteamIds.insert(steamId).second) {
                continue;
            }

            const char* name = SafeGetSanitizedPlayerName(instance);
            if (!name || !*name) name = SafeGetPlayerName(instance);
            if (!name || !*name) name = SafeGetDebugName(instance);

            ViewportPlayerEntry entry;
            entry.displayName = (name && *name) ? name : "Unknown";
            //entry.command = std::string("spec_lock_to_accountid ") + std::to_string(steamId);
            entry.command = std::string("spec_player ") + std::string(name);
            entry.steamId = steamId;
            g_ViewportPlayerCache.emplace_back(std::move(entry));
        }
    }

    std::sort(g_ViewportPlayerCache.begin(), g_ViewportPlayerCache.end(),
        [](const ViewportPlayerEntry& a, const ViewportPlayerEntry& b) {
            if (a.displayName == b.displayName) {
                return a.steamId < b.steamId;
            }
            return a.displayName < b.displayName;
        });

    if (g_ViewportPlayerCache.empty()) {
        g_ViewportPlayersMenuOpen = false;
    }
}

// Sequencer preview behavior:
static bool g_PreviewFollow = false;
// Sequencer preview normalized position [0..1]
static float g_PreviewNorm = 0.0f;


//Mirv camera sliders
static bool  g_uiFovInit   = false; static float  g_uiFov = 90.0f;   static float g_uiFovDefault = 90.0f;
static bool  g_uiRollInit  = false; static float  g_uiRoll = 0.0f;   static float g_uiRollDefault = 0.0f;
static bool  g_uiKsensInit = false; static float  g_uiKsens = 1.0f;  static float g_uiKsensDefault = 1.0f;

// Overlay UI scale (DPI) control
static float g_UiScale = 1.0f;

// Persisted last-used directories (saved in imgui.ini via custom settings handler)
struct OverlayPathsSettings {
    std::string campathDir;
    std::string recordBrowseDir;
    std::string demoDir;
};
static OverlayPathsSettings g_OverlayPaths;
// Persistent demo file dialog (Settings → Open demo)
static ImGui::FileBrowser g_DemoOpenDialog(
    ImGuiFileBrowserFlags_CloseOnEsc |
    ImGuiFileBrowserFlags_EditPathString |
    ImGuiFileBrowserFlags_CreateNewDir
);
static bool g_DemoDialogInit = false;

static bool CallImGuiWndProcGuarded(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    // Enqueue message for processing on the render thread before NewFrame.
    // Do not block the game window thread here.
    static thread_local bool s_in_imgui = false;
    if (s_in_imgui) return false;
    ImGui_EnqueueWin32Msg(hwnd, msg, wparam, lparam);

    // Best-effort prediction to preserve pass-through behavior without blocking.
    ImGuiIO& io = ImGui::GetIO();
    auto is_mouse_msg = [](UINT m){
        switch(m){
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
            case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            case WM_XBUTTONDOWN: case WM_XBUTTONUP:
                return true; default: return false; }
    };
    auto is_kbd_msg = [](UINT m){ return m==WM_KEYDOWN||m==WM_KEYUP||m==WM_SYSKEYDOWN||m==WM_SYSKEYUP||m==WM_CHAR; };
    bool predict_consumed = false;
    if (is_mouse_msg(msg)) predict_consumed = io.WantCaptureMouse;
    else if (is_kbd_msg(msg)) predict_consumed = io.WantCaptureKeyboard;
    return predict_consumed;
}

// TLS flag to detect when we are inside ImGui platform window rendering/present.
// Used by Present hook to avoid re-entrancy into Overlay::BeginFrame/Render.
static thread_local bool g_in_platform_windows_present = false;

bool IsInPlatformWindowsPresent() { return g_in_platform_windows_present; }


// Multikill Parser helpers
static std::wstring Mk_Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return std::wstring();
    std::wstring ws; ws.resize((size_t)wlen - 1);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], wlen);
    return ws;
}

static bool Mk_RunProcessCapture(const std::wstring& cmdLine, std::string& outStdout, std::string& outError)
{
    outStdout.clear(); outError.clear();

    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE hReadOut = NULL, hWriteOut = NULL;
    HANDLE hReadErr = NULL, hWriteErr = NULL;
    if (!CreatePipe(&hReadOut, &hWriteOut, &sa, 0)) { outError = "CreatePipe stdout failed"; return false; }
    if (!CreatePipe(&hReadErr, &hWriteErr, &sa, 0)) { CloseHandle(hReadOut); CloseHandle(hWriteOut); outError = "CreatePipe stderr failed"; return false; }
    SetHandleInformation(hReadOut, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hReadErr, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hWriteOut;
    si.hStdError  = hWriteErr;

    PROCESS_INFORMATION pi{};
    std::wstring cl = cmdLine; // CreateProcessW may modify buffer
    BOOL ok = CreateProcessW(
        nullptr,
        &cl[0],
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );
    CloseHandle(hWriteOut); hWriteOut = NULL;
    CloseHandle(hWriteErr); hWriteErr = NULL;

    if (!ok) {
        DWORD err = GetLastError();
        char buf[128]; _snprintf_s(buf, _TRUNCATE, "CreateProcessW failed (%lu)", (unsigned long)err);
        outError = buf;
        CloseHandle(hReadOut); CloseHandle(hReadErr);
        return false;
    }

    // Read pipes until both are closed
    auto read_all = [](HANDLE h, std::string& dst){
        char tmp[4096]; DWORD got = 0;
        for (;;) {
            if (!ReadFile(h, tmp, (DWORD)sizeof(tmp), &got, nullptr) || got == 0) break;
            dst.append(tmp, tmp + got);
        }
    };

    read_all(hReadOut, outStdout);
    read_all(hReadErr, outError);

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(hReadOut);
    CloseHandle(hReadErr);
    return true;
}

static std::wstring Mk_SearchExeOnPath(const std::wstring& exeName)
{
    wchar_t buf[MAX_PATH];
    DWORD n = SearchPathW(nullptr, exeName.c_str(), nullptr, (DWORD)MAX_PATH, buf, nullptr);
    if (n > 0 && n < MAX_PATH) return std::wstring(buf);
    return exeName; // fall back to provided name
}

// Split a JSON stream (array or NDJSON or concatenated objects) into object strings.
static std::vector<std::string> Mk_SplitJsonObjects(const std::string& s)
{
    std::vector<std::string> out;
    size_t i = 0, n = s.size();
    // Skip optional whitespace and an opening '[' if present
    while (i < n && (unsigned char)s[i] <= ' ') ++i;
    bool hadArray = (i < n && s[i] == '[');
    if (hadArray) ++i;

    int depth = 0; bool inStr = false; bool esc = false; size_t start = std::string::npos;
    for (; i < n; ++i) {
        char c = s[i];
        if (inStr) {
            if (esc) { esc = false; continue; }
            if (c == '\\') { esc = true; continue; }
            if (c == '"') { inStr = false; }
            continue;
        } else {
            if (c == '"') { inStr = true; continue; }
            if (c == '{') {
                if (depth == 0) start = i;
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth == 0 && start != std::string::npos) {
                    out.emplace_back(s.substr(start, i - start + 1));
                    start = std::string::npos;
                }
            }
        }
    }
    return out;
}

static bool Mk_ExtractQuoted(const std::string& s, size_t& i, std::string& out)
{
    out.clear();
    if (i >= s.size() || s[i] != '"') return false;
    ++i; bool esc = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (esc) { out.push_back(c); esc = false; continue; }
        if (c == '\\') { esc = true; continue; }
        if (c == '"') { ++i; return true; }
        out.push_back(c);
    }
    return false;
}

static bool Mk_FindStringField(const std::string& obj, const char* key, std::string& out)
{
    out.clear();
    std::string pat = std::string("\"") + key + "\"";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return false;
    p = obj.find(':', p);
    if (p == std::string::npos) return false;
    // skip whitespace
    while (p < obj.size() && (unsigned char)obj[p] != '"') ++p;
    if (p >= obj.size()) return false;
    return Mk_ExtractQuoted(obj, p, out);
}

static bool Mk_FindIntField(const std::string& obj, const char* key, long long& out)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return false;
    p = obj.find(':', p);
    if (p == std::string::npos) return false;
    ++p; // after ':'
    // skip whitespace
    while (p < obj.size() && (unsigned char)obj[p] <= ' ') ++p;
    // optional quotes (just in case)
    bool quoted = (p < obj.size() && obj[p] == '"');
    if (quoted) ++p;
    size_t start = p;
    while (p < obj.size() && (obj[p] == '-' || (obj[p] >= '0' && obj[p] <= '9'))) ++p;
    if (p == start) return false;
    std::string num = obj.substr(start, p - start);
    out = _strtoui64(num.c_str(), nullptr, 10);
    return true;
}

static bool Mk_FindBoolField(const std::string& obj, const char* key, bool& out)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return false;
    p = obj.find(':', p);
    if (p == std::string::npos) return false;
    ++p; // after ':'
    while (p < obj.size() && (unsigned char)obj[p] <= ' ') ++p;
    if (p >= obj.size()) return false;
    char c = obj[p];
    if (c == 't' || c == 'T' || c == 'f' || c == 'F') {
        // unquoted true/false
        if (p + 4 <= obj.size() && 0 == _strnicmp(&obj[p], "true", 4)) { out = true; return true; }
        if (p + 5 <= obj.size() && 0 == _strnicmp(&obj[p], "false", 5)) { out = false; return true; }
        // Fallback: treat any token starting with 't' as true, 'f' as false
        out = (c == 't' || c == 'T');
        return true;
    }
    if (c == '0' || c == '1') { out = (c == '1'); return true; }
    if (c == '"') {
        ++p; size_t start = p;
        while (p < obj.size() && obj[p] != '"') ++p;
        if (p > start) {
            std::string val = obj.substr(start, p - start);
            out = (0 == _stricmp(val.c_str(), "true") || 0 == _stricmp(val.c_str(), "1") || 0 == _stricmp(val.c_str(), "yes"));
            return true;
        }
    }
    return false;
}

struct Mk_KillRow {
    int tick = 0;
    uint64_t attackerSid = 0;
    std::string attackerName;
    std::string victimName;
    std::string attackerTeam;
    std::string victimTeam;
    std::string weapon;
    bool noscope = false;
    bool inAir = false;
    bool blind = false;
    int penetrated = 0;
    bool smoke = false;
};

static bool Mk_FindArrayOfObjects(const std::string& obj, const char* key, std::vector<std::string>& objects)
{
    objects.clear();
    std::string pat = std::string("\"") + key + "\"";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return false;
    p = obj.find(':', p);
    if (p == std::string::npos) return false;
    ++p; while (p < obj.size() && (unsigned char)obj[p] <= ' ') ++p;
    if (p >= obj.size() || obj[p] != '[') return false;
    // Parse bracketed list extracting object substrings by brace depth
    int depth = 0; bool inStr = false; bool esc = false; size_t startObj = std::string::npos; ++p; // skip '['
    for (; p < obj.size(); ++p) {
        char c = obj[p];
        if (inStr) {
            if (esc) { esc = false; continue; }
            if (c == '\\') { esc = true; continue; }
            if (c == '"') { inStr = false; }
            continue;
        } else {
            if (c == '"') { inStr = true; continue; }
            if (c == '{') { if (depth == 0) startObj = p; depth++; }
            else if (c == '}') { depth--; if (depth == 0 && startObj != std::string::npos) { objects.emplace_back(obj.substr(startObj, p - startObj + 1)); startObj = std::string::npos; } }
            else if (c == ']') { break; }
        }
    }
    return !objects.empty();
}

static bool Mk_ParseKillObject(const std::string& obj, Mk_KillRow& row)
{
    row = Mk_KillRow();
    long long tmp = 0;
    if (Mk_FindIntField(obj, "Tick", tmp)) row.tick = (int)tmp; else return false;
    if (Mk_FindIntField(obj, "AttackerSid", tmp)) row.attackerSid = (uint64_t)tmp; else row.attackerSid = 0;
    Mk_FindStringField(obj, "AttackerName", row.attackerName);
    Mk_FindStringField(obj, "VictimName", row.victimName);
    Mk_FindStringField(obj, "AttackerTeam", row.attackerTeam);
    Mk_FindStringField(obj, "VictimTeam", row.victimTeam);
    Mk_FindStringField(obj, "Weapon", row.weapon);
    // booleans and ints (lenient parsing from literal/string/number)
    long long iv = 0; bool bv = false;
    if (Mk_FindIntField(obj, "Penetrated", iv)) row.penetrated = (int)iv;
    if (Mk_FindBoolField(obj, "Noscope", bv)) row.noscope = bv;
    if (Mk_FindBoolField(obj, "Attackerinair", bv) || Mk_FindBoolField(obj, "AttackerInAir", bv)) row.inAir = bv;
    if (Mk_FindBoolField(obj, "Attackerblind", bv) || Mk_FindBoolField(obj, "AttackerBlind", bv)) row.blind = bv;
    if (Mk_FindBoolField(obj, "Thrusmoke", bv) || Mk_FindBoolField(obj, "Thrusmoke", bv)) row.smoke = bv;
    return !row.attackerName.empty();
}

static bool Mk_ParseRoundObject(const std::string& obj, std::vector<MultikillEvent>& outEvents, std::vector<Mk_EventKill>& outKills)
{
    outEvents.clear();
    long long r = 0; long long startTick = 0;
    if (!Mk_FindIntField(obj, "Round", r)) return false;
    Mk_FindIntField(obj, "StartTick", startTick); // optional

    std::vector<std::string> killObjs;
    if (!Mk_FindArrayOfObjects(obj, "Kills", killObjs)) return false;

    // Parse rows and aggregate per attacker
    std::unordered_map<uint64_t, std::vector<Mk_KillRow>> bySid;
    std::unordered_map<std::string, std::vector<Mk_KillRow>> byName; // fallback if no sid
    for (const auto& ko : killObjs) {
        Mk_KillRow row; if (!Mk_ParseKillObject(ko, row)) continue;
        // Skip teamkills: only count kills where attacker and victim are on different teams (when info present)
        if (!row.attackerTeam.empty() && !row.victimTeam.empty()) {
            if (0 == _stricmp(row.attackerTeam.c_str(), row.victimTeam.c_str())) {
                continue; // ignore teamkill
            }
        }
        // Record for per-kill event views
        Mk_EventKill ke; ke.round=(int)r; ke.tick=row.tick; ke.attackerSid=row.attackerSid; ke.attackerName=row.attackerName; ke.victimName=row.victimName; ke.attackerTeam=row.attackerTeam; ke.victimTeam=row.victimTeam; ke.weapon=row.weapon; ke.noscope=row.noscope; ke.inAir=row.inAir; ke.blind=row.blind; ke.penetrated=row.penetrated; ke.smoke=row.smoke; outKills.push_back(std::move(ke));
        if (row.attackerSid != 0) bySid[row.attackerSid].push_back(row);
        else byName[row.attackerName].push_back(row);
    }
    auto emitFrom = [&](auto& vec, uint64_t sid, const std::string& name){
        if (vec.size() < 2) return; // multi-kills only
        int minTick = INT_MAX, maxTick = INT_MIN; std::vector<std::string> victims; victims.reserve(vec.size());
        for (const auto& k : vec) { if (k.tick < minTick) minTick = k.tick; if (k.tick > maxTick) maxTick = k.tick; if(!k.victimName.empty()) victims.push_back(k.victimName); }
        MultikillEvent ev; ev.steamId = sid; ev.player = name; ev.round = (int)r; ev.count = (int)vec.size(); ev.startTick = (minTick==INT_MAX)?(int)startTick:minTick; ev.endTick = (maxTick==INT_MIN)?(int)startTick:maxTick; ev.victims = std::move(victims); outEvents.push_back(std::move(ev));
    };
    for (auto& kv : bySid) {
        const uint64_t sid = kv.first; auto& rows = kv.second; std::string name = rows.empty()?std::string():rows.front().attackerName; emitFrom(rows, sid, name);
    }
    for (auto& kv : byName) {
        const std::string& name = kv.first; auto& rows = kv.second; emitFrom(rows, 0, name);
    }
    return !outEvents.empty();
}

static void Mk_StartParseThread(const std::string& parserPathUtf8, const std::string& demoPathUtf8)
{
    if (g_MkParsing) return;
    if (g_MkWorker.joinable()) {
        // prior worker finished, join it now
        g_MkWorker.join();
    }
    g_MkParsing = true;
    g_MkParseError.clear();
    {
        std::lock_guard<std::mutex> lk(g_MkMutex);
        g_MkEvents.clear();
    }
    g_MkWorker = std::thread([parserPathUtf8, demoPathUtf8]() {
        std::wstring wExe = Mk_Utf8ToWide(parserPathUtf8);
        if (wExe.find(L"\\") == std::wstring::npos && wExe.find(L"/") == std::wstring::npos) {
            wExe = Mk_SearchExeOnPath(wExe);
        }
        std::wstring wDemo = Mk_Utf8ToWide(demoPathUtf8);
        std::wstring cmd = L"\"" + wExe + L"\" \"" + wDemo + L"\"";

        std::string out; std::string err;
        bool ok = Mk_RunProcessCapture(cmd, out, err);
        std::vector<MultikillEvent> parsed;
        std::string parseError;
        if (!ok) {
            parseError = err.empty() ? std::string("Failed to start DemoParser.exe") : err;
        } else {
            std::vector<Mk_EventKill> allKills;
            auto objs = Mk_SplitJsonObjects(out);
            for (const auto& o : objs) {
                std::vector<MultikillEvent> roundEvents;
                if (Mk_ParseRoundObject(o, roundEvents, allKills)) {
                    parsed.insert(parsed.end(), roundEvents.begin(), roundEvents.end());
                }
            }
            if (parsed.empty()) {
                // If no objects recognized, try NDJSON by lines (per-round only)
                size_t pos = 0; size_t nl;
                while (pos < out.size() && (nl = out.find('\n', pos)) != std::string::npos) {
                    std::string line = out.substr(pos, nl - pos);
                    pos = nl + 1;
                    if (line.find('{') != std::string::npos && line.find('}') != std::string::npos) {
                        std::vector<MultikillEvent> roundEvents; if (Mk_ParseRoundObject(line, roundEvents, allKills)) { parsed.insert(parsed.end(), roundEvents.begin(), roundEvents.end()); }
                    }
                }
                if (parsed.empty() && !err.empty()) parseError = err;
            }

            {
                std::lock_guard<std::mutex> lk(g_MkMutex);
                if (!parseError.empty()) g_MkParseError = parseError; else g_MkParseError.clear();
                g_MkEvents = std::move(parsed);
                g_MkAllKills = std::move(allKills);
            }
        }
        g_MkParsing = false;
    });
}

// ImGui ini handler for persisting workspace window visibility
static void* WorkspaceWindows_ReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
    return (0 == strcmp(name, "WindowVisibility")) ? (void*)1 : nullptr;
}

static void WorkspaceWindows_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    const char* eq = strchr(line, '=');
    if (!eq) return;
    std::string key(line, eq - line);
    int val = atoi(eq + 1);
    bool visible = (val != 0);

    // Store settings to apply later when workspace is created
    g_WorkspaceWindowSettings.hasSettings = true;

    if (key == "Radar") g_WorkspaceWindowSettings.showRadar = visible;
    else if (key == "RadarSettings") g_WorkspaceWindowSettings.showRadarSettings = visible;
    else if (key == "AttachmentControl") g_WorkspaceWindowSettings.showAttachmentControl = visible;
    else if (key == "DOFControl") g_WorkspaceWindowSettings.showDofWindow = visible;
    else if (key == "ObservingBindings") g_WorkspaceWindowSettings.showObservingBindings = visible;
    else if (key == "ObservingCameras") g_WorkspaceWindowSettings.showObservingCameras = visible;
    else if (key == "GroupView") g_WorkspaceWindowSettings.showGroupViewWindow = visible;
    else if (key == "EventBrowser") g_WorkspaceWindowSettings.showMultikillWindow = visible;
    else if (key == "Viewport") g_WorkspaceWindowSettings.showBackbufferWindow = visible;
    else if (key == "Sequencer") g_WorkspaceWindowSettings.showSequencer = visible;
    else if (key == "MirvCamera") g_WorkspaceWindowSettings.showCameraControl = visible;
    else if (key == "Console") g_WorkspaceWindowSettings.showOverlayConsole = visible;
    else if (key == "Gizmo") g_WorkspaceWindowSettings.showGizmo = visible;
}

static void WorkspaceWindows_WriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* out_buf) {
    // Only save when workspace is enabled
    if (!g_GroupIntoWorkspace) return;

    out_buf->appendf("[%s][%s]\n", handler->TypeName, "WindowVisibility");
    out_buf->appendf("Radar=%d\n", g_ShowRadar ? 1 : 0);
    out_buf->appendf("RadarSettings=%d\n", g_ShowRadarSettings ? 1 : 0);
    out_buf->appendf("AttachmentControl=%d\n", g_ShowAttachmentControl ? 1 : 0);
    out_buf->appendf("DOFControl=%d\n", g_ShowDofWindow ? 1 : 0);
    out_buf->appendf("ObservingBindings=%d\n", g_ShowObservingBindings ? 1 : 0);
    out_buf->appendf("ObservingCameras=%d\n", g_ShowObservingCameras ? 1 : 0);
    out_buf->appendf("GroupView=%d\n", g_ShowGroupViewWindow ? 1 : 0);
    out_buf->appendf("EventBrowser=%d\n", g_ShowMultikillWindow ? 1 : 0);
    out_buf->appendf("Viewport=%d\n", g_ShowBackbufferWindow ? 1 : 0);
    out_buf->appendf("Sequencer=%d\n", g_ShowSequencer ? 1 : 0);
    out_buf->appendf("MirvCamera=%d\n", g_ShowCameraControl ? 1 : 0);
    out_buf->appendf("Console=%d\n", g_ShowOverlayConsole ? 1 : 0);
    out_buf->appendf("Gizmo=%d\n", g_ShowGizmo ? 1 : 0);
    out_buf->append("\n");
}

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
    else if (key == "DemoDir") s->demoDir = val;
}
static void OverlayPaths_WriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* out_buf) {
    out_buf->appendf("[%s][%s]\n", handler->TypeName, "Paths");
    if (!g_OverlayPaths.campathDir.empty()) out_buf->appendf("CampathDir=%s\n", g_OverlayPaths.campathDir.c_str());
    if (!g_OverlayPaths.recordBrowseDir.empty()) out_buf->appendf("RecordBrowseDir=%s\n", g_OverlayPaths.recordBrowseDir.c_str());
    if (!g_OverlayPaths.demoDir.empty()) out_buf->appendf("DemoDir=%s\n", g_OverlayPaths.demoDir.c_str());
    out_buf->append("\n");
}

// ImGui ini handler for persisting camera profiles
static void* CameraProfiles_ReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
    // Each profile gets its own section: [HLAECameraProfiles][ProfileName]
    // Create a new profile if needed
    for (auto& profile : g_CameraProfiles) {
        if (profile.name == name) {
            return &profile;
        }
    }
    // Create new profile
    CameraProfile newProfile;
    newProfile.name = name;
    g_CameraProfiles.push_back(newProfile);
    return &g_CameraProfiles.back();
}

static void CameraProfiles_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    CameraProfile* profile = (CameraProfile*)entry;
    const char* eq = strchr(line, '=');
    if (!eq) return;

    std::string key(line, eq - line);
    std::string val(eq + 1);

    // Parse camera entries: Camera_<idx>_Name=..., Camera_<idx>_Path=..., Camera_<idx>_Image=...
    if (key.rfind("Camera_", 0) == 0) {
        // Find separator between index and property
        size_t sep = key.find('_', 7); // after "Camera_"
        if (sep == std::string::npos) return;
        // Parse index
        int cameraIdx = 0;
        try {
            cameraIdx = std::stoi(key.substr(7, sep - 7));
        } catch (...) { return; }
        // Property name after separator
        std::string property = key.substr(sep + 1);

        // Ensure camera exists in vector
        while ((int)profile->cameras.size() <= cameraIdx) {
            profile->cameras.push_back(CameraItem());
        }

        if (property == "Name") {
            profile->cameras[cameraIdx].name = val;
        } else if (property == "Path") {
            profile->cameras[cameraIdx].camPathFile = val;
        } else if (property == "Image") {
            profile->cameras[cameraIdx].imagePath = val;
        }
    }
}

static void CameraProfiles_WriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* out_buf) {
    if (g_CameraProfiles.empty()) return; // Don't write anything if no profiles

    for (const auto& profile : g_CameraProfiles) {
        out_buf->appendf("[%s][%s]\n", handler->TypeName, profile.name.c_str());
        for (size_t i = 0; i < profile.cameras.size(); i++) {
            const CameraItem& camera = profile.cameras[i];
            out_buf->appendf("Camera_%d_Name=%s\n", (int)i, camera.name.c_str());
            if (!camera.camPathFile.empty()) {
                out_buf->appendf("Camera_%d_Path=%s\n", (int)i, camera.camPathFile.c_str());
            }
            if (!camera.imagePath.empty()) {
                out_buf->appendf("Camera_%d_Image=%s\n", (int)i, camera.imagePath.c_str());
            }
        }
        out_buf->append("\n");
    }
}

// Camera Groups Settings Handlers
static void* CameraGroups_ReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
    // Expect "Profile||Group" as composite key; support legacy "Group" only
    std::string composite(name ? name : "");
    std::string prof, grp;

    size_t delim = composite.find("||");
    if (delim != std::string::npos) {
        prof = composite.substr(0, delim);
        grp  = composite.substr(delim + 2);
    } else {
        // Legacy: no profile in key
        grp = composite;
        prof.clear();
    }

    // Find existing (profile, group)
    for (auto& g : g_CameraGroups) {
        if (g.name == grp && g.profileName == prof)
            return &g;
    }

    // Create new group
    CameraGroup newGroup;
    newGroup.name = grp;
    newGroup.profileName = prof; // may be empty for legacy
    g_CameraGroups.push_back(newGroup);
    return &g_CameraGroups.back();
}


static void CameraGroups_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    CameraGroup* group = (CameraGroup*)entry;
    const char* eq = strchr(line, '=');
    if (!eq) return;

    std::string key(line, eq - line);
    std::string val(eq + 1);

    if (key == "Profile") {
        group->profileName = val;                 // NEW
    } else if (key == "Name") {
        group->name = val;                        // NEW (allows rename in file)
    } else if (key == "PlayMode") {
        try { int mode = std::stoi(val); if (mode >= 0 && mode <= 2) group->playMode = (GroupPlayMode)mode; } catch (...) {}
    } else if (key == "SequentialIndex") {
        try { group->sequentialIndex = std::stoi(val); } catch (...) {}
    } else if (key == "CameraIndices") {
        group->cameraIndices.clear();
        std::stringstream ss(val);
        std::string item;
        while (std::getline(ss, item, ',')) {
            try { group->cameraIndices.push_back(std::stoi(item)); } catch (...) {}
        }
    }
}


static void CameraGroups_WriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* out_buf) {
    if (g_CameraGroups.empty()) return;

    for (const auto& group : g_CameraGroups) {
        // Composite key keeps sections unique across profiles
        std::string composite = group.profileName + "||" + group.name; // profile||group
        out_buf->appendf("[%s][%s]\n", handler->TypeName, composite.c_str());

        // Also write explicit fields for clarity/forward-compat
        out_buf->appendf("Profile=%s\n", group.profileName.c_str());
        out_buf->appendf("Name=%s\n", group.name.c_str());
        out_buf->appendf("PlayMode=%d\n", (int)group.playMode);
        out_buf->appendf("SequentialIndex=%d\n", group.sequentialIndex);

        if (!group.cameraIndices.empty()) {
            out_buf->append("CameraIndices=");
            for (size_t i = 0; i < group.cameraIndices.size(); i++) {
                out_buf->appendf("%d", group.cameraIndices[i]);
                if (i + 1 < group.cameraIndices.size()) out_buf->append(",");
            }
            out_buf->append("\n");
        }
        out_buf->append("\n");
    }
}

// Helper function to load grenade icon SVG using NanoSVG
static GrenadeIconTex LoadGrenadeIcon(ID3D11Device* device, const std::wstring& svgPath) {
    GrenadeIconTex result{};
    if (!device) return result;

    // Check if file exists
    DWORD attr = GetFileAttributesW(svgPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return result;

    // Read SVG file
    FILE* f = _wfopen(svgPath.c_str(), L"rb");
    if (!f) return result;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return result; }
    rewind(f);

    std::string svgText;
    svgText.resize((size_t)sz + 1);
    size_t rd = fread(&svgText[0], 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) return result;
    svgText[sz] = '\0';

    // Parse SVG
    NSVGimage* img = nsvgParse(&svgText[0], "px", 96.0f);
    if (!img) return result;

    // Calculate raster size
    float maxDim = (img->width > img->height) ? img->width : img->height;
    float target = 64.0f; // base raster size for grenade icons
    float scale = (maxDim > 0.0f) ? (target / maxDim) : 1.0f;
    int rw = (int)ceilf(img->width * scale);
    int rh = (int)ceilf(img->height * scale);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;

    // Rasterize SVG
    std::vector<unsigned char> rgba((size_t)rw * (size_t)rh * 4u);
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(img); return result; }

    nsvgRasterize(rast, img, 0.0f, 0.0f, scale, rgba.data(), rw, rh, rw * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);

    // Create D3D11 texture
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = rw;
    desc.Height = rh;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = rgba.data();
    init.SysMemPitch = rw * 4;

    ID3D11Texture2D* tex2d = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&desc, &init, &tex2d)) && tex2d) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sdesc{};
        sdesc.Format = desc.Format;
        sdesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sdesc.Texture2D.MostDetailedMip = 0;
        sdesc.Texture2D.MipLevels = 1;

        ID3D11ShaderResourceView* srv = nullptr;
        if (SUCCEEDED(device->CreateShaderResourceView(tex2d, &sdesc, &srv)) && srv) {
            result.srv = srv;
            result.w = rw;
            result.h = rh;
        }
        tex2d->Release();
    }

    return result;
}

// Get or load grenade icon from cache
static GrenadeIconTex GetGrenadeIcon(ID3D11Device* device, const char* iconName) {
    auto it = g_GrenadeIconCache.find(iconName);
    if (it != g_GrenadeIconCache.end()) return it->second;

    GrenadeIconTex tex{};
    if (!device) { g_GrenadeIconCache[iconName] = tex; return tex; }

    // Build path: {hlaeFolder}resources/overlay/weapons/{iconName}.svg
    const wchar_t* hf = GetHlaeFolderW();
    if (!hf || !*hf) { g_GrenadeIconCache[iconName] = tex; return tex; }

    std::wstring svgPath = std::wstring(hf) + L"resources\\overlay\\weapons\\" +
                           std::wstring(iconName, iconName + strlen(iconName)) + L".svg";

    tex = LoadGrenadeIcon(device, svgPath);
    g_GrenadeIconCache[iconName] = tex;
    return tex;
}

// Helper function to load image from file using WIC and create D3D11 texture
static bool LoadImageToTexture(ID3D11Device* device, const std::string& filePath, CameraTexture& outTexture) {
    // Convert to wide string
    std::wstring wFilePath(filePath.begin(), filePath.end());

    // Initialize COM (may already be initialized, ignore error)
    CoInitialize(nullptr);

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(wFilePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) {
        factory->Release();
        return false;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        decoder->Release();
        factory->Release();
        return false;
    }

    IWICFormatConverter* converter = nullptr;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        frame->Release();
        decoder->Release();
        factory->Release();
        return false;
    }

    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        converter->Release();
        frame->Release();
        decoder->Release();
        factory->Release();
        return false;
    }

    UINT width, height;
    converter->GetSize(&width, &height);

    std::vector<BYTE> pixels(width * height * 4);
    hr = converter->CopyPixels(nullptr, width * 4, (UINT)pixels.size(), pixels.data());

    converter->Release();
    frame->Release();
    decoder->Release();
    factory->Release();

    if (FAILED(hr)) return false;

    // Create D3D11 texture
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = pixels.data();
    initData.SysMemPitch = width * 4;

    ID3D11Texture2D* texture = nullptr;
    hr = device->CreateTexture2D(&desc, &initData, &texture);
    if (FAILED(hr)) return false;

    // Create shader resource view
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = device->CreateShaderResourceView(texture, &srvDesc, &outTexture.srv);
    texture->Release();

    if (FAILED(hr)) return false;

    outTexture.width = (int)width;
    outTexture.height = (int)height;

    return true;
}

// Helper function to save a D3D11 texture to PNG file using WIC
static bool SaveTextureToFile(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, const std::wstring& filePath) {
    if (!device || !context || !texture) return false;

    // Get texture description
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    // Create staging texture for CPU readback
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.SampleDesc.Quality = 0;

    ID3D11Texture2D* stagingTexture = nullptr;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr) || !stagingTexture) return false;

    // Copy texture to staging
    if (desc.SampleDesc.Count > 1) {
        // MSAA texture - need to resolve first
        context->ResolveSubresource(stagingTexture, 0, texture, 0, desc.Format);
    } else {
        context->CopyResource(stagingTexture, texture);
    }

    // Map staging texture
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        stagingTexture->Release();
        return false;
    }

    // Initialize WIC
    CoInitialize(nullptr);
    IWICImagingFactory* factory = nullptr;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        context->Unmap(stagingTexture, 0);
        stagingTexture->Release();
        return false;
    }

    // Create stream
    IWICStream* stream = nullptr;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) {
        factory->Release();
        context->Unmap(stagingTexture, 0);
        stagingTexture->Release();
        return false;
    }

    hr = stream->InitializeFromFilename(filePath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        stream->Release();
        factory->Release();
        context->Unmap(stagingTexture, 0);
        stagingTexture->Release();
        return false;
    }

    // Create PNG encoder
    IWICBitmapEncoder* encoder = nullptr;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) {
        stream->Release();
        factory->Release();
        context->Unmap(stagingTexture, 0);
        stagingTexture->Release();
        return false;
    }

    hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        encoder->Release();
        stream->Release();
        factory->Release();
        context->Unmap(stagingTexture, 0);
        stagingTexture->Release();
        return false;
    }

    // Create frame
    IWICBitmapFrameEncode* frame = nullptr;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr)) {
        encoder->Release();
        stream->Release();
        factory->Release();
        context->Unmap(stagingTexture, 0);
        stagingTexture->Release();
        return false;
    }

    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) {
        frame->Release();
        encoder->Release();
        stream->Release();
        factory->Release();
        context->Unmap(stagingTexture, 0);
        stagingTexture->Release();
        return false;
    }

    hr = frame->SetSize(desc.Width, desc.Height);
    if (FAILED(hr)) {
        frame->Release();
        encoder->Release();
        stream->Release();
        factory->Release();
        context->Unmap(stagingTexture, 0);
        stagingTexture->Release();
        return false;
    }

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) {
        frame->Release();
        encoder->Release();
        stream->Release();
        factory->Release();
        context->Unmap(stagingTexture, 0);
        stagingTexture->Release();
        return false;
    }

    // Need to swap R and B channels (BGRA -> RGBA) for correct colors in PNG
    // Create a temporary buffer with swapped channels
    std::vector<BYTE> rgbaData(desc.Height * mapped.RowPitch);
    BYTE* src = (BYTE*)mapped.pData;
    BYTE* dst = rgbaData.data();

    for (UINT y = 0; y < desc.Height; y++) {
        BYTE* rowSrc = src + y * mapped.RowPitch;
        BYTE* rowDst = dst + y * mapped.RowPitch;

        for (UINT x = 0; x < desc.Width; x++) {
            UINT pixelOffset = x * 4;
            // Swap B and R (BGRA -> RGBA)
            rowDst[pixelOffset + 0] = rowSrc[pixelOffset + 2]; // R
            rowDst[pixelOffset + 1] = rowSrc[pixelOffset + 1]; // G
            rowDst[pixelOffset + 2] = rowSrc[pixelOffset + 0]; // B
            rowDst[pixelOffset + 3] = rowSrc[pixelOffset + 3]; // A
        }
    }

    // Write pixels with swapped channels
    hr = frame->WritePixels(desc.Height, mapped.RowPitch, desc.Height * mapped.RowPitch, rgbaData.data());
    if (FAILED(hr)) {
        frame->Release();
        encoder->Release();
        stream->Release();
        factory->Release();
        context->Unmap(stagingTexture, 0);
        stagingTexture->Release();
        return false;
    }

    // Commit
    hr = frame->Commit();
    if (FAILED(hr)) {
        frame->Release();
        encoder->Release();
        stream->Release();
        factory->Release();
        context->Unmap(stagingTexture, 0);
        stagingTexture->Release();
        return false;
    }

    hr = encoder->Commit();

    // Cleanup
    frame->Release();
    encoder->Release();
    stream->Release();
    factory->Release();
    context->Unmap(stagingTexture, 0);
    stagingTexture->Release();

    return SUCCEEDED(hr);
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
    ApplyHlaeDarkStyle();
    ImGuiIO& io = ImGui::GetIO();

    // Always start in normal mode with imgui.ini
    // When user enables workspace, we'll switch to imgui_workspace.ini
    io.IniFilename = g_IniFileNormal.c_str();

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;    // Enable docking for workspace container
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigInputTrickleEventQueue = false;
    // Load our fonts once (before backend init so device objects match the atlas)
    if (!g_FontsLoaded)
    {
        ImGuiIO& lio = ImGui::GetIO();
        ImFontAtlas* atlas = lio.Fonts;
    #ifdef IMGUI_ENABLE_FREETYPE
        lio.Fonts->FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_LightHinting;
        //lio.Fonts->FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_Monochrome;
        lio.Fonts->FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_MonoHinting;
    #endif
        // Always add the default ImGui font:
        g_FontDefault = atlas->AddFontDefault();

        // Helper to check if a Windows font file exists before adding:
        auto addFontIfPresent = [&](const char* path, float size, ImFontConfig* cfg = nullptr)->ImFont*
        {
            FILE* f = nullptr;
            if (0 == fopen_s(&f, path, "rb") && f) { fclose(f); return cfg ? atlas->AddFontFromFileTTF(path, size, cfg) : atlas->AddFontFromFileTTF(path, size); }
            return nullptr;
        };


        // Reasonable base size (UI scaling is handled via FontGlobalScale elsewhere)
        const float baseSize = 16.0f;

        // Windows system fonts (best-effort):
        ImFontConfig cfg{};
        cfg.OversampleH = 1;
        cfg.OversampleV = 1;
        cfg.PixelSnapH  = true;
        cfg.RasterizerMultiply = 1.05f;
        g_FontMono = addFontIfPresent("C:\\Windows\\Fonts\\consola.ttf", baseSize, &cfg);
        g_FontSans  = addFontIfPresent("C:\\Windows\\Fonts\\segoeui.ttf", baseSize, &cfg);   // Segoe UI (sans-serif)
        g_FontSilly = addFontIfPresent("C:\\Windows\\Fonts\\comic.ttf",   baseSize, &cfg);   // Comic Sans (silly)

        // Start with default:
        lio.FontDefault = g_FontDefault ? g_FontDefault : lio.Fonts->Fonts.empty() ? nullptr : lio.Fonts->Fonts[0];

        g_FontsLoaded = true;
    }


    if (!ImGui_ImplWin32_Init(m_Hwnd)) return false;
    if (!ImGui_ImplDX11_Init(m_Device, m_Context)) return false;
    advancedfx::Message("Overlay: renderer=DX11\n");

    // Provide icon device + directory for HUD (resources/overlay)
    {
        Hud::SetIconDevice((void*)m_Device);
        const wchar_t* hf = GetHlaeFolderW();
        if (hf && *hf) {
            // Icons live under resources/overlay/{icons,weapons}
            std::wstring wdir = std::wstring(hf) + L"resources\\overlay";
            Hud::SetIconsDirectoryW(wdir);
        }
    }

    // Register custom ImGui ini settings handler for workspace window visibility
    if (!ImGui::FindSettingsHandler("HLAEWorkspaceWindows")) {
        ImGuiSettingsHandler iniWS;
        iniWS.TypeName = "HLAEWorkspaceWindows";
        iniWS.TypeHash = ImHashStr(iniWS.TypeName);
        iniWS.ReadOpenFn = WorkspaceWindows_ReadOpen;
        iniWS.ReadLineFn = WorkspaceWindows_ReadLine;
        iniWS.WriteAllFn = WorkspaceWindows_WriteAll;
        ImGui::AddSettingsHandler(&iniWS);
    }

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

    // Register custom ImGui ini settings handler for camera profiles
    if (!ImGui::FindSettingsHandler("HLAECameraProfiles")) {
        ImGuiSettingsHandler ini2;
        ini2.TypeName = "HLAECameraProfiles";
        ini2.TypeHash = ImHashStr(ini2.TypeName);
        ini2.ReadOpenFn = CameraProfiles_ReadOpen;
        ini2.ReadLineFn = CameraProfiles_ReadLine;
        ini2.WriteAllFn = CameraProfiles_WriteAll;
        ImGui::AddSettingsHandler(&ini2);
        // Load persisted profiles now that the handler is registered
        ImGuiIO& io3 = ImGui::GetIO();
        if (io3.IniFilename && io3.IniFilename[0]) {
            ImGui::LoadIniSettingsFromDisk(io3.IniFilename);
        }
    }

    // Register Camera Groups settings handler
    if (!ImGui::FindSettingsHandler("HLAECameraGroups")) {
        ImGuiSettingsHandler ini3;
        ini3.TypeName = "HLAECameraGroups";
        ini3.TypeHash = ImHashStr(ini3.TypeName);
        ini3.ReadOpenFn = CameraGroups_ReadOpen;
        ini3.ReadLineFn = CameraGroups_ReadLine;
        ini3.WriteAllFn = CameraGroups_WriteAll;
        ImGui::AddSettingsHandler(&ini3);
        // Load persisted groups now that the handler is registered
        ImGuiIO& io4 = ImGui::GetIO();
        if (io4.IniFilename && io4.IniFilename[0]) {
            ImGui::LoadIniSettingsFromDisk(io4.IniFilename);
        }
    }

    // Route Win32 messages to ImGui when visible
    if (!Overlay::Get().GetInputRouter()) {
        auto router = std::make_unique<InputRouter>();
        if (router->Attach(m_Hwnd)) {
            router->SetMessageCallback([](void* hwnd, unsigned int msg, uint64_t wparam, int64_t lparam) -> bool {
                // Handle activation up-front (don’t involve ImGui yet)
                if (msg == WM_ACTIVATEAPP) {
                    g_windowActive = (wparam != 0);
                    if (g_windowActive) g_dropFirstMouseAfterActivate = true;
                    // Let the game also see this; don't consume here
                    // (no return; we keep processing as usual)
                }
                // Filter mouse input during deactivation or immediately after re-activation
                if (!g_windowActive) {
                    // While app is deactivated, don't feed ImGui with mouse; let the game handle it.
                    switch (msg) {
                        case WM_MOUSEMOVE:
                        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
                        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
                        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
                        case WM_MOUSEWHEEL:
                        case WM_MOUSEHWHEEL:
                            return false; // not consumed by overlay; pass through
                        default: break;
                    }
                }

                // Right after we re-activate, drop the first move to avoid a burst into ImGui
                if (g_dropFirstMouseAfterActivate && msg == WM_MOUSEMOVE) {
                    g_dropFirstMouseAfterActivate = false;
                    return false; // pass through, don't feed ImGui this one
                }
                // If overlay is visible, force an arrow cursor on WM_SETCURSOR to avoid invisible game cursor
                if (Overlay::Get().IsVisible() && msg == WM_SETCURSOR) {
                    // Only force arrow when ImGui actually wants to capture the mouse
                    // to avoid fighting overlay's own RMB-preview control (which hides cursor).
                    if (ImGui::GetIO().WantCaptureMouse && !Overlay::Get().IsRmbPassthroughActive()) {
                        SetCursor(LoadCursor(NULL, IDC_ARROW));
                        return true; // handled
                    }
                }

                // Feed ImGui first so it updates IO states
                bool consumed = CallImGuiWndProcGuarded((HWND)hwnd, (UINT)msg, (WPARAM)wparam, (LPARAM)lparam);

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
                static bool s_prevPassThrough = false;
                Overlay::Get().SetRmbPassthroughActive(passThrough);
                // If RMB passthrough just ended, make sure to release any cursor confinement
                if (s_prevPassThrough && !passThrough) {
                    ClipCursor(nullptr);
                }
                s_prevPassThrough = passThrough;

                // While in RMB passthrough, allow holding 'R' to switch mouse horizontal motion to camera roll control.
                static bool s_rollModeActive = false;
                static bool s_rollPrevCamEnabled = false;
                bool rHeld = (GetKeyState('R') & 0x8000) != 0;
                bool wantRollMode = passThrough && rHeld;
                if (wantRollMode != s_rollModeActive) {
                    s_rollModeActive = wantRollMode;
                    if (MirvInput* pMirv = Afx_GetMirvInput()) {
                        // Ensure cursor lock like mirv camera mode by enabling it temporarily if needed
                        if (s_rollModeActive) {
                            s_rollPrevCamEnabled = pMirv->GetCameraControlMode();
                            if (!s_rollPrevCamEnabled) pMirv->SetCameraControlMode(true);
                            // Exclusivity: disable FOV-mode if it was on
                            if (pMirv->GetMouseFovMode()) pMirv->SetMouseFovMode(false);
                            pMirv->SetMouseRollMode(true);
                        } else {
                            pMirv->SetMouseRollMode(false);
                            // Restore previous camera control mode only if no other modifier mode is active
                            if (!pMirv->GetMouseFovMode())
                                pMirv->SetCameraControlMode(s_rollPrevCamEnabled);
                        }
                    }
                }

                // While in RMB passthrough, allow holding 'F' to switch mouse horizontal motion to camera FOV control.
                static bool s_fovModeActive = false;
                static bool s_fovPrevCamEnabled = false;
                bool fHeld = (GetKeyState('F') & 0x8000) != 0;
                bool wantFovMode = passThrough && fHeld;
                if (wantFovMode != s_fovModeActive) {
                    s_fovModeActive = wantFovMode;
                    if (MirvInput* pMirv = Afx_GetMirvInput()) {
                        if (s_fovModeActive) {
                            s_fovPrevCamEnabled = pMirv->GetCameraControlMode();
                            if (!s_fovPrevCamEnabled) pMirv->SetCameraControlMode(true);
                            // Exclusivity: disable Roll-mode if it was on
                            if (pMirv->GetMouseRollMode()) pMirv->SetMouseRollMode(false);
                            pMirv->SetMouseFovMode(true);
                        } else {
                            pMirv->SetMouseFovMode(false);
                            // Restore previous camera control mode only if no other modifier mode is active
                            if (!pMirv->GetMouseRollMode())
                                pMirv->SetCameraControlMode(s_fovPrevCamEnabled);
                        }
                    }
                }

                // While in RMB passthrough mode, intercept scroll wheel to control mirv camera instead of passing to game
                if (passThrough && msg == WM_MOUSEWHEEL) {
                    int zDelta = GET_WHEEL_DELTA_WPARAM((WPARAM)wparam);
                    int steps = zDelta / WHEEL_DELTA;
                    if (steps != 0) {
                        if (MirvInput* pMirv = Afx_GetMirvInput()) {
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
                    return true; // consume wheel
                }

                // In passthrough, swallow Shift/Ctrl (used for scroll modifiers) and swallow 'R'/'F' while in modifier modes
                if (passThrough) {
                    switch (msg) {
                        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
                            WPARAM vk = (WPARAM)wparam;
                            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL)
                                return true; // consume modifiers
                            if ((s_rollModeActive && vk == 'R') || (s_fovModeActive && vk == 'F'))
                                return true; // consume R to avoid in-game actions while roll-mode active
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
                if (!consumed && overlayVisible && !g_GroupIntoWorkspace) {
                    switch (msg) {
                        case WM_MOUSEMOVE:
                        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
                        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
                        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
                        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
                        case WM_MOUSEWHEEL:
                        case WM_MOUSEHWHEEL:
                        case WM_INPUT: // block raw input from reaching the game while overlay active
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
    // Ensure multikill worker thread is not left joinable (MSVC debug would abort on destruction).
    if (g_MkWorker.joinable()) {
        g_MkWorker.join();
    }
    m_Rtv.width = m_Rtv.height = 0;
    ReleaseBackbufferPreview();
    // Release radar texture if loaded
    if (g_RadarCtx.srv) { g_RadarCtx.srv->Release(); g_RadarCtx.srv = nullptr; }
    g_RadarAssetsLoaded = false;

    // Stop GSI server
    g_GsiServer.Stop();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    // Do not destroy ImGui context here, shared across overlays.
    m_Initialized = false;
#endif
}

void OverlayDx11::BeginFrame(float dtSeconds) {
#ifdef _WIN32
    if (!m_Initialized) return;

    {
        ImGuiIO& io0 = ImGui::GetIO();
        if (io0.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    // Serialize with WndProc touching ImGui’s input/event queue
    std::lock_guard<std::mutex> lock(g_imguiInputMutex);

    ImGuiIO& io = ImGui::GetIO();
    if (dtSeconds > 0.0f) io.DeltaTime = dtSeconds;

    // Keep backend order consistent with Dear ImGui examples
    ImGui_ImplDX11_NewFrame();
    // Drain queued Win32 messages into ImGui before Win32_NewFrame so IO is in sync
    ImGui_DrainQueuedWin32Msgs();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Optional Workspace container: a parent window that hosts a DockSpace and can hold
    // all other overlay windows as tabs/panes. This allows dragging a single platform window
    // to move all overlay windows together outside the game.
    if (g_GroupIntoWorkspace) {
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;  // Enable multi-viewport / platform windows

        ImGuiWindowFlags ws_flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;

        // NEW: keep the workspace as its own platform window from frame 1
        ImGuiWindowClass wc{};
        wc.ViewportFlagsOverrideSet   = ImGuiViewportFlags_NoAutoMerge;      // per-window "no merge"
        // Optional extras:
        // wc.ViewportFlagsOverrideSet |= ImGuiViewportFlags_NoTaskBarIcon;   // no taskbar icon
        // wc.ViewportFlagsOverrideSet |= ImGuiViewportFlags_NoDecoration;    // no OS decorations
        ImGui::SetNextWindowClass(&wc);

        // Optional: initial placement/size on first show (not required for NoAutoMerge)
        // ImGui::SetNextWindowPos(ImGui::GetMainViewport()->Pos + ImVec2(80, 80), ImGuiCond_Appearing);
        // ImGui::SetNextWindowSize(ImVec2(1000, 700), ImGuiCond_Appearing);

        if (ImGui::Begin("HLAE Workspace", &g_WorkspaceOpen, ws_flags)) {
            if (ImGuiViewport* vp = ImGui::GetWindowViewport())
                g_WorkspaceViewportId = vp->ID;

            g_WorkspaceDockspaceId = ImGui::GetID("HLAE_WorkspaceDockSpace");
            ImGui::DockSpace(g_WorkspaceDockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_None);

            // Build default layout only when:
            // 1. User clicked "Redock all" (g_WorkspaceForceRedock), OR
            // 2. Workspace was just enabled AND there's no saved layout in .ini
            ImGuiDockNode* existingNode = ImGui::DockBuilderGetNode(g_WorkspaceDockspaceId);

            bool shouldBuildLayout = g_WorkspaceForceRedock || (g_WorkspaceNeedsLayout && !existingNode);

            if (shouldBuildLayout) {
                g_WorkspaceNeedsLayout = false;
                g_WorkspaceForceRedock = false;
                g_WorkspaceLayoutInitialized = true;
                ImGui::DockBuilderRemoveNode(g_WorkspaceDockspaceId);
                ImGui::DockBuilderAddNode(g_WorkspaceDockspaceId, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(g_WorkspaceDockspaceId, ImGui::GetWindowSize());

                const char* kWindowsToDock[] = {
                    "HLAE Overlay","Radar","Radar Settings","Attachment Control","DOF Control",
                    "Observing Bindings","Observing Cameras","Group View","Event Browser","Viewport",
                    "Smooth Camera Settings","HLAE Sequencer","Mirv Camera","HLAE Console","Gizmo",
                };
                for (const char* w : kWindowsToDock)
                    ImGui::DockBuilderDockWindow(w, g_WorkspaceDockspaceId);

                ImGui::DockBuilderFinish(g_WorkspaceDockspaceId);
            } else if (g_WorkspaceNeedsLayout && existingNode) {
                // Workspace was enabled but .ini already has a saved layout - use that instead
                g_WorkspaceNeedsLayout = false;
                g_WorkspaceLayoutInitialized = true;
            }
        }
        ImGui::End();

        // Apply stored window visibility settings now that workspace exists
        if (g_WorkspaceWindowSettings.hasSettings) {
            g_ShowRadar = g_WorkspaceWindowSettings.showRadar;
            g_ShowRadarSettings = g_WorkspaceWindowSettings.showRadarSettings;
            g_ShowAttachmentControl = g_WorkspaceWindowSettings.showAttachmentControl;
            g_ShowDofWindow = g_WorkspaceWindowSettings.showDofWindow;
            g_ShowObservingBindings = g_WorkspaceWindowSettings.showObservingBindings;
            g_ShowObservingCameras = g_WorkspaceWindowSettings.showObservingCameras;
            g_ShowGroupViewWindow = g_WorkspaceWindowSettings.showGroupViewWindow;
            g_ShowMultikillWindow = g_WorkspaceWindowSettings.showMultikillWindow;
            g_ShowBackbufferWindow = g_WorkspaceWindowSettings.showBackbufferWindow;
            g_ShowSequencer = g_WorkspaceWindowSettings.showSequencer;
            g_ShowCameraControl = g_WorkspaceWindowSettings.showCameraControl;
            g_ShowOverlayConsole = g_WorkspaceWindowSettings.showOverlayConsole;
            g_ShowGizmo = g_WorkspaceWindowSettings.showGizmo;
            g_WorkspaceWindowSettings.hasSettings = false; // Only apply once
        }
    } else {
        g_WorkspaceViewportId = 0;
    }

    {
        // Query entity data for bird camera if active
        if (advancedfx::overlay::BirdCamera_IsActive()) {
            int fromIdx = 0;
            int toIdx = 0;
            advancedfx::overlay::BirdCamera_GetControllerIndices(fromIdx, toIdx);

            // Query target A (from player)
            {
                float pos[3] = {0, 0, 0};
                float ang[3] = {0, 0, 0};
                bool valid = false;

                if (g_pEntityList && *g_pEntityList && g_GetEntityFromIndex) {
                    CEntityInstance* controller = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, fromIdx);
                    if (controller && controller->IsPlayerController()) {
                        auto pawnHandle = controller->GetPlayerPawnHandle();
                        if (pawnHandle.IsValid()) {
                            int pawnIdx = pawnHandle.GetEntryIndex();
                            CEntityInstance* pawn = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, pawnIdx);
                            if (pawn && pawn->IsPlayerPawn()) {
                                pawn->GetRenderEyeOrigin(pos);
                                pawn->GetRenderEyeAngles(ang);
                                // Don't add height here - BirdCamera will add it based on phase
                                valid = true;
                            } else {
                                advancedfx::Message("BirdCamera: Target A - pawn not found or not player pawn (fromIdx=%d, pawnIdx=%d)\n",
                                          fromIdx, pawnIdx);
                            }
                        } else {
                            advancedfx::Message("BirdCamera: Target A - pawn handle invalid (fromIdx=%d)\n", fromIdx);
                        }
                    } else {
                        advancedfx::Message("BirdCamera: Target A - controller not found or not player controller (fromIdx=%d)\n", fromIdx);
                    }
                } else {
                    advancedfx::Message("BirdCamera: Target A - entity system not available\n");
                }
                advancedfx::overlay::BirdCamera_SetTargetA(pos, ang, valid);
            }

            // Query target B (to player)
            {
                float pos[3] = {0, 0, 0};
                float ang[3] = {0, 0, 0};
                bool valid = false;

                if (g_pEntityList && *g_pEntityList && g_GetEntityFromIndex) {
                    CEntityInstance* controller = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, toIdx);
                    if (controller && controller->IsPlayerController()) {
                        auto pawnHandle = controller->GetPlayerPawnHandle();
                        if (pawnHandle.IsValid()) {
                            int pawnIdx = pawnHandle.GetEntryIndex();
                            CEntityInstance* pawn = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, pawnIdx);
                            if (pawn && pawn->IsPlayerPawn()) {
                                pawn->GetRenderEyeOrigin(pos);
                                pawn->GetRenderEyeAngles(ang);
                                // For descend phase: normal eye position
                                valid = true;
                            }
                        }
                    }
                }
                advancedfx::overlay::BirdCamera_SetTargetB(pos, ang, valid);
            }
        }

        // Update bird camera transitions
        advancedfx::overlay::BirdCamera_Update(io.DeltaTime, (float)g_MirvTime.curtime_get());

        // Check if bird camera needs spec command executed
        int specTargetIdx = advancedfx::overlay::BirdCamera_GetPendingSpecTarget();
        if (specTargetIdx >= 0) {
            // Get player name for spec command
            if (g_pEntityList && *g_pEntityList && g_GetEntityFromIndex) {
                CEntityInstance* controller = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, specTargetIdx);
                if (controller && SafeIsPlayerController(controller)) {
                    const char* name = SafeGetSanitizedPlayerName(controller);
                    if (!name || !*name) name = SafeGetPlayerName(controller);

                    if (name && *name) {
                        char cmd[512];
                        // Quote name if it contains spaces
                        bool hasSpace = strchr(name, ' ') != nullptr;
                        Afx_ExecClientCmd("spec_mode 2");
                        if (hasSpace) {
                            _snprintf_s(cmd, _TRUNCATE, "spec_player \"%s\"", name);
                        } else {
                            _snprintf_s(cmd, _TRUNCATE, "spec_player %s", name);
                        }
                        Afx_ExecClientCmd(cmd);
                        advancedfx::Message("BirdCamera: Executing spec_player %s (idx=%d)\n", name, specTargetIdx);
                    }
                }
            }
        }

        if (g_DimGameWhileViewport && g_ShowBackbufferWindow) {
            ImVec2 ds = ImGui::GetIO().DisplaySize;
            const int a = (int)(g_DimOpacity * 255.0f + 0.5f);
            // Slightly grey, not pure black, so the UI still feels readable.
            ImGui::GetBackgroundDrawList()->AddRectFilled(
                ImVec2(0.0f, 0.0f), ds, IM_COL32(32, 32, 32, a)
            );
        }
    }

    if (g_ShowRadar) {
        // Radar in its own ImGui window instead of drawing on the background
        ImGui::SetNextWindowSize(ImVec2(360, 360), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Radar", &g_ShowRadar, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar)) {
            ImGui::End();
            // Even if closed this frame, still proceed with asset housekeeping below in next frames
        } else {
        // Auto-load radar map from GSI data
        {
            auto mapOpt = g_GsiServer.TryGetMapName();
            if (mapOpt.has_value()) {
                const std::string& gsiMapName = *mapOpt;
                // Check if map has changed
                if (_stricmp(gsiMapName.c_str(), g_RadarMapName) != 0) {
                    strncpy_s(g_RadarMapName, gsiMapName.c_str(), _TRUNCATE);

                    // Load config for new map
                    bool okCfg = false;
                    auto it = g_RadarAllConfigs.find(g_RadarMapName);
                    if (it != g_RadarAllConfigs.end()) { g_RadarCtx.cfg = it->second; okCfg = true; }

                    if (!okCfg) { g_RadarCtx.cfg.pos_x = 0.0; g_RadarCtx.cfg.pos_y = 0.0; g_RadarCtx.cfg.scale = 1.0; }

                    // Load texture
                    if (g_RadarCtx.srv) { g_RadarCtx.srv->Release(); g_RadarCtx.srv = nullptr; }
                    g_RadarCtx.texW = g_RadarCtx.texH = 0;
                    bool texOk = false;
                    if (!g_RadarCtx.cfg.imagePath.empty()) {
                        std::wstring wpath = Mk_Utf8ToWide(g_RadarCtx.cfg.imagePath);
                        texOk = Radar::LoadTextureWIC(m_Device, wpath, &g_RadarCtx.srv, &g_RadarCtx.texW, &g_RadarCtx.texH);
                    }
                    if (!texOk) {
                        const wchar_t* hf = GetHlaeFolderW();
                        if (hf && *hf) {
                            std::wstring base = std::wstring(hf) + L"resources\\overlay\\radar\\ingame\\";
                            std::wstring wmap; wmap.assign(g_RadarMapName, g_RadarMapName + strlen(g_RadarMapName));
                            std::wstring p1 = base + wmap + L".png";
                            DWORD a1 = GetFileAttributesW(p1.c_str());
                            if (a1 != INVALID_FILE_ATTRIBUTES && !(a1 & FILE_ATTRIBUTE_DIRECTORY)) {
                                texOk = Radar::LoadTextureWIC(m_Device, p1, &g_RadarCtx.srv, &g_RadarCtx.texW, &g_RadarCtx.texH);
                            } else {
                                std::wstring p2 = base + wmap + L"_lower.png";
                                DWORD a2 = GetFileAttributesW(p2.c_str());
                                if (a2 != INVALID_FILE_ATTRIBUTES && !(a2 & FILE_ATTRIBUTE_DIRECTORY)) {
                                    texOk = Radar::LoadTextureWIC(m_Device, p2, &g_RadarCtx.srv, &g_RadarCtx.texW, &g_RadarCtx.texH);
                                }
                            }
                        }
                    }
                    g_RadarAssetsLoaded = texOk;
                }
            }
        }

        // Radar canvas inside the window (square that fits available content)
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float side = (std::min)(avail.x, avail.y);
        if (side < 64.0f) side = 64.0f;
        ImVec2 canvasMin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##radar_canvas", ImVec2(side, side), ImGuiButtonFlags_AllowOverlap);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 radarPos = canvasMin;
        float radarWidth = side;
        float radarHeight = side;

        // If assets are loaded and we have a valid config, render it
        if (g_RadarAssetsLoaded && (g_RadarCtx.cfg.scale != 0.0)) {
            struct RadarPlayer { Radar::Entity ent; int team = 0; int slot = -1; };
            std::vector<RadarPlayer> players;

            // Populate from CS2 GSI via embedded HTTP server
            {
                auto opt = g_GsiServer.TryGetRadarPlayers();
                if (opt.has_value()) {
                    for (const auto &rp : *opt) {
                        if (!rp.alive) continue; // mimic pawn presence behavior
                        Radar::Entity e;
                        e.id = rp.id;
                        // Pre-smooth to keep label and marker in sync.
                        {
                            Radar::Vec3 pRaw { rp.pos[0], rp.pos[1], rp.pos[2] };
                            auto &s = g_RadarCtx.smooth[e.id];
                            s.push(pRaw);
                            Radar::Vec3 pAvg = s.avg();
                            e.pos = { pAvg.x, pAvg.y, pAvg.z };
                            e.smooth = false; // already smoothed
                        }
                        e.fwd = { rp.fwd[0], rp.fwd[1] };
                        // Team-based coloring (2=T, 3=CT)
                        ImU32 col = IM_COL32(200, 200, 200, 255);
                        if (rp.teamSide == 2) col = IM_COL32(255, 120, 80, 255);
                        else if (rp.teamSide == 3) col = IM_COL32(90, 160, 255, 255);
                        if (rp.hasBomb) col = IM_COL32(220, 50, 50, 255);
                        e.color = col;
                        players.push_back({ std::move(e), rp.teamSide, rp.observerSlot });
                    }
                }
            }

            // Draw radar background first (no entities, no border)
            {
                std::vector<Radar::Entity> none;
                Radar::Render(drawList, radarPos, ImVec2(radarWidth, radarHeight), g_RadarCtx, none, 0.0f, false, true);
            }

            // Grenade overlays (smoke + inferno) between background and players
            {
                auto optg = g_GsiServer.TryGetRadarGrenades();
                uint64_t currentHeartbeat = g_GsiServer.GetHeartbeat();

                if (optg.has_value()) {
                    // Constants from cs-hud (fennec theme): sizes are relative to 1024
                    const float uvSmokeRadius = (28.8f / 1024.0f); // 57.6 diameter
                    const float uvInfernoRadius = (7.0f / 1024.0f); // 14 diameter per flame
                    const float positionThreshold = 1.0f; // Distance threshold to consider smoke stationary
                    const int stationaryUpdatesRequired = 2; // GSI updates smoke must be stationary to be considered detonated

                    auto colorFor = [](int side, bool smoke)->ImU32{
                        if (smoke) {
                            return side==3 ? IM_COL32(99,134,150,190) : (side==2 ? IM_COL32(119,115,78,190) : IM_COL32(110,110,110,180));
                        } else {
                            return side==3 ? IM_COL32(142,79,111,190) : (side==2 ? IM_COL32(193,68,2,190) : IM_COL32(160,80,20,190));
                        }
                    };

                    // Update smoke trackers with current smoke positions (only on GSI heartbeat change)
                    static uint64_t lastProcessedHeartbeat = 0;
                    if (currentHeartbeat != lastProcessedHeartbeat) {
                        lastProcessedHeartbeat = currentHeartbeat;

                        std::set<int> currentSmokeKeys;
                        for (const auto &g : *optg) {
                            if (g.type == GsiHttpServer::RadarGrenade::Smoke) {
                                // Create a simple hash key from position (rounded to avoid floating point issues)
                                int key = (int)(g.pos[0] / 10.0f) * 1000000 + (int)(g.pos[1] / 10.0f) * 1000 + (int)(g.pos[2] / 10.0f);
                                currentSmokeKeys.insert(key);

                                auto it = g_SmokeTrackers.find(key);
                                if (it == g_SmokeTrackers.end()) {
                                    // New smoke - add to tracker
                                    SmokeTracker tracker;
                                    tracker.pos[0] = g.pos[0]; tracker.pos[1] = g.pos[1]; tracker.pos[2] = g.pos[2];
                                    tracker.lastPos[0] = g.pos[0]; tracker.lastPos[1] = g.pos[1]; tracker.lastPos[2] = g.pos[2];
                                    tracker.ownerSide = g.ownerSide;
                                    tracker.isDetonated = false;
                                    tracker.lastHeartbeat = currentHeartbeat;
                                    tracker.stationaryUpdates = 0;
                                    tracker.detonationTime = 0.0;
                                    g_SmokeTrackers[key] = tracker;
                                } else {
                                    // Update existing smoke on new GSI update
                                    SmokeTracker &tracker = it->second;

                                    // Calculate distance moved since last GSI update
                                    float dx = g.pos[0] - tracker.pos[0];
                                    float dy = g.pos[1] - tracker.pos[1];
                                    float dz = g.pos[2] - tracker.pos[2];
                                    float distMoved = sqrtf(dx*dx + dy*dy + dz*dz);

                                    if (distMoved < positionThreshold) {
                                        tracker.stationaryUpdates++;
                                        if (tracker.stationaryUpdates >= stationaryUpdatesRequired && !tracker.isDetonated) {
                                            tracker.isDetonated = true;
                                            tracker.detonationTime = ImGui::GetTime();
                                        }
                                    } else {
                                        tracker.stationaryUpdates = 0;
                                        tracker.isDetonated = false;
                                    }

                                    tracker.lastPos[0] = tracker.pos[0]; tracker.lastPos[1] = tracker.pos[1]; tracker.lastPos[2] = tracker.pos[2];
                                    tracker.pos[0] = g.pos[0]; tracker.pos[1] = g.pos[1]; tracker.pos[2] = g.pos[2];
                                    tracker.ownerSide = g.ownerSide;
                                    tracker.lastHeartbeat = currentHeartbeat;
                                }
                            }
                        }

                        // Remove old smokes that are no longer in GSI data
                        for (auto it = g_SmokeTrackers.begin(); it != g_SmokeTrackers.end(); ) {
                            if (currentSmokeKeys.find(it->first) == currentSmokeKeys.end()) {
                                it = g_SmokeTrackers.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    }

                    // Draw grenades (projectiles and detonated)
                    double currentTime = ImGui::GetTime();
                    const float grenadeIconHeight = (16.0f / 1024.0f) * radarWidth; // Icon height for projectiles

                    // Helper lambda to draw grenade icon
                    auto drawGrenadeIcon = [&](const char* iconName, ImVec2 center, float height) {
                        GrenadeIconTex icon = GetGrenadeIcon(m_Device, iconName);
                        if (icon.srv && icon.h > 0) {
                            float w = height * (float)icon.w / (float)icon.h;
                            ImVec2 tl = ImVec2(center.x - w * 0.5f, center.y - height * 0.5f);
                            ImVec2 br = ImVec2(tl.x + w, tl.y + height);
                            drawList->AddImage((ImTextureID)icon.srv, tl, br, ImVec2(0,0), ImVec2(1,1), IM_COL32(255,255,255,255));
                        }
                    };

                    for (const auto &g : *optg) {
                        Radar::Vec2 uv = Radar::WorldToUV({ g.pos[0], g.pos[1], g.pos[2] }, g_RadarCtx.cfg);
                        ImVec2 center = ImVec2(radarPos.x + uv.x * radarWidth, radarPos.y + uv.y * radarHeight);

                        if (g.type == GsiHttpServer::RadarGrenade::Smoke) {
                            int key = (int)(g.pos[0] / 10.0f) * 1000000 + (int)(g.pos[1] / 10.0f) * 1000 + (int)(g.pos[2] / 10.0f);
                            auto it = g_SmokeTrackers.find(key);

                            if (it != g_SmokeTrackers.end() && it->second.isDetonated) {
                                // Draw detonated smoke plume
                                float r = uvSmokeRadius * radarWidth;
                                ImU32 col = colorFor(g.ownerSide, true);
                                drawList->AddCircleFilled(center, r, col, 20);

                                // Calculate and draw countdown text
                                double elapsed = currentTime - it->second.detonationTime;
                                float timeRemaining = 20.0f - (float)elapsed;
                                if (timeRemaining < 0.0f) timeRemaining = 0.0f;

                                int timeLeft = (int)ceilf(timeRemaining);
                                if (timeLeft < 0) timeLeft = 0;
                                char timeText[8];
                                snprintf(timeText, sizeof(timeText), "%d", timeLeft);

                                ImVec2 textSize = ImGui::CalcTextSize(timeText);
                                ImVec2 textPos = ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f);

                                // Draw text with outline for visibility
                                drawList->AddText(ImVec2(textPos.x + 1, textPos.y + 1), IM_COL32(0, 0, 0, 255), timeText);
                                drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), timeText);
                            } else {
                                // Draw smoke projectile icon (before detonation)
                                drawGrenadeIcon("smokegrenade", center, grenadeIconHeight);
                            }
                        } else if (g.type == GsiHttpServer::RadarGrenade::Inferno) {
                            // Draw inferno flames
                            float r = uvInfernoRadius * radarWidth;
                            ImU32 col = colorFor(g.ownerSide, false);
                            drawList->AddCircleFilled(center, r, col, 20);
                        } else if (g.type == GsiHttpServer::RadarGrenade::Decoy) {
                            // Draw decoy projectile icon
                            drawGrenadeIcon("tagrenade", center, grenadeIconHeight);
                        } else if (g.type == GsiHttpServer::RadarGrenade::Molotov) {
                            // Draw molotov/firebomb projectile icon
                            drawGrenadeIcon("molotov", center, grenadeIconHeight);
                        } else if (g.type == GsiHttpServer::RadarGrenade::Flashbang) {
                            // Draw flashbang projectile icon
                            drawGrenadeIcon("flashbang", center, grenadeIconHeight);
                        } else if (g.type == GsiHttpServer::RadarGrenade::Frag) {
                            // Draw frag/HE grenade projectile icon
                            drawGrenadeIcon("frag_grenade", center, grenadeIconHeight);
                        }
                    }
                }
            }

            // Bomb marker (if dropped or planted and has position)
            {
                auto optb = g_GsiServer.TryGetRadarBomb();
                if (optb.has_value()) {
                    const auto &b = *optb;
                    if (b.hasPosition && !b.state.empty() && _stricmp(b.state.c_str(), "carried") != 0) {
                        Radar::Vec2 uv = Radar::WorldToUV({ b.pos[0], b.pos[1], b.pos[2] }, g_RadarCtx.cfg);
                        ImVec2 center = ImVec2(radarPos.x + uv.x * radarWidth, radarPos.y + uv.y * radarHeight);
                        float rpx = (0.012f * radarWidth); if (rpx < 4.0f) rpx = 4.0f; if (rpx > 18.0f) rpx = 18.0f;
                        ImU32 col = IM_COL32(255, 255, 255, 230);
                        if (!_stricmp(b.state.c_str(), "planted")) col = IM_COL32(255, 220, 50, 230);
                        else if (!_stricmp(b.state.c_str(), "defusing")) col = IM_COL32(120, 200, 255, 230);
                        else if (!_stricmp(b.state.c_str(), "defused")) col = IM_COL32(90, 210, 120, 230);
                        else if (!_stricmp(b.state.c_str(), "exploded")) col = IM_COL32(255, 90, 30, 230);
                        // diamond
                        ImVec2 p0(center.x, center.y - rpx);
                        ImVec2 p1(center.x + rpx, center.y);
                        ImVec2 p2(center.x, center.y + rpx);
                        ImVec2 p3(center.x - rpx, center.y);
                        drawList->AddQuadFilled(p0, p1, p2, p3, col);
                        drawList->AddQuad(p0, p1, p2, p3, IM_COL32(0,0,0,220), 1.5f);
                    }
                }
            }

            // Prepare entities for draw
            std::vector<Radar::Entity> entities; entities.reserve(players.size());
            for (auto &p : players) entities.emplace_back(std::move(p.ent));

            // Scale markers with radar size (base = 300px => radius 7px)
            float scale = radarWidth > 1.0f ? (radarWidth / 300.0f) : 1.0f;
            if (scale < 0.25f) scale = 0.25f; if (scale > 5.0f) scale = 5.0f;
            float markerRadius = 7.0f * scale * g_RadarDotScale;
            // Draw only entities and border (background already drawn)
            Radar::Render(drawList, radarPos, ImVec2(radarWidth, radarHeight), g_RadarCtx, entities, markerRadius, false, false);

            // Focus ring for spectated player (above markers, below labels)
            {
                auto hopt = g_GsiServer.TryGetHudState();
                if (hopt.has_value() && hopt->focusedPlayerId != -1) {
                    int focusedId = hopt->focusedPlayerId;
                    for (const auto &p : players) {
                        if (p.ent.id != focusedId) continue;
                        Radar::Vec2 uv = Radar::WorldToUV({ p.ent.pos.x, p.ent.pos.y, p.ent.pos.z }, g_RadarCtx.cfg);
                        ImVec2 pt = ImVec2(radarPos.x + uv.x * radarWidth, radarPos.y + uv.y * radarHeight);
                        float thick = (std::max)(1.5f, 1.5f * scale);
                        drawList->AddCircle(pt, markerRadius + thick*0.75f, IM_COL32(255,255,255,255), 0, thick);
                        break;
                    }
                }
            }

            // Assign labels to match HUD sidebar ordering (1-5 left, 6-9,0 right)
            // This keeps labels stable and matching the visual HUD layout
            std::unordered_map<int, char> reassignedLabels; // entity id -> label digit
            {
                auto hopt = g_GsiServer.TryGetHudState();
                if (hopt.has_value()) {
                    const auto& hudState = *hopt;
                    // Left sidebar gets 1-5, right sidebar gets 6-9,0
                    const char rightDigits[5] = {'6','7','8','9','0'};

                    // Assign labels based on position in HUD lists (uses observerSlot as stable key)
                    for (size_t i = 0; i < hudState.leftPlayers.size() && i < 5; ++i) {
                        const auto& hp = hudState.leftPlayers[i];
                        if (hp.observerSlot >= 0) {
                            char digit = Radar_GetOrAssignDigitForController(hp.observerSlot, hp.teamSide);
                            // Override to ensure correct order (1-5 for left in HUD order)
                            digit = (char)('1' + i);
                            g_RadarLabelByController[hp.observerSlot] = digit;
                            g_RadarLabelTeam[hp.observerSlot] = hp.teamSide;
                            reassignedLabels[hp.id] = digit;
                        }
                    }
                    for (size_t i = 0; i < hudState.rightPlayers.size() && i < 5; ++i) {
                        const auto& hp = hudState.rightPlayers[i];
                        if (hp.observerSlot >= 0) {
                            char digit = rightDigits[i];
                            g_RadarLabelByController[hp.observerSlot] = digit;
                            g_RadarLabelTeam[hp.observerSlot] = hp.teamSide;
                            reassignedLabels[hp.id] = digit;
                        }
                    }
                }
            }

            // Draw labels centered on markers
            for (const auto &p : players) {
                auto it = reassignedLabels.find(p.ent.id);
                if (it == reassignedLabels.end()) continue;
                char d = it->second;
                if (!d) continue;
                char txt[2] = { d, '\0' };


                // Position on radar
                Radar::Vec2 uv = Radar::WorldToUV({ p.ent.pos.x, p.ent.pos.y, p.ent.pos.z }, g_RadarCtx.cfg);
                ImVec2 pt = ImVec2(radarPos.x + uv.x * radarWidth, radarPos.y + uv.y * radarHeight);

                // Scale label font size with radar size
                ImFont* font = ImGui::GetFont();
                if (!font) font = ImGui::GetIO().FontDefault;
                float baseSize = ImGui::GetFontSize();
                float fontSize = baseSize * scale *g_RadarDotScale;
                if (fontSize < 8.0f) fontSize = 8.0f; if (fontSize > 60.0f) fontSize = 60.0f;

                // Text size and centered position
                ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, txt);
                ImVec2 p0 = ImVec2(pt.x - ts.x * 0.5f, pt.y - ts.y * 0.5f);

                // Outline + foreground for readability (outline size scales a bit too)
                float o = std::roundf((std::max)(1.0f, scale));
                drawList->AddText(font, fontSize, ImVec2(p0.x + o, p0.y + o), IM_COL32(0,0,0,220), txt);
                drawList->AddText(font, fontSize, p0, IM_COL32(255,255,255,255), txt);
            }

            // Handle Ctrl+Left-click on radar player dots (check after all drawing is done)
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsKeyDown(ImGuiKey_LeftCtrl)) {
                ImVec2 mousePos = ImGui::GetMousePos();
                float hitRadius = markerRadius + 4.0f; // Slightly larger than marker for easier clicking

                for (const auto &p : players) {
                    // Position on radar
                    Radar::Vec2 uv = Radar::WorldToUV({ p.ent.pos.x, p.ent.pos.y, p.ent.pos.z }, g_RadarCtx.cfg);
                    ImVec2 pt = ImVec2(radarPos.x + uv.x * radarWidth, radarPos.y + uv.y * radarHeight);

                    // Check if mouse is within circular click region
                    float dx = mousePos.x - pt.x;
                    float dy = mousePos.y - pt.y;
                    float distSq = dx * dx + dy * dy;

                    if (distSq <= hitRadius * hitRadius) {
                        // Ctrl+Left-click detected on this player - trigger GetSmooth
                        float pos[3] = {0, 0, 0};

                        // Find controller index from observerSlot
                        int clickIdx = -1;
                        if (p.slot >= 0) {
                            // Map observerSlot (0-9) to keyboard format (1-9,0)
                            int keyNum = (p.slot == 9) ? 0 : (p.slot + 1);
                            auto bindIt = g_ObservingHotkeyBindings.find(keyNum);
                            if (bindIt != g_ObservingHotkeyBindings.end()) {
                                clickIdx = bindIt->second;
                            }
                        }

                        if (clickIdx >= 0) {
                            g_GetSmoothIndex = clickIdx;
                            if (g_pEntityList && *g_pEntityList && g_GetEntityFromIndex) {
                                CEntityInstance* controller = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, clickIdx);
                                if (controller && controller->IsPlayerController()) {
                                    auto pawnHandle = controller->GetPlayerPawnHandle();
                                    if (pawnHandle.IsValid()) {
                                        int pawnIdx = pawnHandle.GetEntryIndex();
                                        CEntityInstance* pawn = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, pawnIdx);
                                        if (pawn && pawn->IsPlayerPawn()) {
                                            // Store first position for velocity calculation
                                            pawn->GetRenderEyeOrigin(pos);
                                            g_GetSmoothFirstPos[0] = pos[0];
                                            g_GetSmoothFirstPos[1] = pos[1];
                                            g_GetSmoothFirstPos[2] = pos[2];

                                            // Mark for next-frame capture
                                            g_GetSmoothPass = true;
                                            g_GetSmoothFirstFrame = true;
                                            g_GetSmoothTime = ImGui::GetTime();
                                        }
                                    }
                                }
                            }
                        }
                        break; // Only process one click
                    }
                }
            }
        } else {
            // Placeholder box when assets not yet loaded
            ImVec2 radarEnd = ImVec2(radarPos.x + radarWidth, radarPos.y + radarHeight);
            drawList->AddRectFilled(radarPos, radarEnd, IM_COL32(50, 50, 50, 128));
            drawList->AddRect(radarPos, radarEnd, IM_COL32(255, 255, 255, 255));
            const char* text = "Radar not loaded";
            ImVec2 textSize = ImGui::CalcTextSize(text);
            drawList->AddText(ImVec2(radarPos.x + (radarWidth - textSize.x) / 2, radarPos.y + (radarHeight - textSize.y) / 2), IM_COL32(255, 255, 255, 255), text);
        }
        ImGui::End();
        }
    }

    UpdateBackbufferPreviewTexture();
    // Reset preview-rect tracking for this frame; will be set when preview window renders
    g_PreviewRectValid = false;


    // Diagnostic watermark always (when overlay visible)
    ImGui::GetForegroundDrawList()->AddText(ImVec2(8,8), IM_COL32(255,255,255,255), "HLAE Overlay - Press F8 to toggle", nullptr);

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
                    if (g_GroupIntoWorkspace && g_WorkspaceViewportId)
                        ImGui::SetNextWindowViewport(g_WorkspaceViewportId);
                    s_campathOpenDialog.Display();
                    if (g_GroupIntoWorkspace && g_WorkspaceViewportId)
                        ImGui::SetNextWindowViewport(g_WorkspaceViewportId);
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
            ImGui::SameLine();
            ImGui::Checkbox("Camera Control", &g_ShowCameraControl);
            ImGui::SameLine();
            ImGui::Checkbox("Show Gizmo", &g_ShowGizmo);
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
                if (g_GroupIntoWorkspace && g_WorkspaceViewportId)
                    ImGui::SetNextWindowViewport(g_WorkspaceViewportId);
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
                bool screenEnabled = AfxStreams_GetRecordScreenEnabled();
                if (ImGui::Checkbox("Record screen enabled", &screenEnabled)) { AfxStreams_SetRecordScreenEnabled(screenEnabled); }
                ImGui::SameLine();
                static bool s_exportCam = false;
                if (ImGui::Checkbox("Export Cam", &s_exportCam)) {
                    if (s_exportCam) Afx_ExecClientCmd("mirv_streams record cam enabled 1"); else Afx_ExecClientCmd("mirv_streams record cam enabled 0");
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
            ImGui::SameLine();
            static bool s_fixAnims = false;
            if (ImGui::Checkbox("Fix Anims", &s_fixAnims)) {
                if (s_fixAnims) Afx_ExecClientCmd("mirv_fix animations 1"); else Afx_ExecClientCmd("mirv_fix animations 0");
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
        //Misc
        if (ImGui::BeginTabItem("Misc")) {
            if (ImGui::Checkbox("Show DOF Control", &g_ShowDofWindow)) {
                if (!g_cvarsUnhidden){
                    Afx_ExecClientCmd("mirv_cvar_unhide_all");
                    g_cvarsUnhidden = true;
                }
            }
            if (ImGui::Checkbox("Show Attachment Control", &g_ShowAttachmentControl)) {
            }
            ImGui::Checkbox("Show Event Browser", &g_ShowMultikillWindow);
            static bool s_nearZtoggle = false;
            if (ImGui::Checkbox("Near-Z 1", &s_nearZtoggle)) {
                if (s_nearZtoggle) Afx_ExecClientCmd("r_nearz 1"); else Afx_ExecClientCmd("r_nearz -1");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Reduces Near Clipping to the lowest value.");
            }
            static bool s_noVisToggle = false;
            if (ImGui::Checkbox("No Vis", &s_noVisToggle)) {
                if (s_noVisToggle) {
                    if (!g_cvarsUnhidden) {
                        Afx_ExecClientCmd("mirv_cvar_unhide_all");
                        g_cvarsUnhidden = true;
                    }
                    Afx_ExecClientCmd("sc_no_vis 1"); 
                } else Afx_ExecClientCmd("sc_no_vis 0");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Sets sc_no_vis 1, removes culling when out of bounds.");
            }
            ImGui::EndTabItem();
        }
        // Observing
        if (ImGui::BeginTabItem("Observing")) {
            ImGui::Checkbox("Enable Observing mode", &g_ObservingEnabled);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Enable spectating features and hotkey bindings.");
            }

            if (ImGui::Checkbox("Show Bindings", &g_ShowObservingBindings)) {
                // Window toggle
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Open window to bind 0-9 hotkeys to players.");
            }

            if (ImGui::Checkbox("Show Cameras", &g_ShowObservingCameras)) {
                // Window toggle
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Open window to manage campath profiles and cameras.");
            }

            ImGui::Separator();
            ImGui::Checkbox("Show viewport HUD", &g_ShowHud);
            ImGui::Checkbox("Show Radar", &g_ShowRadar);
            ImGui::SameLine();
            if (ImGui::SmallButton("Settings")) {
                g_ShowRadarSettings = !g_ShowRadarSettings;
            }
            // Load map list from built-in radars.json if not yet loaded
            if (!g_RadarMapsLoaded) {
                const wchar_t* hf = GetHlaeFolderW();
                if (hf && *hf) {
                    std::wstring wJson = std::wstring(hf) + L"resources\\overlay\\radar\\radars.json";
                    DWORD attr = GetFileAttributesW(wJson.c_str());
                    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                        int need = WideCharToMultiByte(CP_UTF8, 0, wJson.c_str(), -1, nullptr, 0, nullptr, nullptr);
                        std::string jsonUtf8; if (need > 0) { jsonUtf8.resize((size_t)need - 1); WideCharToMultiByte(CP_UTF8, 0, wJson.c_str(), -1, &jsonUtf8[0], need, nullptr, nullptr);}            
                        std::wstring wImgBase = std::wstring(hf) + L"resources\\overlay\\radar\\ingame";
                        need = WideCharToMultiByte(CP_UTF8, 0, wImgBase.c_str(), -1, nullptr, 0, nullptr, nullptr);
                        std::string imgBase; if (need > 0) { imgBase.resize((size_t)need - 1); WideCharToMultiByte(CP_UTF8, 0, wImgBase.c_str(), -1, &imgBase[0], need, nullptr, nullptr);}            
                        g_RadarAllConfigs.clear(); g_RadarMapList.clear();
                        if (Radar::LoadRadarsJson(jsonUtf8, g_RadarAllConfigs, imgBase)) {
                            for (const auto& kv : g_RadarAllConfigs) g_RadarMapList.push_back(kv.first);
                            std::sort(g_RadarMapList.begin(), g_RadarMapList.end());
                            g_RadarMapsLoaded = true;
                        }
                    }
                }
            }
            // Map is now loaded automatically from GSI data
            ImGui::TextUnformatted("Map:");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", g_RadarMapName[0] ? g_RadarMapName : "(waiting for GSI...)");
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset Numbers")) {
                // Clear persistent controller->digit mapping (useful at half-time)
                g_RadarLabelByController.clear();
                g_RadarLabelTeam.clear();
                advancedfx::Message("Overlay: Reset radar number mapping (half-time)\n");
            }

            ImGui::Separator();

            ImGui::Text("Game State Integration server:");
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputInt("Port", &g_GsiPort);
            ImGui::SameLine();
            if (ImGui::SmallButton(g_GsiServer.IsRunning() ? "Restart" : "Start")) {
                g_GsiServer.Stop(); g_GsiServer.Start(g_GsiPort);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop")) {
                g_GsiServer.Stop();
            }
            ImGui::Text("GSI: %s", g_GsiServer.IsRunning() ? "listening" : "stopped");

            ImGui::Separator();
            ImGui::TextUnformatted("Coach filter:");
            ImGui::SetNextItemWidth(300.0f);
            if (ImGui::InputText("##FilteredPlayers", g_FilteredPlayers, sizeof(g_FilteredPlayers))) {
                // Input changed - update the filter in GSI server
                g_GsiServer.SetFilteredPlayers(std::string(g_FilteredPlayers));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Comma-separated player names to hide from HUD/radar (e.g., \"Coach1, Coach2\")");
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

            // Toggle overlay console + Open demo button (same line)
            ImGui::Checkbox("Show Console", &g_ShowOverlayConsole);
            ImGui::SameLine();
            {
                if (!g_DemoDialogInit) { g_DemoOpenDialog.SetTitle("Open demo"); g_DemoDialogInit = true; }

                if (ImGui::Button("Open demo")) {
                    // Prefer last-used demo directory, otherwise default to game\csgo
                    if (!g_OverlayPaths.demoDir.empty()) {
                        g_DemoOpenDialog.SetDirectory(g_OverlayPaths.demoDir);
                    } else {
                        const char* gameRoot = GetProcessFolder(); // typically ends with ...\\game\\
                        if (gameRoot && gameRoot[0]) {
                            std::string def = std::string(gameRoot);
                            // Default to ...\\game\\csgo if present
                            def += "csgo";
                            g_DemoOpenDialog.SetDirectory(def);
                        }
                    
                    g_DemoOpenDialog.Open();
                }

                // Render and handle selection
                if (g_GroupIntoWorkspace && g_WorkspaceViewportId)
                    ImGui::SetNextWindowViewport(g_WorkspaceViewportId);
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

            ImGui::NewLine();
            if (ImGui::Checkbox("Show Viewport", &g_ShowBackbufferWindow)) {
                // Toggle handled via g_ShowBackbufferWindow.
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Displays a copy of the game's backbuffer inside the overlay.");
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("00.00x").x + ImGui::GetStyle().FramePadding.x * 6.0f);
            if (ImGui::SliderFloat("Look sens", &g_PreviewLookMultiplier, 0.1f, 5.0f, "%.2fx")) {
                // no-op, used at runtime to scale the baseline look sensitivity
            }
            ImGui::SameLine();
            static bool s_noFlash = false;
            if (ImGui::Checkbox("No Flash", &s_noFlash)) {
                if (s_noFlash) Afx_ExecClientCmd("mirv_noflash 1"); else Afx_ExecClientCmd("mirv_noflash 0");
            }
            // Grey-matte toggle + strength
            if (ImGui::Checkbox("Dim Game", &g_DimGameWhileViewport)) {
                // nothing else to do
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("100%").x + ImGui::GetStyle().FramePadding.x * 6.0f);
            float pct = g_DimOpacity * 100.0f;
            if (ImGui::SliderFloat("Dim strength", &pct, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
                g_DimOpacity = pct / 100.0f;
            }
            // Layout section: allow grouping all overlay windows inside a workspace window (with DockSpace)
            {
                ImGui::BeginDisabled(g_GroupIntoWorkspace);
                bool prev = g_GroupIntoWorkspace;
                if (ImGui::Checkbox("External workspace mode", &g_GroupIntoWorkspace)) {
                    if (g_GroupIntoWorkspace && !prev) {
                        // Save current .ini before switching
                        ImGui::SaveIniSettingsToDisk(g_IniFileNormal.c_str());

                        // Switch to workspace .ini
                        ImGuiIO& io = ImGui::GetIO();
                        io.IniFilename = g_IniFileWorkspace.c_str();
                        ImGui::LoadIniSettingsFromDisk(g_IniFileWorkspace.c_str());

                        g_WorkspaceNeedsLayout = true; // build layout next frame
                        g_WorkspaceOpen = true;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Requires restart to disable");
                }
                ImGui::EndDisabled();
                if (g_GroupIntoWorkspace) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Redock all")) {
                        g_WorkspaceForceRedock = true;
                    }
                }
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();

    if (g_ShowRadarSettings) {
        ImGui::Begin("Radar Settings", &g_ShowRadarSettings);
        {
            // Position and size controls
            ImGui::TextUnformatted("Radar Placement");
            ImGui::PushID("radar_placement");
            ImGui::SliderFloat("Pos X", &g_RadarUiPosX, 0.0f, ImGui::GetIO().DisplaySize.x, "%.0f");
            ImGui::SliderFloat("Pos Y", &g_RadarUiPosY, 0.0f, ImGui::GetIO().DisplaySize.y, "%.0f");
            ImGui::SliderFloat("Size", &g_RadarUiSize, 100.0f, 1600.0f, "%.0f px");
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##radar_ui")) { g_RadarUiPosX = 50.0f; g_RadarUiPosY = 50.0f; g_RadarUiSize = 300.0f; }
            ImGui::SliderFloat("Dotscale", &g_RadarDotScale, 0.1f, 2.0f, "%.1f px");
            ImGui::PopID();
        }
        ImGui::End();
    }

    if (g_ShowAttachmentControl) {
        ImGui::Begin("Attachment Control");
        {
            // Enable/Disable button
            const char* btnLbl = g_AttachCamEnabled ? "Detach Camera" : "Attach Camera";
            if (ImGui::Button(btnLbl)) {
                if (!g_AttachCamEnabled) {
                    // Enable: mark attach active only if an entity is selected
                    g_AttachCamEnabled = (g_AttachSelectedHandle > 0);
                    advancedfx::overlay::AttachCam_SetEnabled(g_AttachCamEnabled);
                    advancedfx::overlay::AttachCam_SetEntityHandle(g_AttachSelectedHandle);
                    advancedfx::overlay::AttachCam_SetAttachmentIndex(g_AttachSelectedAttachmentIdx);
                    advancedfx::overlay::AttachCam_SetOffsetPos(g_AttachOffsetPos);
                    advancedfx::overlay::AttachCam_SetOffsetRot(g_AttachOffsetRot);
                    advancedfx::Message("Overlay: attach camera %s\n", g_AttachCamEnabled ? "on" : "off (no entity selected)");
                } else {
                    // Disable: just clear the attach flag
                    g_AttachCamEnabled = false;
                    advancedfx::overlay::AttachCam_SetEnabled(false);
                    advancedfx::Message("Overlay: attach camera off\n");
                }
            }
            // Entity selection (players and weapons)
            ImGui::SeparatorText("Entity");
            std::string curEntLabel = "<none>";
            if (g_AttachSelectedHandle > 0) {
                // Try to find current name from cache
                for (const auto& e : g_AttachEntityCache) {
                    if (e.handle == g_AttachSelectedHandle) { curEntLabel = e.name; break; }
                }
            }
            static bool entityRefreshed = false;
            if (ImGui::BeginCombo("##attach_entity", curEntLabel.c_str())) {
                if (!entityRefreshed){
                    UpdateAttachEntityCache();
                    entityRefreshed = true;
                }
                for (const auto& e : g_AttachEntityCache) {
                    char label[256];
                    _snprintf_s(label, _TRUNCATE, "%s %s (idx %d)", e.isPawn ? "[Player]" : (e.isWeapon ? "[Weapon]" : "[Other]"), e.name.c_str(), e.index);
                    bool selected = (g_AttachSelectedHandle == e.handle);
                    if (ImGui::Selectable(label, selected)) {
                        g_AttachSelectedHandle = e.handle;
                        g_AttachSelectedIndex = e.index;
                        // Reset attachment index guess when entity changes
                        g_AttachSelectedAttachmentIdx = 1;
                        advancedfx::overlay::AttachCam_SetEntityHandle(g_AttachSelectedHandle);
                        advancedfx::overlay::AttachCam_SetAttachmentIndex(g_AttachSelectedAttachmentIdx);
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            } else {entityRefreshed = false;}

            // Attachment selection: probe name heuristics then fill with remaining indices
            // build when selection changes, when combo opens, or on refresh
            ImGui::SeparatorText("Attachment");

            const bool needBuild = (!g_AttachCacheValid || g_AttachCacheHandle != g_AttachSelectedHandle);
            // Current selection label
            char curBuf[128];
            {
                const auto it = g_AttachIdxToName.find(g_AttachSelectedAttachmentIdx);
                if (it != g_AttachIdxToName.end() && !it->second.empty()) {
                    _snprintf_s(curBuf, _TRUNCATE, "%d - %s", g_AttachSelectedAttachmentIdx, it->second.c_str());
                } else if (!g_AttachItems.empty()) {
                    _snprintf_s(curBuf, _TRUNCATE, "%d", g_AttachSelectedAttachmentIdx);
                } else {
                    strcpy_s(curBuf, "<none>");
                }
            }

            if (ImGui::BeginCombo("##attach_index", curBuf)) {
                if (needBuild) RebuildAttachmentCacheForSelected();
                for (const auto& it : g_AttachItems) {
                    const bool sel = (g_AttachSelectedAttachmentIdx == it.idx);
                    char label[128];
                    if (it.name) _snprintf_s(label, _TRUNCATE, "%d - %s", it.idx, it.name);
                    else _snprintf_s(label, _TRUNCATE, "%d", it.idx);
                    if (ImGui::Selectable(label, sel)) {
                        g_AttachSelectedAttachmentIdx = it.idx;
                        advancedfx::overlay::AttachCam_SetAttachmentIndex(it.idx);
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            // Offsets
            ImGui::SeparatorText("Offsets");
            ImGui::TextUnformatted("Position (Fwd, Right, Up)");
            if (ImGui::DragFloat3("##attach_ofs_pos", g_AttachOffsetPos, 0.1f)) {
                advancedfx::overlay::AttachCam_SetOffsetPos(g_AttachOffsetPos);
            }
            ImGui::TextUnformatted("Rotation (Pitch, Yaw, Roll)");
            if (ImGui::DragFloat3("##attach_ofs_rot", g_AttachOffsetRot, 0.5f)) {
                advancedfx::overlay::AttachCam_SetOffsetRot(g_AttachOffsetRot);
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset")) {
                g_AttachOffsetPos[0] = g_AttachOffsetPos[1] = g_AttachOffsetPos[2] = 0.0f;
                g_AttachOffsetRot[0] = g_AttachOffsetRot[1] = g_AttachOffsetRot[2] = 0.0f;
                advancedfx::overlay::AttachCam_SetOffsetPos(g_AttachOffsetPos);
                advancedfx::overlay::AttachCam_SetOffsetRot(g_AttachOffsetRot);
            }
        }
        ImGui::End();
    }
    if (g_ShowDofWindow) {
        ImGui::Begin("DOF Control");
        {
            static bool s_toggleDof = false;
            const char* dofLabel = s_toggleDof ? "Disable DOF" : "Enable DOF";
            if (ImGui::Button(dofLabel)) {
                s_toggleDof = !s_toggleDof;
                if (s_toggleDof) {
                    Afx_ExecClientCmd("r_dof_override 1");
                } else {
                    Afx_ExecClientCmd("r_dof_override 0");
                }
            }
        }
        ImGui::SameLine();
        {
            const char* dofTimelineLabel = g_EnableDofTimeline ? "Disable Timeline" : "Enable Timeline";
            if (ImGui::Button(dofTimelineLabel)) {
                g_EnableDofTimeline = !g_EnableDofTimeline;
                if (!g_EnableDofTimeline) {
                    Dof_RemoveScheduled();
                } else {
                    Dof_RebuildScheduled();
                }
            }
        }
        if (g_EnableDofTimeline){
            if (ImGui::Button("Add Key")) {
                int curTick = 0;
                if (g_MirvTime.GetCurrentDemoTick(curTick)) {
                    DofKeyframe k;
                    k.tick = curTick;
                    k.farBlurry   = g_FarBlurry;
                    k.farCrisp    = g_FarCrisp;
                    k.nearBlurry  = g_NearBlurry;
                    k.nearCrisp   = g_NearCrisp;
                    k.maxBlur     = g_MaxBlur;
                    k.radiusScale = g_DofRadiusScale;
                    k.tilt        = g_DofTilt;

                    g_DofKeys.push_back(k);
                    std::sort(g_DofKeys.begin(), g_DofKeys.end(), [](const DofKeyframe& a, const DofKeyframe& b){ return a.tick < b.tick; });
                    Dof_SyncTicksFromKeys();
                    Dof_RebuildScheduled();
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(g_DofKeys.size() < 4);
            {
                bool cubic = g_DofInterpCubic;
                if (ImGui::Checkbox("Cubic", &cubic)) {
                    g_DofInterpCubic = cubic;
                    Dof_RebuildScheduled();
                }
            }
            ImGui::EndDisabled();
        }
        // Far blur slider (reserve right space so label is always visible)
        {
            ImGuiStyle& st3 = ImGui::GetStyle();
            float avail = ImGui::GetContentRegionAvail().x;
            const char* lbl = "Far Blurry";
            float lblW = ImGui::CalcTextSize(lbl).x;
            float rightGap = st3.ItemInnerSpacing.x * 3.0f + st3.FramePadding.x * 2.0f;
            float width = avail - (lblW + rightGap);
            if (width < 100.0f) width = avail * 0.6f; // fallback
            ImGui::SetNextItemWidth(width);
        }
        {
            float tmp = g_FarBlurry;
            bool changed = ImGui::SliderFloat("Far Blurry", &tmp, 1.0f, 10000.0f, "%.1f");
            bool reset = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
            if (reset) {
                g_FarBlurry = g_FarBlurryDefault;
                ImGui::ClearActiveID();
                Afx_ExecClientCmd(("r_dof_override_far_blurry " + std::to_string(g_FarBlurry)).c_str());
            } else if (changed) {
                g_FarBlurry = tmp;
                Afx_ExecClientCmd(("r_dof_override_far_blurry " + std::to_string(g_FarBlurry)).c_str());
            }
        }
        {
            ImGuiStyle& st3 = ImGui::GetStyle();
            float avail = ImGui::GetContentRegionAvail().x;
            const char* lbl = "Far Crisp";
            float lblW = ImGui::CalcTextSize(lbl).x;
            float rightGap = st3.ItemInnerSpacing.x * 3.0f + st3.FramePadding.x * 2.0f;
            float width = avail - (lblW + rightGap);
            if (width < 100.0f) width = avail * 0.6f; // fallback
            ImGui::SetNextItemWidth(width);
        }
        {
            float tmp = g_FarCrisp;
            bool changed = ImGui::SliderFloat("Far Crisp", &tmp, 1.0f, 10000.0f, "%.1f");
            bool reset = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
            if (reset) {
                g_FarCrisp = g_FarCrispDefault;
                ImGui::ClearActiveID();
                Afx_ExecClientCmd(("r_dof_override_far_crisp " + std::to_string(g_FarCrisp)).c_str());
            } else if (changed) {
                g_FarCrisp = tmp;
                Afx_ExecClientCmd(("r_dof_override_far_crisp " + std::to_string(g_FarCrisp)).c_str());
            }
        }
        {
            ImGuiStyle& st3 = ImGui::GetStyle();
            float avail = ImGui::GetContentRegionAvail().x;
            const char* lbl = "Near Blurry";
            float lblW = ImGui::CalcTextSize(lbl).x;
            float rightGap = st3.ItemInnerSpacing.x * 3.0f + st3.FramePadding.x * 2.0f;
            float width = avail - (lblW + rightGap);
            if (width < 100.0f) width = avail * 0.6f; // fallback
            ImGui::SetNextItemWidth(width);
        }
        {
            float tmp = g_NearBlurry;
            bool changed = ImGui::SliderFloat("Near Blurry", &tmp, -1000.0f, 1000.0f, "%.1f");
            bool reset = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
            if (reset) {
                g_NearBlurry = g_NearBlurryDefault;
                ImGui::ClearActiveID();
                Afx_ExecClientCmd(("r_dof_override_near_blurry " + std::to_string(g_NearBlurry)).c_str());
            } else if (changed) {
                g_NearBlurry = tmp;
                Afx_ExecClientCmd(("r_dof_override_near_blurry " + std::to_string(g_NearBlurry)).c_str());
            }
        }
        {
            ImGuiStyle& st3 = ImGui::GetStyle();
            float avail = ImGui::GetContentRegionAvail().x;
            const char* lbl = "Near Crisp";
            float lblW = ImGui::CalcTextSize(lbl).x;
            float rightGap = st3.ItemInnerSpacing.x * 3.0f + st3.FramePadding.x * 2.0f;
            float width = avail - (lblW + rightGap);
            if (width < 100.0f) width = avail * 0.6f; // fallback
            ImGui::SetNextItemWidth(width);
        }
        {
            float tmp = g_NearCrisp;
            bool changed = ImGui::SliderFloat("Near Crisp", &tmp, 0.0f, 1000.0f, "%.1f");
            bool reset = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
            if (reset) {
                g_NearCrisp = g_NearCrispDefault;
                ImGui::ClearActiveID();
                Afx_ExecClientCmd(("r_dof_override_near_crisp " + std::to_string(g_NearCrisp)).c_str());
            } else if (changed) {
                g_NearCrisp = tmp;
                Afx_ExecClientCmd(("r_dof_override_near_crisp " + std::to_string(g_NearCrisp)).c_str());
            }
        }
        {
            ImGuiStyle& st3 = ImGui::GetStyle();
            float avail = ImGui::GetContentRegionAvail().x;
            const char* lbl = "Max Blur Size";
            float lblW = ImGui::CalcTextSize(lbl).x;
            float rightGap = st3.ItemInnerSpacing.x * 3.0f + st3.FramePadding.x * 2.0f;
            float width = avail - (lblW + rightGap);
            if (width < 100.0f) width = avail * 0.6f; // fallback
            ImGui::SetNextItemWidth(width);
        }
        {
            float tmp = g_MaxBlur;
            bool changed = ImGui::SliderFloat("Max Blur Size", &tmp, 0.0f, 11.0f, "%.1f");
            bool reset = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
            if (reset) {
                g_MaxBlur = g_MaxBlurDefault;
                ImGui::ClearActiveID();
                Afx_ExecClientCmd(("r_dof2_maxblursize " + std::to_string(g_MaxBlur)).c_str());
            } else if (changed) {
                g_MaxBlur = tmp;
                Afx_ExecClientCmd(("r_dof2_maxblursize " + std::to_string(g_MaxBlur)).c_str());
            }
        }
        {
            ImGuiStyle& st3 = ImGui::GetStyle();
            float avail = ImGui::GetContentRegionAvail().x;
            const char* lbl = "Radius Scale";
            float lblW = ImGui::CalcTextSize(lbl).x;
            float rightGap = st3.ItemInnerSpacing.x * 3.0f + st3.FramePadding.x * 2.0f;
            float width = avail - (lblW + rightGap);
            if (width < 100.0f) width = avail * 0.6f; // fallback
            ImGui::SetNextItemWidth(width);
        }
        {
            float tmp = g_DofRadiusScale;
            bool changed = ImGui::SliderFloat("Radius Scale", &tmp, 0.10f, 5.00f, "%.01f");
            bool reset = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
            if (reset) {
                g_DofRadiusScale = g_DofRadiusScaleDefault;
                ImGui::ClearActiveID();
                Afx_ExecClientCmd(("r_dof2_radiusscale " + std::to_string(g_DofRadiusScale)).c_str());
            } else if (changed) {
                g_DofRadiusScale = tmp;
                Afx_ExecClientCmd(("r_dof2_radiusscale " + std::to_string(g_DofRadiusScale)).c_str());
            }
        }
        {
            ImGuiStyle& st3 = ImGui::GetStyle();
            float avail = ImGui::GetContentRegionAvail().x;
            const char* lbl = "Tilt";
            float lblW = ImGui::CalcTextSize(lbl).x;
            float rightGap = st3.ItemInnerSpacing.x * 3.0f + st3.FramePadding.x * 2.0f;
            float width = avail - (lblW + rightGap);
            if (width < 100.0f) width = avail * 0.6f; // fallback
            ImGui::SetNextItemWidth(width);
        }
        {
            float tmp = g_DofTilt;
            bool changed = ImGui::SliderFloat("Tilt", &tmp, 0.0f, 10.0f, "%.1f");
            bool reset = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
            if (reset) {
                g_DofTilt = g_DofTiltDefault;
                ImGui::ClearActiveID();
                Afx_ExecClientCmd(("r_dof_override_tilt_to_ground " + std::to_string(g_DofTilt)).c_str());
            } else if (changed) {
                g_DofTilt = tmp;
                Afx_ExecClientCmd(("r_dof_override_tilt_to_ground " + std::to_string(g_DofTilt)).c_str());
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

    // Observing Bindings Window
    if (g_ShowObservingBindings) {
        ImGui::Begin("Observing Bindings", &g_ShowObservingBindings);

        // Build player list - stored as static so it persists
        static std::vector<std::pair<int, std::string>> playerList; // <index, name>

        // Refresh button to update player list
        if (ImGui::Button("Refresh Players")) {
            playerList.clear();
            if (g_pEntityList && *g_pEntityList && g_GetEntityFromIndex) {
                int highest = GetHighestEntityIndex();
                for (int i = 0; i <= highest; i++) {
                    if (auto* ent = static_cast<CEntityInstance*>(g_GetEntityFromIndex(*g_pEntityList, i))) {
                        if (SafeIsPlayerController(ent)) {
                            // Use same pattern as other code: try sanitized name first, then regular name
                            const char* name = SafeGetSanitizedPlayerName(ent);
                            if (!name || !*name) name = SafeGetPlayerName(ent);
                            if (!name || !*name) name = SafeGetDebugName(ent);

                            // Fallback to generic name if still invalid
                            if (name && *name) {
                                playerList.push_back({i, std::string(name)});
                            } else {
                                char fallback[64];
                                _snprintf_s(fallback, _TRUNCATE, "Player_%d", i);
                                playerList.push_back({i, std::string(fallback)});
                            }
                        }
                    }
                }
            }
            advancedfx::Message("Overlay: Refreshed player list, found %d players\n", (int)playerList.size());
        }
        ImGui::SameLine();
        if (ImGui::Button("Auto Assign")) {
            AutoPopulateObservingBindingsFromEntities();
        }
        ImGui::SameLine();
        ImGui::Text("(%d players)", (int)playerList.size());

        ImGui::Separator();

        // Show 10 rows, order 1..9 then 0
        {
            const int keyOrder[10] = {1,2,3,4,5,6,7,8,9,0};
            for (int ko = 0; ko < 10; ++ko) {
                int keyNum = keyOrder[ko];
                ImGui::PushID(keyNum);

            // Hotkey label
            ImGui::Text("Key %d:", keyNum);
            ImGui::SameLine();

            // Get current binding for this key
            int currentBinding = -1;
            auto it = g_ObservingHotkeyBindings.find(keyNum);
            if (it != g_ObservingHotkeyBindings.end()) {
                currentBinding = it->second;
            }

            // Build label for combo - use C strings to avoid allocations
            char comboLabel[256];
            if (currentBinding >= 0) {
                // Find player name
                bool found = false;
                for (const auto& p : playerList) {
                    if (p.first == currentBinding) {
                        _snprintf_s(comboLabel, _TRUNCATE, "%s (idx: %d)", p.second.c_str(), p.first);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    _snprintf_s(comboLabel, _TRUNCATE, "Controller %d", currentBinding);
                }
            } else {
                strcpy_s(comboLabel, "<None>");
            }

            // Player combo
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::BeginCombo("##player", comboLabel)) {
                // None option
                if (ImGui::Selectable("<None>", currentBinding < 0)) {
                    g_ObservingHotkeyBindings.erase(keyNum);
                    advancedfx::Message("Overlay: Cleared binding for key %d\n", keyNum);
                }

                // Player options
                for (const auto& player : playerList) {
                    char label[256];
                    _snprintf_s(label, _TRUNCATE, "%s (idx: %d)", player.second.c_str(), player.first);
                    bool selected = (player.first == currentBinding);
                    if (ImGui::Selectable(label, selected)) {
                        g_ObservingHotkeyBindings[keyNum] = player.first;
                        advancedfx::Message("Overlay: Bound key %d to controller index %d\n", keyNum, player.first);
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

                ImGui::PopID();
            }
        }

        ImGui::End();
    }

    // Observing Cameras Window
    if (g_ShowObservingCameras) {
        ImGui::Begin("Observing Cameras", &g_ShowObservingCameras);

        // Modal dialogs for input
        static bool showAddProfileModal = false;
        static bool showAddCameraModal = false;
        static char profileNameInput[256] = {0};
        static char cameraNameInput[256] = {0};

        // Header with buttons
        if (ImGui::Button("Add Profile")) {
            profileNameInput[0] = '\0';
            showAddProfileModal = true;
            ImGui::OpenPopup("Add Profile##modal");
        }

        ImGui::SameLine();

        ImGui::BeginDisabled(g_SelectedProfileIndex < 0 || g_SelectedProfileIndex >= (int)g_CameraProfiles.size());
        if (ImGui::Button("Add Camera")) {
            cameraNameInput[0] = '\0';
            showAddCameraModal = true;
            ImGui::OpenPopup("Add Camera##modal");
        }
        ImGui::EndDisabled();

        ImGui::SameLine();

        // Profile dropdown
        ImGui::SetNextItemWidth(200.0f);
        std::string currentProfileLabel = "<No Profile>";
        if (g_SelectedProfileIndex >= 0 && g_SelectedProfileIndex < (int)g_CameraProfiles.size()) {
            currentProfileLabel = g_CameraProfiles[g_SelectedProfileIndex].name;
        }

        if (ImGui::BeginCombo("Profile", currentProfileLabel.c_str())) {
            for (int i = 0; i < (int)g_CameraProfiles.size(); i++) {
                bool selected = (i == g_SelectedProfileIndex);
                if (ImGui::Selectable(g_CameraProfiles[i].name.c_str(), selected)) {
                    g_SelectedProfileIndex = i;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        // Remove Profile button
        ImGui::SameLine();
        if (ImGui::Button("Remove Profile")) {
            if (g_SelectedProfileIndex >= 0 && g_SelectedProfileIndex < (int)g_CameraProfiles.size()) {
                // Release any cached textures used by this profile
                const auto &prof = g_CameraProfiles[g_SelectedProfileIndex];
                for (const auto &cam : prof.cameras) {
                    if (!cam.imagePath.empty()) {
                        auto it = g_CameraTextures.find(cam.imagePath);
                        if (it != g_CameraTextures.end()) {
                            if (it->second.srv) it->second.srv->Release();
                            g_CameraTextures.erase(it);
                        }
                    }
                }
                advancedfx::Message("Overlay: Removed camera profile '%s'\n", g_CameraProfiles[g_SelectedProfileIndex].name.c_str());
                g_CameraProfiles.erase(g_CameraProfiles.begin() + g_SelectedProfileIndex);
                if (g_SelectedProfileIndex >= (int)g_CameraProfiles.size()) g_SelectedProfileIndex = (int)g_CameraProfiles.size() - 1;
                ImGui::MarkIniSettingsDirty();
            }
        }

        // Scale slider
        ImGui::SameLine();
        ImGui::TextUnformatted("Scale");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("##cam_rect_scale", &g_CameraRectScale, 0.5f, 3.0f, "%.2fx");

        // Save button
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            ImGui::MarkIniSettingsDirty();
            ImGuiIO& io_s = ImGui::GetIO();
            if (io_s.IniFilename && io_s.IniFilename[0]) {
                ImGui::SaveIniSettingsToDisk(io_s.IniFilename);
                advancedfx::Message("Overlay: Saved camera profiles to '%s'\n", io_s.IniFilename);
            }
        }

        // Add Profile Modal
        if (ImGui::BeginPopupModal("Add Profile##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter profile name:");
            ImGui::SetNextItemWidth(300.0f);
            ImGui::InputText("##profile_name", profileNameInput, sizeof(profileNameInput));

            ImGui::Separator();

            if (ImGui::Button("Create", ImVec2(120, 0))) {
                if (profileNameInput[0] != '\0') {
                    CameraProfile newProfile;
                    newProfile.name = profileNameInput;
                    g_CameraProfiles.push_back(newProfile);
                    g_SelectedProfileIndex = (int)g_CameraProfiles.size() - 1;
                    advancedfx::Message("Overlay: Created camera profile '%s'\n", profileNameInput);
                    ImGui::MarkIniSettingsDirty();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        // Add Camera Modal
        if (ImGui::BeginPopupModal("Add Camera##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter camera name:");
            ImGui::SetNextItemWidth(300.0f);
            ImGui::InputText("##camera_name", cameraNameInput, sizeof(cameraNameInput));

            ImGui::Separator();

            if (ImGui::Button("Create", ImVec2(120, 0))) {
                if (cameraNameInput[0] != '\0' && g_SelectedProfileIndex >= 0 && g_SelectedProfileIndex < (int)g_CameraProfiles.size()) {
                    CameraItem newCamera;
                    newCamera.name = cameraNameInput;
                    g_CameraProfiles[g_SelectedProfileIndex].cameras.push_back(newCamera);
                    advancedfx::Message("Overlay: Added camera '%s' to profile '%s'\n", cameraNameInput, g_CameraProfiles[g_SelectedProfileIndex].name.c_str());
                    ImGui::MarkIniSettingsDirty();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        ImGui::Separator();

        // Display cameras for selected profile
        if (g_SelectedProfileIndex >= 0 && g_SelectedProfileIndex < (int)g_CameraProfiles.size()) {
            CameraProfile& profile = g_CameraProfiles[g_SelectedProfileIndex];

            // Toggle button for groups sidebar (vertical button at edge)
            ImVec2 contentRegion = ImGui::GetContentRegionAvail();
            ImVec2 cursorPos = ImGui::GetCursorPos();

            // Draw vertical toggle button at right edge
            ImGui::PushStyleColor(ImGuiCol_Button, g_ShowGroupsSidebar ? ImVec4(0.3f, 0.5f, 0.8f, 1.0f) : ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
            ImGui::SetCursorPos(ImVec2(cursorPos.x + contentRegion.x - 20.0f, cursorPos.y));
            if (ImGui::Button("G\nR\nO\nU\nP\nS", ImVec2(20.0f, 120.0f))) {
                g_ShowGroupsSidebar = !g_ShowGroupsSidebar;
            }
            ImGui::PopStyleColor();

            // Restore cursor for main content
            ImGui::SetCursorPos(cursorPos);

            // Static file browsers for each camera (using map keyed by camera index)
            static std::map<size_t, ImGui::FileBrowser> cameraFileBrowsers;
            static std::map<size_t, ImGui::FileBrowser> imageFileBrowsers;
            // Track drag state across items to suppress click-to-play when reordering
            static bool s_CameraDragActive = false;
            // Track press position/index to decide click vs drag on release
            static int   s_CameraPressedIndex = -1;
            static ImVec2 s_CameraPressPos = ImVec2(0,0);

            // Load any images that haven't been loaded yet
            for (const auto& camera : profile.cameras) {
                if (!camera.imagePath.empty() && g_CameraTextures.find(camera.imagePath) == g_CameraTextures.end()) {
                    CameraTexture tex;
                    if (LoadImageToTexture(m_Device, camera.imagePath, tex)) {
                        g_CameraTextures[camera.imagePath] = tex;
                    }
                }
            }

            // Groups Sidebar
            ImVec2 baseButton(160.0f, 90.0f);
            ImVec2 buttonSize(baseButton.x * g_CameraRectScale, baseButton.y * g_CameraRectScale);
            float sidebarWidth = g_ShowGroupsSidebar ? (buttonSize.x + 20.0f) : 0.0f;

            if (g_ShowGroupsSidebar) {
                ImGui::BeginChild("GroupsSidebar", ImVec2(sidebarWidth, 0), true);

                if (ImGui::Button("Add Group", ImVec2(-1, 0))) {
                    ImGui::OpenPopup("Add Group##modal");
                }

                // Add Group Modal
                static char groupNameInput[256] = {0};
                if (ImGui::BeginPopupModal("Add Group##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::Text("Enter group name:");
                    ImGui::SetNextItemWidth(200.0f);
                    ImGui::InputText("##group_name", groupNameInput, sizeof(groupNameInput));
                    ImGui::Separator();

                    if (ImGui::Button("Create", ImVec2(90, 0))) {
                        if (groupNameInput[0] != '\0' && g_SelectedProfileIndex >= 0 && g_SelectedProfileIndex < (int)g_CameraProfiles.size()) {
                            CameraGroup newGroup;
                            newGroup.name = groupNameInput;
                            newGroup.profileName = g_CameraProfiles[g_SelectedProfileIndex].name;
                            g_CameraGroups.push_back(newGroup);
                            advancedfx::Message("Overlay: Created camera group '%s' for profile '%s'\n", groupNameInput, newGroup.profileName.c_str());
                            ImGui::MarkIniSettingsDirty();
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(90, 0))) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }

                ImGui::Separator();

                // Display groups
                for (size_t grpIdx = 0; grpIdx < g_CameraGroups.size(); grpIdx++) {
                    CameraGroup& group = g_CameraGroups[grpIdx];
                    if (group.profileName != profile.name) continue;
                    ImGui::PushID((int)grpIdx);

                    // Group rectangle background
                    ImVec2 rectMin = ImGui::GetCursorScreenPos();
                    ImVec2 rectMax = ImVec2(rectMin.x + buttonSize.x, rectMin.y + buttonSize.y);
                    ImDrawList* dl = ImGui::GetWindowDrawList();

                    // Draw background
                    dl->AddRectFilled(rectMin, rectMax, IM_COL32(60, 60, 80, 255));
                    dl->AddRect(rectMin, rectMax, IM_COL32(100, 100, 150, 255));

                    // Group name
                    ImVec2 textPos = ImVec2(rectMin.x + 5, rectMin.y + 5);
                    dl->AddText(textPos, IM_COL32(255, 255, 255, 255), group.name.c_str());

                    // Camera count
                    char countBuf[32];
                    _snprintf_s(countBuf, _TRUNCATE, "%d cameras", (int)group.cameraIndices.size());
                    ImVec2 countPos = ImVec2(rectMin.x + 5, rectMin.y + 25);
                    dl->AddText(countPos, IM_COL32(180, 180, 180, 255), countBuf);

                    const char* modeLabels[] = {"Rnd", "Seq", "Fst"};

                    // Buttons (Delete, View, Mode)
                    float btnY = rectMax.y - 25.0f;
                    ImVec2 btnSize(buttonSize.x / 3.0f - 4.0f, 20.0f);

                    // Delete button
                    ImGui::SetCursorScreenPos(ImVec2(rectMin.x + 2, btnY));
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
                    if (ImGui::Button("Del", btnSize)) {
                        g_CameraGroups.erase(g_CameraGroups.begin() + grpIdx);
                        ImGui::MarkIniSettingsDirty();
                        ImGui::PopStyleColor();
                        ImGui::PopID();
                        break; // Exit loop after deletion
                    }
                    ImGui::PopStyleColor();

                    // View button
                    ImGui::SetCursorScreenPos(ImVec2(rectMin.x + buttonSize.x / 3.0f + 1, btnY));
                    if (ImGui::Button("View", btnSize)) {
                        g_SelectedGroupForView = (int)grpIdx;
                        g_ShowGroupViewWindow = true;
                    }

                    // Mode toggle button
                    ImGui::SetCursorScreenPos(ImVec2(rectMin.x + 2 * buttonSize.x / 3.0f + 2, btnY));
                    if (ImGui::Button(modeLabels[(int)group.playMode], btnSize)) {
                        group.playMode = (GroupPlayMode)(((int)group.playMode + 1) % 3);
                        ImGui::MarkIniSettingsDirty();
                    }

                    // Invisible button for clicking (also drag-drop target for adding cameras)
                    ImGui::SetCursorScreenPos(rectMin);
                    ImGui::InvisibleButton("##grouprect", ImVec2(buttonSize.x, buttonSize.y));

                    // Drag-drop target to add cameras to this group
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HLAE_OBS_CAM_REORDER")) {
                            int droppedCamIdx = *(const int*)payload->Data;
                            // Check if not already in group
                            bool alreadyInGroup = false;
                            for (int idx : group.cameraIndices) {
                                if (idx == droppedCamIdx) {
                                    alreadyInGroup = true;
                                    break;
                                }
                            }
                            if (!alreadyInGroup && droppedCamIdx >= 0 && droppedCamIdx < (int)profile.cameras.size()) {
                                group.cameraIndices.push_back(droppedCamIdx);
                                advancedfx::Message("Overlay: Added camera '%s' to group '%s'\n",
                                                  profile.cameras[droppedCamIdx].name.c_str(), group.name.c_str());
                                ImGui::MarkIniSettingsDirty();
                            } else if (alreadyInGroup) {
                                advancedfx::Message("Overlay: Camera already in group '%s'\n", group.name.c_str());
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }

                    if (ImGui::IsItemClicked()) {
                        // Play camera based on play mode
                        int camIdx = -1;

                        if (group.playMode == GroupPlayMode::Random) {
                            if (!group.cameraIndices.empty()) {
                                camIdx = group.cameraIndices[rand() % group.cameraIndices.size()];
                            }
                        } else if (group.playMode == GroupPlayMode::Sequentially) {
                            if (!group.cameraIndices.empty()) {
                                camIdx = group.cameraIndices[group.sequentialIndex % group.cameraIndices.size()];
                                group.sequentialIndex = (group.sequentialIndex + 1) % (int)group.cameraIndices.size();
                            }
                        } else if (group.playMode == GroupPlayMode::FromStart) {
                            // Reset if different group/camera was played
                            std::string thisGroup = "group_" + std::to_string(grpIdx);
                            if (g_LastPlayedGroupOrCamera != thisGroup) {
                                group.sequentialIndex = 0;
                            }
                            if (!group.cameraIndices.empty()) {
                                camIdx = group.cameraIndices[group.sequentialIndex % group.cameraIndices.size()];
                                group.sequentialIndex = (group.sequentialIndex + 1) % (int)group.cameraIndices.size();
                            }
                            g_LastPlayedGroupOrCamera = thisGroup;
                        }

                        // Load the camera if valid
                        if (camIdx >= 0 && camIdx < (int)profile.cameras.size()) {
                            const CameraItem& camera = profile.cameras[camIdx];
                            if (!camera.camPathFile.empty()) {
                                std::string cmd = "mirv_campath clear; mirv_campath load \"" + camera.camPathFile + "\"; mirv_campath enabled 1; spec_mode 4; mirv_input end; mirv_campath select all; mirv_campath edit start";
                                Afx_ExecClientCmd(cmd.c_str());
                                advancedfx::Message("Overlay: [Group '%s'] Loading camera '%s' (%s mode, idx=%d)\n",
                                                  group.name.c_str(), camera.name.c_str(),
                                                  modeLabels[(int)group.playMode], camIdx);
                                g_ActiveCameraCampathPath = camera.camPathFile;
                            } else {
                                advancedfx::Message("Overlay: [Group '%s'] Camera at index %d has no campath file\n",
                                                  group.name.c_str(), camIdx);
                            }
                        }
                    }

                    // Add spacing before next group
                    ImGui::SetCursorScreenPos(ImVec2(rectMin.x, rectMax.y + 5));
                    ImGui::Dummy(ImVec2(0, 0));

                    ImGui::PopID();
                }

                ImGui::EndChild();
                ImGui::SameLine();
            }

            // Grid layout parameters
            ImVec2 gridOrigin = ImGui::GetCursorScreenPos();
            ImGuiStyle& st_cam = ImGui::GetStyle();
            float spacingX = st_cam.ItemSpacing.x;
            float spacingY = st_cam.ItemSpacing.y;
            float contentW = ImGui::GetContentRegionAvail().x;
            int columns = (int)floor((contentW + spacingX) / (buttonSize.x + spacingX));
            if (columns < 1) columns = 1;

            for (size_t camIdx = 0; camIdx < profile.cameras.size(); camIdx++) {
                CameraItem& camera = profile.cameras[camIdx];
                ImGui::PushID((int)camIdx);

                // Determine grid position
                int col = (int)(camIdx % (size_t)columns);
                int row = (int)(camIdx / (size_t)columns);
                ImVec2 itemMin = ImVec2(
                    gridOrigin.x + col * (buttonSize.x + spacingX),
                    gridOrigin.y + row * (buttonSize.y + spacingY)
                );
                ImVec2 itemMax = ImVec2(itemMin.x + buttonSize.x, itemMin.y + buttonSize.y);
                ImDrawList* drawList = ImGui::GetWindowDrawList();

                // Draw background and border
                drawList->AddRectFilled(itemMin, itemMax, IM_COL32(60, 60, 60, 255));
                drawList->AddRect(itemMin, itemMax, IM_COL32(100, 100, 100, 255));

                // Draw image if available
                if (!camera.imagePath.empty()) {
                    auto texIt = g_CameraTextures.find(camera.imagePath);
                    if (texIt != g_CameraTextures.end() && texIt->second.srv) {
                        // Calculate scaled size to fit within button
                        float scaleX = buttonSize.x / texIt->second.width;
                        float scaleY = buttonSize.y / texIt->second.height;
                        float scale = (scaleX < scaleY) ? scaleX : scaleY;
                        ImVec2 imgSize(texIt->second.width * scale, texIt->second.height * scale);
                        ImVec2 imgPos(itemMin.x + (buttonSize.x - imgSize.x) * 0.5f,
                                     itemMin.y + (buttonSize.y - imgSize.y) * 0.5f);
                        drawList->AddImage((ImTextureID)texIt->second.srv, imgPos,
                                         ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y));
                    }
                }

                // Draw camera name (truncate if too long) and scale with rectangle
                float pad = 4.0f * g_CameraRectScale;
                ImVec2 textPos = ImVec2(itemMin.x + pad, itemMin.y + pad);
                char truncatedName[64];
                _snprintf_s(truncatedName, _TRUNCATE, "%.20s", camera.name.c_str());

                ImFont* nameFont = ImGui::GetFont();
                if (!nameFont) nameFont = ImGui::GetIO().FontDefault;
                float baseNameSize = ImGui::GetFontSize();
                float nameSize = baseNameSize * g_CameraRectScale;
                if (nameSize < 8.0f) nameSize = 8.0f; if (nameSize > 48.0f) nameSize = 48.0f;
                float o = std::roundf((std::max)(1.0f, g_CameraRectScale));
                // Shadow + foreground
                drawList->AddText(nameFont, nameSize, ImVec2(textPos.x + o, textPos.y + o), IM_COL32(0, 0, 0, 200), truncatedName);
                drawList->AddText(nameFont, nameSize, textPos, IM_COL32(255, 255, 255, 255), truncatedName);

                // Draw campath file path (show just filename, not full path)
                const char* pathText;
                char fileNameOnly[64];
                if (camera.camPathFile.empty()) {
                    pathText = "<No file>";
                } else {
                    size_t lastSlash = camera.camPathFile.find_last_of("/\\");
                    if (lastSlash != std::string::npos) {
                        _snprintf_s(fileNameOnly, _TRUNCATE, "%.18s", camera.camPathFile.c_str() + lastSlash + 1);
                    } else {
                        _snprintf_s(fileNameOnly, _TRUNCATE, "%.18s", camera.camPathFile.c_str());
                    }
                    pathText = fileNameOnly;
                }
                ImVec2 pathPos = ImVec2(itemMin.x + pad, itemMin.y + 24.0f * g_CameraRectScale);
                drawList->AddText(ImVec2(pathPos.x + 1, pathPos.y + 1), IM_COL32(0, 0, 0, 200), pathText);
                drawList->AddText(pathPos, IM_COL32(200, 200, 200, 255), pathText);

                // Now draw buttons on top (using absolute positioning)
                // Track if any button was clicked
                bool anyButtonClicked = false;

                // Remove button (top right)
                ImGui::SetCursorScreenPos(ImVec2(itemMax.x - 60, itemMin.y + 2.0f * g_CameraRectScale));
                if (ImGui::SmallButton("Remove")) {
                    profile.cameras.erase(profile.cameras.begin() + camIdx);
                    advancedfx::Message("Overlay: Removed camera '%s'\n", camera.name.c_str());
                    ImGui::MarkIniSettingsDirty();
                    anyButtonClicked = true;
                    ImGui::PopID();
                    break;
                }
                anyButtonClicked |= ImGui::IsItemHovered();

                // Browse button (below Remove)
                ImGui::SetCursorScreenPos(ImVec2(itemMax.x - 60, itemMin.y + 20.0f * g_CameraRectScale));
                if (ImGui::SmallButton("Browse")) {
                    if (cameraFileBrowsers.find(camIdx) == cameraFileBrowsers.end()) {
                        cameraFileBrowsers[camIdx] = ImGui::FileBrowser(
                            ImGuiFileBrowserFlags_CloseOnEsc |
                            ImGuiFileBrowserFlags_EditPathString
                        );
                        cameraFileBrowsers[camIdx].SetTitle("Select Campath File");
                    }
                    if (!g_OverlayPaths.campathDir.empty())
                        cameraFileBrowsers[camIdx].SetDirectory(g_OverlayPaths.campathDir);
                    cameraFileBrowsers[camIdx].Open();
                    anyButtonClicked = true;
                }
                anyButtonClicked |= ImGui::IsItemHovered();

                // Image button (below Browse)
                ImGui::SetCursorScreenPos(ImVec2(itemMax.x - 60, itemMin.y + 38.0f * g_CameraRectScale));
                if (ImGui::SmallButton("Image")) {
                    if (imageFileBrowsers.find(camIdx) == imageFileBrowsers.end()) {
                        imageFileBrowsers[camIdx] = ImGui::FileBrowser(
                            ImGuiFileBrowserFlags_CloseOnEsc |
                            ImGuiFileBrowserFlags_EditPathString
                        );
                        imageFileBrowsers[camIdx].SetTitle("Select Image File");
                        imageFileBrowsers[camIdx].SetTypeFilters({".png", ".jpg", ".jpeg", ".bmp"});
                    }
                    if (!g_OverlayPaths.campathDir.empty())
                        imageFileBrowsers[camIdx].SetDirectory(g_OverlayPaths.campathDir);
                    imageFileBrowsers[camIdx].Open();
                    anyButtonClicked = true;
                }
                anyButtonClicked |= ImGui::IsItemHovered();

                // Screen button (below Image) - captures backbuffer and saves it
                ImGui::SetCursorScreenPos(ImVec2(itemMax.x - 60, itemMin.y + 56.0f * g_CameraRectScale));
                if (ImGui::SmallButton("Screen")) {
                    // Capture backbuffer and save it with the campath filename
                    if (m_BackbufferPreview.texture && !camera.camPathFile.empty()) {
                        // Extract filename from campath file (without extension)
                        std::filesystem::path campathPath(camera.camPathFile);
                        std::string basename = campathPath.stem().string();

                        // Build output path: campathDir + basename + ".png"
                        std::filesystem::path outputDir = g_OverlayPaths.campathDir.empty()
                            ? campathPath.parent_path()
                            : std::filesystem::path(g_OverlayPaths.campathDir);
                        std::filesystem::path outputPath = outputDir / (basename + ".png");
                        std::wstring wOutputPath = outputPath.wstring();

                        // Save texture to file
                        if (SaveTextureToFile(m_Device, m_Context, m_BackbufferPreview.texture, wOutputPath)) {
                            // Release old texture if camera had a different image
                            if (!camera.imagePath.empty() && camera.imagePath != outputPath.string()) {
                                auto texIt = g_CameraTextures.find(camera.imagePath);
                                if (texIt != g_CameraTextures.end() && texIt->second.srv) {
                                    texIt->second.srv->Release();
                                    g_CameraTextures.erase(texIt);
                                }
                            }

                            // Assign the image to the camera
                            camera.imagePath = outputPath.string();

                            // Load the image as texture for preview
                            CameraTexture tex;
                            if (LoadImageToTexture(m_Device, camera.imagePath, tex)) {
                                g_CameraTextures[camera.imagePath] = tex;
                                advancedfx::Message("Overlay: Captured screen and assigned to camera '%s': %s\n",
                                    camera.name.c_str(), camera.imagePath.c_str());
                            } else {
                                advancedfx::Warning("Overlay: Captured screen but failed to load as texture: %s\n",
                                    camera.imagePath.c_str());
                            }
                            ImGui::MarkIniSettingsDirty();
                        } else {
                            advancedfx::Warning("Overlay: Failed to save screenshot to %s\n", outputPath.string().c_str());
                        }
                    } else if (!m_BackbufferPreview.texture) {
                        advancedfx::Warning("Overlay: No backbuffer available. Please open the Viewport window first.\n");
                    } else {
                        advancedfx::Warning("Overlay: Camera has no campath file assigned. Please assign a campath file first.\n");
                    }
                    anyButtonClicked = true;
                }
                anyButtonClicked |= ImGui::IsItemHovered();

                // Draw invisible button for rectangle area; will be used for click and drag-drop
                ImGui::SetCursorScreenPos(itemMin);
                ImGui::InvisibleButton(("##cam_rect_" + std::to_string(camIdx)).c_str(), buttonSize);
                bool didReorderHere = false;
                // Remember press origin to differentiate click vs drag later
                if (ImGui::IsItemActivated()) {
                    s_CameraPressedIndex = (int)camIdx;
                    s_CameraPressPos = ImGui::GetMousePos();
                }

                // Begin drag source for reordering cameras within the current profile
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
                    s_CameraDragActive = true;
                    int srcIndex = (int)camIdx;
                    // Single payload for both reordering and adding to groups
                    ImGui::SetDragDropPayload("HLAE_OBS_CAM_REORDER", &srcIndex, sizeof(srcIndex));
                    // Simple drag preview
                    ImGui::Text("Move %s", camera.name.c_str());
                    ImGui::EndDragDropSource();
                }
                // Accept drop target on the whole rectangle
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HLAE_OBS_CAM_REORDER")) {
                        if (payload && payload->Data && payload->DataSize == sizeof(int)) {
                            int src = *(const int*)payload->Data;
                            int dst = (int)camIdx;
                            if (src >= 0 && src < (int)profile.cameras.size() && dst >= 0 && dst < (int)profile.cameras.size() && src != dst) {
                                // Move item src -> dst, preserving relative order of others
                                CameraItem moved = std::move(profile.cameras[src]);
                                profile.cameras.erase(profile.cameras.begin() + src);
                                // Insert semantics:
                                // - dragging to an earlier index: insert before target (dst)
                                // - dragging to a later index:  insert after target (original dst),
                                //   which after erase is accomplished by inserting at index 'dst'.
                                int insertIdx = dst; // works for both directions as explained above
                                if (insertIdx < 0) insertIdx = 0;
                                if (insertIdx > (int)profile.cameras.size()) insertIdx = (int)profile.cameras.size();
                                profile.cameras.insert(profile.cameras.begin() + insertIdx, std::move(moved));

                                // Update all group indices to reflect the reordering
                                for (auto& group : g_CameraGroups) {
                                    if (group.profileName != g_CameraProfiles[g_SelectedProfileIndex].name) continue;
                                    for (int& idx : group.cameraIndices) {
                                        // The camera at 'src' is now at 'insertIdx'
                                        if (idx == src) {
                                            idx = insertIdx;
                                        }
                                        // Indices between src and insertIdx shift by one
                                        else if (src < insertIdx && idx > src && idx <= insertIdx) {
                                            // Moving forward: indices in between shift left
                                            idx--;
                                        }
                                        else if (src > insertIdx && idx >= insertIdx && idx < src) {
                                            // Moving backward: indices in between shift right
                                            idx++;
                                        }
                                    }
                                }

                                // Clear per-index file browser caches since indices changed
                                cameraFileBrowsers.clear();
                                imageFileBrowsers.clear();
                                ImGui::MarkIniSettingsDirty();
                                advancedfx::Message("Overlay: Reordered camera to index %d\n", dst);
                                // Avoid using stale indices in this frame
                                didReorderHere = true;
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (didReorderHere) { ImGui::PopID(); continue; }

                // On release: treat as click only if the cursor hasn't moved (no drag)
                if (ImGui::IsItemDeactivated() && !s_CameraDragActive && s_CameraPressedIndex == (int)camIdx && !anyButtonClicked && !camera.camPathFile.empty()) {
                    ImVec2 cur = ImGui::GetMousePos();
                    float dx = cur.x - s_CameraPressPos.x;
                    float dy = cur.y - s_CameraPressPos.y;
                    float dist2 = dx*dx + dy*dy;
                    const float clickThreshold = ImGui::GetIO().MouseDragThreshold; // pixels
                    if (dist2 <= clickThreshold * clickThreshold) {
                    std::string cmd = "mirv_campath clear; mirv_campath load \"" + camera.camPathFile + "\"; mirv_campath enabled 1; spec_mode 4; mirv_input end; mirv_campath select all; mirv_campath edit start";
                    Afx_ExecClientCmd(cmd.c_str());
                    advancedfx::Message("Overlay: Loading campath '%s' from camera '%s'\n", camera.camPathFile.c_str(), camera.name.c_str());
                    g_ActiveCameraCampathPath = camera.camPathFile;
                    }
                }

                // Handle file browsers
                if (cameraFileBrowsers.find(camIdx) != cameraFileBrowsers.end()) {
                    if (g_GroupIntoWorkspace && g_WorkspaceViewportId)
                        ImGui::SetNextWindowViewport(g_WorkspaceViewportId);
                    cameraFileBrowsers[camIdx].Display();
                if (cameraFileBrowsers[camIdx].HasSelected()) {
                    camera.camPathFile = cameraFileBrowsers[camIdx].GetSelected().string();
                    cameraFileBrowsers[camIdx].ClearSelected();
                    advancedfx::Message("Overlay: Set campath file for '%s' to '%s'\n", camera.name.c_str(), camera.camPathFile.c_str());
                    try { g_OverlayPaths.campathDir = std::filesystem::path(camera.camPathFile).parent_path().string(); } catch(...) {}
                    ImGui::MarkIniSettingsDirty();
                }
            }

                if (imageFileBrowsers.find(camIdx) != imageFileBrowsers.end()) {
                    if (g_GroupIntoWorkspace && g_WorkspaceViewportId)
                        ImGui::SetNextWindowViewport(g_WorkspaceViewportId);
                    imageFileBrowsers[camIdx].Display();
                    if (imageFileBrowsers[camIdx].HasSelected()) {
                        std::string newImagePath = imageFileBrowsers[camIdx].GetSelected().string();

                        // Release old texture if changing image
                        if (!camera.imagePath.empty() && camera.imagePath != newImagePath) {
                            auto texIt = g_CameraTextures.find(camera.imagePath);
                            if (texIt != g_CameraTextures.end() && texIt->second.srv) {
                                texIt->second.srv->Release();
                                g_CameraTextures.erase(texIt);
                            }
                        }

                        camera.imagePath = newImagePath;

                        // Load texture if not already cached
                        if (g_CameraTextures.find(newImagePath) == g_CameraTextures.end()) {
                            CameraTexture tex;
                            if (LoadImageToTexture(m_Device, newImagePath, tex)) {
                                g_CameraTextures[newImagePath] = tex;
                                advancedfx::Message("Overlay: Loaded image '%s' for camera '%s' (%dx%d)\n",
                                    newImagePath.c_str(), camera.name.c_str(), tex.width, tex.height);
                            } else {
                                advancedfx::Warning("Overlay: Failed to load image '%s'\n", newImagePath.c_str());
                            }
                        }

                        try { g_OverlayPaths.campathDir = std::filesystem::path(newImagePath).parent_path().string(); } catch(...) {}
                        imageFileBrowsers[camIdx].ClearSelected();
                        ImGui::MarkIniSettingsDirty();
                    }
                }

                // Draw progress indicator if this camera is the active one
                if (!camera.camPathFile.empty() && !g_ActiveCameraCampathPath.empty() && 0 == _stricmp(camera.camPathFile.c_str(), g_ActiveCameraCampathPath.c_str())) {
                    size_t cpCount = g_CamPath.GetSize();
                    if (cpCount >= 2) {
                        double tMin = g_CamPath.GetLowerBound();
                        double tMax = g_CamPath.GetUpperBound();
                        double denom = tMax - tMin;
                        if (denom > 1e-9) {
                            double tCur = g_MirvTime.curtime_get() - g_CamPath.GetOffset();
                            float prog = (float)((tCur - tMin) / denom);
                            if (prog < 0.0f) prog = 0.0f; else if (prog > 1.0f) prog = 1.0f;
                            float thickness = 3.0f * g_CameraRectScale;
                            float y = itemMax.y - thickness * 0.5f - 1.0f;
                            ImVec2 p0(itemMin.x, y);
                            ImVec2 p1(itemMin.x + buttonSize.x * prog, y);
                            drawList->AddLine(p0, p1, IM_COL32(220, 64, 64, 255), thickness);
                        }
                    }
                }

                ImGui::PopID();
            }

            // Advance cursor below grid to keep layout consistent
            int rows = (int)((profile.cameras.size() + (size_t)columns - 1) / (size_t)columns);
            float totalHeight = rows * buttonSize.y + (rows > 0 ? (rows - 1) * spacingY : 0.0f);
            ImGui::SetCursorScreenPos(ImVec2(gridOrigin.x, gridOrigin.y + totalHeight));
            ImGui::Dummy(ImVec2(0, 0));
            // Reset drag/click tracking when mouse is released
            if (!ImGui::IsMouseDown(0)) { s_CameraDragActive = false; s_CameraPressedIndex = -1; }
        } else {
            ImGui::TextDisabled("No profile selected. Create or select a profile to add cameras.");
        }

        ImGui::End();
    }

    // Group View Window
    if (g_ShowGroupViewWindow && g_SelectedGroupForView >= 0 && g_SelectedGroupForView < (int)g_CameraGroups.size() &&
        g_SelectedProfileIndex >= 0 && g_SelectedProfileIndex < (int)g_CameraProfiles.size()) {

        ImGui::Begin("Group View", &g_ShowGroupViewWindow);
        CameraGroup& group = g_CameraGroups[g_SelectedGroupForView];
        CameraProfile& profile = g_CameraProfiles[g_SelectedProfileIndex];

        if (group.profileName != profile.name) {
            // Selected group no longer matches current profile; hide window
            g_ShowGroupViewWindow = false;
            ImGui::End();
            return;
        }
        ImGui::Text("Group: %s", group.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%d cameras)", (int)group.cameraIndices.size());

        ImGui::Separator();

        // Display cameras in group (inside a child window that can accept drops)
        ImVec2 baseButton(160.0f, 90.0f);
        ImVec2 buttonSize(baseButton.x * g_CameraRectScale, baseButton.y * g_CameraRectScale);

        ImGui::BeginChild("GroupViewContent", ImVec2(0, 0), true);

        // Make the entire child window a drag-drop target
        if (ImGui::BeginDragDropTarget()) {
            // Accept the same payload used for camera reordering
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HLAE_OBS_CAM_REORDER")) {
                int droppedCamIdx = *(const int*)payload->Data;
                // Check if not already in group
                bool alreadyInGroup = false;
                for (int idx : group.cameraIndices) {
                    if (idx == droppedCamIdx) {
                        alreadyInGroup = true;
                        break;
                    }
                }
                if (!alreadyInGroup && droppedCamIdx >= 0 && droppedCamIdx < (int)profile.cameras.size()) {
                    group.cameraIndices.push_back(droppedCamIdx);
                    advancedfx::Message("Overlay: Added camera '%s' to group '%s'\n",
                                      profile.cameras[droppedCamIdx].name.c_str(), group.name.c_str());
                    ImGui::MarkIniSettingsDirty();
                }
            }
            ImGui::EndDragDropTarget();
        }

        ImGuiStyle& st = ImGui::GetStyle();
        float spacingX = st.ItemSpacing.x;
        float spacingY = st.ItemSpacing.y;
        float contentW = ImGui::GetContentRegionAvail().x;
        int columns = (int)floor((contentW + spacingX) / (buttonSize.x + spacingX));
        if (columns < 1) columns = 1;

        ImVec2 gridOrigin = ImGui::GetCursorScreenPos();
        static int s_GroupCameraDragSource = -1;
        static int s_GroupCameraDragTarget = -1;

        // Track drag state for click vs drag detection
        static bool s_GroupDragActive = false;
        static int s_GroupPressedIndex = -1;
        static ImVec2 s_GroupPressPos = ImVec2(0, 0);

        for (size_t i = 0; i < group.cameraIndices.size(); i++) {
            int camIdx = group.cameraIndices[i];
            if (camIdx < 0 || camIdx >= (int)profile.cameras.size()) continue;

            CameraItem& camera = profile.cameras[camIdx];
            ImGui::PushID((int)i);

            // Grid position
            int col = (int)(i % (size_t)columns);
            int row = (int)(i / (size_t)columns);
            ImVec2 itemMin = ImVec2(
                gridOrigin.x + col * (buttonSize.x + spacingX),
                gridOrigin.y + row * (buttonSize.y + spacingY)
            );
            ImVec2 itemMax = ImVec2(itemMin.x + buttonSize.x, itemMin.y + buttonSize.y);
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Background
            dl->AddRectFilled(itemMin, itemMax, IM_COL32(60, 60, 60, 255));
            dl->AddRect(itemMin, itemMax, IM_COL32(100, 100, 100, 255));

            // Image preview if available
            if (!camera.imagePath.empty()) {
                auto texIt = g_CameraTextures.find(camera.imagePath);
                if (texIt != g_CameraTextures.end() && texIt->second.srv) {
                    // Calculate scaled size to fit within button
                    float scaleX = buttonSize.x / texIt->second.width;
                    float scaleY = buttonSize.y / texIt->second.height;
                    float scale = (scaleX < scaleY) ? scaleX : scaleY;
                    ImVec2 imgSize(texIt->second.width * scale, texIt->second.height * scale);
                    ImVec2 imgPos(itemMin.x + (buttonSize.x - imgSize.x) * 0.5f,
                                    itemMin.y + (buttonSize.y - imgSize.y) * 0.5f);
                    dl->AddImage((ImTextureID)texIt->second.srv, imgPos,
                                        ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y));
                }
            }
            // Unique IDs for this tile
            char id_buf[64];
            _snprintf_s(id_buf, _TRUNCATE, "##groupcam_%zu", i);

            // Remove button (unique)
            char rm_buf[64];
            _snprintf_s(rm_buf, _TRUNCATE, "Remove##%zu", i);
            // Camera name
            float o = std::roundf((std::max)(1.0f, g_CameraRectScale));
            ImVec2 textPos = ImVec2(itemMin.x + 5, itemMin.y + 5);
            dl->AddText(ImVec2(textPos.x + o, textPos.y + o), IM_COL32(0, 0, 0, 200), camera.name.c_str());
            dl->AddText(textPos, IM_COL32(255, 255, 255, 255), camera.name.c_str());
            // Remove button
            float btnY = itemMax.y - 22.0f;
            ImGui::SetCursorScreenPos(ImVec2(itemMin.x + 2, btnY));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            bool removeClicked = ImGui::Button(rm_buf, ImVec2(buttonSize.x - 4, 20));
            ImGui::PopStyleColor();

            if (removeClicked) {
                group.cameraIndices.erase(group.cameraIndices.begin() + i);
                ImGui::MarkIniSettingsDirty();
                ImGui::PopID();
                break;
            }

            // Invisible button for interaction
            ImGui::SetCursorScreenPos(itemMin);
            ImGui::InvisibleButton(id_buf, buttonSize);

            bool didReorderHere = false;

            // Remember press origin to differentiate click vs drag
            if (ImGui::IsItemActivated()) {
                s_GroupPressedIndex = (int)i;
                s_GroupPressPos = ImGui::GetMousePos();
            }

            // Drag source for reordering within group
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
                s_GroupDragActive = true;
                int srcIndex = (int)i;
                ImGui::SetDragDropPayload("GROUP_CAMERA_REORDER", &srcIndex, sizeof(int));
                ImGui::Text("%s", camera.name.c_str());
                ImGui::EndDragDropSource();
            }

            // Drag target for reordering
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GROUP_CAMERA_REORDER")) {
                    int src = *(const int*)payload->Data;
                    int dst = (int)i;
                    if (src >= 0 && src < (int)group.cameraIndices.size() && dst >= 0 && dst < (int)group.cameraIndices.size() && src != dst) {
                        // Move item src -> dst using same logic as main window
                        int movedCamIdx = group.cameraIndices[src];
                        group.cameraIndices.erase(group.cameraIndices.begin() + src);
                        // Insert at dst (works for both directions after erase)
                        int insertIdx = dst;
                        if (insertIdx < 0) insertIdx = 0;
                        if (insertIdx > (int)group.cameraIndices.size()) insertIdx = (int)group.cameraIndices.size();
                        group.cameraIndices.insert(group.cameraIndices.begin() + insertIdx, movedCamIdx);
                        ImGui::MarkIniSettingsDirty();
                        didReorderHere = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }

            if (didReorderHere) { ImGui::PopID(); continue; }

            // On release: treat as click only if cursor hasn't moved (no drag)
            if (ImGui::IsItemDeactivated() && !s_GroupDragActive && s_GroupPressedIndex == (int)i && !removeClicked && !camera.camPathFile.empty()) {
                ImVec2 cur = ImGui::GetMousePos();
                float dx = cur.x - s_GroupPressPos.x;
                float dy = cur.y - s_GroupPressPos.y;
                float dist2 = dx*dx + dy*dy;
                const float clickThreshold = ImGui::GetIO().MouseDragThreshold;
                if (dist2 <= clickThreshold * clickThreshold) {
                    std::string cmd = "mirv_campath clear; mirv_campath load \"" + camera.camPathFile + "\"; mirv_campath enabled 1; spec_mode 4; mirv_input end; mirv_campath select all; mirv_campath edit start";
                    Afx_ExecClientCmd(cmd.c_str());
                    advancedfx::Message("Overlay: [Group View] Loading camera '%s'\n", camera.name.c_str());
                    g_ActiveCameraCampathPath = camera.camPathFile;
                }
            }

            ImGui::PopID();
        }

        // Reset drag tracking when mouse released
        if (!ImGui::IsMouseDown(0)) {
            s_GroupDragActive = false;
            s_GroupPressedIndex = -1;
        }

        // Dummy to reserve grid space
        int rows = ((int)group.cameraIndices.size() + columns - 1) / columns;
        float totalHeight = rows * buttonSize.y + (rows > 0 ? (rows - 1) * spacingY : 0.0f);
        ImGui::SetCursorScreenPos(ImVec2(gridOrigin.x, gridOrigin.y + totalHeight));
        ImGui::Dummy(ImVec2(0, 0));

        ImGui::EndChild(); // End GroupViewContent child

        ImGui::End();
    }

    if (g_ShowMultikillWindow) {
        ImGui::Begin("Event Browser", &g_ShowMultikillWindow, ImGuiWindowFlags_NoCollapse);
        // One-time default parser path: HLAE folder + DemoParser.exe if available
        static bool s_mkInit = false;
        if (!s_mkInit) {
            const wchar_t* hf = GetHlaeFolderW();
            if (hf && *hf) {
                std::wstring w = hf; w += L"x64/DemoParser.exe";
                DWORD attr = GetFileAttributesW(w.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                    int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
                    if (needed > 0) {
                        std::string u; u.resize((size_t)needed - 1);
                        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &u[0], needed, nullptr, nullptr);
                        strncpy_s(g_MkParserPath, u.c_str(), _TRUNCATE);
                    }
                }
            }
            s_mkInit = true;
        }
        // Inputs: parser path and demo path
        ImGuiStyle& stMk = ImGui::GetStyle();
        float availMk = ImGui::GetContentRegionAvail().x;
        float btnW = ImGui::CalcTextSize("Browse...").x + stMk.FramePadding.x * 2.0f;
        float gap = stMk.ItemInnerSpacing.x;
        float boxW = availMk - (btnW + gap);
        if (boxW < 100.0f) boxW = 100.0f;

        ImGui::TextUnformatted("Demo (.dem)");
        ImGui::SetNextItemWidth(boxW);
        ImGui::InputText("##mk_demo", g_MkDemoPath, (int)sizeof(g_MkDemoPath));
        ImGui::SameLine();
        static ImGui::FileBrowser s_mkDialog(
            ImGuiFileBrowserFlags_CloseOnEsc |
            ImGuiFileBrowserFlags_EditPathString |
            ImGuiFileBrowserFlags_CreateNewDir
        );
        static bool s_mkDialogInit = false;
        if (!s_mkDialogInit) { s_mkDialog.SetTitle("Select demo"); s_mkDialogInit = true; }
        if (ImGui::Button("Browse...##mk_demo")) {
            if (!g_OverlayPaths.demoDir.empty()) s_mkDialog.SetDirectory(g_OverlayPaths.demoDir);
            s_mkDialog.Open();
        }
        if (g_GroupIntoWorkspace && g_WorkspaceViewportId)
            ImGui::SetNextWindowViewport(g_WorkspaceViewportId);
        s_mkDialog.Display();
        if (s_mkDialog.HasSelected()) {
            const std::string path = s_mkDialog.GetSelected().string();
            strncpy_s(g_MkDemoPath, path.c_str(), _TRUNCATE);
            try { g_OverlayPaths.demoDir = std::filesystem::path(path).parent_path().string(); } catch(...) {}
            ImGui::MarkIniSettingsDirty();
            s_mkDialog.ClearSelected();
        }

        // Parse button
        bool canParse = !g_MkParsing && g_MkDemoPath[0] != 0 && g_MkParserPath[0] != 0;
        if (!canParse) ImGui::BeginDisabled();
        if (ImGui::Button("Parse demo")) {
            Mk_StartParseThread(std::string(g_MkParserPath), std::string(g_MkDemoPath));
        }
        if (!canParse) ImGui::EndDisabled();
        if (g_MkParsing) {
            ImGui::SameLine();
            ImGui::TextUnformatted("Parsing... please wait");
        }
        // Results
        std::vector<MultikillEvent> snapshot;
        std::string parseErr;
        std::vector<Mk_EventKill> killsnap;
        {
            std::lock_guard<std::mutex> lk(g_MkMutex);
            snapshot = g_MkEvents;
            parseErr = g_MkParseError;
            killsnap = g_MkAllKills;
        }
        if (!g_MkParsing && (!snapshot.empty() || !killsnap.empty())) {
            ImGui::SameLine();
            const char* modes[] = { "Multikills", "Noscope", "Wallbang", "Jumpshot", "Smoke", "Blind", "AWP" };
            int prev = g_MkViewMode;
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("Multikills   ").x + ImGui::GetStyle().FramePadding.x * 6.0f);
            ImGui::Combo("##mk_view", &g_MkViewMode, modes, (int)(sizeof(modes)/sizeof(modes[0])));
            (void)prev;
        }
        if (!parseErr.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255,80,80,255));
            ImGui::TextWrapped("%s", parseErr.c_str());
            ImGui::PopStyleColor();
        }
        if (g_MkViewMode == 0) {
            if (!snapshot.empty()) {
            const ImGuiTableFlags tblFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable;
            if (ImGui::BeginTable("##mk_tbl_all", 4, tblFlags)) {
                // Columns: Round, Player, Kills, Victims, Actions
                ImGui::TableSetupColumn("Round", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000").x + ImGui::GetStyle().FramePadding.x * 2.0f);
                ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Kills", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000").x + ImGui::GetStyle().FramePadding.x * 2.0f);
                ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Goto").x + ImGui::GetStyle().FramePadding.x * 8.0f);
                ImGui::TableHeadersRow();

                // Build order
                std::vector<int> order; order.reserve(snapshot.size());
                for (int i = 0; i < (int)snapshot.size(); ++i) order.push_back(i);

                // Apply sort specs (primary column only)
                if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
                    if (sort->SpecsCount > 0) {
                        const ImGuiTableColumnSortSpecs& sp = sort->Specs[0];
                        const bool asc = (sp.SortDirection == ImGuiSortDirection_Ascending);
                        auto icompare = [&](int a, int b)->bool {
                            const auto& ea = snapshot[(size_t)a];
                            const auto& eb = snapshot[(size_t)b];
                            int r = 0;
                            switch (sp.ColumnIndex) {
                                case 0: // Round
                                    r = (ea.round < eb.round) ? -1 : (ea.round > eb.round ? 1 : 0);
                                    break;
                                case 1: // Player
                                    r = _stricmp(ea.player.c_str(), eb.player.c_str());
                                    break;
                                case 2: // Kills
                                    r = (ea.count < eb.count) ? -1 : (ea.count > eb.count ? 1 : 0);
                                    break;
                                default:
                                    r = 0; break;
                            }
                            return asc ? (r < 0) : (r > 0);
                        };
                        std::stable_sort(order.begin(), order.end(), icompare);
                        sort->SpecsDirty = false;
                    } else {
                        std::stable_sort(order.begin(), order.end(), [&](int a, int b){ return snapshot[(size_t)a].round < snapshot[(size_t)b].round; });
                    }
                } else {
                    std::stable_sort(order.begin(), order.end(), [&](int a, int b){ return snapshot[(size_t)a].round < snapshot[(size_t)b].round; });
                }

                for (int idx : order) {
                    const auto& e = snapshot[(size_t)idx];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%d", e.round);
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(e.player.c_str());
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%d", e.count);
                    if (ImGui::IsItemHovered()) {
                        std::string vj; vj.reserve(64);
                        for (size_t vi = 0; vi < e.victims.size(); ++vi) { if (vi) vj += ", "; vj += e.victims[vi]; }
                        ImGui::SetTooltip("%s", vj.c_str());
                    }
                    ImGui::TableSetColumnIndex(3);
                    char startLbl[64]; _snprintf_s(startLbl, _TRUNCATE, "Goto##mk_%d", idx);
                    static double s_gotoFrame = -1.0;
                    static std::string s_pendingCmd;
                    if (ImGui::SmallButton(startLbl)) {
                        s_gotoFrame = ImGui::GetFrameCount();
                        char cmd1[128]; _snprintf_s(cmd1, _TRUNCATE, "demo_gototick %d", e.startTick - 128);
                        char cmd2[128]; _snprintf_s(cmd2, _TRUNCATE, "spec_player %s; spec_mode 2", e.player.c_str());
                        s_pendingCmd = cmd2;
                        Afx_ExecClientCmd(cmd1);
                    }
                    if (s_gotoFrame >= 0.0 && ImGui::GetFrameCount() > s_gotoFrame) {
                        Afx_ExecClientCmd(s_pendingCmd.c_str());
                        s_pendingCmd.clear();
                        s_gotoFrame = -1.0;
                    }
                }
                ImGui::EndTable();
            }
            }
        } else {
            // Alternate event views based on per-kill flags
            // Build filtered index
            std::vector<int> order; order.reserve(killsnap.size());
            for (int i = 0; i < (int)killsnap.size(); ++i) {
                const auto& k = killsnap[(size_t)i];
                bool keep = false;
                if (g_MkViewMode == 1) keep = k.noscope;
                else if (g_MkViewMode == 2) keep = (k.penetrated > 0);
                else if (g_MkViewMode == 3) keep = k.inAir;
                else if (g_MkViewMode == 4) keep = k.smoke;
                else if (g_MkViewMode == 5) keep = k.blind;
                else if (g_MkViewMode == 6) keep = (k.weapon == "awp");
                if (keep) order.push_back(i);
            }
            if (!order.empty()) {
                const ImGuiTableFlags tblFlags2 = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable;
                if (ImGui::BeginTable("##mk_tbl_kills", 4, tblFlags2)) {
                    ImGui::TableSetupColumn("Round", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000").x + ImGui::GetStyle().FramePadding.x * 2.0f);
                    ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Victim", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Goto").x + ImGui::GetStyle().FramePadding.x * 8.0f);
                    ImGui::TableHeadersRow();

                    if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
                        if (sort->SpecsCount > 0) {
                            const ImGuiTableColumnSortSpecs& sp = sort->Specs[0];
                            const bool asc = (sp.SortDirection == ImGuiSortDirection_Ascending);
                            auto cmp = [&](int a, int b){
                                const auto &ka = killsnap[(size_t)a], &kb = killsnap[(size_t)b];
                                int r = 0;
                                switch (sp.ColumnIndex) {
                                    case 0: r = (ka.round < kb.round) ? -1 : (ka.round > kb.round ? 1 : 0); break;
                                    case 1: r = _stricmp(ka.attackerName.c_str(), kb.attackerName.c_str()); break;
                                    case 2: r = _stricmp(ka.victimName.c_str(), kb.victimName.c_str()); break;
                                    default: r = 0; break;
                                }
                                return asc ? (r < 0) : (r > 0);
                            };
                            std::stable_sort(order.begin(), order.end(), cmp);
                            sort->SpecsDirty = false;
                        } else {
                            std::stable_sort(order.begin(), order.end(), [&](int a, int b){ return killsnap[(size_t)a].round < killsnap[(size_t)b].round; });
                        }
                    }

                    for (int idx : order) {
                        const auto& k = killsnap[(size_t)idx];
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::Text("%d", k.round);
                        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(k.attackerName.c_str());
                        ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(k.victimName.c_str());
                        ImGui::TableSetColumnIndex(3);
                        char gotoLbl[64]; _snprintf_s(gotoLbl, _TRUNCATE, "Goto##mk_k_%d", idx);
                        static double s2_gotoFrame = -1.0; static std::string s2_pending;
                        if (ImGui::SmallButton(gotoLbl)) {
                            s2_gotoFrame = ImGui::GetFrameCount();
                            char cmd1[128]; _snprintf_s(cmd1, _TRUNCATE, "demo_gototick %d", k.tick - 128);
                            char cmd2[256]; _snprintf_s(cmd2, _TRUNCATE, "spec_player %s; spec_mode 2", k.attackerName.c_str());
                            s2_pending = cmd2;
                            Afx_ExecClientCmd(cmd1);
                        }
                        if (s2_gotoFrame >= 0.0 && ImGui::GetFrameCount() > s2_gotoFrame) {
                            Afx_ExecClientCmd(s2_pending.c_str()); s2_pending.clear(); s2_gotoFrame = -1.0;
                        }
                    }

                    ImGui::EndTable();
                }
            }
        }
        // Ensure background parser thread is joined after completion to avoid Debug CRT abort on joinable destructor.
        if (!g_MkParsing && g_MkWorker.joinable()) {
            g_MkWorker.join();
        }
        ImGui::End();
    }

    if (g_ShowBackbufferWindow) {
        ImGui::SetNextWindowSize(ImVec2(480.0f, 320.0f), ImGuiCond_FirstUseEver);
        const bool viewportVisible = ImGui::Begin("Viewport", &g_ShowBackbufferWindow, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);
        if (!g_ShowBackbufferWindow) {
            g_ViewportPlayersMenuOpen = false;
        }

        if (viewportVisible) {
            UpdateViewportPlayerCache();

            if (!m_BackbufferPreview.srv || m_BackbufferPreview.width == 0 || m_BackbufferPreview.height == 0) {
                ImGui::TextDisabled("Waiting for viewport source...");
            } else {
                const bool msaa = m_BackbufferPreview.isMsaa;

                // Fit the backbuffer image into the available content region while preserving aspect ratio
                ImVec2 avail = ImGui::GetContentRegionAvail();
                if (avail.x < 1.0f) avail.x = 1.0f;
                if (avail.y < 1.0f) avail.y = 1.0f;
                const float srcW = (float)m_BackbufferPreview.width;
                const float srcH = (float)m_BackbufferPreview.height;
                const float sx = avail.x / srcW;
                const float sy = avail.y / srcH;
                const float fit = sx < sy ? sx : sy;
                ImVec2 previewSize(srcW * fit, srcH * fit);
                // Reserve layout space and capture item rect
                ImVec2 pMin = ImGui::GetCursorScreenPos();
                // Allow items to overlap this button so controls can be placed on top
                ImGui::InvisibleButton("##viewport_image", previewSize, ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_AllowOverlap);
                ImVec2 imgMin = ImGui::GetItemRectMin();
                ImVec2 imgMax = ImGui::GetItemRectMax();
                // Draw with opaque blend override to avoid alpha-related dropouts
                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (dl) {
                    dl->AddCallback(ImGui_SetOpaqueBlendCallback, nullptr);
                    dl->AddImage((ImTextureID)m_BackbufferPreview.srv, imgMin, imgMax);
                    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
                }
                g_PreviewRectValid = true;
                g_PreviewRectMin = imgMin;
                g_PreviewRectSize = ImVec2(imgMax.x - imgMin.x, imgMax.y - imgMin.y);
                g_PreviewDrawList = ImGui::GetWindowDrawList();

                // Draw native HUD inside the viewport image
                if (g_ShowHud && g_PreviewRectValid && g_PreviewDrawList) {
                    Hud::Viewport vp{ g_PreviewRectMin, g_PreviewRectSize };
                    auto opt = g_GsiServer.TryGetHudState();
                    if (opt.has_value()) {
                        Hud::RenderAll(g_PreviewDrawList, vp, *opt, &g_HudPlayerRects);
                    } else {
                        // Fallback/demo data if no websocket
                        Hud::State st{};
                        st.leftTeam.name = "TERRORISTS";
                        st.leftTeam.side = 2;
                        st.leftTeam.score = 0;
                        st.leftTeam.timeoutsLeft = 0;
                        st.leftTeam.color = IM_COL32(219,170,98,255);
                        st.rightTeam.name = "COUNTER-TERRORISTS";
                        st.rightTeam.side = 3;
                        st.rightTeam.score = 0;
                        st.rightTeam.timeoutsLeft = 0;
                        st.rightTeam.color = IM_COL32(125,168,198,255);
                        for (int i=0;i<5;i++) {
                            Hud::Player p{}; p.id = 100+i; p.name = std::string("T Player ")+char('A'+i); p.observerSlot = i+1; p.teamSide=2; p.health=100-(i*7); p.armor = (i*20)%100; p.kills=i; p.deaths=i/2; p.money=800+i*400; p.isAlive = (i!=4); st.leftPlayers.push_back(p);
                        }
                        for (int i=0;i<5;i++) {
                            Hud::Player p{}; p.id = 200+i; p.name = std::string("CT Player ")+char('A'+i); p.observerSlot = i+6; p.teamSide=3; p.health=100-(i*9); p.armor = (i*25)%100; p.kills=i; p.deaths=i/3; p.money=900+i*350; p.isAlive = (i!=0); st.rightPlayers.push_back(p);
                        }
                        st.leftPlayers[1].isFocused = true; st.focusedPlayerId = st.leftPlayers[1].id;
                        st.round.number = 5; st.round.phase = "Please start GSI server"; st.round.timeLeft = 73.0f;
                        Hud::RenderAll(g_PreviewDrawList, vp, st, &g_HudPlayerRects);
                    }
                } else {
                    g_HudPlayerRects.clear();
                }
                const ImGuiHoveredFlags hoverFlags = ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_AllowWhenOverlapped;
                bool hoveredImage = ImGui::IsItemHovered(hoverFlags);

                // Handle right-clicks on HUD player rows (only when RMB control disabled)
                if (hoveredImage && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsKeyDown(ImGuiKey_LeftCtrl)) {
                    ImVec2 mousePos = ImGui::GetMousePos();
                    for (const auto& rect : g_HudPlayerRects) {
                        if (mousePos.x >= rect.min.x && mousePos.x <= rect.max.x &&
                            mousePos.y >= rect.min.y && mousePos.y <= rect.max.y) {
                            g_BirdContextTargetObserverSlot = rect.observerSlot;
                            g_BirdContextTargetPlayerId = rect.playerId;
                            g_BirdContextMenuOpen = true;
                            ImGui::OpenPopup("##bird_context");
                            break;
                        }
                    }
                }

                if (hoveredImage && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsKeyDown(ImGuiKey_LeftCtrl)) {
                    ImVec2 mousePos = ImGui::GetMousePos();
                    for (const auto& rect : g_HudPlayerRects) {
                        if (mousePos.x >= rect.min.x && mousePos.x <= rect.max.x &&
                            mousePos.y >= rect.min.y && mousePos.y <= rect.max.y) {
                            g_BirdContextTargetObserverSlot = rect.observerSlot;
                            g_BirdContextTargetPlayerId = rect.playerId;
                            int keyNum = (g_BirdContextTargetObserverSlot == 9) ? 0 : (g_BirdContextTargetObserverSlot + 1);
                            int toIdx = -1;
                            auto it = g_ObservingHotkeyBindings.find(keyNum);
                            if (it != g_ObservingHotkeyBindings.end()) {
                                toIdx = it->second;
                            }
                            char cmd[128];
                            _snprintf_s(cmd, _TRUNCATE, "spec_mode 2; spec_player %d; mirv_campath enabled 0; mirv_input end", toIdx);
                            Afx_ExecClientCmd(cmd);
                        }
                    }
                }
                // Smooth camera mode state
                static double s_smoothTargetX = 0.0, s_smoothTargetY = 0.0, s_smoothTargetZ = 0.0;
                static double s_smoothTargetRx = 0.0, s_smoothTargetRy = 0.0, s_smoothTargetRz = 0.0;
                static float s_smoothTargetFov = 90.0f;
                static double s_smoothCurrentX = 0.0, s_smoothCurrentY = 0.0, s_smoothCurrentZ = 0.0;
                static double s_smoothCurrentRx = 0.0, s_smoothCurrentRy = 0.0, s_smoothCurrentRz = 0.0;
                static float s_smoothCurrentFov = 90.0f;
                static bool s_smoothInitialized = false;
                // Bird camera context menu - velocity-based smooth tracking
                if (g_GetSmoothPass && g_GetSmoothFirstFrame && ImGui::GetTime() - g_GetSmoothTime >= 0.5) {
                    // Second frame: fetch position again and calculate velocity
                    float pos[3] = {0, 0, 0};
                    if (g_pEntityList && *g_pEntityList && g_GetEntityFromIndex) {
                        CEntityInstance* controller = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, g_GetSmoothIndex);
                        if (controller && controller->IsPlayerController()) {
                            auto pawnHandle = controller->GetPlayerPawnHandle();
                            if (pawnHandle.IsValid()) {
                                int pawnIdx = pawnHandle.GetEntryIndex();
                                CEntityInstance* pawn = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, pawnIdx);
                                if (pawn && pawn->IsPlayerPawn()) {
                                    pawn->GetRenderEyeOrigin(pos);

                                    // Calculate time delta and velocity
                                    float deltaTime = ImGui::GetTime() - g_GetSmoothTime;
                                    float velocity[3] = {
                                        (pos[0] - g_GetSmoothFirstPos[0]) / deltaTime,
                                        (pos[1] - g_GetSmoothFirstPos[1]) / deltaTime,
                                        (pos[2] - g_GetSmoothFirstPos[2]) / deltaTime
                                    };

                                    // Calculate movement direction vector for angle
                                    float moveDir[3] = {
                                        pos[0] - g_GetSmoothFirstPos[0],
                                        pos[1] - g_GetSmoothFirstPos[1],
                                        pos[2] - g_GetSmoothFirstPos[2]
                                    };
                                    // Set current position and angles on second frame
                                    s_smoothCurrentX = g_GetSmoothFirstPos[0];
                                    s_smoothCurrentY = g_GetSmoothFirstPos[1];
                                    s_smoothCurrentZ = g_GetSmoothFirstPos[2];

                                    s_smoothCurrentFov = 90.0f;
                                    // Calculate yaw and pitch from movement direction
                                    // Only update angle if there's significant movement
                                    float moveMagnitude = sqrtf(moveDir[0]*moveDir[0] + moveDir[1]*moveDir[1] + moveDir[2]*moveDir[2]);
                                    if (moveMagnitude > 0.01f) {
                                        // Normalize movement direction
                                        moveDir[0] /= moveMagnitude;
                                        moveDir[1] /= moveMagnitude;
                                        moveDir[2] /= moveMagnitude;

                                        // Calculate angles from direction vector
                                        // yaw = atan2(y, x) in degrees
                                        // pitch = asin(z) in degrees
                                        float targetYaw = atan2f(moveDir[1], moveDir[0]) * (180.0f / 3.14159265359f);
                                        float targetPitch = asinf(-moveDir[2]) * (180.0f / 3.14159265359f);
                                        s_smoothCurrentRy = targetYaw;
                                        s_smoothCurrentRx = targetPitch;
                                        s_smoothCurrentRz = 0.0f;
                                        s_smoothTargetRy = targetYaw;
                                        s_smoothTargetRx = targetPitch;
                                        s_smoothTargetRz = 0.0f;
                                    }

                                    // Apply velocity-based target with some lookahead (0.5 seconds)
                                    float lookahead = 0.5f;
                                    s_smoothTargetX = pos[0] + velocity[0] * lookahead;
                                    s_smoothTargetY = pos[1] + velocity[1] * lookahead;
                                    s_smoothTargetZ = pos[2] + velocity[2] * lookahead;
                                    s_smoothTargetFov = 90.0f;

                                    // Calculate and set appropriate keyboard sensitivity based on player velocity
                                    if (MirvInput* pMirv = Afx_GetMirvInput()) {
                                        float velocityMagnitude = sqrtf(velocity[0]*velocity[0] + velocity[1]*velocity[1] + velocity[2]*velocity[2]);

                                        // Only set if there's significant movement
                                        if (velocityMagnitude > 1.0f) {
                                            // Calculate required ksens: velocity = ksens * KeyboardForwardSpeed
                                            // So: ksens = velocity / KeyboardForwardSpeed
                                            double forwardSpeed = pMirv->KeyboardForwardSpeed_get();
                                            if (forwardSpeed > 0.001) {
                                                double calculatedKsens = (double)velocityMagnitude / forwardSpeed;
                                                calculatedKsens = std::clamp(calculatedKsens, 0.01, 100.0);
                                                pMirv->SetKeyboardSensitivity(calculatedKsens);
                                                g_uiKsens = (float)calculatedKsens;
                                                g_uiKsensInit = true;
                                            }
                                        }
                                    }

                                    g_GetSmoothPass = false;
                                    g_GetSmoothFirstFrame = false;
                                }
                            }
                        }
                    }
                }
                if (ImGui::BeginPopup("##bird_context")) {
                    if (ImGui::MenuItem("ToBird")) {
                        // Get current focused player's controller index from HUD
                        int fromIdx = GetFocusedPlayerControllerIndex();

                        // Map observer slot to controller index using hotkey bindings
                        // observerSlot from HUD is 0-9, but bindings use keyboard format 1-9,0
                        // So we need to convert: 0->1, 1->2, ..., 8->9, 9->0
                        int keyNum = (g_BirdContextTargetObserverSlot == 9) ? 0 : (g_BirdContextTargetObserverSlot + 1);
                        int toIdx = -1;
                        auto it = g_ObservingHotkeyBindings.find(keyNum);
                        if (it != g_ObservingHotkeyBindings.end()) {
                            toIdx = it->second;
                        }

                        advancedfx::Message("BirdCamera: ToBird clicked - fromIdx=%d, toIdx=%d, observerSlot=%d, keyNum=%d\n",
                                  fromIdx, toIdx, g_BirdContextTargetObserverSlot, keyNum);

                        if (fromIdx >= 0 && toIdx >= 0) {
                            advancedfx::overlay::BirdCamera_StartGoto(fromIdx, toIdx, 1000.0f);
                        } else {
                            if (fromIdx < 0) {
                                advancedfx::Message("BirdCamera: Failed to get focused player (fromIdx=%d). Make sure HUD and GSI are working.\n", fromIdx);
                            }
                            if (toIdx < 0) {
                                advancedfx::Message("BirdCamera: Failed to get target player (toIdx=%d, observerSlot=%d, keyNum=%d). Make sure Observing Bindings are configured.\n",
                                                  toIdx, g_BirdContextTargetObserverSlot, keyNum);
                            }
                        }
                        g_BirdContextMenuOpen = false;
                    }
                    if (ImGui::MenuItem("GetSmooth")) {
                        float pos[3] = {0, 0, 0};

                        int keyNum = (g_BirdContextTargetObserverSlot == 9) ? 0 : (g_BirdContextTargetObserverSlot + 1);
                        int clickIdx = -1;
                        auto it = g_ObservingHotkeyBindings.find(keyNum);
                        if (it != g_ObservingHotkeyBindings.end()) {
                            clickIdx = it->second;
                        }
                        g_GetSmoothIndex = clickIdx;
                        if (g_pEntityList && *g_pEntityList && g_GetEntityFromIndex) {
                            CEntityInstance* controller = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, clickIdx);
                            if (controller && controller->IsPlayerController()) {
                                auto pawnHandle = controller->GetPlayerPawnHandle();
                                if (pawnHandle.IsValid()) {
                                    int pawnIdx = pawnHandle.GetEntryIndex();
                                    CEntityInstance* pawn = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, pawnIdx);
                                    if (pawn && pawn->IsPlayerPawn()) {
                                        // Store first position for velocity calculation
                                        pawn->GetRenderEyeOrigin(pos);
                                        g_GetSmoothFirstPos[0] = pos[0];
                                        g_GetSmoothFirstPos[1] = pos[1];
                                        g_GetSmoothFirstPos[2] = pos[2];

                                        // Mark for next-frame capture (don't set s_smoothCurrent yet)
                                        g_GetSmoothPass = true;
                                        g_GetSmoothFirstFrame = true;
                                        g_GetSmoothTime = ImGui::GetTime();
                                    }
                                }
                            }
                        }
                    }
                    ImGui::EndPopup();
                } else if (g_BirdContextMenuOpen) {
                    g_BirdContextMenuOpen = false;
                }



                // RMB-native control over Mirv camera inside preview image
                static bool s_bbCtrlActive = false;
                static bool s_bbSkipFirstDelta = false;
                static POINT s_bbCtrlSavedCursor = {0,0};



                if (g_ViewportEnableRmbControl) {
                    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                        if (s_bbCtrlActive) {
                            // End control: restore cursor position and visibility
                            ShowCursor(TRUE);
                            g_pendingWarpPt.x = s_bbCtrlSavedCursor.x;
                            g_pendingWarpPt.y = s_bbCtrlSavedCursor.y;
                            g_hasPendingWarp  = true;
                            s_bbCtrlActive = false;
                            // In smooth mode, don't stop immediately - let interpolation finish
                        }
                    } else if (hoveredImage && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    // Begin control: save cursor, hide it
                        GetCursorPos(&s_bbCtrlSavedCursor);
                        ShowCursor(FALSE);
                        {
                            ImVec2 itemMin = ImGui::GetItemRectMin();
                            ImVec2 itemMax = ImGui::GetItemRectMax();
                            ImVec2 itemCenter = ImVec2((itemMin.x + itemMax.x) * 0.5f, (itemMin.y + itemMax.y) * 0.5f);
                            POINT ptCenterScreen = { (LONG)itemCenter.x, (LONG)itemCenter.y };
                            HWND ownerHwnd = m_Hwnd;
                            if (ImGui::GetWindowViewport() && ImGui::GetWindowViewport()->PlatformHandleRaw)
                                ownerHwnd = (HWND)ImGui::GetWindowViewport()->PlatformHandleRaw;
                            // Also compute client center for later deltas
                            POINT ptCenterClient = ptCenterScreen; ScreenToClient(ownerHwnd, &ptCenterClient);
                            (void)ptCenterClient; // silence unused warning in release
                            // Lock immediately to center of image in screen space
                            ::SetCursorPos(ptCenterScreen.x, ptCenterScreen.y);
                        }
                        s_bbCtrlActive = true;
                        s_bbSkipFirstDelta = true;
                    }

                    // Handle input when RMB is held
                    if (s_bbCtrlActive) {
                        // Compute center of the preview in client + screen coords
                        ImVec2 itemMin = ImGui::GetItemRectMin();
                        ImVec2 itemMax = ImGui::GetItemRectMax();
                        ImVec2 itemCenter = ImVec2((itemMin.x + itemMax.x) * 0.5f, (itemMin.y + itemMax.y) * 0.5f);

                        POINT ptCenterScreen = { (LONG)itemCenter.x, (LONG)itemCenter.y };
                        HWND ownerHwnd = m_Hwnd;
                        if (ImGui::GetWindowViewport() && ImGui::GetWindowViewport()->PlatformHandleRaw)
                            ownerHwnd = (HWND)ImGui::GetWindowViewport()->PlatformHandleRaw;
                        // Convert to client coordinates for delta computation
                        POINT ptCenterClient = ptCenterScreen; ScreenToClient(ownerHwnd, &ptCenterClient);

                        // Read current cursor and compute delta relative to center in client space
                        POINT ptScreen; GetCursorPos(&ptScreen);
                        POINT ptClient = ptScreen; ScreenToClient(ownerHwnd, &ptClient);
                        float dx = (float)(ptClient.x - ptCenterClient.x);
                        float dy = (float)(ptClient.y - ptCenterClient.y);
                        // Lock cursor to preview center
                        if (s_bbSkipFirstDelta) { dx = 0.0f; dy = 0.0f; s_bbSkipFirstDelta = false; }
                        ::SetCursorPos(ptCenterScreen.x, ptCenterScreen.y);
                        ImGuiIO& io = ImGui::GetIO();
                        float wheel = io.MouseWheel;
                        if (MirvInput* pMirv = Afx_GetMirvInput()) {
                            pMirv->SetCameraControlMode(true);
                            int fromIdx = GetFocusedPlayerControllerIndex();
                            if (fromIdx != -1) Afx_ExecClientCmd("spec_mode 4");

                            // Get current camera data
                            double cx,cy,cz, rx,ry,rz; float fov;
                            Afx_GetLastCameraData(cx,cy,cz, rx,ry,rz, fov);

                            // Initialize smooth mode targets on first run or when switching modes
                            if (!s_smoothInitialized) {
                                s_smoothTargetX = s_smoothCurrentX = cx;
                                s_smoothTargetY = s_smoothCurrentY = cy;
                                s_smoothTargetZ = s_smoothCurrentZ = cz;
                                s_smoothTargetRx = s_smoothCurrentRx = rx;
                                s_smoothTargetRy = s_smoothCurrentRy = ry;
                                s_smoothTargetRz = s_smoothCurrentRz = rz;
                                s_smoothTargetFov = s_smoothCurrentFov = fov;
                                s_smoothInitialized = true;
                            }

                            const float dt = ImGui::GetIO().DeltaTime;
                            const double sens = pMirv->GetMouseSensitivty();
                            const double yawS = pMirv->MouseYawSpeed_get();
                            const double pitchS = pMirv->MousePitchSpeed_get();
                            const double lookScale = (double)(g_PreviewLookScale * g_PreviewLookMultiplier);

                            // Base: yaw/pitch from mouse
                            const double dYaw = sens * yawS * (double)(-dx) * lookScale;
                            const double dPitch = sens * pitchS * (double)(dy) * lookScale;

                            if (g_ViewportSmoothMode) {
                                // ===== SMOOTH MODE INPUT HANDLING =====
                                // Modifiers: Shift=FOV (with scroll), R=up, F=down
                                const bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                                const bool spaceHeld = (GetKeyState(VK_SPACE) & 0x8000) != 0;
                                const bool ctrlHeld = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

                                // Handle scroll wheel
                                if (wheel != 0.0f) {
                                    int clicks = (wheel > 0.f) ? (int)floorf(wheel + 0.0001f)
                                                            : (int)ceilf (wheel - 0.0001f);

                                    if (shiftHeld) {
                                        // Shift scroll: adjust keyboard sensitivity (movement speed)
                                        double ks = pMirv->GetKeyboardSensitivty();
                                        const double up   = 1.0 + (double)g_ViewportSmoothScrollSpeedIncrement;
                                        const double down = 1.0 / up;
                                        if (clicks > 0) for (int i = 0; i < clicks; ++i) ks *= up;
                                        if (clicks < 0) for (int i = 0; i < -clicks; ++i) ks *= down;
                                        ks = std::clamp(ks, 0.01, 100.0);
                                        pMirv->SetKeyboardSensitivity(ks);
                                        g_uiKsens = (float)ks; g_uiKsensInit = true;
                                    } else {
                                        // scroll: adjust FOV
                                        const float fovDelta = clicks * g_ViewportSmoothScrollFovIncrement;
                                        s_smoothTargetFov = std::clamp(s_smoothTargetFov + fovDelta, 1.0f, 179.0f);
                                    }
                                }

                                // Update target angles from mouse movement (always allow mouse look)
                                s_smoothTargetRy += dYaw;
                                s_smoothTargetRx += dPitch;

                                // WASD + R/F movement (camera space)
                                double moveF = 0.0, moveR = 0.0, moveU = 0.0;

                                if (g_ViewportSmoothAnalogInput) {
                                    // Analog input mode: read gamepad stick values
                                    // Left stick: WASD (forward/back, right/left)
                                    // Right stick: Space/Ctrl (up/down)
                                    const ImGuiIO& io = ImGui::GetIO();
                                    const int lStickUpIdx = ImGuiKey_GamepadLStickUp - ImGuiKey_NamedKey_BEGIN;
                                    const int lStickDownIdx = ImGuiKey_GamepadLStickDown - ImGuiKey_NamedKey_BEGIN;
                                    const int lStickRightIdx = ImGuiKey_GamepadLStickRight - ImGuiKey_NamedKey_BEGIN;
                                    const int lStickLeftIdx = ImGuiKey_GamepadLStickLeft - ImGuiKey_NamedKey_BEGIN;
                                    const int rStickUpIdx = ImGuiKey_GamepadRStickUp - ImGuiKey_NamedKey_BEGIN;
                                    const int rStickDownIdx = ImGuiKey_GamepadRStickDown - ImGuiKey_NamedKey_BEGIN;

                                    // Left stick Y-axis: forward/backward (up = W, down = S)
                                    float lStickUp = io.KeysData[lStickUpIdx].AnalogValue;
                                    float lStickDown = io.KeysData[lStickDownIdx].AnalogValue;
                                    moveF += lStickUp * pMirv->KeyboardForwardSpeed_get();
                                    moveF -= lStickDown * pMirv->KeyboardBackwardSpeed_get();

                                    // Left stick X-axis: right/left (right = D, left = A)
                                    float lStickRight = io.KeysData[lStickRightIdx].AnalogValue;
                                    float lStickLeft = io.KeysData[lStickLeftIdx].AnalogValue;
                                    moveR += lStickRight * pMirv->KeyboardRightSpeed_get();
                                    moveR -= lStickLeft * pMirv->KeyboardLeftSpeed_get();

                                    // Right stick Y-axis: up/down (up = Space, down = Ctrl)
                                    float rStickUp = io.KeysData[rStickUpIdx].AnalogValue;
                                    float rStickDown = io.KeysData[rStickDownIdx].AnalogValue;
                                    moveU += rStickUp * pMirv->KeyboardForwardSpeed_get();
                                    moveU -= rStickDown * pMirv->KeyboardForwardSpeed_get();
                                } else {
                                    // Regular keyboard input mode
                                    if ((GetKeyState('W') & 0x8000) != 0) moveF += pMirv->KeyboardForwardSpeed_get();
                                    if ((GetKeyState('S') & 0x8000) != 0) moveF -= pMirv->KeyboardBackwardSpeed_get();
                                    if ((GetKeyState('D') & 0x8000) != 0) moveR += pMirv->KeyboardRightSpeed_get();
                                    if ((GetKeyState('A') & 0x8000) != 0) moveR -= pMirv->KeyboardLeftSpeed_get();

                                    // Space/Ctrl for up/down movement
                                    if (spaceHeld) moveU += pMirv->KeyboardForwardSpeed_get();
                                    if (ctrlHeld) moveU -= pMirv->KeyboardForwardSpeed_get();
                                }

                                if (moveF != 0.0 || moveR != 0.0 || moveU != 0.0) {
                                    const double ksens = pMirv->GetKeyboardSensitivty();
                                    const double speedF = ksens * moveF * (double)dt;
                                    const double speedR = ksens * moveR * (double)dt;
                                    const double speedU = ksens * moveU * (double)dt;

                                    // Use current target angles for movement direction
                                    double fwdQ[3], rightQ[3], upQ[3];
                                    Afx::Math::MakeVectors(/*roll*/0.0, /*pitch*/s_smoothTargetRx, /*yaw*/s_smoothTargetRy, fwdQ, rightQ, upQ);

                                    // Update position targets
                                    // WASD: forward/right movement in camera space
                                    // R/F: up/down movement in camera's local space (using camera's up vector)
                                    s_smoothTargetX += fwdQ[0] * speedF + rightQ[0] * speedR + upQ[0] * speedU;
                                    s_smoothTargetY += fwdQ[1] * speedF + rightQ[1] * speedR + upQ[1] * speedU;
                                    s_smoothTargetZ += fwdQ[2] * speedF + rightQ[2] * speedR + upQ[2] * speedU;
                                }

                            } else {
                                // ===== REGULAR MODE (original behavior) =====
                                if (wheel != 0.0f) {
                                    int clicks = (wheel > 0.f) ? (int)floorf(wheel + 0.0001f)
                                                            : (int)ceilf (wheel - 0.0001f);
                                    double ks = pMirv->GetKeyboardSensitivty();

                                    const double up   = 1.10;
                                    const double down = 1.0 / up;

                                    if (clicks > 0) for (int i = 0; i < clicks; ++i) ks *= up;
                                    if (clicks < 0) for (int i = 0; i < -clicks; ++i) ks *= down;

                                    // Clamp to your preferred range
                                    ks = std::clamp(ks, 0.01, 100.0);

                                    pMirv->SetKeyboardSensitivity(ks);
                                    g_uiKsens = (float)ks; g_uiKsensInit = true; // sync UI
                                }

                                // Modifiers: R=roll, F=fov
                                const bool rHeld = (GetKeyState('R') & 0x8000) != 0;
                                const bool fHeld = (GetKeyState('F') & 0x8000) != 0;

                                double curYaw = ry;
                                double curPitch = rx;
                                if (!rHeld && !fHeld) {
                                    curYaw += dYaw;
                                    curPitch += dPitch;
                                    pMirv->SetRy((float)curYaw);
                                    pMirv->SetRx((float)curPitch);
                                } else if (rHeld) {
                                    pMirv->SetRz((float)(rz + sens * yawS * (double)(dx) * lookScale));
                                } else if (fHeld) {
                                    pMirv->SetFov((float)(fov + sens * yawS * (double)(-dx) * lookScale));
                                }

                                // WASD movement (camera space), scaled by keyboard sensitivity and speeds
                                double moveF = 0.0, moveR = 0.0;
                                if ((GetKeyState('W') & 0x8000) != 0) moveF += pMirv->KeyboardForwardSpeed_get();
                                if ((GetKeyState('S') & 0x8000) != 0) moveF -= pMirv->KeyboardBackwardSpeed_get();
                                if ((GetKeyState('D') & 0x8000) != 0) moveR += pMirv->KeyboardRightSpeed_get();
                                if ((GetKeyState('A') & 0x8000) != 0) moveR -= pMirv->KeyboardLeftSpeed_get();

                                if (moveF != 0.0 || moveR != 0.0) {
                                    const double ksens = pMirv->GetKeyboardSensitivty();
                                    const double speedF = ksens * moveF * (double)dt;
                                    const double speedR = ksens * moveR * (double)dt;
                                    // Use HLAE's vector builder (Z-up, Source-like) from angles (roll=0, pitch, yaw)
                                    double fwdQ[3], rightQ[3], upQ[3];
                                    Afx::Math::MakeVectors(/*roll*/0.0, /*pitch*/curPitch, /*yaw*/curYaw, fwdQ, rightQ, upQ);

                                    const double nx = cx + fwdQ[0] * speedF + rightQ[0] * speedR;
                                    const double ny = cy + fwdQ[1] * speedF + rightQ[1] * speedR;
                                    const double nz = cz + fwdQ[2] * speedF + rightQ[2] * speedR;
                                    pMirv->SetTx((float)nx);
                                    pMirv->SetTy((float)ny);
                                    pMirv->SetTz((float)nz);
                                }

                                // Reset smooth mode initialization when not in smooth mode
                                s_smoothInitialized = false;
                            }
                        }


                    }

                    // Apply smooth camera interpolation (runs even when RMB is released)
                    if (g_ViewportSmoothMode && s_smoothInitialized) {
                        if (MirvInput* pMirv = Afx_GetMirvInput()) {
                            bool camEnabled = pMirv->GetCameraControlMode();
                            if (camEnabled) {
                                const float dt = ImGui::GetIO().DeltaTime;

                                // Apply exponential smoothing with individual halftimes
                                const double smoothFactorPos = 1.0 - pow(0.5, (double)dt / (double)g_ViewportSmoothHalftimePos);
                                const double smoothFactorAngle = 1.0 - pow(0.5, (double)dt / (double)g_ViewportSmoothHalftimeAngle);
                                const double smoothFactorFov = 1.0 - pow(0.5, (double)dt / (double)g_ViewportSmoothHalftimeFov);

                                // Interpolate position
                                s_smoothCurrentX  += (s_smoothTargetX  - s_smoothCurrentX)  * smoothFactorPos;
                                s_smoothCurrentY  += (s_smoothTargetY  - s_smoothCurrentY)  * smoothFactorPos;
                                s_smoothCurrentZ  += (s_smoothTargetZ  - s_smoothCurrentZ)  * smoothFactorPos;

                                // Interpolate angles
                                s_smoothCurrentRx += (s_smoothTargetRx - s_smoothCurrentRx) * smoothFactorAngle;
                                s_smoothCurrentRy += (s_smoothTargetRy - s_smoothCurrentRy) * smoothFactorAngle;
                                s_smoothCurrentRz += (s_smoothTargetRz - s_smoothCurrentRz) * smoothFactorAngle;

                                // Interpolate FOV
                                s_smoothCurrentFov += (s_smoothTargetFov - s_smoothCurrentFov) * smoothFactorFov;

                                // Apply smoothed values to camera
                                pMirv->SetTx((float)s_smoothCurrentX);
                                pMirv->SetTy((float)s_smoothCurrentY);
                                pMirv->SetTz((float)s_smoothCurrentZ);
                                pMirv->SetRx((float)s_smoothCurrentRx);
                                pMirv->SetRy((float)s_smoothCurrentRy);
                                pMirv->SetRz((float)s_smoothCurrentRz);
                                pMirv->SetFov(s_smoothCurrentFov);
                            }
                        }
                    }
                } else {
                    // RMB control disabled: ensure camera control is released if it was active
                    if (s_bbCtrlActive) {
                        ShowCursor(TRUE);
                        g_pendingWarpPt.x = s_bbCtrlSavedCursor.x;
                        g_pendingWarpPt.y = s_bbCtrlSavedCursor.y;
                        g_hasPendingWarp  = true;
                        s_bbCtrlActive = false;
                    }
                }
            }

            ImVec2 contentMin = ImGui::GetWindowContentRegionMin();
            ImVec2 contentMax = ImGui::GetWindowContentRegionMax();
            const float toggleHeight = ImGui::GetFrameHeight();
            const float spacingY = ImGui::GetStyle().ItemSpacing.y;
            const float contentWidth = (std::max)(0.0f, contentMax.x - contentMin.x);
            const float availableHeight = (std::max)(0.0f, contentMax.y - contentMin.y);
            const float maxChildHeight = (std::max)(0.0f, availableHeight - toggleHeight - spacingY);
            const bool hasPlayers = !g_ViewportPlayerCache.empty();

            // Viewport source selection - positioned at top-left of viewport
            {
                ImGui::SetCursorPos(ImVec2(contentMin.x + spacingY, contentMin.y + spacingY));
                const char* srcNames[] = { "Backbuffer", "BeforeUi" };
                ImGui::SetNextItemWidth(120.0f);
                ImGui::Combo("Source", &g_ViewportSourceMode, srcNames, (int)(sizeof(srcNames)/sizeof(srcNames[0])));
                ImGui::SameLine();
                ImGui::Checkbox("RMB Control", &g_ViewportEnableRmbControl);
                ImGui::SameLine();
                ImGui::Checkbox("Smooth", &g_ViewportSmoothMode);
                if (g_ViewportSmoothMode) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Settings##SmoothSettings")) {
                        g_ViewportSmoothSettingsOpen = !g_ViewportSmoothSettingsOpen;
                    }
                }
            }

            // Smooth camera settings window
            if (g_ViewportSmoothSettingsOpen) {
                ImGui::SetNextWindowSize(ImVec2(380, 320), ImGuiCond_FirstUseEver);
                if (ImGui::Begin("Smooth Camera Settings", &g_ViewportSmoothSettingsOpen)) {
                    ImGui::Text("Halftime Settings");
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::Text("Position Halftime:");
                    ImGui::SliderFloat("##PosHalftime", &g_ViewportSmoothHalftimePos, 0.01f, 1.0f, "%.3f s");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Time for position to move halfway to target");
                    }
                    ImGui::Spacing();

                    ImGui::Text("Angle Halftime:");
                    ImGui::SliderFloat("##AngleHalftime", &g_ViewportSmoothHalftimeAngle, 0.01f, 1.0f, "%.3f s");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Time for rotation to move halfway to target");
                    }
                    ImGui::Spacing();

                    ImGui::Text("FOV Halftime:");
                    ImGui::SliderFloat("##FovHalftime", &g_ViewportSmoothHalftimeFov, 0.01f, 1.0f, "%.3f s");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Time for FOV to move halfway to target");
                    }
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::Text("Scroll Increment Settings");
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::Text("Movement Speed Scroll Multiplier:");
                    ImGui::SliderFloat("##SpeedScrollInc", &g_ViewportSmoothScrollSpeedIncrement, 0.01f, 0.50f, "%.2f");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Speed multiplier per scroll click (e.g., 0.10 = 10%% increase/decrease per click)");
                    }
                    ImGui::Spacing();

                    ImGui::Text("FOV Scroll Increment:");
                    ImGui::SliderFloat("##FovScrollInc", &g_ViewportSmoothScrollFovIncrement, 0.5f, 10.0f, "%.1f");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("FOV change per Shift+Scroll click");
                    }
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::Text("Input Settings");
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::Checkbox("Enable Analog Input", &g_ViewportSmoothAnalogInput);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Enable analog keyboard input (Wooting, etc.)\nSet your analog keyboard to 'Xbox Controller' mode\nLeft Stick: WASD movement | Right Stick Up/Down: Space/Ctrl (Up/Down movement)");
                    }
                    ImGui::Spacing();
                    ImGui::Separator();

                    if (ImGui::Button("Reset to Defaults")) {
                        g_ViewportSmoothHalftimePos = 0.15f;
                        g_ViewportSmoothHalftimeAngle = 0.10f;
                        g_ViewportSmoothHalftimeFov = 0.20f;
                        g_ViewportSmoothScrollSpeedIncrement = 0.10f;
                        g_ViewportSmoothScrollFovIncrement = 2.0f;
                        g_ViewportSmoothAnalogInput = false;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Close")) {
                        g_ViewportSmoothSettingsOpen = false;
                    }
                }
                ImGui::End();
            }

            if (g_ViewportPlayersMenuOpen) {
                const float spacing_y = ImGui::GetStyle().ItemSpacing.y;
                const float row_h     = ImGui::GetFrameHeight();      // SmallButton height
                const int   max_rows  = 2;                            // how many wrapped rows you want visible
                const float child_h   = row_h * max_rows + spacing_y * (max_rows - 1);

                // position the child at the very bottom-left, right above the toolbar
                ImGui::SetCursorPos(ImVec2(contentMin.x,
                                        contentMax.y - toggleHeight - spacing_y - child_h));

                if (ImGui::BeginChild("ViewportPlayerPicker",
                                    ImVec2(contentWidth, child_h), false,
                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
                {
                    auto& style = ImGui::GetStyle();

                    const float left_x  = ImGui::GetCursorPosX();
                    const float right_x = left_x + contentWidth;

                    // flow layout with wrapping
                    for (const ViewportPlayerEntry& entry : g_ViewportPlayerCache) {
                        const char* label = entry.displayName.c_str();

                        // predict width of SmallButton(label)
                        ImVec2 sz = ImGui::CalcTextSize(label);
                        sz.x += style.FramePadding.x * 2.0f;
                        sz.y  = row_h;

                        // wrap if next button would exceed the row
                        float next_x = ImGui::GetCursorPosX() + sz.x;
                        if (next_x > right_x && ImGui::GetCursorPosX() > left_x) {
                            ImGui::NewLine();
                        }

                        ImGui::PushID(entry.command.c_str());
                        if (ImGui::SmallButton(label)) {
                            Afx_ExecClientCmd(entry.command.c_str());
                            if (MirvInput* pMirv = Afx_GetMirvInput()) {pMirv->SetCameraControlMode(false);}
                            g_ViewportPlayersMenuOpen = false;
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("SteamID: %llu",
                                static_cast<unsigned long long>(entry.steamId));
                        }
                        ImGui::PopID();

                        // keep flowing on the same row
                        ImGui::SameLine(0.0f, style.ItemSpacing.x);
                    }

                    // end the last SameLine chain
                    ImGui::NewLine();
                }
                ImGui::EndChild();
            }

            ImGui::SetCursorPos(ImVec2(contentMin.x, contentMax.y - toggleHeight));
            if (!hasPlayers) {
                ImGui::BeginDisabled(true);
            }
            // Bird camera controls
            if (ImGui::SmallButton("Bird")) {
                // Go to bird's eye view of current player
                int curIdx = GetFocusedPlayerControllerIndex();
                advancedfx::Message("BirdCamera: Bird button clicked - curIdx=%d\n", curIdx);
                if (curIdx >= 0) {
                    advancedfx::overlay::BirdCamera_StartPlayer(curIdx, 1000.0f);
                } else {
                    advancedfx::Message("BirdCamera: Failed to get focused player controller index. Make sure HUD is visible and GSI is connected.\n");
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Bird's eye view of current player");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Return")) {
                advancedfx::Message("BirdCamera: Return button clicked\n");
                advancedfx::overlay::BirdCamera_StartPlayerReturn();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Return to player POV");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop")) {
                advancedfx::Message("BirdCamera: Stop button clicked\n");
                advancedfx::overlay::BirdCamera_Stop();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Stop bird camera");
            }
            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();

            if (ImGui::SmallButton("<")) {
                if (MirvInput* pMirv = Afx_GetMirvInput()) {pMirv->SetCameraControlMode(false);}
                Afx_ExecClientCmd("spec_prev");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Players")) {
                if (hasPlayers) {
                    g_ViewportPlayersMenuOpen = !g_ViewportPlayersMenuOpen;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(">")) {
                if (MirvInput* pMirv = Afx_GetMirvInput()) {pMirv->SetCameraControlMode(false);}
                Afx_ExecClientCmd("spec_next");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("mode")) {
                if (MirvInput* pMirv = Afx_GetMirvInput()) {pMirv->SetCameraControlMode(false);}
                Afx_ExecClientCmd("spec_mode");
            }
            if (!hasPlayers) {
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("No player controllers found");
                }
                ImGui::EndDisabled();
                g_ViewportPlayersMenuOpen = false;
            }
        }
        ImGui::End();
    } else {
        g_ViewportPlayersMenuOpen = false;
    }

    // Sequencer window (ImGui Neo Sequencer)
    if (g_ShowSequencer) {
        // Sequencer: horizontally resizable; adjust height to content each frame
        ImGui::SetNextWindowSize(ImVec2(720, 200), ImGuiCond_FirstUseEver);
        bool sequencerWindowOpen = ImGui::Begin("HLAE Sequencer", &g_ShowSequencer, ImGuiWindowFlags_NoCollapse);
        if (sequencerWindowOpen) {
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

            // Compute a tight sequencer height: topbar + spacing + zoom + exactly one timeline row
            const ImGuiNeoSequencerStyle& neo = ImGui::GetNeoSequencerStyle();
            float topBarH = neo.TopBarHeight > 0.0f ? neo.TopBarHeight : (ImGui::CalcTextSize("100").y + st.FramePadding.y * 2.0f);
            float zoomH   = ImGui::GetFontSize() * neo.ZoomHeightScale + st.FramePadding.y * 2.0f;
            float rowLabelH = ImGui::CalcTextSize("Campath").y + st.FramePadding.y * 2.0f + neo.ItemSpacing.y * 2.0f;
            float desiredSeqH  = topBarH + neo.TopBarSpacing + zoomH + rowLabelH;

            // Clamp: at least 100 px, at most what’s left after the controls row
            float timelineScale = 25.0f;
            if (!g_EnableDofTimeline) {
                timelineScale = 30.0f;
            } else {
                timelineScale = 60.0f;
            }
            float minSeqH      = ImMax(timelineScale * g_UiScale, topBarH + neo.TopBarSpacing + zoomH + rowLabelH);
            float maxSeqHAvail = ImMax(minSeqH, ImMax(55.0f, availY - btnRowH));
            float seqH         = 50.0f + timelineScale * g_UiScale;

            // Use an explicit child height sized to sequencer content for stable layout across DPI scales
            ImGui::BeginChild("seq_child", ImVec2(0.0f, seqH), false); // scrolling lives inside this child
            {
                const ImVec2 seqSize(ImVec2(ImGui::GetContentRegionAvail().x, seqH));
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
                        // Also invalidate standalone curve cache so it rebuilds with updated times
                        g_CurveCache.valid = false;
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
                            // Mirror to curve editor
                            g_CurveSelection = curUiSelection;
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
                            g_CurveCache.valid = false;

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
                    
                    if (g_EnableDofTimeline) {
                        // Ensure ticks mirror keys
                        if (g_DofTicks.size() != g_DofKeys.size()) Dof_SyncTicksFromKeys();
                        if (ImGui::BeginNeoTimelineEx("DOF", &open, ImGuiNeoTimelineFlags_None)) {
                            // Draw keyframes and support selection / context
                            static int s_dofCtxIndex = -1; // persist selection across frames while popup is open
                            for (int i = 0; i < (int)g_DofTicks.size(); ++i) {
                                ImGui::NeoKeyframe(&g_DofTicks[i]);
                                if (ImGui::IsNeoKeyframeRightClicked()) { s_dofCtxIndex = i; ImGui::OpenPopup("dof_kf_ctx"); }
                            }

                            // If user finished dragging, commit new ticks back to keys and rebuild
                            static bool s_dofWasDragging = false;
                            bool draggingNow = ImGui::NeoIsDraggingSelection();
                            if (s_dofWasDragging && !draggingNow) {
                                // Map ticks back, then stable sort by tick
                                for (size_t i = 0; i < g_DofKeys.size() && i < g_DofTicks.size(); ++i)
                                    g_DofKeys[i].tick = (int)g_DofTicks[i];
                                std::sort(g_DofKeys.begin(), g_DofKeys.end(), [](const DofKeyframe& a, const DofKeyframe& b){ return a.tick < b.tick; });
                                Dof_SyncTicksFromKeys();
                                Dof_RebuildScheduled();
                            }
                            s_dofWasDragging = draggingNow;

                            // Keyframe context menu (Remove only)
                            if (ImGui::BeginPopup("dof_kf_ctx")) {
                                if (s_dofCtxIndex >= 0 && s_dofCtxIndex < (int)g_DofKeys.size()) {
                                    ImGui::Text("Tick: %d", g_DofKeys[s_dofCtxIndex].tick);
                                    if (ImGui::Selectable("Remove")) {
                                        g_DofKeys.erase(g_DofKeys.begin() + s_dofCtxIndex);
                                        Dof_SyncTicksFromKeys();
                                        Dof_RebuildScheduled();
                                        s_dofCtxIndex = -1;
                                        ImGui::CloseCurrentPopup();
                                    }
                                } else {
                                    ImGui::TextDisabled("No selection");
                                }
                                ImGui::EndPopup();
                            }
                            if (!ImGui::IsPopupOpen("dof_kf_ctx")) s_dofCtxIndex = -1;

                            ImGui::EndNeoTimeLine();
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
                if (!g_CurveCache.valid || g_CurveCache.times.size() != g_CamPath.GetSize())
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
                        for (CamPathIterator it = g_CamPath.GetBegin(); it != g_CamPath.GetEnd(); ++it, ++j) { if (j == idx) { out = it.GetValue(); break; } }
                        return out;
                    };
                    auto compEval = [&](int ch, double t)->double {
                        CamPathValue vv = g_CamPath.Eval(t);
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
                            bool isCustom = (ch==3) ? (g_CamPath.FovInterpMethod_get() == CamPath::DI_CUSTOM)
                                                    : (ch<=2 ? (g_CamPath.PositionInterpMethod_get() == CamPath::DI_CUSTOM)
                                                            : (g_CamPath.RotationInterpMethod_get() == CamPath::QI_CUSTOM));

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
                                    g_CamPath.SelectNone(); g_CamPath.SelectAdd((size_t)i,(size_t)i);

                                    ImGuiIO& io = ImGui::GetIO();
                                    bool altHeld = io.KeyAlt || ((io.KeyMods & ImGuiMod_Alt) != 0);
                                    bool shiftHeld = io.KeyShift || ((io.KeyMods & ImGuiMod_Shift) != 0);

                                    CamPath::Channel cch2 = mapChToEnum(ch);
                                    if (shiftHeld) {
                                        double tMouse = fromX(ImGui::GetIO().MousePos.x);
                                        double newW = (tMouse - t0) / (h/3.0);
                                        newW = (std::max)(0.01, (std::min)(newW, 5.0));
                                        g_CamPath.SetTangentMode(cch2, false, true, (unsigned char)CamPath::TM_FREE);
                                        g_CamPath.SetTangentWeight(cch2, false, true, 1.0, newW);
                                    } else {
                                        double baseY0 = y0;
                                        double newV = fromYToValue(ch);
                                        if (ch >= 4) { while (newV - baseY0 > 180.0) newV -= 360.0; while (newV - baseY0 < -180.0) newV += 360.0; }
                                        double newSlope = (newV - baseY0) / (h/3.0);
                                        if (altHeld) {
                                            g_CamPath.SetTangentMode(cch2, false, true, (unsigned char)CamPath::TM_FREE);
                                            g_CamPath.SetTangent(cch2, false, true, 0.0, newSlope);
                                        } else {
                                            g_CamPath.SetTangentMode(cch2, true, true, (unsigned char)CamPath::TM_FREE);
                                            g_CamPath.SetTangent(cch2, true, true, newSlope, newSlope);
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
                                    g_CamPath.SelectNone(); g_CamPath.SelectAdd((size_t)(i+1),(size_t)(i+1));

                                    ImGuiIO& io2 = ImGui::GetIO();
                                    bool altHeld2 = io2.KeyAlt || ((io2.KeyMods & ImGuiMod_Alt) != 0);
                                    bool shiftHeld2 = io2.KeyShift || ((io2.KeyMods & ImGuiMod_Shift) != 0);

                                    CamPath::Channel cch2 = mapChToEnum(ch);
                                    if (shiftHeld2) {
                                        double tMouse = fromX(ImGui::GetIO().MousePos.x);
                                        double newW = (t1 - tMouse) / (h/3.0);
                                        newW = (std::max)(0.01, (std::min)(newW, 5.0));
                                        g_CamPath.SetTangentMode(cch2, true, false, (unsigned char)CamPath::TM_FREE);
                                        g_CamPath.SetTangentWeight(cch2, true, false, newW, 1.0);
                                    } else {
                                        double baseY1 = y1;
                                        double newV = fromYToValue(ch);
                                        if (ch >= 4) { while (baseY1 - newV > 180.0) newV += 360.0; while (baseY1 - newV < -180.0) newV -= 360.0; }
                                        double newSlope = (baseY1 - newV) / (h/3.0);
                                        if (altHeld2) {
                                            g_CamPath.SetTangentMode(cch2, true, false, (unsigned char)CamPath::TM_FREE);
                                            g_CamPath.SetTangent(cch2, true, false, newSlope, 0.0);
                                        } else {
                                            g_CamPath.SetTangentMode(cch2, true, true, (unsigned char)CamPath::TM_FREE);
                                            g_CamPath.SetTangent(cch2, true, true, newSlope, newSlope);
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
                                g_CamPath.SelectNone();
                                g_CamPath.SelectAdd((size_t)g_CurveCtxKeyIndex, (size_t)g_CurveCtxKeyIndex);
                                g_CamPath.SetTangentMode(mapChToEnum(g_CurveCtxChannel), setIn, setOut, mode);
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
                // Optional: clamp to a small minimum to avoid collapse
                // Lowered to reduce excess bottom gap when preview slider is hidden
                float min_h = ImGui::GetFrameHeightWithSpacing() * 3.0f;
                if (desired < min_h) desired = min_h;
                ImGui::SetWindowSize(ImVec2(cur.x, desired));
            }
        }
        ImGui::End();
    }

    // Mirv input camera controls/indicator
    if (g_ShowCameraControl) {
        if (MirvInput* pMirv = Afx_GetMirvInput()) {
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

    // Global hotkeys for gizmo mode and mirv camera
    if (ImGui::GetActiveID() == 0) {
        if (ImGui::IsKeyPressed(ImGuiKey_G, false)) {
            g_GizmoOp = ImGuizmo::TRANSLATE;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
            g_GizmoOp = ImGuizmo::ROTATE;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !Overlay::Get().IsRmbPassthroughActive()) {
            Afx_ExecClientCmd("demo_togglepause");
        }
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            if (MirvInput* pMirv = Afx_GetMirvInput()) {
                bool camEnabled = pMirv->GetCameraControlMode();
                pMirv->SetCameraControlMode(!camEnabled);
            }
        }

        // Observing mode hotkeys (0-9)
        if (g_ObservingEnabled) {
            // Map ImGuiKey to numeric value 0-9
            static const ImGuiKey numKeys[10] = {
                ImGuiKey_0, ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4,
                ImGuiKey_5, ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9
            };

            for (int i = 0; i <= 9; i++) {
                if (ImGui::IsKeyPressed(numKeys[i], false)) {
                    auto it = g_ObservingHotkeyBindings.find(i);
                    if (it != g_ObservingHotkeyBindings.end()) {
                        int controllerIndex = it->second;
                        char cmd[128];
                        _snprintf_s(cmd, _TRUNCATE, "spec_mode 2; spec_player %d; mirv_campath enabled 0; mirv_input end", controllerIndex);
                        Afx_ExecClientCmd(cmd);
                        BirdCamera_Stop();
                        //advancedfx::Message("Overlay: Executing %s\n", cmd);
                    }
                }
            }
        }
    }

    // Tiny panel to pick operation/mode
    if (g_ShowGizmo){
        ImGui::Begin("Gizmo", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        if (ImGui::RadioButton("Pos (G)", g_GizmoOp == ImGuizmo::TRANSLATE)) g_GizmoOp = ImGuizmo::TRANSLATE;
        ImGui::SameLine();
        if (ImGui::RadioButton("Rot (R)",    g_GizmoOp == ImGuizmo::ROTATE))    g_GizmoOp = ImGuizmo::ROTATE;
        if (ImGui::RadioButton("Local",     g_GizmoMode == ImGuizmo::LOCAL))   g_GizmoMode = ImGuizmo::LOCAL;
        ImGui::SameLine();
        if (ImGui::RadioButton("World",     g_GizmoMode == ImGuizmo::WORLD))   g_GizmoMode = ImGuizmo::WORLD;
        ImGui::TextUnformatted("Hold CTRL to snap");
        ImGui::End();
    }

    // Only draw a gizmo if we have a selected keyframe
    using advancedfx::overlay::g_LastCampathCtx;
    if (g_LastCampathCtx.active)
    {
        // Where to draw: if preview window is active, draw into it; otherwise use full overlay
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        // While right mouse button passthrough is active, make gizmo intangible
        // so it doesn't steal focus or reveal the cursor while the camera is controlled.
        ImGuizmo::Enable(!Overlay::Get().IsRmbPassthroughActive());
        ImGuizmo::SetOrthographic(false);
        if (g_ShowBackbufferWindow && g_PreviewRectValid && g_PreviewDrawList) {
            ImGuizmo::SetDrawlist(g_PreviewDrawList);
            ImGuizmo::SetRect(g_PreviewRectMin.x, g_PreviewRectMin.y, g_PreviewRectSize.x, g_PreviewRectSize.y);
        } else {
            ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList()); // draw on top
            // Use main viewport rect for ImGuizmo normalization (accounts for viewport offset/DPI)
            ImGuizmo::SetRect(0.0f, 0.0f, ds.x, ds.y);
        }
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

        // Fetch current camera (position, angles, fov) from engine state
        double cx, cy, cz, rX, rY, rZ; float cfovDeg;
        Afx_GetLastCameraData(cx, cy, cz, rX, rY, rZ, cfovDeg);

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

        // Convert game FOV to a vertical FOV for our GL-style projection.
        float fovyDeg = cfovDeg;
        {
            const float baseAspect = 4.0f/3.0f;
            const float fovRad = (float)(cfovDeg * 3.14159265358979323846/180.0);
            const float half = 0.5f * fovRad;
            const float halfVert = atanf(tanf(half) / baseAspect);
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

        // Commit while dragging and on release
        if ((usingNow && changed) || (wasUsing && !usingNow && (s_dragChanged || s_hasModelOverride))) {
            float t[3], r_dummy[3], s_dummy[3];
            // Use the live model matrix that was just updated by Manipulate().
            const float* finalModel = model;
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
            if (!usingNow) {
                s_hasModelOverride = false;
                s_dragChanged = false;
            }
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

    // Declare variables for D3D11 state backup/restore outside mutex block
    ID3D11RenderTargetView* prevRtv = nullptr;
    ID3D11DepthStencilView* prevDsv = nullptr;
    UINT prevVpCount = 0;
    D3D11_VIEWPORT prevVp = {};

    // Use a scoped block for the mutex so we can release it before platform windows
    {
        std::lock_guard<std::mutex> lock(g_imguiInputMutex);

        // Update backbuffer size cache
        UpdateBackbufferSize();

        // Prepare opaque blend state for viewport image callbacks
        g_ImguiD3DContext = m_Context;
        if (!g_ViewportOpaqueBlend && m_Device) {
            D3D11_BLEND_DESC bd = {};
            bd.AlphaToCoverageEnable = FALSE;
            bd.IndependentBlendEnable = FALSE;
            bd.RenderTarget[0].BlendEnable = FALSE; // disable blending -> opaque
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            ID3D11BlendState* bs = nullptr;
            if (SUCCEEDED(m_Device->CreateBlendState(&bd, &bs))) {
                g_ViewportOpaqueBlend = bs;
            }
        }

        // Recreate ImGui device objects once after a resize/device-loss
        if (m_ImGuiNeedRecreate) {
            ImGui_ImplDX11_CreateDeviceObjects();
            m_ImGuiNeedRecreate = false;

            // Refresh viewport preview from swapchain just before drawing overlay
            if (g_ShowBackbufferWindow) { UpdateBackbufferPreviewTexture(); }
        }
        // Per-frame Mirv rendering below (no attach-camera updates here)

        // Backup current state we might disturb (render targets + viewport)
        m_Context->OMGetRenderTargets(1, &prevRtv, &prevDsv);

        m_Context->RSGetViewports(&prevVpCount, nullptr);
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
    } // Release mutex before platform windows to avoid deadlock

    // Update and render ImGui platform windows (multi-viewport support)
    // Must be outside mutex lock since platform window Present calls trigger our hooks.
    // Mark scope so Present hook can skip overlay re-entrancy when these swapchains present.
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        g_in_platform_windows_present = true;
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        g_in_platform_windows_present = false;

        // Note: do not pump Win32 messages here to avoid re-entrancy while rendering.
    }

    // Restore viewport (outside mutex, but still safe - these are local variables)
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
#ifdef _WIN32
    if (!m_Initialized) return;

    // Now flush any pending cursor warp outside the lock
    if (g_hasPendingWarp && g_windowActive && !g_dropFirstMouseAfterActivate) {
        ::SetCursorPos(g_pendingWarpPt.x, g_pendingWarpPt.y);
        g_hasPendingWarp = false;
    }
#endif
}

void OverlayDx11::OnDeviceLost() {
#ifdef _WIN32
    if (!m_Initialized) return;
    m_Rtv.width = m_Rtv.height = 0;
    ReleaseBackbufferPreview();
    ImGui_ImplDX11_InvalidateDeviceObjects();
    m_ImGuiNeedRecreate = true;
#endif
}

void OverlayDx11::OnResize(uint32_t, uint32_t) {
#ifdef _WIN32
    if (!m_Initialized) return;
    m_Rtv.width = m_Rtv.height = 0;
    ReleaseBackbufferPreview();
    ImGui_ImplDX11_InvalidateDeviceObjects();
    // Defer device object and RTV recreation to the next Render() after resize has completed
    m_ImGuiNeedRecreate = true;
#endif
}
void OverlayDx11::UpdateBackbufferPreviewTexture() {
#ifdef _WIN32
    if (!m_Initialized) return;
    if (!m_Device || !m_Context || !m_Swapchain) {
        ReleaseBackbufferPreview();
        return;
    }

    if (!g_ShowBackbufferWindow) {
        if (m_BackbufferPreview.texture || m_BackbufferPreview.srv) {
            ReleaseBackbufferPreview();
        }
        return;
    }

    // Choose viewport source based on UI selection
    ID3D11Texture2D* backbuffer = nullptr;
    HRESULT hr = E_FAIL;
    bool got = false;
    switch (g_ViewportSourceMode) {
    case 0: // Backbuffer (with UI)
        got = false; // handled below via swapchain
        break;
    case 1: // BeforeUi
        got = RenderSystemDX11_GetBeforeUiTexture(&backbuffer);
        break;
    default:
        got = false;
        break;
    }

    // Explicit-mode fallback behavior:
    if (!got) {
        if (g_ViewportSourceMode == 0) {
            hr = m_Swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer);
        } else {
            ReleaseBackbufferPreview();
            return; // no source available for BeforeUi
        }
    } else {
        hr = S_OK;
    }
    if (FAILED(hr) || !backbuffer) {
        ReleaseBackbufferPreview();
        return;
    }

    D3D11_TEXTURE2D_DESC srcDesc = {};
    backbuffer->GetDesc(&srcDesc);
    bool srcMsaa = srcDesc.SampleDesc.Count > 1;
    auto ChooseTypedSrvFormat = [](DXGI_FORMAT f) -> DXGI_FORMAT {
        switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:   return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:   return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:   return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:return DXGI_FORMAT_R16G16B16A16_FLOAT; // HDR color buffers are commonly FP16
        default: return f;
        }
    };

    // Fast-path: if source already has SRV bind and is single-sample, create SRV directly and skip extra copy
    // Exception: For explicit Backbuffer mode, always go through copy path to avoid driver/backbuffer quirks.
    bool srcHasSrvBind = (srcDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
    if (g_ViewportSourceMode == 0) srcHasSrvBind = false;
    bool usedDirectSrv = false;
    if (srcHasSrvBind && !srcMsaa) {
        // Recreate SRV if size/format changed or if we previously used a copy
        bool srvNeedsRecreate = !m_BackbufferPreview.srv
            || m_BackbufferPreview.width != srcDesc.Width
            || m_BackbufferPreview.height != srcDesc.Height
            || m_BackbufferPreview.format != srcDesc.Format
            || m_BackbufferPreview.texture != nullptr; // switching from copy-texture path to direct SRV

        if (srvNeedsRecreate) {
            // Release previous
            ReleaseBackbufferPreview();
            // Create SRV directly on source
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = ChooseTypedSrvFormat(srcDesc.Format);
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = 1;
            hr = m_Device->CreateShaderResourceView(backbuffer, &srvDesc, &m_BackbufferPreview.srv);
            if (FAILED(hr) || !m_BackbufferPreview.srv) {
                // Fall back to copy path below
            } else {
                m_BackbufferPreview.width = srcDesc.Width;
                m_BackbufferPreview.height = srcDesc.Height;
                m_BackbufferPreview.format = srcDesc.Format;
                m_BackbufferPreview.isMsaa = false;
                usedDirectSrv = true;
            }
        } else {
            // SRV is compatible already; reuse
            usedDirectSrv = true;
        }
    }

    if (!usedDirectSrv) {
        bool needsRecreate = !m_BackbufferPreview.texture
            || m_BackbufferPreview.width != srcDesc.Width
            || m_BackbufferPreview.height != srcDesc.Height
            || m_BackbufferPreview.format != srcDesc.Format
            || m_BackbufferPreview.isMsaa != srcMsaa;

        if (needsRecreate) {
            ReleaseBackbufferPreview();

            D3D11_TEXTURE2D_DESC copyDesc = srcDesc;
            copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            copyDesc.CPUAccessFlags = 0;
            copyDesc.MipLevels = 1;
            copyDesc.MiscFlags = 0;
            copyDesc.SampleDesc.Count = 1;
            copyDesc.SampleDesc.Quality = 0;
            copyDesc.Usage = D3D11_USAGE_DEFAULT;
            copyDesc.ArraySize = 1;

            hr = m_Device->CreateTexture2D(&copyDesc, nullptr, &m_BackbufferPreview.texture);
            if (FAILED(hr) || !m_BackbufferPreview.texture) {
                backbuffer->Release();
                ReleaseBackbufferPreview();
                return;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = ChooseTypedSrvFormat(copyDesc.Format);
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = 1;
            hr = m_Device->CreateShaderResourceView(m_BackbufferPreview.texture, &srvDesc, &m_BackbufferPreview.srv);
            if (FAILED(hr) || !m_BackbufferPreview.srv) {
                backbuffer->Release();
                ReleaseBackbufferPreview();
                return;
            }

            m_BackbufferPreview.width = copyDesc.Width;
            m_BackbufferPreview.height = copyDesc.Height;
            m_BackbufferPreview.format = srcDesc.Format;
            m_BackbufferPreview.isMsaa = srcMsaa;
        }

        if (m_BackbufferPreview.texture) {
            if (srcMsaa) {
                DXGI_FORMAT typed = ChooseTypedSrvFormat(srcDesc.Format);
                m_Context->ResolveSubresource(m_BackbufferPreview.texture, 0, backbuffer, 0, typed);
            } else {
                m_Context->CopyResource(m_BackbufferPreview.texture, backbuffer);
            }
        }
    }

    backbuffer->Release();
#endif
}

void OverlayDx11::ReleaseBackbufferPreview() {
#ifdef _WIN32
    if (m_BackbufferPreview.srv) {
        m_BackbufferPreview.srv->Release();
        m_BackbufferPreview.srv = nullptr;
    }
    if (m_BackbufferPreview.texture) {
        m_BackbufferPreview.texture->Release();
        m_BackbufferPreview.texture = nullptr;
    }
    m_BackbufferPreview.width = 0;
    m_BackbufferPreview.height = 0;
    m_BackbufferPreview.format = DXGI_FORMAT_UNKNOWN;
    m_BackbufferPreview.isMsaa = false;
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
