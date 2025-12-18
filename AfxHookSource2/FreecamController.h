#pragma once

#include "ObsInputReceiver.h"
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
    {}
};

/// Freecam controller for professional eSports observing
/// Ported from mirv_camera.mjs with optimizations for C++
class CFreecamController {
public:
    CFreecamController();
    ~CFreecamController();

    /// Enable/disable freecam
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return m_bEnabled; }

    /// Enable/disable input processing (gates input without disabling camera)
    void SetInputEnabled(bool enabled) { m_bInputEnabled = enabled; }
    bool IsInputEnabled() const { return m_bInputEnabled; }

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

    // Math helpers
    float Clamp(float value, float min, float max);
    float Lerp(float a, float b, float t);
    void GetForwardVector(float pitch, float yaw, float& outX, float& outY, float& outZ);
    void GetRightVector(float yaw, float& outX, float& outY, float& outZ);
    void GetUpVector(float pitch, float yaw, float& outX, float& outY, float& outZ);

    bool m_bEnabled;
    bool m_bInputEnabled;  // Gates input processing without disabling camera
    bool m_bInitialized;
    FreecamConfig m_Config;

    // Camera state
    CameraTransform m_Transform;
    CameraTransform m_SmoothedTransform;

    // Velocity
    float m_VelocityX, m_VelocityY, m_VelocityZ;

    // Mouse state
    float m_MouseVelocityX, m_MouseVelocityY;

    float m_SpeedScalar;
    bool m_SpeedDirty;
    bool m_LastMouseButton4;
    bool m_LastMouseButton5;
    float m_MouseButton4Hold;
    float m_MouseButton5Hold;

    // Roll state
    float m_TargetRoll;
    float m_CurrentRoll;

    // Player lock state
    bool m_PlayerLockActive;
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
