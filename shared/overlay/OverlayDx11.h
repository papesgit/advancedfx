#pragma once

#include "IOverlayRenderer.h"
#include <memory>
#include <stdint.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;
struct HWND__;
typedef HWND__* HWND;

namespace advancedfx { namespace overlay {

class OverlayDx11 : public IOverlayRenderer {
public:
    OverlayDx11(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapchain, HWND hwnd);
    ~OverlayDx11() override;

    bool Initialize() override;
    void Shutdown() override;
    void BeginFrame(float dtSeconds) override;
    void Render() override;
    void EndFrame() override;
    void OnDeviceLost() override;
    void OnResize(uint32_t width, uint32_t height) override;

    // Accessors
    HWND GetHwnd() const { return m_Hwnd; }

private:
    ID3D11Device* m_Device = nullptr;
    ID3D11DeviceContext* m_Context = nullptr;
    IDXGISwapChain* m_Swapchain = nullptr;
    HWND m_Hwnd = nullptr;

    bool m_Initialized = false;

    // Backbuffer size cache (RTV is created per-frame and not cached).
    struct RtvState {
        uint32_t width = 0;
        uint32_t height = 0;
    } m_Rtv;

    // When true, ImGui backend device objects need recreation after a resize/device-loss.
    bool m_ImGuiNeedRecreate = false;

    void UpdateBackbufferSize();
};

}} // namespace
