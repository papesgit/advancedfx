#pragma once

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../shared/AfxMath.h"
#include "../shared/AfxConsole.h"

#include <d3d11.h>
#define _XM_NO_INTRINSICS_
#include <DirectXMath.h>

#include <map>
#include <deque>
#include <set>
#include <mutex>
#include <cfloat>
#include <chrono>

class CEntityInstance;

class CPlayerPathDrawer {
public:
    CPlayerPathDrawer();
    ~CPlayerPathDrawer();

    void Begin();
    void End();

    void BeginDevice(ID3D11Device* device);
    void EndDevice();

    void OnEngineThread_SetupViewDone();
    void OnEngineThread_EndFrame();
    void OnRenderThread_Draw(ID3D11DeviceContext* pImmediateContext, const D3D11_VIEWPORT* pViewPort, ID3D11RenderTargetView* pRenderTargetView2, ID3D11DepthStencilView* pDepthStencilView2);
    void OnRenderThread_Present();

    void OnGameEvent(const char* eventName);

    void Console_Command(advancedfx::ICommandArgs* args);

private:
    enum class RoundState {
        Inactive,
        Collecting,
        Ended
    };

    enum class DrawMode {
        Forward,
        Backward
    };

    enum class DepthMode {
        Test,
        Overlay
    };

    enum class AlphaMode {
        Dynamic,
        Opaque
    };

    enum class ColorMode {
        Uniform,
        Shift,
        Palette
    };

    struct Sample {
        double Time;
        Afx::Math::Vector3 Position;
    };

    struct Vertex {
        FLOAT x, y, z;
        DWORD diffuse;
        FLOAT t0u, t0v, t0w;
        FLOAT t1u, t1v, t1w;
        FLOAT t2u, t2v, t2w;
    };

    struct VS_CONSTANT_BUFFER {
        DirectX::XMFLOAT4X4 matrix;
        DirectX::XMFLOAT4 plane0;
        DirectX::XMFLOAT4 paneN;
        DirectX::XMFLOAT4 screenInfo;
    };

    struct VS_CONSTANT_BUFFER_WIDTH {
        DirectX::XMFLOAT4 width;
    };

    struct RenderData {
        bool Enabled = false;
        bool ShowAll = true;
        bool ShowSlots[10] = { false, false, false, false, false, false, false, false, false, false };
        DrawMode Mode = DrawMode::Backward;
        DepthMode Depth = DepthMode::Test;
        AlphaMode Alpha = AlphaMode::Dynamic;
        ColorMode Colors = ColorMode::Palette;
        bool PlaybackActive = false;
        double PlaybackCursorTime = 0.0;
        double PlaybackUpperTime = 0.0;
        RoundState Round = RoundState::Inactive;
        double RoundStartTime = 0.0;
        double RoundEndTime = 0.0;
        double CurrentTime = 0.0;
        std::map<int, std::deque<Sample>> SamplesByController;
        std::map<int, int> TeamByController;
        std::map<int, int> PaletteSlotByController;
    };

    void ResetState_NoLock();
    void HandleRoundStart_NoLock(double now);
    void HandleRoundEnd_NoLock(double now);
    void SamplePlayers_NoLock(double now);
    void RefreshRenderData_NoLock(double now);

    bool ShouldTrackController(CEntityInstance* controller, int team) const;
    DWORD MakeColorForController(int controllerIndex, int team, int paletteSlot, float alpha01, ColorMode colorMode) const;
    float CalcAlphaForTime(double segmentTime, double focusTime, double fromTime, double toTime) const;
    void GetViewPlaneFromWorldToScreen(const SOURCESDK::VMatrix& worldToScreenMatrix, Afx::Math::Vector3& outPlaneOrigin, Afx::Math::Vector3& outPlaneNormal) const;

    bool LockVertexBuffer();
    void UnlockVertexBuffer();
    void UnloadVertexBuffer();
    void SetLineWidth(float width);
    void BuildSingleLine(Afx::Math::Vector3 from, Afx::Math::Vector3 to, Vertex* pOutVertexData);
    void BuildSingleLine(DWORD colorFrom, DWORD colorTo, Vertex* pOutVertexData);
    void AutoSingleLine(Afx::Math::Vector3 from, DWORD colorFrom, Afx::Math::Vector3 to, DWORD colorTo);
    void AutoSingleLineFlush();

private:
    std::mutex m_DataMutex;
    RenderData m_RenderData;

    bool m_Enabled = false;
    bool m_ShowAll = true;
    bool m_ShowSlots[10] = { false, false, false, false, false, false, false, false, false, false };
    DrawMode m_DrawMode = DrawMode::Backward;
    DepthMode m_DepthMode = DepthMode::Test;
    AlphaMode m_AlphaMode = AlphaMode::Dynamic;
    ColorMode m_ColorMode = ColorMode::Palette;
    double m_SampleInterval = 0.25;
    double m_NextSampleTime = 0.0;
    double m_LastSeenTime = -DBL_MAX;
    bool m_HasLastDemoTick = false;
    int m_LastDemoTick = 0;
    bool m_Debug = false;
    bool m_PlaybackActive = false;
    double m_PlaybackCursorTime = 0.0;
    double m_PlaybackUpperTime = 0.0;
    double m_PlaybackSpeed = 1.0;
    std::chrono::steady_clock::time_point m_PlaybackLastRealTime = (std::chrono::steady_clock::time_point::min)();
    RoundState m_RoundState = RoundState::Inactive;
    double m_RoundStartTime = 0.0;
    double m_RoundEndTime = 0.0;
    const size_t m_MaxSamplesPerPlayer = 8192;
    std::map<int, std::deque<Sample>> m_SamplesByController;
    std::map<int, int> m_TeamByController;
    std::map<int, int> m_PaletteSlotByController;

    ID3D11Device* m_Device = nullptr;
    ID3D11DeviceContext* m_DeviceContext = nullptr;
    ID3D11DeviceContext* m_ImmediateContext = nullptr;
    const D3D11_VIEWPORT* m_pViewPort = nullptr;
    ID3D11RenderTargetView* m_Rtv2 = nullptr;
    ID3D11DepthStencilView* m_Dsv2 = nullptr;
    ID3D11DepthStencilState* m_DepthStencilStateTest = nullptr;
    ID3D11DepthStencilState* m_DepthStencilStateOverlay = nullptr;
    ID3D11RasterizerState* m_RasterizerStateLines = nullptr;
    ID3D11BlendState* m_BlendState = nullptr;
    ID3D11InputLayout* m_InputLayoutLines = nullptr;
    ID3D11Buffer* m_ConstantBuffer = nullptr;
    ID3D11Buffer* m_ConstantBufferWidth = nullptr;
    ID3D11PixelShader* m_PixelShader = nullptr;
    ID3D11VertexShader* m_VertexShader = nullptr;
    ID3D11Buffer* m_VertexBuffer = nullptr;
    Vertex* m_LockedVertexBuffer = nullptr;
    UINT m_VertexBufferVertexCount = 0;
};

extern CPlayerPathDrawer g_PlayerPathDrawer;
