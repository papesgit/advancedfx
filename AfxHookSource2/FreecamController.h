#pragma once

#include "ObsInputReceiver.h"
#include "../shared/AfxMath.h"
#include <chrono>

/// Camera transform for CS2 view
struct CameraTransform {
    float x, y, z;          // Position
    float pitch, yaw, roll; // Rotation (degrees)
    float fov;              // Field of view

    CameraTransform()
        : x(0), y(0), z(0)
        , pitch(0), yaw(0), roll(0)
        , fov(90.0f)
    {}
};

/// Freecam configuration
struct FreecamConfig {
    // Mouse
    float mouseSensitivity;

    // Movement
    float moveSpeed;
    float sprintMultiplier;
    float verticalSpeed;
    float speedAdjustRate;     // Multiplier delta per second when holding mouse4/5
    float speedMinMultiplier;  // Lower clamp for speed scaling
    float speedMaxMultiplier;  // Upper clamp for speed scaling

    // Roll
    float rollSpeed;
    float rollSmoothing;
    float leanStrength;
    float leanAccelScale;
    float leanVelocityScale;
    float leanMaxAngle;
    float leanHalfTime;
    bool clampPitch;

    // FOV
    float fovMin;
    float fovMax;
    float fovStep;
    float defaultFov;

    // Smoothing
    bool smoothEnabled;
    float halfVec;  // Half-time for position smoothing (seconds)
    float halfRot;  // Half-time for rotation smoothing (seconds)
    float lockHalfRot;  // Half-time for rotation smoothing when player lock is active (seconds)
    float lockHalfRotTransition;  // Duration to blend halfRot <-> lockHalfRot (seconds)
    float halfFov;  // Half-time for FOV smoothing (seconds)
    bool rotCriticalDamping;  // Use critically damped rotation smoothing (vs long-path slerp)
    float rotDampingRatio;  // Damping ratio for critical damping (>= 1.0)

    FreecamConfig()
        : mouseSensitivity(0.12f)
        , moveSpeed(200.0f)         // Increased from 320 for faster movement
        , sprintMultiplier(2.5f)
        , verticalSpeed(200.0f)     // Increased from 320 for faster vertical
        , speedAdjustRate(1.1f)
        , speedMinMultiplier(0.05f)
        , speedMaxMultiplier(5.0f)
        , rollSpeed(45.0f)
        , rollSmoothing(0.8f)
        , leanStrength(1.0f)
        , leanAccelScale(0.0015f)
        , leanVelocityScale(0.01f)
        , leanMaxAngle(20.0f)
        , leanHalfTime(0.18f)
        , clampPitch(false)
        , fovMin(10.0f)
        , fovMax(150.0f)
        , fovStep(2.0f)
        , defaultFov(90.0f)
        , smoothEnabled(true)
        , halfVec(0.5f)             // Exponential half-time for position
        , halfRot(0.5f)             // Exponential half-time for rotation
        , lockHalfRot(0.2f)
        , lockHalfRotTransition(1.0f)
        , halfFov(0.5f)             // Exponential half-time for FOV
        , rotCriticalDamping(false)
        , rotDampingRatio(1.0f)
    {}
};

/// Freecam controller for professional eSports observing
/// Ported from mirv_camera.mjs with optimizations for C++
class CFreecamController {
public:
    enum class HoldMovementMode {
        Camera,
        World
    };

    CFreecamController();
    ~CFreecamController();

    /// Enable/disable freecam
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return m_bEnabled; }

    /// Enable/disable input processing (gates input without disabling camera)
    void SetInputEnabled(bool enabled);
    bool IsInputEnabled() const { return m_bInputEnabled; }

    /// Enable/disable input hold (keeps last input motion while ignoring new input)
    void SetInputHold(bool enabled);
    bool IsInputHold() const { return m_bInputHold; }
    void SetHoldMovementMode(HoldMovementMode mode);
    HoldMovementMode GetHoldMovementMode() const { return m_HoldMovementMode; }

    bool m_PlayerLockActive;

    /// Get configuration (mutable for settings)
    FreecamConfig& GetConfig() { return m_Config; }

    /// Update camera with input state (call every frame)
    /// @param input Input state from ObsInputReceiver
    /// @param deltaTime Time since last update (seconds)
    void Update(const InputState& input, float deltaTime);

    /// Get current camera transform
    const CameraTransform& GetTransform() const { return m_SmoothedTransform; }

    float GetCurrentMoveSpeed() const { return m_Config.moveSpeed * m_SpeedScalar; }
    float GetCurrentVerticalSpeed() const { return m_Config.verticalSpeed * m_SpeedScalar; }
    float GetSpeedMultiplier() const { return m_SpeedScalar; }
    void SetSpeedScalar(float value);
    float GetMinMoveSpeed() const { return m_Config.moveSpeed * m_Config.speedMinMultiplier; }
    float GetMaxMoveSpeed() const { return m_Config.moveSpeed * m_Config.speedMaxMultiplier; }
    bool IsSpeedDirty() const { return m_SpeedDirty; }
    void ClearSpeedDirtyFlag() { m_SpeedDirty = false; }

    /// Reset camera to specified position/rotation
    void Reset(const CameraTransform& transform);

    /// Override smoothed transform (for seamless handoff)
    void SetSmoothedTransform(const CameraTransform& transform);

    /// Reset camera to origin
    void Reset();

private:
    void UpdateMouseLook(const InputState& input, float deltaTime);
    void UpdateMovement(const InputState& input, float deltaTime);
    void UpdateRoll(const InputState& input, float deltaTime);
    void UpdateFOV(const InputState& input);
    void UpdateSpeed(const InputState& input, float deltaTime);
    void ResetSpeed();
    void UpdatePlayerLock(const InputState& input, float deltaTime);
    bool TryAcquirePlayerLockTarget(float& outX, float& outY, float& outZ, int& outHandle);
    bool GetLockedTargetEye(float& outX, float& outY, float& outZ);
    void StartHalfRotTransition(float targetHalfRot);
    void UpdateHalfRotTransition(float deltaTime);
    void ApplyLockAngles(float targetX, float targetY, float targetZ);
    void ApplySmoothing(float deltaTime);
    void ComputeHoldAngularVelocity();
    void ApplyHoldRotation(float deltaTime);
    void ComputeHoldMovementBasis();
    void ApplyHoldMovement(float deltaTime);
    Afx::Math::Quaternion BuildQuat(const CameraTransform& transform) const;
    void UpdateAnglesFromQuat(const Afx::Math::Quaternion& q, CameraTransform& out, const CameraTransform& hint) const;
    Afx::Math::Vector3 GetWorldAngularVelocity(float yawRateDeg, float pitchRateDeg, float rollRateDeg, const CameraTransform& transform) const;
    Afx::Math::Quaternion IntegrateQuat(const Afx::Math::Quaternion& q, const Afx::Math::Vector3& angularVelocity, float deltaTime) const;

    // Math helpers
    float Clamp(float value, float min, float max);
    float Lerp(float a, float b, float t);
    float SmoothDamp(float current, float target, float& currentVelocity, float smoothTime, float deltaTime);
    void GetForwardVector(float pitch, float yaw, float& outX, float& outY, float& outZ) const;
    void GetRightVector(float yaw, float& outX, float& outY, float& outZ) const;
    void GetUpVector(float pitch, float yaw, float& outX, float& outY, float& outZ) const;

    bool m_bEnabled;
    bool m_bInputEnabled;  // Gates input processing without disabling camera
    bool m_bInputHold;     // Keeps last input motion while ignoring new input
    bool m_bInitialized;
    FreecamConfig m_Config;

    // Camera state
    CameraTransform m_Transform;
    CameraTransform m_SmoothedTransform;

    // Velocity
    float m_VelocityX, m_VelocityY, m_VelocityZ;

    // Mouse state
    float m_MouseVelocityX, m_MouseVelocityY;
    float m_HoldYawVelocity;
    float m_HoldPitchVelocity;
    float m_HoldLocalForward;
    float m_HoldLocalRight;
    float m_HoldLocalUp;
    float m_HoldWorldVelocityX;
    float m_HoldWorldVelocityY;
    float m_HoldWorldVelocityZ;
    HoldMovementMode m_HoldMovementMode;

    float m_SpeedScalar;
    bool m_SpeedDirty;
    bool m_LastMouseButton4;
    bool m_LastMouseButton5;
    float m_MouseButton4Hold;
    float m_MouseButton5Hold;

    // Roll state
    float m_TargetRoll;
    float m_CurrentRoll;
    float m_RollVelocity;
    float m_LastLateralVelocity;
    float m_LastSmoothedX;
    float m_LastSmoothedY;
    float m_LastSmoothedZ;
    Afx::Math::Quaternion m_RawQuat;
    Afx::Math::Quaternion m_SmoothedQuat;
    Afx::Math::Vector3 m_RotVelocity;
    Afx::Math::Vector3 m_HoldRotVelocity;

    // Player lock state
    bool m_LastKeyVDown;
    int m_PlayerLockHandle;
    float m_PlayerLockReturnHalfRot;
    float m_CurrentHalfRot;
    float m_HalfRotTransitionStart;
    float m_HalfRotTransitionTarget;
    float m_HalfRotTransitionElapsed;
    bool m_HalfRotTransitionActive;

    // Timing
    std::chrono::steady_clock::time_point m_LastUpdateTime;
};
