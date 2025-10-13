#pragma once

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"

#define ADVANCEDFX_STARTMOVIE_WAV_KEY "advancedfx-802bb089-972b-4841-bdf3-5108175ab59d"

bool AfxStreams_IsRcording();
const wchar_t * AfxStreams_GetTakeDir();
const char * AfxStreams_GetRecordNameUtf8();

// Mirv input camera accessors
class MirvInput;
MirvInput * Afx_GetMirvInput();
float GetLastCameraFov();
float GetLastCameraRoll();
void Afx_GetLastCameraData(double & x, double & y, double & z, double & rX, double & rY, double & rZ, float & fov);
void Afx_GotoDemoTick(int tick);

// Recording controls for overlay
const char * AfxStreams_GetRecordNameUtf8();
void AfxStreams_SetRecordNameUtf8(const char * name);
bool AfxStreams_GetRecordScreenEnabled();
void AfxStreams_SetRecordScreenEnabled(bool enabled);
bool AfxStreams_GetOverrideFps();
float AfxStreams_GetOverrideFpsValue();
void AfxStreams_SetOverrideFpsDefault();
void AfxStreams_SetOverrideFpsValue(float value);
void AfxStreams_RecordStart();
void AfxStreams_RecordEnd();
void Afx_ExecClientCmd(const char * cmd);

void RenderSystemDX11_EngineThread_Prepare();

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
