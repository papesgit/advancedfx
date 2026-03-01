#pragma once

#include <string>
#include <cstdint>

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

bool AfxSharedTexture_DuplicateHandleForPid(unsigned int pid, unsigned long long & outHandleValue, std::string & outError);

uint64_t RenderedSetup_OnSetupViewCandidate(float x, float y, float z, float pitch, float yaw, float roll, float fov, float deltaTime);
uint64_t RenderedSetup_GetLatestCandidateSerial();
void RenderedSetup_OnWorldToScreenCandidate(uint64_t setupSerial, const SOURCESDK::VMatrix& worldToScreenMatrix);
bool RenderedSetup_GetPublishedWorldToScreenMatrix(SOURCESDK::VMatrix& outWorldToScreenMatrix);
void RenderedSetup_OnPlayerSetupViewRendered(uint64_t setupSerial);
void RenderedSetup_OnBeforePresent();
