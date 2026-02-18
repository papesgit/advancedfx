#pragma once

#include "FreecamController.h"
#include "../deps/release/prop/cs2/sdk_src/public/entityhandle.h"
#include <map>

/// Configuration for grenade camera
struct NadeCamConfig {
    float detectionRadius;      // Radius to detect grenades near spectated player
    float followDistance;       // Distance behind grenade
    float followHeightOffset;   // Height offset above the follow position
    float minAboveGrenade;      // Minimum height above grenade when flattening
    float moveDirHalftimeSec;   // Smoothing half-time for movement direction
    float posHalftimeSec;       // Smoothing half-time for camera position
    float angHalftimeSec;       // Smoothing half-time for camera angles
    float flattenStartSpeed;    // Speed at which flattening starts
    float flattenFullSpeed;     // Speed at which flattening is fully applied
    float worldScanIntervalSec; // Interval between world scans for grenades

    NadeCamConfig()
        : detectionRadius(200.0f)
        , followDistance(96.0f)
        , followHeightOffset(12.0f)
        , minAboveGrenade(20.0f)
        , moveDirHalftimeSec(0.08f)
        , posHalftimeSec(0.05f)
        , angHalftimeSec(0.05f)
        , flattenStartSpeed(100.0f)
        , flattenFullSpeed(40.0f)
        , worldScanIntervalSec(0.2f)
    {}
};

/// Grenade-following camera controller
/// Implements the same behavior as mirv_nade.js script natively
class CNadeCam {
public:
    CNadeCam();
    ~CNadeCam();

    /// Enable/disable nade cam
    /// @param restoreSpectator When disabling, restore previous spectated player if true.
    void SetEnabled(bool enabled, bool restoreSpectator = true);
    bool IsEnabled() const { return m_bEnabled; }

    /// Get configuration (mutable for settings)
    NadeCamConfig& GetConfig() { return m_Config; }

    /// Update camera and get transform
    /// @param deltaTime Time since last update (seconds)
    /// @param curTime Current game time (for world scan timing)
    /// @param currentCam Current camera transform (used to initialize smoothly)
    /// @param outTransform Output camera transform
    /// @return true if camera should be overridden
    bool Update(float deltaTime, float curTime, const CameraTransform& currentCam, CameraTransform& outTransform);

    /// Reset all tracking state
    void Reset();

private:
    // Grenade detection
    static bool IsGrenadeEntity(void* entity);
    void ScanWorldForGrenades(const float observedOrigin[3]);
    void PruneGrenades();
    bool AcquireGrenade(const float observedOrigin[3], float curTime);

    // Observer helpers
    bool GetObservedPawnOrigin(float outOrigin[3]) const;
    int GetObservedControllerIndex() const;
    uint8_t GetObserverMode() const;

    // Tracking
    void ResetTracking();
    void* ResolveTrackedEntity();

    // Math helpers
    float HalfExp(float halftime, float dt) const;
    void SmoothPosition(float current[3], const float target[3], float dt);
    void SmoothAngles(float& currentPitch, float& currentYaw, float targetPitch, float targetYaw, float dt);
    void SmoothDirection(float current[3], const float target[3], float dt);
    void ComputeAngles(const float from[3], const float to[3], float fallbackPitch, float fallbackYaw, float& outPitch, float& outYaw) const;
    void AnglesToForward(float pitch, float yaw, float out[3]) const;
    float NormalizeYaw(float a) const;
    float AngleDiff(float a, float b) const;
    void KeepDirectionContinuous(float& pitch, float& yaw, float prevPitch, float prevYaw) const;
    void FlattenDirection(const float* dir, float fallbackPitch, float fallbackYaw, float out[3]) const;
    float SquaredDistance(const float a[3], const float b[3]) const;

    bool m_bEnabled;
    NadeCamConfig m_Config;

    // Tracking state
    int m_TrackedIndex;
    float m_LastPos[3];
    bool m_HasLastPos;
    float m_LastAngles[2];  // pitch, yaw
    bool m_HasLastAngles;
    float m_LastMoveDir[3];
    bool m_HasLastMoveDir;
    float m_LastCamPos[3];
    bool m_HasLastCamPos;
    float m_LastCamAngles[2];  // pitch, yaw
    bool m_HasLastCamAngles;
    bool m_AcquisitionLocked;

    // Observer state
    int m_LastObserverMode;
    int m_LastObserverIndex;

    // Grenade tracking
    std::map<int, void*> m_Grenades;  // index -> entity pointer
    float m_LastWorldScan;
};

extern CNadeCam* g_pNadeCam;
