#include "stdafx.h"

#include "PlayerPathDrawer.h"

#include "AfxShaders.h"
#include "ClientEntitySystem.h"
#include "MirvTime.h"
#include "ObsSpectatorBindings.h"

#include "../shared/AfxConsole.h"

#include "../shaders/build/afx_line_ps_5_0.h"
#include "../shaders/build/afx_line_vs_5_0.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
    const UINT c_VertexBufferVertexCount = 4096;
    const FLOAT c_PlayerPathPixelWidth = 6.0f;

    bool StartsWithIgnoreCaseAscii(const char* s, const char* lit) {
        for (; *lit; ++s, ++lit) {
            if (!*s) return false;
            char a = *s, b = *lit;
            if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
            if (a != b) return false;
        }
        return true;
    }

    int PipeLikeBytes(const unsigned char* p) {
        if (!p || !*p) return 0;
        if (p[0] == 0x7C) return 1;
        if (p[0] == 0xC2 && p[1] == 0xA6) return 2;
        if (p[0] == 0xEF && p[1] == 0xBD && p[2] == 0x9C) return 3;
        if (p[0] == 0xE2 && p[1] == 0x88 && p[2] == 0xA3) return 3;
        return 0;
    }

    bool IsCoachName(const char* name) {
        if (!name || !*name) return false;
        if (!StartsWithIgnoreCaseAscii(name, "coach")) return false;
        const unsigned char* after = (const unsigned char*)(name + 5);
        int sepLen = PipeLikeBytes(after);
        if (sepLen == 0) return false;
        return after[sepLen] != '\0';
    }
}

CPlayerPathDrawer g_PlayerPathDrawer;

extern int g_iWidth;
extern int g_iHeight;
extern SOURCESDK::VMatrix g_WorldToScreenMatrix;

CPlayerPathDrawer::CPlayerPathDrawer() {}

CPlayerPathDrawer::~CPlayerPathDrawer() {
    EndDevice();
}

void CPlayerPathDrawer::Begin() {}

void CPlayerPathDrawer::End() {}

void CPlayerPathDrawer::ResetState_NoLock() {
    m_RoundState = RoundState::Inactive;
    m_RoundStartTime = 0.0;
    m_RoundEndTime = 0.0;
    m_PlaybackActive = false;
    m_PlaybackCursorTime = 0.0;
    m_PlaybackUpperTime = 0.0;
    m_PlaybackLastRealTime = (std::chrono::steady_clock::time_point::min)();
    m_SamplesByController.clear();
    m_TeamByController.clear();
    m_PaletteSlotByController.clear();
    m_AliveSnapshotControllers.clear();
    m_NextSampleTime = 0.0;
}

void CPlayerPathDrawer::HandleRoundStart_NoLock(double now) {
    m_SamplesByController.clear();
    m_TeamByController.clear();
    m_PaletteSlotByController.clear();
    m_AliveSnapshotControllers.clear();
    m_RoundState = RoundState::Collecting;
    m_RoundStartTime = now;
    m_RoundEndTime = now;
    m_NextSampleTime = now;
}

void CPlayerPathDrawer::HandleRoundEnd_NoLock(double now) {
    if (m_RoundState == RoundState::Collecting) {
        m_RoundState = RoundState::Ended;
        m_RoundEndTime = now;
    }
}

bool CPlayerPathDrawer::ShouldTrackController(CEntityInstance* controller, int team) const {
    if (!controller || !controller->IsPlayerController()) return false;
    if (team != 2 && team != 3) return false;

    const char* name = controller->GetSanitizedPlayerName();
    if (!name || '\0' == name[0]) {
        name = controller->GetPlayerName();
    }
    if (IsCoachName(name)) return false;

    return true;
}

bool CPlayerPathDrawer::IsControllerAlive(int controllerIndex) const {
    if (!g_pEntityList || !g_GetEntityFromIndex) return false;

    CEntityInstance* controller = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, controllerIndex);
    if (!controller || !controller->IsPlayerController()) return false;

    const auto pawnHandle = controller->GetPlayerPawnHandle();
    if (!pawnHandle.IsValid()) return false;

    const int pawnIndex = pawnHandle.GetEntryIndex();
    if (pawnIndex < 0) return false;

    CEntityInstance* pawn = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, pawnIndex);
    if (!pawn) return false;

    return 0 < (int)pawn->GetHealth();
}

void CPlayerPathDrawer::SamplePlayers_NoLock(double now) {
    if (!g_pEntityList || !g_GetEntityFromIndex || !g_GetHighestEntityIndex) return;

    std::vector<int> ctControllers;
    std::vector<int> tControllers;

    int highestIndex = g_GetHighestEntityIndex(*g_pEntityList, false);
    for (int i = 0; i <= highestIndex; ++i) {
        CEntityInstance* controller = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, i);
        if (!controller) continue;

        const int team = controller->GetTeam();
        if (!ShouldTrackController(controller, team)) continue;
        m_TeamByController[i] = team;
        if (team == 3) ctControllers.push_back(i);
        else if (team == 2) tControllers.push_back(i);

        const auto pawnHandle = controller->GetPlayerPawnHandle();
        if (!pawnHandle.IsValid()) continue;

        const int pawnIndex = pawnHandle.GetEntryIndex();
        if (pawnIndex < 0) continue;

        CEntityInstance* pawn = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, pawnIndex);
        if (!pawn) continue;
        if (pawn->GetHealth() <= 0) continue;

        float eyeOrigin[3] = { 0.0f, 0.0f, 0.0f };
        pawn->GetRenderEyeOrigin(eyeOrigin);

        if (!std::isfinite(eyeOrigin[0]) || !std::isfinite(eyeOrigin[1]) || !std::isfinite(eyeOrigin[2])) continue;

        auto& samples = m_SamplesByController[i];
        samples.push_back({ now, Afx::Math::Vector3(eyeOrigin[0], eyeOrigin[1], eyeOrigin[2]) });
        while (samples.size() > m_MaxSamplesPerPlayer) {
            samples.pop_front();
        }
    }

    std::sort(ctControllers.begin(), ctControllers.end());
    std::sort(tControllers.begin(), tControllers.end());

    m_PaletteSlotByController.clear();
    for (size_t i = 0; i < ctControllers.size() && i < 5; ++i) {
        m_PaletteSlotByController[ctControllers[i]] = (int)i;
    }
    for (size_t i = 0; i < tControllers.size() && i < 5; ++i) {
        m_PaletteSlotByController[tControllers[i]] = (int)i;
    }
}

void CPlayerPathDrawer::RefreshRenderData_NoLock(double now) {
    m_RenderData.Enabled = m_Enabled;
    m_RenderData.ShowAll = m_ShowAll;
    m_RenderData.ShowAliveSnapshot = m_ShowAliveSnapshot;
    std::memcpy(m_RenderData.ShowSlots, m_ShowSlots, sizeof(m_ShowSlots));
    m_RenderData.Mode = m_DrawMode;
    m_RenderData.Depth = m_DepthMode;
    m_RenderData.Alpha = m_AlphaMode;
    m_RenderData.Colors = m_ColorMode;
    m_RenderData.PlaybackActive = m_PlaybackActive;
    m_RenderData.PlaybackCursorTime = m_PlaybackCursorTime;
    m_RenderData.PlaybackUpperTime = m_PlaybackUpperTime;
    m_RenderData.Round = m_RoundState;
    m_RenderData.RoundStartTime = m_RoundStartTime;
    m_RenderData.RoundEndTime = m_RoundEndTime;
    m_RenderData.CurrentTime = now;
    m_RenderData.SamplesByController = m_SamplesByController;
    m_RenderData.TeamByController = m_TeamByController;
    m_RenderData.PaletteSlotByController = m_PaletteSlotByController;
    m_RenderData.AliveSnapshotControllers = m_AliveSnapshotControllers;
}

void CPlayerPathDrawer::OnEngineThread_SetupViewDone() {
    const double now = g_MirvTime.curtime_get();

    std::unique_lock<std::mutex> lock(m_DataMutex);

    int demoTick = 0;
    const bool hasDemoTick = g_MirvTime.GetCurrentDemoTick(demoTick);

    if (hasDemoTick) {
        if (m_HasLastDemoTick && demoTick + 2 < m_LastDemoTick) {
            if (m_Debug) {
                advancedfx::Message(
                    "mirv_playerpath debug: reset on demo tick decrease (last=%d now=%d)\n",
                    m_LastDemoTick,
                    demoTick
                );
            }
            ResetState_NoLock();
        }
        m_LastDemoTick = demoTick;
        m_HasLastDemoTick = true;
    }
    else {
        // Fallback: only treat larger negative jumps as seek-like events.
        if (m_LastSeenTime - now > 0.25) {
            if (m_Debug) {
                advancedfx::Message(
                    "mirv_playerpath debug: reset on time decrease (last=%.6f now=%.6f)\n",
                    m_LastSeenTime,
                    now
                );
            }
            ResetState_NoLock();
        }
        m_HasLastDemoTick = false;
    }

    m_LastSeenTime = now;

    if (m_PlaybackActive) {
        auto realNow = std::chrono::steady_clock::now();
        const double liveUpperTime = m_RoundState == RoundState::Ended ? m_RoundEndTime : now;
        m_PlaybackUpperTime = liveUpperTime;

        if (m_PlaybackLastRealTime != (std::chrono::steady_clock::time_point::min)()) {
            const double dt = std::chrono::duration_cast<std::chrono::duration<double>>(realNow - m_PlaybackLastRealTime).count();
            if (m_DrawMode == DrawMode::Forward) {
                m_PlaybackCursorTime += dt * m_PlaybackSpeed;
                if (m_PlaybackCursorTime >= liveUpperTime) {
                    m_PlaybackCursorTime = liveUpperTime;
                }
            } else {
                m_PlaybackCursorTime -= dt * m_PlaybackSpeed;
                if (m_PlaybackCursorTime <= m_RoundStartTime) {
                    m_PlaybackCursorTime = m_RoundStartTime;
                }
            }
        }
        m_PlaybackLastRealTime = realNow;
    }

    if (m_Enabled && m_RoundState == RoundState::Collecting && now >= m_NextSampleTime) {
        SamplePlayers_NoLock(now);
        m_NextSampleTime = now + m_SampleInterval;
    }

    RefreshRenderData_NoLock(now);
}

void CPlayerPathDrawer::OnEngineThread_EndFrame() {}

void CPlayerPathDrawer::OnRenderThread_Present() {}

void CPlayerPathDrawer::OnGameEvent(const char* eventName) {
    if (!eventName) return;

    std::unique_lock<std::mutex> lock(m_DataMutex);

    const double now = g_MirvTime.curtime_get();
    if (0 == _stricmp(eventName, "round_freeze_end")) {
        HandleRoundStart_NoLock(now);
    }
    else if (0 == _stricmp(eventName, "round_end")) {
        HandleRoundEnd_NoLock(now);
    }
    else if (0 == _stricmp(eventName, "round_start")) {
        ResetState_NoLock();
    }
}

void CPlayerPathDrawer::Console_Command(advancedfx::ICommandArgs* args) {
    const int argC = args->ArgC();
    const char* cmd0 = args->ArgV(0);
    auto parseSlotLabel = [](const char* text, int& outIndex) -> bool {
        if (!text || !text[0] || text[1]) return false;
        if (text[0] == '0') {
            outIndex = 9;
            return true;
        }
        if ('1' <= text[0] && text[0] <= '9') {
            outIndex = text[0] - '1';
            return true;
        }
        return false;
    };
    auto hasAnyShownSlot = [](const bool (&slots)[10]) -> bool {
        for (bool value : slots) {
            if (value) return true;
        }
        return false;
    };

    if (argC >= 2) {
        const char* cmd1 = args->ArgV(1);

        if (0 == _stricmp(cmd1, "enabled")) {
            std::unique_lock<std::mutex> lock(m_DataMutex);
            if (argC >= 3) {
                m_Enabled = 0 != atoi(args->ArgV(2));
                if (!m_Enabled) {
                    ResetState_NoLock();
                }
                return;
            }
            advancedfx::Message("%s enabled 0|1\nCurrent value: %i\n", cmd0, m_Enabled ? 1 : 0);
            return;
        }

        if (0 == _stricmp(cmd1, "show")) {
            std::unique_lock<std::mutex> lock(m_DataMutex);
            if (argC >= 3) {
                const char* cmd2 = args->ArgV(2);
                if (0 == _stricmp(cmd2, "all")) {
                    m_ShowAll = true;
                    m_ShowAliveSnapshot = false;
                    m_AliveSnapshotControllers.clear();
                    return;
                }
                if (0 == _stricmp(cmd2, "none")) {
                    m_ShowAll = false;
                    m_ShowAliveSnapshot = false;
                    m_AliveSnapshotControllers.clear();
                    std::memset(m_ShowSlots, 0, sizeof(m_ShowSlots));
                    return;
                }
                if (0 == _stricmp(cmd2, "alive")) {
                    m_ShowAll = true;
                    m_ShowAliveSnapshot = true;
                    m_AliveSnapshotControllers.clear();
                    for (const auto& kv : m_SamplesByController) {
                        if (IsControllerAlive(kv.first)) {
                            m_AliveSnapshotControllers.insert(kv.first);
                        }
                    }
                    std::memset(m_ShowSlots, 0, sizeof(m_ShowSlots));
                    return;
                }
                if (argC >= 4) {
                    int slotIndex = -1;
                    if (!parseSlotLabel(cmd2, slotIndex)) {
                        advancedfx::Warning("show slot must be one of 0-9\n");
                        return;
                    }
                    m_ShowAll = false;
                    m_ShowAliveSnapshot = false;
                    m_AliveSnapshotControllers.clear();
                    m_ShowSlots[slotIndex] = 0 != atoi(args->ArgV(3));
                    return;
                }
            }
            advancedfx::Message(
                "%s show all|none|alive|<0-9> <0|1>\n"
                "Current value: %s\n",
                cmd0,
                m_ShowAliveSnapshot ? "alive" : (m_ShowAll ? "all" : (hasAnyShownSlot(m_ShowSlots) ? "slots" : "none"))
            );
            return;
        }

        if (0 == _stricmp(cmd1, "depth")) {
            std::unique_lock<std::mutex> lock(m_DataMutex);
            if (argC >= 3) {
                const char* cmd2 = args->ArgV(2);
                if (0 == _stricmp(cmd2, "test")) {
                    m_DepthMode = DepthMode::Test;
                    return;
                }
                if (0 == _stricmp(cmd2, "overlay")) {
                    m_DepthMode = DepthMode::Overlay;
                    return;
                }
            }
            advancedfx::Message("%s depth test|overlay\n", cmd0);
            return;
        }

        if (0 == _stricmp(cmd1, "alpha")) {
            std::unique_lock<std::mutex> lock(m_DataMutex);
            if (argC >= 3) {
                const char* cmd2 = args->ArgV(2);
                if (0 == _stricmp(cmd2, "dynamic")) {
                    m_AlphaMode = AlphaMode::Dynamic;
                    return;
                }
                if (0 == _stricmp(cmd2, "opaque")) {
                    m_AlphaMode = AlphaMode::Opaque;
                    return;
                }
            }
            advancedfx::Message("%s alpha dynamic|opaque\n", cmd0);
            return;
        }

        if (0 == _stricmp(cmd1, "colors")) {
            std::unique_lock<std::mutex> lock(m_DataMutex);
            if (argC >= 3) {
                const char* cmd2 = args->ArgV(2);
                if (0 == _stricmp(cmd2, "uniform")) {
                    m_ColorMode = ColorMode::Uniform;
                    return;
                }
                if (0 == _stricmp(cmd2, "shift")) {
                    m_ColorMode = ColorMode::Shift;
                    return;
                }
                if (0 == _stricmp(cmd2, "palette")) {
                    m_ColorMode = ColorMode::Palette;
                    return;
                }
            }
            advancedfx::Message("%s colors uniform|shift|palette\n", cmd0);
            return;
        }

        if (0 == _stricmp(cmd1, "sampleInterval")) {
            std::unique_lock<std::mutex> lock(m_DataMutex);
            if (argC >= 3) {
                const double value = atof(args->ArgV(2));
                if (value > 0.0) {
                    m_SampleInterval = value;
                    return;
                }
                advancedfx::Warning("sampleInterval must be > 0\n");
                return;
            }
            advancedfx::Message("%s sampleInterval <seconds>\nCurrent value: %.3f\n", cmd0, m_SampleInterval);
            return;
        }

        if (0 == _stricmp(cmd1, "play")) {
            std::unique_lock<std::mutex> lock(m_DataMutex);

            if (argC >= 3 && 0 == _stricmp(args->ArgV(2), "stop")) {
                m_PlaybackActive = false;
                return;
            }

            bool showAll = false;
            bool showSlots[10] = { false, false, false, false, false, false, false, false, false, false };
            std::set<int> aliveSnapshotControllers;
            const char* direction = nullptr;
            const char* speedArg = nullptr;

            if (argC >= 5 && 0 == _stricmp(args->ArgV(2), "all")) {
                showAll = true;
                direction = args->ArgV(3);
                speedArg = args->ArgV(4);
            }
            else if (argC >= 5 && 0 == _stricmp(args->ArgV(2), "alive")) {
                showAll = true;
                for (const auto& kv : m_SamplesByController) {
                    if (IsControllerAlive(kv.first)) {
                        aliveSnapshotControllers.insert(kv.first);
                    }
                }
                direction = args->ArgV(3);
                speedArg = args->ArgV(4);
            }
            else if (argC >= 5) {
                int slotIndex = -1;
                if (!parseSlotLabel(args->ArgV(2), slotIndex)) {
                    advancedfx::Warning("play target must be all, alive, or one of 0-9\n");
                    return;
                }
                showSlots[slotIndex] = true;
                direction = args->ArgV(3);
                speedArg = args->ArgV(4);
            }
            else {
                advancedfx::Message("%s play all|alive|<0-9> forwards|backwards <speed>\n%s play stop\n", cmd0, cmd0);
                return;
            }

            if (!direction || !speedArg) {
                advancedfx::Warning("play: invalid arguments\n");
                return;
            }

            DrawMode playMode;
            if (0 == _stricmp(direction, "forwards") || 0 == _stricmp(direction, "forward")) {
                playMode = DrawMode::Forward;
            }
            else if (0 == _stricmp(direction, "backwards") || 0 == _stricmp(direction, "backward")) {
                playMode = DrawMode::Backward;
            }
            else {
                advancedfx::Warning("play direction must be forwards|backwards\n");
                return;
            }

            const double speed = atof(speedArg);
            if (!(speed > 0.0)) {
                advancedfx::Warning("play speed must be > 0\n");
                return;
            }

            if (m_RoundState == RoundState::Inactive) {
                advancedfx::Warning("play: no active/ended round data.\n");
                return;
            }

            const double now = g_MirvTime.curtime_get();
            const double upperTime = m_RoundState == RoundState::Ended ? m_RoundEndTime : now;
            if (!(upperTime > m_RoundStartTime)) {
                advancedfx::Warning("play: invalid round time span.\n");
                return;
            }

            m_ShowAll = showAll;
            m_ShowAliveSnapshot = !aliveSnapshotControllers.empty();
            m_AliveSnapshotControllers = aliveSnapshotControllers;
            std::memcpy(m_ShowSlots, showSlots, sizeof(showSlots));
            m_DrawMode = playMode;
            m_PlaybackSpeed = speed;
            m_PlaybackUpperTime = upperTime;
            m_PlaybackCursorTime = playMode == DrawMode::Forward ? m_RoundStartTime : upperTime;
            m_PlaybackActive = true;
            m_PlaybackLastRealTime = std::chrono::steady_clock::now();
            return;
        }

        if (0 == _stricmp(cmd1, "debug")) {
            std::unique_lock<std::mutex> lock(m_DataMutex);
            if (argC >= 3) {
                m_Debug = 0 != atoi(args->ArgV(2));
                return;
            }
            advancedfx::Message("%s debug 0|1\nCurrent value: %i\n", cmd0, m_Debug ? 1 : 0);
            return;
        }

        if (0 == _stricmp(cmd1, "clear")) {
            std::unique_lock<std::mutex> lock(m_DataMutex);
            ResetState_NoLock();
            return;
        }

        if (0 == _stricmp(cmd1, "print")) {
            std::unique_lock<std::mutex> lock(m_DataMutex);
            size_t trackedPlayers = m_SamplesByController.size();
            size_t sampleCount = 0;
            for (const auto& it : m_SamplesByController) sampleCount += it.second.size();
            const char* roundState =
                m_RoundState == RoundState::Inactive ? "inactive"
                : m_RoundState == RoundState::Collecting ? "collecting"
                : "ended";
            advancedfx::Message(
                "enabled: %i\nshow: %s\nplayDirection: %s\ndepth: %s\nalpha: %s\ncolors: %s\nsampleInterval: %.3f\nplayback: %s speed=%.3f cursor=%.3f upper=%.3f\nround: %s\ntrackedPlayers: %zu\nsamples: %zu\n",
                m_Enabled ? 1 : 0,
                m_ShowAliveSnapshot ? "alive" : (m_ShowAll ? "all" : (hasAnyShownSlot(m_ShowSlots) ? "slots" : "none")),
                m_DrawMode == DrawMode::Forward ? "forward" : "backward",
                m_DepthMode == DepthMode::Test ? "test" : "overlay",
                m_AlphaMode == AlphaMode::Dynamic ? "dynamic" : "opaque",
                m_ColorMode == ColorMode::Uniform ? "uniform" : (m_ColorMode == ColorMode::Shift ? "shift" : "palette"),
                m_SampleInterval,
                m_PlaybackActive ? "active" : "inactive",
                m_PlaybackSpeed,
                m_PlaybackCursorTime,
                m_PlaybackUpperTime,
                roundState,
                trackedPlayers,
                sampleCount
            );
            return;
        }
    }

    advancedfx::Message(
        "%s enabled [0|1]\n"
        "%s show all|none|alive|<0-9> <0|1>\n"
        "%s play all|alive|<0-9> forwards|backwards <speed>\n"
        "%s play stop\n"
        "%s depth test|overlay\n"
        "%s alpha dynamic|opaque\n"
        "%s colors uniform|shift|palette\n"
        "%s sampleInterval [seconds]\n"
        "%s debug [0|1]\n"
        "%s clear\n"
        "%s print\n",
        cmd0, cmd0, cmd0, cmd0, cmd0, cmd0, cmd0, cmd0, cmd0, cmd0, cmd0
    );
}

void CPlayerPathDrawer::BeginDevice(ID3D11Device* device) {
    EndDevice();

    if (!device) return;

    device->AddRef();
    m_Device = device;
    m_Device->CreateDeferredContext(0, &m_DeviceContext);

    {
        size_t size;
        void* so = LoadFromAcsShaderFileInMemory(L"afx_line_vs_5_0.acs", ShaderCombo_afx_line_vs_5_0::GetCombo(), size);
        if (so) {
            m_Device->CreateVertexShader(so, size, NULL, &m_VertexShader);
            D3D11_INPUT_ELEMENT_DESC inputDesc[5] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            m_Device->CreateInputLayout(inputDesc, 5, so, size, &m_InputLayoutLines);
        }
        if (so) free(so);
    }

    {
        size_t size;
        void* so = LoadFromAcsShaderFileInMemory(L"afx_line_ps_5_0.acs", ShaderCombo_afx_line_ps_5_0::GetCombo(), size);
        if (so) m_Device->CreatePixelShader(so, size, NULL, &m_PixelShader);
        if (so) free(so);
    }

    {
        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = sizeof(VS_CONSTANT_BUFFER);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m_Device->CreateBuffer(&cbDesc, NULL, &m_ConstantBuffer);
    }

    {
        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = sizeof(VS_CONSTANT_BUFFER_WIDTH);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m_Device->CreateBuffer(&cbDesc, NULL, &m_ConstantBufferWidth);
    }

    {
        D3D11_DEPTH_STENCIL_DESC depthDesc{
            TRUE, D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_COMPARISON_LESS_EQUAL, FALSE,
            D3D11_DEFAULT_STENCIL_WRITE_MASK, D3D11_DEFAULT_STENCIL_READ_MASK,
            { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS },
            { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS },
        };
        m_Device->CreateDepthStencilState(&depthDesc, &m_DepthStencilStateTest);
    }

    {
        D3D11_DEPTH_STENCIL_DESC depthDesc{
            FALSE, D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_COMPARISON_ALWAYS, FALSE,
            D3D11_DEFAULT_STENCIL_WRITE_MASK, D3D11_DEFAULT_STENCIL_READ_MASK,
            { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS },
            { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS },
        };
        m_Device->CreateDepthStencilState(&depthDesc, &m_DepthStencilStateOverlay);
    }

    {
        D3D11_RASTERIZER_DESC rasterizerDesc{
            D3D11_FILL_SOLID,
            D3D11_CULL_FRONT,
            TRUE,
            0, 0, 0,
            TRUE,
            FALSE,
            TRUE,
            FALSE
        };
        m_Device->CreateRasterizerState(&rasterizerDesc, &m_RasterizerStateLines);
    }

    {
        D3D11_BLEND_DESC blendDesc{
            FALSE,
            FALSE,
            {
                {
                    TRUE,
                    D3D11_BLEND_SRC_ALPHA,
                    D3D11_BLEND_INV_SRC_ALPHA,
                    D3D11_BLEND_OP_ADD,
                    D3D11_BLEND_SRC_ALPHA,
                    D3D11_BLEND_INV_SRC_ALPHA,
                    D3D11_BLEND_OP_ADD,
                    D3D11_COLOR_WRITE_ENABLE_ALL
                }
            }
        };
        for (int i = 1; i < 8; ++i) {
            blendDesc.RenderTarget[i] = { FALSE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL };
        }
        m_Device->CreateBlendState(&blendDesc, &m_BlendState);
    }
}

void CPlayerPathDrawer::EndDevice() {
    UnloadVertexBuffer();

    if (m_BlendState) { m_BlendState->Release(); m_BlendState = nullptr; }
    if (m_RasterizerStateLines) { m_RasterizerStateLines->Release(); m_RasterizerStateLines = nullptr; }
    if (m_DepthStencilStateOverlay) { m_DepthStencilStateOverlay->Release(); m_DepthStencilStateOverlay = nullptr; }
    if (m_DepthStencilStateTest) { m_DepthStencilStateTest->Release(); m_DepthStencilStateTest = nullptr; }
    if (m_ConstantBufferWidth) { m_ConstantBufferWidth->Release(); m_ConstantBufferWidth = nullptr; }
    if (m_ConstantBuffer) { m_ConstantBuffer->Release(); m_ConstantBuffer = nullptr; }
    if (m_InputLayoutLines) { m_InputLayoutLines->Release(); m_InputLayoutLines = nullptr; }
    if (m_PixelShader) { m_PixelShader->Release(); m_PixelShader = nullptr; }
    if (m_VertexShader) { m_VertexShader->Release(); m_VertexShader = nullptr; }
    if (m_DeviceContext) { m_DeviceContext->Release(); m_DeviceContext = nullptr; }
    if (m_Device) { m_Device->Release(); m_Device = nullptr; }
}

DWORD CPlayerPathDrawer::MakeColorForController(int controllerIndex, int team, int paletteSlot, float alpha01, ColorMode colorMode) const {
    // Keep team hue family while providing 5 evenly spaced shades per team.
    static const BYTE tPalette[5][3] = {
        {255, 185, 70},   // amber
        {255, 150, 55},   // orange
        {255, 110, 45},   // orange-red
        {235, 75, 45},    // vermilion
        {210, 45, 45}     // red
    };
    static const BYTE ctPalette[5][3] = {
        {70, 200, 255},   // cyan
        {70, 160, 255},   // sky blue
        {80, 120, 255},   // blue
        {105, 95, 245},   // indigo
        {140, 80, 225}    // violet
    };

    BYTE r = team == 2 ? 244 : 112;
    BYTE g = team == 2 ? 132 : 168;
    BYTE b = team == 2 ? 70 : 250;

    if (colorMode == ColorMode::Palette) {
        const int slot = ((paletteSlot % 5) + 5) % 5;
        r = team == 2 ? tPalette[slot][0] : ctPalette[slot][0];
        g = team == 2 ? tPalette[slot][1] : ctPalette[slot][1];
        b = team == 2 ? tPalette[slot][2] : ctPalette[slot][2];
    }
    else if (colorMode == ColorMode::Shift) {
        // Deterministic per-team slot shift (0..4), less wide than full palette.
        static const int kShiftBySlot[5] = { -18, -9, 0, 9, 18 };
        const int slot = ((paletteSlot % 5) + 5) % 5;
        const int mod = kShiftBySlot[slot];

        const int rI = team == 2
            ? std::clamp((int)r + mod, 0, 255)
            : std::clamp((int)r + (mod / 2), 0, 255);
        const int gI = std::clamp((int)g - (mod / 3), 0, 255);
        const int bI = team == 2
            ? std::clamp((int)b - (mod / 2), 0, 255)
            : std::clamp((int)b + mod, 0, 255);
        r = (BYTE)rI;
        g = (BYTE)gI;
        b = (BYTE)bI;
    }

    const BYTE a = (BYTE)std::clamp(int(alpha01 * 255.0f), 0, 255);
    return (DWORD)((a << 24) | (r << 16) | (g << 8) | b);
}

float CPlayerPathDrawer::CalcAlphaForTime(double segmentTime, double focusTime, double fromTime, double toTime) const {
    const double span = (std::max)(1.0e-6, toTime - fromTime);
    const double dist = fabs(segmentTime - focusTime);
    const double normDist = std::clamp(dist / span, 0.0, 1.0);
    const double alpha = 0.35 + 0.65 * (1.0 - normDist);
    return (float)alpha;
}

void CPlayerPathDrawer::GetViewPlaneFromWorldToScreen(const SOURCESDK::VMatrix& worldToScreenMatrix, Afx::Math::Vector3& outPlaneOrigin, Afx::Math::Vector3& outPlaneNormal) const {
    double plane0[4] = { 0,0,0,1 };
    double planeN[4] = { 1,0,0,1 };

    unsigned char P[4];
    unsigned char Q[4];
    double L[4][4];
    double U[4][4];

    double M[4][4] = {
        worldToScreenMatrix.m[0][0], worldToScreenMatrix.m[0][1], worldToScreenMatrix.m[0][2], 0,
        worldToScreenMatrix.m[1][0], worldToScreenMatrix.m[1][1], worldToScreenMatrix.m[1][2], 0,
        worldToScreenMatrix.m[2][0], worldToScreenMatrix.m[2][1], worldToScreenMatrix.m[2][2], 0,
        worldToScreenMatrix.m[3][0], worldToScreenMatrix.m[3][1], worldToScreenMatrix.m[3][2], -1,
    };

    double b0[4] = {
        0 - worldToScreenMatrix.m[0][3],
        0 - worldToScreenMatrix.m[1][3],
        0 - worldToScreenMatrix.m[2][3],
        -worldToScreenMatrix.m[3][3],
    };
    double bN[4] = {
        0 - worldToScreenMatrix.m[0][3],
        0 - worldToScreenMatrix.m[1][3],
        1 - worldToScreenMatrix.m[2][3],
        -worldToScreenMatrix.m[3][3],
    };

    if (Afx::Math::LUdecomposition(M, P, Q, L, U)) {
        Afx::Math::SolveWithLU(L, U, P, Q, b0, plane0);
        Afx::Math::SolveWithLU(L, U, P, Q, bN, planeN);
    }

    outPlaneOrigin = Afx::Math::Vector3(plane0[0], plane0[1], plane0[2]);
    outPlaneNormal = Afx::Math::Vector3(planeN[0] - plane0[0], planeN[1] - plane0[1], planeN[2] - plane0[2]);
    outPlaneNormal.Normalize();
}

void CPlayerPathDrawer::OnRenderThread_Draw(ID3D11DeviceContext* pImmediateContext, const D3D11_VIEWPORT* pViewPort, ID3D11RenderTargetView* pRenderTargetView2, ID3D11DepthStencilView* pDepthStencilView2) {
    if (!(m_Device && m_DeviceContext && m_VertexShader && m_PixelShader && m_ConstantBuffer && m_ConstantBufferWidth && m_InputLayoutLines && m_BlendState && m_RasterizerStateLines && m_DepthStencilStateTest && m_DepthStencilStateOverlay)) {
        return;
    }
    if (!pImmediateContext || !pViewPort || !pRenderTargetView2) return;

    RenderData renderData;
    {
        std::unique_lock<std::mutex> lock(m_DataMutex);
        renderData = m_RenderData;
    }

    if (!renderData.Enabled || renderData.SamplesByController.empty()) return;

    std::set<int> visibleControllers;
    if (renderData.ShowAliveSnapshot) {
        for (const int controllerIndex : renderData.AliveSnapshotControllers) {
            if (renderData.SamplesByController.find(controllerIndex) != renderData.SamplesByController.end()) {
                visibleControllers.insert(controllerIndex);
            }
        }
    }
    else if (renderData.ShowAll) {
        for (const auto& kv : renderData.SamplesByController) {
            visibleControllers.insert(kv.first);
        }
    }
    else {
        for (int i = 0; i < 10; ++i) {
            if (!renderData.ShowSlots[i]) continue;
            const int controllerIndex = g_SpectatorBindings[i];
            if (controllerIndex >= 0) {
                visibleControllers.insert(controllerIndex);
            }
        }
    }
    if (visibleControllers.empty()) return;

    m_ImmediateContext = pImmediateContext;
    m_Rtv2 = pRenderTargetView2;
    m_Dsv2 = pDepthStencilView2;
    m_pViewPort = pViewPort;

    m_DeviceContext->OMSetRenderTargets(1, &m_Rtv2, m_Dsv2);
    m_DeviceContext->OMSetBlendState(m_BlendState, NULL, 0xffffffff);
    m_DeviceContext->RSSetViewports(1, m_pViewPort);
    m_DeviceContext->IASetInputLayout(m_InputLayoutLines);
    m_DeviceContext->VSSetShader(m_VertexShader, NULL, 0);
    m_DeviceContext->PSSetShader(m_PixelShader, NULL, 0);
    m_DeviceContext->RSSetState(m_RasterizerStateLines);
    m_DeviceContext->OMSetDepthStencilState(
        renderData.Depth == DepthMode::Test ? m_DepthStencilStateTest : m_DepthStencilStateOverlay,
        0
    );

    Afx::Math::Vector3 planeOrigin;
    Afx::Math::Vector3 planeNormal;
    GetViewPlaneFromWorldToScreen(g_WorldToScreenMatrix, planeOrigin, planeNormal);

    VS_CONSTANT_BUFFER constantBuffer = {
        {
            g_WorldToScreenMatrix.m[0][0], g_WorldToScreenMatrix.m[0][1], g_WorldToScreenMatrix.m[0][2], g_WorldToScreenMatrix.m[0][3],
            g_WorldToScreenMatrix.m[1][0], g_WorldToScreenMatrix.m[1][1], g_WorldToScreenMatrix.m[1][2], g_WorldToScreenMatrix.m[1][3],
            g_WorldToScreenMatrix.m[2][0], g_WorldToScreenMatrix.m[2][1], g_WorldToScreenMatrix.m[2][2], g_WorldToScreenMatrix.m[2][3],
            g_WorldToScreenMatrix.m[3][0], g_WorldToScreenMatrix.m[3][1], g_WorldToScreenMatrix.m[3][2], g_WorldToScreenMatrix.m[3][3]
        },
        { (float)planeOrigin.X, (float)planeOrigin.Y, (float)planeOrigin.Z, 0.0f },
        { (float)planeNormal.X, (float)planeNormal.Y, (float)planeNormal.Z, 0.0f },
        { 0 != g_iWidth ? 1.0f / g_iWidth : 0.0f, 0 != g_iHeight ? 1.0f / g_iHeight : 0.0f, 0.0f, 0.0f }
    };

    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(m_DeviceContext->Map(m_ConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, &constantBuffer, sizeof(constantBuffer));
            m_DeviceContext->Unmap(m_ConstantBuffer, 0);
        }
    }
    m_DeviceContext->VSSetConstantBuffers(0, 1, &m_ConstantBuffer);

    SetLineWidth(c_PlayerPathPixelWidth);

    const double liveUpperTime = renderData.Round == RoundState::Ended
        ? renderData.RoundEndTime
        : renderData.CurrentTime;
    double fromTime = renderData.PlaybackActive
        ? (renderData.Mode == DrawMode::Backward ? renderData.PlaybackCursorTime : renderData.RoundStartTime)
        : renderData.RoundStartTime;
    double toTime = renderData.PlaybackActive
        ? (renderData.Mode == DrawMode::Forward ? renderData.PlaybackCursorTime : liveUpperTime)
        : liveUpperTime;
    if (toTime < fromTime) {
        fromTime = toTime;
    }
    const double focusTime = renderData.PlaybackActive
        ? (renderData.Mode == DrawMode::Forward ? renderData.PlaybackCursorTime : liveUpperTime)
        : renderData.CurrentTime;

    for (const int controllerIndex : visibleControllers) {
        auto it = renderData.SamplesByController.find(controllerIndex);
        if (it == renderData.SamplesByController.end()) continue;
        const auto& samples = it->second;
        if (samples.size() < 2) continue;

        int team = 0;
        auto teamIt = renderData.TeamByController.find(controllerIndex);
        if (teamIt != renderData.TeamByController.end()) {
            team = teamIt->second;
        }
        int paletteSlot = ((controllerIndex % 5) + 5) % 5;
        auto paletteIt = renderData.PaletteSlotByController.find(controllerIndex);
        if (paletteIt != renderData.PaletteSlotByController.end()) {
            paletteSlot = paletteIt->second;
        }

        for (size_t i = 1; i < samples.size(); ++i) {
            const Sample& a = samples[i - 1];
            const Sample& b = samples[i];

            if ((b.Position - a.Position).Length() <= 1.0e-6) continue;

            if (b.Time > toTime) break;
            if (a.Time < fromTime) continue;

            const float alphaA = renderData.Alpha == AlphaMode::Opaque
                ? 1.0f
                : CalcAlphaForTime(a.Time, focusTime, fromTime, toTime);
            const float alphaB = renderData.Alpha == AlphaMode::Opaque
                ? 1.0f
                : CalcAlphaForTime(b.Time, focusTime, fromTime, toTime);
            const DWORD colorA = MakeColorForController(controllerIndex, team, paletteSlot, alphaA, renderData.Colors);
            const DWORD colorB = MakeColorForController(controllerIndex, team, paletteSlot, alphaB, renderData.Colors);

            AutoSingleLine(a.Position, colorA, b.Position, colorB);
        }
    }

    AutoSingleLineFlush();

    ID3D11CommandList* pCommandList = nullptr;
    m_DeviceContext->FinishCommandList(FALSE, &pCommandList);
    if (pCommandList) {
        m_ImmediateContext->ExecuteCommandList(pCommandList, TRUE);
        pCommandList->Release();
    }

    m_pViewPort = nullptr;
    m_Dsv2 = nullptr;
    m_Rtv2 = nullptr;
    m_ImmediateContext = nullptr;
}

void CPlayerPathDrawer::BuildSingleLine(Afx::Math::Vector3 from, Afx::Math::Vector3 to, Vertex* pOutVertexData) {
    Afx::Math::Vector3 normal = (to - from).Normalize();
    double length = (to - from).Length() / 8192;

    pOutVertexData[1].x = pOutVertexData[0].x = (float)from.X;
    pOutVertexData[3].x = pOutVertexData[2].x = (float)to.X;
    pOutVertexData[1].y = pOutVertexData[0].y = (float)from.Y;
    pOutVertexData[3].y = pOutVertexData[2].y = (float)to.Y;
    pOutVertexData[1].z = pOutVertexData[0].z = (float)from.Z;
    pOutVertexData[3].z = pOutVertexData[2].z = (float)to.Z;

    pOutVertexData[3].t1u = pOutVertexData[2].t1u = pOutVertexData[1].t1u = pOutVertexData[0].t1u = (float)-normal.X;
    pOutVertexData[3].t1v = pOutVertexData[2].t1v = pOutVertexData[1].t1v = pOutVertexData[0].t1v = (float)-normal.Y;
    pOutVertexData[3].t1w = pOutVertexData[2].t1w = pOutVertexData[1].t1w = pOutVertexData[0].t1w = (float)-normal.Z;

    pOutVertexData[3].t2u = pOutVertexData[2].t2u = pOutVertexData[1].t2u = pOutVertexData[0].t2u = (float)normal.X;
    pOutVertexData[3].t2v = pOutVertexData[2].t2v = pOutVertexData[1].t2v = pOutVertexData[0].t2v = (float)normal.Y;
    pOutVertexData[3].t2w = pOutVertexData[2].t2w = pOutVertexData[1].t2w = pOutVertexData[0].t2w = (float)normal.Z;

    pOutVertexData[2].t0u = pOutVertexData[0].t0u = 1.0f;
    pOutVertexData[3].t0u = pOutVertexData[1].t0u = -1.0f;

    pOutVertexData[1].t0v = pOutVertexData[0].t0v = 0.0f;
    pOutVertexData[1].t0w = pOutVertexData[0].t0w = (float)length;
    pOutVertexData[3].t0v = pOutVertexData[2].t0v = (float)length;
    pOutVertexData[3].t0w = pOutVertexData[2].t0w = 0.0f;
}

void CPlayerPathDrawer::BuildSingleLine(DWORD colorFrom, DWORD colorTo, Vertex* pOutVertexData) {
    pOutVertexData[1].diffuse = pOutVertexData[0].diffuse = colorFrom;
    pOutVertexData[3].diffuse = pOutVertexData[2].diffuse = colorTo;
}

bool CPlayerPathDrawer::LockVertexBuffer() {
    if (m_VertexBuffer) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (!SUCCEEDED(m_DeviceContext->Map(m_VertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            m_LockedVertexBuffer = nullptr;
            return false;
        }
        m_LockedVertexBuffer = (Vertex*)mapped.pData;
        return true;
    }

    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = c_VertexBufferVertexCount * sizeof(Vertex);
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (!SUCCEEDED(m_Device->CreateBuffer(&vbDesc, NULL, &m_VertexBuffer))) {
        if (m_VertexBuffer) m_VertexBuffer->Release();
        m_VertexBuffer = nullptr;
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!SUCCEEDED(m_DeviceContext->Map(m_VertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        m_LockedVertexBuffer = nullptr;
        return false;
    }
    m_LockedVertexBuffer = (Vertex*)mapped.pData;
    return true;
}

void CPlayerPathDrawer::UnlockVertexBuffer() {
    if (m_VertexBuffer && m_LockedVertexBuffer) {
        m_DeviceContext->Unmap(m_VertexBuffer, 0);
        m_LockedVertexBuffer = nullptr;
    }
}

void CPlayerPathDrawer::UnloadVertexBuffer() {
    if (m_VertexBuffer) {
        m_VertexBuffer->Release();
        m_VertexBuffer = nullptr;
    }
}

void CPlayerPathDrawer::SetLineWidth(float width) {
    VS_CONSTANT_BUFFER_WIDTH widthData = { { width, 0, 0, 0 } };
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(m_DeviceContext->Map(m_ConstantBufferWidth, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &widthData, sizeof(widthData));
        m_DeviceContext->Unmap(m_ConstantBufferWidth, 0);
    }
    m_DeviceContext->VSSetConstantBuffers(1, 1, &m_ConstantBufferWidth);
}

void CPlayerPathDrawer::AutoSingleLine(Afx::Math::Vector3 from, DWORD colorFrom, Afx::Math::Vector3 to, DWORD colorTo) {
    if (c_VertexBufferVertexCount < m_VertexBufferVertexCount + 4) {
        AutoSingleLineFlush();
    }

    if (!m_LockedVertexBuffer) {
        if (!LockVertexBuffer()) return;
    }

    Vertex* curBuf = &(m_LockedVertexBuffer[m_VertexBufferVertexCount]);
    BuildSingleLine(from, to, curBuf);
    BuildSingleLine(colorFrom, colorTo, curBuf);
    m_VertexBufferVertexCount += 4;
}

void CPlayerPathDrawer::AutoSingleLineFlush() {
    if (!m_LockedVertexBuffer || m_VertexBufferVertexCount == 0) return;

    UnlockVertexBuffer();

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    m_DeviceContext->IASetVertexBuffers(0, 1, &m_VertexBuffer, &stride, &offset);

    UINT startVertex = 0;
    UINT lineCount = m_VertexBufferVertexCount / 4;
    for (UINT i = 0; i < lineCount; ++i) {
        m_DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        m_DeviceContext->Draw(4, startVertex);
        startVertex += 4;
    }

    m_VertexBufferVertexCount = 0;
}
