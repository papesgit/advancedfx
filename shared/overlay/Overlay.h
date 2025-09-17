#pragma once

#include <memory>
#include <stdint.h>

namespace advancedfx {
namespace overlay {

class IOverlayRenderer;
class InputRouter;

class Overlay {
public:
    static Overlay & Get();

    // Visibility
    void ToggleVisible();
    void SetVisible(bool v);
    bool IsVisible() const { return m_Visible; }

    // Timing helpers
    void UpdateDeltaTime();

    // Renderer binding (runtime-lazy)
    void SetRenderer(std::unique_ptr<IOverlayRenderer> renderer);
    bool HasRenderer() const { return m_Renderer != nullptr; }

    // Per-frame hooks
    void BeginFrame();
    void RenderFrame();
    void EndFrame();

    // Device/resize events
    void OnDeviceLost();
    void OnResize(uint32_t width, uint32_t height);

    // Input router lifetime
    void AttachInputRouter(std::unique_ptr<InputRouter> router);
    InputRouter* GetInputRouter() const { return m_InputRouter.get(); }

    // RMB passthrough to mirv input while overlay is visible
    void SetRmbPassthroughActive(bool v) { m_RmbPassthrough = v; }
    bool IsRmbPassthroughActive() const { return m_RmbPassthrough; }

    void RequestRmbPassthroughThisFrame();
    bool IsRmbPassthroughRequested() const { return m_RmbPassthroughRequest; }
    // Query ImGui IO capture flags (valid during frames when renderer is active)
    bool WantCaptureMouse() const;
    bool WantCaptureKeyboard() const;

private:
    Overlay();
    ~Overlay();

    bool m_Visible = false;
    float m_DeltaTime = 0.0f;
    double m_LastTime = 0.0;
    bool m_RmbPassthrough = false;
    bool m_RmbPassthroughRequest = false;

    std::unique_ptr<IOverlayRenderer> m_Renderer;
    std::unique_ptr<InputRouter> m_InputRouter;
};

} // namespace overlay
} // namespace advancedfx
