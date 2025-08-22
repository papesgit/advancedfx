#pragma once

#include "IOverlayRenderer.h"

namespace advancedfx { namespace overlay {

// Vulkan overlay renderer (scaffold).
class OverlayVk : public IOverlayRenderer {
public:
    OverlayVk() = default;
    ~OverlayVk() override = default;

    bool Initialize() override { return false; }
    void Shutdown() override {}
    void BeginFrame(float) override {}
    void Render() override {}
    void EndFrame() override {}
    void OnDeviceLost() override {}
    void OnResize(uint32_t, uint32_t) override {}
};

}} // namespace

