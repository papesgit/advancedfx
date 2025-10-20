#include "Overlay.h"
#include "IOverlayRenderer.h"
#include "InputRouter.h"
#include "../AfxConsole.h"

#include <chrono>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// For WantCaptureMouse/Keyboard
#include "third_party/imgui/imgui.h"

namespace advancedfx {
namespace overlay {

// Forward decl
class InputRouter;

Overlay & Overlay::Get() {
    static Overlay g_instance;
    return g_instance;
}

Overlay::Overlay() {
    // Force-visible diagnostic on startup.
    m_Visible = true;
    advancedfx::Message("Overlay: visible=true (startup diagnostic)\n");
}
Overlay::~Overlay() {}

void Overlay::ToggleVisible() {
    SetVisible(!m_Visible);
}

void Overlay::SetVisible(bool v) {
    if (m_Visible == v) return;
    m_Visible = v;
    advancedfx::Message("Overlay: visible=%s\n", m_Visible ? "true" : "false");
    if (!m_Visible) {
        m_RmbPassthrough = false;
        m_RmbPassthroughRequest = false;
    }
#ifdef _WIN32
    // Proactively release any OS cursor confinement when overlay becomes visible
    if (m_Visible) {
        ClipCursor(nullptr);
    }
#endif
}

void Overlay::RequestRmbPassthroughThisFrame() {
    m_RmbPassthroughRequest = true;
}


void Overlay::UpdateDeltaTime() {
    using clock = std::chrono::steady_clock;
    static bool first = true;
    auto now = clock::now();
    static auto last = now;
    if (first) {
        first = false;
        last = now;
        m_DeltaTime = 1.0f / 60.0f;
        return;
    }
    auto dt = std::chrono::duration<float>(now - last).count();
    last = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.25f) dt = 0.25f; // clamp
    m_DeltaTime = dt;
}

void Overlay::SetRenderer(std::unique_ptr<IOverlayRenderer> renderer) {
    if (m_Renderer) m_Renderer->Shutdown();
    m_Renderer = std::move(renderer);
    if (m_Renderer) m_Renderer->Initialize();
}

void Overlay::BeginFrame() {
    m_RmbPassthroughRequest = false;
    if (!m_Visible || !m_Renderer) return;
    UpdateDeltaTime();

#ifdef _WIN32
    // Only poll for toggles if we own a Win32 WndProc via InputRouter (Source 2).
    // On Source 1 we integrate with the game's WndProc; polling can double-toggle.
    // Disabled when viewports are enabled to prevent crashes with external windows.
    static int s_noKeydownFrames = 0;
    static bool s_prevDownMain = false;
    if (m_InputRouter) {
        bool sawKeydown = InputRouter::ConsumeKeydownSeenThisFrame();
        if (sawKeydown) {
            s_noKeydownFrames = 0;
        } else {
            s_noKeydownFrames++;
            if (s_noKeydownFrames > 120) { // ~2s at 60 FPS without keydown messages
                // Check if viewports are enabled - if so, disable toggle to prevent crashes
                ImGuiContext* ctx = ImGui::GetCurrentContext();
                bool viewportsEnabled = ctx && (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable);

                if (!viewportsEnabled) {
                    int vkMain = InputRouter::GetToggleKey();
                    SHORT s = (SHORT)0;
                    if (vkMain) s = GetAsyncKeyState(vkMain);
                    bool down = (s & 0x8000) != 0;
                    if (down && !s_prevDownMain) {
                        advancedfx::Message("Overlay: toggle hotkey (poll) vk=0x%02X\n", (unsigned)vkMain);
                        ToggleVisible();
                    }
                    s_prevDownMain = down;
                }
            }
        }
    } else {
        // No router: ensure polling state machines don't linger
        s_noKeydownFrames = 0;
        s_prevDownMain = false;
    }
#endif

    if (!m_Visible) return;
    m_Renderer->BeginFrame(m_DeltaTime);
}

void Overlay::RenderFrame() {
    if (!m_Visible || !m_Renderer) return;
    m_Renderer->Render();
}

void Overlay::EndFrame() {
    if (!m_Visible || !m_Renderer) return;
    m_Renderer->EndFrame();
}

void Overlay::OnDeviceLost() {
    if (m_Renderer) m_Renderer->OnDeviceLost();
}

void Overlay::OnResize(uint32_t w, uint32_t h) {
    if (m_Renderer) m_Renderer->OnResize(w, h);
}

void Overlay::AttachInputRouter(std::unique_ptr<InputRouter> router) {
    m_InputRouter = std::move(router);
}

bool Overlay::WantCaptureMouse() const {
    if (!m_Visible || !m_Renderer) return false;
    if (!ImGui::GetCurrentContext()) return false;
    ImGuiIO& io = ImGui::GetIO();
    return io.WantCaptureMouse;
}

bool Overlay::WantCaptureKeyboard() const {
    if (!m_Visible || !m_Renderer) return false;
    if (!ImGui::GetCurrentContext()) return false;
    ImGuiIO& io = ImGui::GetIO();
    return io.WantCaptureKeyboard;
}

} // namespace overlay
} // namespace advancedfx
