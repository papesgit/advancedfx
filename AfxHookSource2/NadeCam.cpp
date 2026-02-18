#include "stdafx.h"
#include "NadeCam.h"
#include "ClientEntitySystem.h"
#include "WrpConsole.h"
#include "../shared/AfxConsole.h"
#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdio>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// External declarations from ClientEntitySystem
extern void** g_pEntityList;
extern GetHighestEntityIndex_t g_GetHighestEntityIndex;
extern GetEntityFromIndex_t g_GetEntityFromIndex;

// From ClientEntitySystem.cpp
typedef CEntityInstance* (__fastcall* ClientDll_GetSplitScreenPlayer_t)(int slot);
extern ClientDll_GetSplitScreenPlayer_t g_ClientDll_GetSplitScreenPlayer;

// From main.cpp
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace {
    // Grenade class name tokens
    const char* grenadeTokens[] = {
        "hegrenade",
        "flashbang",
        "smokegrenade",
        "decoy",
        "molotov",
        "incendiary"
    };
    const int numGrenadeTokens = sizeof(grenadeTokens) / sizeof(grenadeTokens[0]);

    bool StringContainsIgnoreCase(const char* haystack, const char* needle) {
        if (!haystack || !needle) return false;

        size_t haystackLen = strlen(haystack);
        size_t needleLen = strlen(needle);

        if (needleLen > haystackLen) return false;

        for (size_t i = 0; i <= haystackLen - needleLen; ++i) {
            bool match = true;
            for (size_t j = 0; j < needleLen; ++j) {
                char h = haystack[i + j];
                char n = needle[j];
                // Lowercase comparison
                if (h >= 'A' && h <= 'Z') h += 32;
                if (n >= 'A' && n <= 'Z') n += 32;
                if (h != n) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        return false;
    }
}

CNadeCam::CNadeCam()
    : m_bEnabled(false)
    , m_TrackedIndex(-1)
    , m_HasLastPos(false)
    , m_HasLastAngles(false)
    , m_HasLastMoveDir(false)
    , m_HasLastCamPos(false)
    , m_HasLastCamAngles(false)
    , m_AcquisitionLocked(false)
    , m_LastObserverMode(-1)
    , m_LastObserverIndex(-1)
    , m_LastWorldScan(-1.0f)
{
    memset(m_LastPos, 0, sizeof(m_LastPos));
    memset(m_LastAngles, 0, sizeof(m_LastAngles));
    memset(m_LastMoveDir, 0, sizeof(m_LastMoveDir));
    memset(m_LastCamPos, 0, sizeof(m_LastCamPos));
    memset(m_LastCamAngles, 0, sizeof(m_LastCamAngles));
}

CNadeCam::~CNadeCam() {
}

void CNadeCam::SetEnabled(bool enabled, bool restoreSpectator) {
    if (m_bEnabled == enabled) {
        return;
    }

    m_bEnabled = enabled;

    if (enabled) {
        Reset();
        advancedfx::Message("[NadeCam] Watching for grenades near the spectated player\n");
        // Note: spec_mode 4 will be called when grenade is acquired
    } else {
        // Return to normal spectator mode
        if (restoreSpectator && g_pEngineToClient && m_LastObserverIndex != -1) {
            g_pEngineToClient->ExecuteClientCmd(0, "spec_mode 2", true);
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "spec_player %d", m_LastObserverIndex);
            g_pEngineToClient->ExecuteClientCmd(0, cmd, true);
        }

        Reset();
        advancedfx::Message("[NadeCam] Stopped\n");
    }
}

void CNadeCam::Reset() {
    ResetTracking();
    m_Grenades.clear();
    m_LastWorldScan = -1.0f;
    m_AcquisitionLocked = false;
    m_LastObserverMode = -1;
    m_LastObserverIndex = -1;
}

void CNadeCam::ResetTracking() {
    m_TrackedIndex = -1;
    m_HasLastPos = false;
    m_HasLastAngles = false;
    m_HasLastMoveDir = false;
    m_HasLastCamPos = false;
    m_HasLastCamAngles = false;
}

bool CNadeCam::IsGrenadeEntity(void* entity) {
    if (!entity) return false;

    CEntityInstance* ent = (CEntityInstance*)entity;

    const char* clientClassName = ent->GetClientClassName();
    const char* className = ent->GetClassName();
    const char* debugName = ent->GetDebugName();

    // Check for "_projectile" suffix
    bool hasProjectileSuffix = false;
    if (clientClassName && StringContainsIgnoreCase(clientClassName, "_projectile")) hasProjectileSuffix = true;
    if (!hasProjectileSuffix && className && StringContainsIgnoreCase(className, "_projectile")) hasProjectileSuffix = true;
    if (!hasProjectileSuffix && debugName && StringContainsIgnoreCase(debugName, "_projectile")) hasProjectileSuffix = true;

    if (!hasProjectileSuffix) return false;

    // Check for grenade tokens
    for (int i = 0; i < numGrenadeTokens; ++i) {
        if (clientClassName && StringContainsIgnoreCase(clientClassName, grenadeTokens[i])) return true;
        if (className && StringContainsIgnoreCase(className, grenadeTokens[i])) return true;
        if (debugName && StringContainsIgnoreCase(debugName, grenadeTokens[i])) return true;
    }

    // Fallback: check for "grenade" and "projectile" together
    auto checkBoth = [](const char* name) {
        return name && StringContainsIgnoreCase(name, "grenade") && StringContainsIgnoreCase(name, "projectile");
    };

    if (checkBoth(clientClassName)) return true;
    if (checkBoth(className)) return true;
    if (checkBoth(debugName)) return true;

    return false;
}

void CNadeCam::ScanWorldForGrenades(const float observedOrigin[3]) {
    if (m_AcquisitionLocked) return;
    if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) return;

    float detectionRadiusSq = m_Config.detectionRadius * m_Config.detectionRadius;
    int highestIndex = GetHighestEntityIndex();

    for (int i = 0; i <= highestIndex; ++i) {
        CEntityInstance* ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, i);
        if (!ent) continue;

        if (!IsGrenadeEntity(ent)) continue;

        if (observedOrigin) {
            float origin[3];
            ent->GetOrigin(origin[0], origin[1], origin[2]);

            float distSq = SquaredDistance(observedOrigin, origin);
            if (distSq > detectionRadiusSq) continue;
        }

        m_Grenades[i] = ent;
    }
}

void CNadeCam::PruneGrenades() {
    if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) return;

    for (auto it = m_Grenades.begin(); it != m_Grenades.end(); ) {
        CEntityInstance* ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, it->first);
        if (!ent) {
            it = m_Grenades.erase(it);
        } else {
            ++it;
        }
    }
}

bool CNadeCam::AcquireGrenade(const float observedOrigin[3], float curTime) {
    if (m_AcquisitionLocked) return false;

    // Periodic world scan
    if (m_LastWorldScan < 0 || curTime - m_LastWorldScan >= m_Config.worldScanIntervalSec) {
        ScanWorldForGrenades(observedOrigin);
        m_LastWorldScan = curTime;
    }

    PruneGrenades();

    float detectionRadiusSq = m_Config.detectionRadius * m_Config.detectionRadius;
    int bestIdx = -1;
    void* bestEnt = nullptr;
    float bestDist = FLT_MAX;

    for (auto& kv : m_Grenades) {
        CEntityInstance* ent = (CEntityInstance*)kv.second;
        if (!ent) continue;

        float origin[3];
        ent->GetOrigin(origin[0], origin[1], origin[2]);

        float dist = SquaredDistance(observedOrigin, origin);
        if (dist <= detectionRadiusSq && dist < bestDist) {
            bestDist = dist;
            bestIdx = kv.first;
            bestEnt = ent;
        }
    }

    if (bestIdx != -1 && bestEnt) {
        m_TrackedIndex = bestIdx;
        ResetTracking();
        m_TrackedIndex = bestIdx;  // Restore after reset
        m_AcquisitionLocked = true;

        CEntityInstance* ent = (CEntityInstance*)bestEnt;
        const char* name = ent->GetClientClassName();
        if (!name) name = ent->GetDebugName();
        if (!name) name = ent->GetClassName();
        if (!name) name = "grenade";

        advancedfx::Message("[NadeCam] Tracking %s (#%d)\n", name, bestIdx);

        // Switch to free camera mode
        if (g_pEngineToClient) {
            g_pEngineToClient->ExecuteClientCmd(0, "spec_mode 4", true);
        }

        return true;
    }

    return false;
}

bool CNadeCam::GetObservedPawnOrigin(float outOrigin[3]) const {
    if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex || !g_ClientDll_GetSplitScreenPlayer) return false;

    // Get local controller from split screen player 0
    CEntityInstance* controller = g_ClientDll_GetSplitScreenPlayer(0);
    if (!controller) return false;

    // Get pawn handle from controller
    auto pawnHandle = controller->GetPlayerPawnHandle();
    if (!pawnHandle.IsValid()) return false;

    int pawnIndex = pawnHandle.GetEntryIndex();
    CEntityInstance* pawn = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, pawnIndex);
    if (!pawn) return false;

    // Check if observing another player
    auto observerHandle = pawn->GetObserverTarget();
    if (observerHandle.IsValid()) {
        int observedIndex = observerHandle.GetEntryIndex();
        CEntityInstance* observedPawn = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, observedIndex);
        if (observedPawn) {
            observedPawn->GetRenderEyeOrigin(outOrigin);
            return true;
        }
    }

    // Fallback to own pawn
    pawn->GetRenderEyeOrigin(outOrigin);
    return true;
}

int CNadeCam::GetObservedControllerIndex() const {
    if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex || !g_ClientDll_GetSplitScreenPlayer) return -1;

    CEntityInstance* localCtrl = g_ClientDll_GetSplitScreenPlayer(0);
    if (!localCtrl) return -1;

    auto pawnHandle = localCtrl->GetPlayerPawnHandle();
    if (!pawnHandle.IsValid()) return -1;

    int pawnIdx = pawnHandle.GetEntryIndex();
    CEntityInstance* localPawn = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, pawnIdx);
    if (!localPawn) return -1;

    auto tgtPawnHandle = localPawn->GetObserverTarget();
    if (!tgtPawnHandle.IsValid()) return -1;

    int tgtPawnIdx = tgtPawnHandle.GetEntryIndex();
    CEntityInstance* tgtPawn = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, tgtPawnIdx);
    if (!tgtPawn) return -1;

    auto ctrlHandle = tgtPawn->GetPlayerControllerHandle();
    if (!ctrlHandle.IsValid()) return -1;

    return ctrlHandle.GetEntryIndex();
}

uint8_t CNadeCam::GetObserverMode() const {
    if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex || !g_ClientDll_GetSplitScreenPlayer) return 0;

    CEntityInstance* controller = g_ClientDll_GetSplitScreenPlayer(0);
    if (!controller) return 0;

    auto pawnHandle = controller->GetPlayerPawnHandle();
    if (!pawnHandle.IsValid()) return 0;

    int pawnIndex = pawnHandle.GetEntryIndex();
    CEntityInstance* pawn = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, pawnIndex);
    if (!pawn) return 0;

    return pawn->GetObserverMode();
}

void* CNadeCam::ResolveTrackedEntity() {
    if (m_TrackedIndex < 0) return nullptr;
    if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) return nullptr;

    CEntityInstance* ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, m_TrackedIndex);
    return ent;
}

bool CNadeCam::Update(float deltaTime, float curTime, const CameraTransform& currentCam, CameraTransform& outTransform) {
    if (!m_bEnabled) return false;

    // Get observed player origin
    float observedOrigin[3];
    if (!GetObservedPawnOrigin(observedOrigin)) return false;

    // Update observer index for return
    int ctrlIdx = GetObservedControllerIndex();
    if (ctrlIdx >= 0) {
        m_LastObserverIndex = ctrlIdx;
    }

    // Check observer mode changes
    uint8_t obsMode = GetObserverMode();
    if ((obsMode == 0 || obsMode == 1 || obsMode == 2 || obsMode == 5) &&
        m_LastObserverMode != (int)obsMode &&
        m_TrackedIndex >= 0) {
        // Observer mode changed to non-freecam, reset tracking
        ResetTracking();
        m_AcquisitionLocked = false;
    }
    m_LastObserverMode = (int)obsMode;

    // Try to acquire grenade if not tracking
    void* tracked = ResolveTrackedEntity();
    bool justAcquired = false;
    if (!tracked) {
        justAcquired = AcquireGrenade(observedOrigin, curTime);
        tracked = ResolveTrackedEntity();
    }

    // If still no grenade, don't override the camera
    if (!tracked) {
        // Return last camera position if we were previously tracking
        if (m_HasLastCamPos && m_HasLastCamAngles) {
            outTransform.x = m_LastCamPos[0];
            outTransform.y = m_LastCamPos[1];
            outTransform.z = m_LastCamPos[2];
            outTransform.pitch = m_LastCamAngles[0];
            outTransform.yaw = m_LastCamAngles[1];
            outTransform.roll = 0.0f;
            outTransform.fov = currentCam.fov;
            return true;
        }
        return false;
    }

    // Initialize camera state from current game camera when first acquiring a grenade
    // This ensures smooth transition from wherever the camera currently is
    // Get grenade position
    CEntityInstance* grenadeEnt = (CEntityInstance*)tracked;
    float currentPos[3];
    grenadeEnt->GetOrigin(currentPos[0], currentPos[1], currentPos[2]);

    // Compute angles from previous position
    // When no previous position exists, use current camera angles (like the JS script does)
    // This ensures the camera starts behind the grenade in the direction the camera was looking
    float pitch, yaw;
    if (m_HasLastPos) {
        float fallbackPitch = m_HasLastAngles ? m_LastAngles[0] : currentCam.pitch;
        float fallbackYaw = m_HasLastAngles ? m_LastAngles[1] : currentCam.yaw;
        ComputeAngles(m_LastPos, currentPos, fallbackPitch, fallbackYaw, pitch, yaw);
    } else {
        // First frame: use current camera angles to initialize movement direction
        pitch = currentCam.pitch;
        yaw = currentCam.yaw;
    }

    // Keep direction continuous (avoid 180 degree flips)
    if (m_HasLastAngles) {
        KeepDirectionContinuous(pitch, yaw, m_LastAngles[0], m_LastAngles[1]);
    }

    // Compute movement direction and speed
    float moveDir[3] = {0, 0, 0};
    float speed = 0.0f;
    if (m_HasLastPos && deltaTime > 0.0001f) {
        float dx = currentPos[0] - m_LastPos[0];
        float dy = currentPos[1] - m_LastPos[1];
        float dz = currentPos[2] - m_LastPos[2];
        float len = sqrtf(dx * dx + dy * dy + dz * dz);
        if (len > 0.001f) {
            speed = len / deltaTime;
            moveDir[0] = dx / len;
            moveDir[1] = dy / len;
            moveDir[2] = dz / len;
        }
    }

    // Fallback to last move dir or forward from angles
    if (moveDir[0] == 0.0f && moveDir[1] == 0.0f && moveDir[2] == 0.0f) {
        if (m_HasLastMoveDir) {
            memcpy(moveDir, m_LastMoveDir, sizeof(moveDir));
        } else {
            AnglesToForward(pitch, yaw, moveDir);
        }
    }

    // Smooth movement direction
    if (m_HasLastMoveDir) {
        SmoothDirection(m_LastMoveDir, moveDir, deltaTime);
        memcpy(moveDir, m_LastMoveDir, sizeof(moveDir));
    }
    memcpy(m_LastMoveDir, moveDir, sizeof(m_LastMoveDir));
    m_HasLastMoveDir = true;

    // Flatten direction for slow grenades
    float offsetDirFlat[3];
    FlattenDirection(moveDir, pitch, yaw, offsetDirFlat);

    float flattenT = 0.0f;
    if (speed > 0.0f && m_Config.flattenStartSpeed > m_Config.flattenFullSpeed) {
        flattenT = (m_Config.flattenStartSpeed - speed) / (m_Config.flattenStartSpeed - m_Config.flattenFullSpeed);
        flattenT = (std::max)(0.0f, (std::min)(1.0f, flattenT));
    }

    // Mix movement direction with flattened direction
    float offsetDir[3] = {
        moveDir[0] * (1.0f - flattenT) + offsetDirFlat[0] * flattenT,
        moveDir[1] * (1.0f - flattenT) + offsetDirFlat[1] * flattenT,
        moveDir[2] * (1.0f - flattenT) + offsetDirFlat[2] * flattenT
    };
    float offsetLen = sqrtf(offsetDir[0] * offsetDir[0] + offsetDir[1] * offsetDir[1] + offsetDir[2] * offsetDir[2]);
    if (offsetLen > 1e-5f) {
        offsetDir[0] /= offsetLen;
        offsetDir[1] /= offsetLen;
        offsetDir[2] /= offsetLen;
    } else {
        memcpy(offsetDir, offsetDirFlat, sizeof(offsetDir));
    }

    // Make sure offset dir points same way as move dir
    float dot = offsetDir[0] * moveDir[0] + offsetDir[1] * moveDir[1] + offsetDir[2] * moveDir[2];
    if (dot < 0.0f) {
        offsetDir[0] = -offsetDir[0];
        offsetDir[1] = -offsetDir[1];
        offsetDir[2] = -offsetDir[2];
    }

    // Compute camera position (behind grenade)
    float camPos[3] = {
        currentPos[0] - offsetDir[0] * m_Config.followDistance,
        currentPos[1] - offsetDir[1] * m_Config.followDistance,
        currentPos[2] - offsetDir[2] * m_Config.followDistance + m_Config.followHeightOffset
    };

    // Ensure camera is above grenade when flattening
    if (flattenT > 0.0f) {
        camPos[2] = (std::max)(camPos[2], currentPos[2] + m_Config.minAboveGrenade * flattenT);
    }

    // Compute look angles
    float lookPitch, lookYaw;
    ComputeAngles(camPos, currentPos, pitch, yaw, lookPitch, lookYaw);

    // Initialize camera state on first frame (skip smoothing), then smooth on subsequent frames
    // This matches the JS script behavior: lastCamPos = camPos on first frame
    if (!m_HasLastCamPos) {
        memcpy(m_LastCamPos, camPos, sizeof(m_LastCamPos));
        m_HasLastCamPos = true;
    } else {
        SmoothPosition(m_LastCamPos, camPos, deltaTime);
    }

    if (!m_HasLastCamAngles) {
        m_LastCamAngles[0] = lookPitch;
        m_LastCamAngles[1] = lookYaw;
        m_HasLastCamAngles = true;
    } else {
        SmoothAngles(m_LastCamAngles[0], m_LastCamAngles[1], lookPitch, lookYaw, deltaTime);
    }

    // Update last state
    memcpy(m_LastPos, currentPos, sizeof(m_LastPos));
    m_HasLastPos = true;
    m_LastAngles[0] = pitch;
    m_LastAngles[1] = yaw;
    m_HasLastAngles = true;

    // Output transform
    outTransform.x = m_LastCamPos[0];
    outTransform.y = m_LastCamPos[1];
    outTransform.z = m_LastCamPos[2];
    outTransform.pitch = m_LastCamAngles[0];
    outTransform.yaw = m_LastCamAngles[1];
    outTransform.roll = 0.0f;
    outTransform.fov = currentCam.fov;

    return true;
}

// Math helpers

float CNadeCam::HalfExp(float halftime, float dt) const {
    float ht = halftime <= 0.0001f ? 0.0001f : halftime;
    return 1.0f - powf(0.5f, dt / ht);
}

void CNadeCam::SmoothPosition(float current[3], const float target[3], float dt) {
    float f = HalfExp(m_Config.posHalftimeSec, dt);
    current[0] += (target[0] - current[0]) * f;
    current[1] += (target[1] - current[1]) * f;
    current[2] += (target[2] - current[2]) * f;
}

void CNadeCam::SmoothAngles(float& currentPitch, float& currentYaw, float targetPitch, float targetYaw, float dt) {
    float f = HalfExp(m_Config.angHalftimeSec, dt);

    float yawDelta = NormalizeYaw(targetYaw - currentYaw);
    float pitchDelta = targetPitch - currentPitch;

    currentPitch += pitchDelta * f;
    currentYaw = NormalizeYaw(currentYaw + yawDelta * f);
}

void CNadeCam::SmoothDirection(float current[3], const float target[3], float dt) {
    float f = HalfExp(m_Config.moveDirHalftimeSec, dt);

    float blended[3] = {
        current[0] + (target[0] - current[0]) * f,
        current[1] + (target[1] - current[1]) * f,
        current[2] + (target[2] - current[2]) * f
    };

    float len = sqrtf(blended[0] * blended[0] + blended[1] * blended[1] + blended[2] * blended[2]);
    if (len > 1e-5f) {
        current[0] = blended[0] / len;
        current[1] = blended[1] / len;
        current[2] = blended[2] / len;
    }
}

void CNadeCam::ComputeAngles(const float from[3], const float to[3], float fallbackPitch, float fallbackYaw, float& outPitch, float& outYaw) const {
    float dx = to[0] - from[0];
    float dy = to[1] - from[1];
    float dz = to[2] - from[2];

    float flat = sqrtf(dx * dx + dy * dy);
    if (flat < 0.001f && fabsf(dz) < 0.001f) {
        outPitch = fallbackPitch;
        outYaw = fallbackYaw;
        return;
    }

    outYaw = atan2f(dy, dx) * (180.0f / (float)M_PI);
    outPitch = -atan2f(dz, flat) * (180.0f / (float)M_PI);
}

void CNadeCam::AnglesToForward(float pitch, float yaw, float out[3]) const {
    float pitchRad = pitch * ((float)M_PI / 180.0f);
    float yawRad = yaw * ((float)M_PI / 180.0f);

    float cp = cosf(pitchRad);
    float sp = sinf(pitchRad);
    float cy = cosf(yawRad);
    float sy = sinf(yawRad);

    out[0] = cp * cy;
    out[1] = cp * sy;
    out[2] = -sp;
}

float CNadeCam::NormalizeYaw(float a) const {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

float CNadeCam::AngleDiff(float a, float b) const {
    return fabsf(NormalizeYaw(a - b));
}

void CNadeCam::KeepDirectionContinuous(float& pitch, float& yaw, float prevPitch, float prevYaw) const {
    float oppositePitch = -pitch;
    float oppositeYaw = NormalizeYaw(yaw + 180.0f);

    float diff = AngleDiff(yaw, prevYaw) + fabsf(pitch - prevPitch);
    float diffOpp = AngleDiff(oppositeYaw, prevYaw) + fabsf(oppositePitch - prevPitch);

    if (diffOpp < diff) {
        pitch = oppositePitch;
        yaw = oppositeYaw;
    }
}

void CNadeCam::FlattenDirection(const float* dir, float fallbackPitch, float fallbackYaw, float out[3]) const {
    float base[3];
    if (dir) {
        memcpy(base, dir, sizeof(base));
    } else {
        AnglesToForward(fallbackPitch, fallbackYaw, base);
    }

    float planarLen = sqrtf(base[0] * base[0] + base[1] * base[1]);
    if (planarLen < 1e-4f) {
        // Fallback to camera forward on plane
        float fwd[3];
        AnglesToForward(fallbackPitch, fallbackYaw, fwd);
        base[0] = fwd[0];
        base[1] = fwd[1];
        base[2] = 0.0f;
        planarLen = sqrtf(base[0] * base[0] + base[1] * base[1]);
    }

    if (planarLen < 1e-4f) {
        out[0] = 1.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
    } else {
        out[0] = base[0] / planarLen;
        out[1] = base[1] / planarLen;
        out[2] = 0.0f;
    }
}

float CNadeCam::SquaredDistance(const float a[3], const float b[3]) const {
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

// Console command
CON_COMMAND(mirv_nadecam, "Grenade following camera.")
{
    auto argC = args->ArgC();
    auto arg0 = args->ArgV(0);

    if (argC >= 2) {
        const char* action = args->ArgV(1);

        if (0 == _stricmp(action, "start")) {
            if (g_pNadeCam) {
                g_pNadeCam->SetEnabled(true);
            }
            return;
        }

        if (0 == _stricmp(action, "stop")) {
            if (g_pNadeCam) {
                g_pNadeCam->SetEnabled(false);
            }
            return;
        }

        if (0 == _stricmp(action, "radius") && argC >= 3) {
            float v = (float)atof(args->ArgV(2));
            if (v > 0 && g_pNadeCam) {
                g_pNadeCam->GetConfig().detectionRadius = v;
                advancedfx::Message("[NadeCam] Detection radius = %.1f\n", v);
            } else {
                advancedfx::Message("%s radius <float> - Set detection radius (units). Current: %.1f\n",
                    arg0, g_pNadeCam ? g_pNadeCam->GetConfig().detectionRadius : 100.0f);
            }
            return;
        }
    }

    advancedfx::Message(
        "%s start - Start grenade camera\n"
        "%s stop - Stop grenade camera\n"
        "%s radius <units> - Set detection radius (current: %.1f)\n",
        arg0, arg0, arg0,
        g_pNadeCam ? g_pNadeCam->GetConfig().detectionRadius : 100.0f
    );
}
