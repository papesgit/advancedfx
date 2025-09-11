#pragma once

#include "IOverlayRenderer.h"

struct IDirect3DDevice9;
struct HWND__;
typedef HWND__* HWND;

namespace advancedfx { namespace overlay {

class OverlayDx9 : public IOverlayRenderer {
public:
    OverlayDx9(IDirect3DDevice9* device, HWND hwnd);
    ~OverlayDx9() override;

    bool Initialize() override;
    void Shutdown() override;
    void BeginFrame(float dtSeconds) override;
    void Render() override;
    void EndFrame() override;
    void OnDeviceLost() override;
    void OnResize(uint32_t width, uint32_t height) override;

private:
    IDirect3DDevice9* m_Device = nullptr;
    HWND m_Hwnd = nullptr;
    bool m_Initialized = false;
    bool m_DeviceObjectsValid = false;

    void ApplyStyle();
};

}} // namespace

