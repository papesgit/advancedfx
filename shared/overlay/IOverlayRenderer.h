#pragma once

#include <stdint.h>

namespace advancedfx {
namespace overlay {

class IOverlayRenderer {
public:
    virtual ~IOverlayRenderer() = default;

    // Initialize renderer backend. Returns true on success.
    virtual bool Initialize() = 0;

    // Shutdown renderer backend and free resources.
    virtual void Shutdown() = 0;

    // Begin a new frame for rendering the overlay. dtSeconds may be 0 when unknown.
    virtual void BeginFrame(float dtSeconds) = 0;

    // Render any pending ImGui draw data to the current backbuffer/RT.
    virtual void Render() = 0;

    // Render Dear ImGui platform windows (multi-viewport) if supported.
    // Default implementation is a no-op; backends that support multi-viewport
    // may override this to call ImGui::UpdatePlatformWindows()/RenderPlatformWindowsDefault().
    virtual void RenderPlatformWindows() {}

    // End the frame (submit if needed).
    virtual void EndFrame() = 0;

    // Handle device/context loss or swapchain resize.
    virtual void OnDeviceLost() = 0;
    virtual void OnResize(uint32_t width, uint32_t height) = 0;
};

} // namespace overlay
} // namespace advancedfx
