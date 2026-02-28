#pragma once

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"

#define ADVANCEDFX_STARTMOVIE_WAV_KEY "advancedfx-802bb089-972b-4841-bdf3-5108175ab59d"

bool AfxStreams_IsRcording();
const wchar_t * AfxStreams_GetTakeDir();

void AfxStreams_ShutDown();

void RenderSystemDX11_EngineThread_Prepare();
void RenderSystemDX11_EngineThread_BeforeRender();

bool RenderSystemDX11_EngineThread_HasNextRenderPass();

void RenderSystemDX11_EngineThread_BeginNextRenderPass();
void RenderSystemDX11_EngineThread_EndNextRenderPass();

void RenderSystemDX11_EngineThread_BeginMainRenderPass();
void RenderSystemDX11_EngineThread_EndMainRenderPass();

void Hook_RenderSystemDX11(void * hModule);

void Hook_SceneSystem(void * hModule);

void RenderSystemDX11_SupplyProjectionMatrix(const SOURCESDK::VMatrix & projectionMatrix);

// Returns an AddRef'd pointer to the best-guess color buffer to preview this frame.
// Prefer the last bound main render target; fall back to the swapchain backbuffer.
// Caller must Release the returned texture when done. Returns false if none available.
struct ID3D11Texture2D; // fwd decl to avoid including d3d11.h here
bool RenderSystemDX11_GetPreviewSourceTexture(ID3D11Texture2D** outTexture);
// Explicit variants used by the overlay viewport:
bool RenderSystemDX11_GetBeforeUiTexture(ID3D11Texture2D** outTexture);
