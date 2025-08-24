#include "Overlay.h"
#include "IOverlayRenderer.h"
#include "InputRouter.h"
#include "../AfxConsole.h"

#include <chrono>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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
    if (!m_Visible || !m_Renderer) return;
    UpdateDeltaTime();

#ifdef _WIN32
    // Fallback polling if we did not see WM_KEYDOWN for a while
    static int s_noKeydownFrames = 0;
    static bool s_prevDownMain = false;
    static bool s_prevDownAlt = false;

    bool sawKeydown = InputRouter::ConsumeKeydownSeenThisFrame();
    if (sawKeydown) {
        s_noKeydownFrames = 0;
    } else {
        s_noKeydownFrames++;
        if (s_noKeydownFrames > 120) { // ~2s at 60 FPS without keydown messages
            SHORT s = GetAsyncKeyState(InputRouter::GetToggleKey());
            bool down = (s & 0x8000) != 0;
            if (down && !s_prevDownMain) {
                advancedfx::Message("Overlay: toggle hotkey (poll) vk=0x%02X\n", (unsigned)InputRouter::GetToggleKey());
                ToggleVisible();
            }
            s_prevDownMain = down;
            // also allow alternate toggle key
            s = GetAsyncKeyState(InputRouter::GetAltToggleKey());
            down = (s & 0x8000) != 0;
            if (down && !s_prevDownAlt) {
                advancedfx::Message("Overlay: toggle hotkey (poll) vk=0x%02X\n", (unsigned)InputRouter::GetAltToggleKey());
                ToggleVisible();
            }
            s_prevDownAlt = down;
        }
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

} // namespace overlay
} // namespace advancedfx
