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
#ifdef IMGUI_ENABLE_FREETYPE
#include "third_party/imgui/misc/freetype/imgui_freetype.h"
#endif

// Campath info (points and duration): access global campath and time.
#include "../CamPath.h"
#include "../../AfxHookSource2/MirvTime.h"
#include "../../AfxHookSource2/CampathDrawer.h"
#include "../MirvInput.h"
#include "../../AfxHookSource2/RenderSystemDX11Hooks.h"
// For default game folder (…/game/) resolution
#include "../../AfxHookSource2/hlaeFolder.h"

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
static bool g_ShowCameraControl = false;
static bool g_ShowGizmo = false;
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

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
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

    // Sequencer window (ImGui Neo Sequencer)
    if (g_ShowSequencer) {
        // Sequencer: horizontally resizable; adjust height to content each frame
        ImGui::SetNextWindowSize(ImVec2(720, 200), ImGuiCond_FirstUseEver);
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

            // Compute a tight sequencer height: topbar + spacing + zoom + exactly one timeline row
            const ImGuiNeoSequencerStyle& neo = ImGui::GetNeoSequencerStyle();
            float topBarH = neo.TopBarHeight > 0.0f ? neo.TopBarHeight : (ImGui::CalcTextSize("100").y + st.FramePadding.y * 2.0f);
            float zoomH   = ImGui::GetFontSize() * neo.ZoomHeightScale + st.FramePadding.y * 2.0f;
            float rowLabelH = ImGui::CalcTextSize("Campath").y + st.FramePadding.y * 2.0f + neo.ItemSpacing.y * 2.0f;
            float desiredSeqH  = topBarH + neo.TopBarSpacing + zoomH + rowLabelH;

            // Clamp: at least 100 px, at most what’s left after the controls row
            float minSeqH      = ImMax(60.0f, topBarH + neo.TopBarSpacing + zoomH + rowLabelH);
            float maxSeqHAvail = ImMax(minSeqH, ImMax(55.0f, availY - btnRowH));
            float seqH         = ImClamp(desiredSeqH, minSeqH, maxSeqHAvail);

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
            ImGui::End();
        }
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
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            if (MirvInput* pMirv = Afx_GetMirvInput()) {
                bool camEnabled = pMirv->GetCameraControlMode();
                pMirv->SetCameraControlMode(!camEnabled);
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
