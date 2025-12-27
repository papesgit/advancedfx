#include "FreecamController.h"
#include "ClientEntitySystem.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {
constexpr uint8_t kVkCtrl       = 0x11;
constexpr uint8_t kVkLeftCtrl   = 0xA2;
constexpr uint8_t kVkRightCtrl  = 0xA3;

constexpr uint8_t kVkShift      = 0x10;
constexpr uint8_t kVkLeftShift  = 0xA0;
constexpr uint8_t kVkRightShift = 0xA1;

constexpr uint8_t kVkAlt        = 0x12;  // VK_MENU
constexpr uint8_t kVkLeftAlt    = 0xA4;  // VK_LMENU
constexpr uint8_t kVkRightAlt   = 0xA5;  // VK_RMENU
constexpr uint8_t kVkV          = 0x56;

bool IsCtrlDown(const InputState& input) {
    return input.IsAnyKeyDown({kVkLeftCtrl, kVkRightCtrl, kVkCtrl});
}

bool IsShiftDown(const InputState& input) {
    return input.IsAnyKeyDown({kVkLeftShift, kVkRightShift, kVkShift});
}

bool IsAltDown(const InputState& input) {
    return input.IsAnyKeyDown({kVkLeftAlt, kVkRightAlt, kVkAlt});
}

double CalcDeltaExpSmooth(double deltaT, double deltaVal) {
    const double limitTime = 19.931568569324174087221916576936;
    if (deltaT < 0.0) {
        return 0.0;
    }
    if (limitTime < deltaT) {
        return deltaVal;
    }

    const double halfTime = 0.69314718055994530941723212145818;
    double x = 1.0 / exp(deltaT * halfTime);
    return (1.0 - x) * deltaVal;
}
}

CFreecamController::CFreecamController()
    : m_bEnabled(false)
    , m_bInputEnabled(false)
    , m_bInputHold(false)
    , m_bInitialized(false)
    , m_VelocityX(0), m_VelocityY(0), m_VelocityZ(0)
    , m_MouseVelocityX(0), m_MouseVelocityY(0)
    , m_HoldYawVelocity(0), m_HoldPitchVelocity(0)
    , m_HoldLocalForward(0), m_HoldLocalRight(0), m_HoldLocalUp(0)
    , m_HoldWorldVelocityX(0), m_HoldWorldVelocityY(0), m_HoldWorldVelocityZ(0)
    , m_HoldMovementMode(HoldMovementMode::Camera)
    , m_SpeedScalar(1.0f), m_SpeedDirty(false)
    , m_LastMouseButton4(false), m_LastMouseButton5(false)
    , m_MouseButton4Hold(0.0f), m_MouseButton5Hold(0.0f)
    , m_TargetRoll(0), m_CurrentRoll(0), m_RollVelocity(0.0f), m_LastLateralVelocity(0.0f)
    , m_LastSmoothedX(0.0f), m_LastSmoothedY(0.0f), m_LastSmoothedZ(0.0f)
    , m_RawQuat(1.0, 0.0, 0.0, 0.0)
    , m_SmoothedQuat(1.0, 0.0, 0.0, 0.0)
    , m_RotVelocity(0.0, 0.0, 0.0)
    , m_HoldRotVelocity(0.0, 0.0, 0.0)
    , m_PlayerLockActive(false)
    , m_LastKeyVDown(false)
    , m_PlayerLockHandle(-1)
    , m_PlayerLockReturnHalfRot(0.0f)
    , m_CurrentHalfRot(0.0f)
    , m_HalfRotTransitionStart(0.0f)
    , m_HalfRotTransitionTarget(0.0f)
    , m_HalfRotTransitionElapsed(0.0f)
    , m_HalfRotTransitionActive(false)
{
    m_LastUpdateTime = std::chrono::steady_clock::now();
    m_CurrentHalfRot = m_Config.halfRot;
    m_PlayerLockReturnHalfRot = m_Config.halfRot;
}

CFreecamController::~CFreecamController() {
}

void CFreecamController::SetEnabled(bool enabled) {
    if (m_bEnabled == enabled) {
        return;
    }

    m_bEnabled = enabled;
    m_bInputEnabled = enabled;  // Enable/disable input processing along with freecam

    if (enabled) {
        // Reset state on enable
        m_bInitialized = false;
        m_VelocityX = m_VelocityY = m_VelocityZ = 0;
        m_MouseVelocityX = m_MouseVelocityY = 0;
        m_bInputHold = false;
        m_HoldYawVelocity = 0;
        m_HoldPitchVelocity = 0;
        m_HoldLocalForward = 0;
        m_HoldLocalRight = 0;
        m_HoldLocalUp = 0;
        m_HoldWorldVelocityX = 0;
        m_HoldWorldVelocityY = 0;
        m_HoldWorldVelocityZ = 0;
        m_TargetRoll = m_CurrentRoll = 0;
        m_RollVelocity = 0.0f;
        m_LastLateralVelocity = 0.0f;
        m_LastSmoothedX = 0.0f;
        m_LastSmoothedY = 0.0f;
        m_LastSmoothedZ = 0.0f;
        m_RawQuat = BuildQuat(m_Transform);
        m_SmoothedQuat = m_RawQuat;
        m_RotVelocity = Afx::Math::Vector3(0.0, 0.0, 0.0);
        m_HoldRotVelocity = Afx::Math::Vector3(0.0, 0.0, 0.0);
        m_PlayerLockActive = false;
        m_LastKeyVDown = false;
        m_PlayerLockHandle = -1;
        m_PlayerLockReturnHalfRot = m_Config.halfRot;
        m_CurrentHalfRot = m_Config.halfRot;
        m_HalfRotTransitionActive = false;
        m_HalfRotTransitionElapsed = 0.0f;
        ResetSpeed();
        m_LastUpdateTime = std::chrono::steady_clock::now();
    } else {
        m_bInputHold = false;
        m_HoldYawVelocity = 0;
        m_HoldPitchVelocity = 0;
        m_HoldLocalForward = 0;
        m_HoldLocalRight = 0;
        m_HoldLocalUp = 0;
        m_HoldWorldVelocityX = 0;
        m_HoldWorldVelocityY = 0;
        m_HoldWorldVelocityZ = 0;
    }
}

void CFreecamController::SetInputEnabled(bool enabled) {
    m_bInputEnabled = enabled;
    if (enabled) {
        m_bInputHold = false;
        m_HoldYawVelocity = 0;
        m_HoldPitchVelocity = 0;
        m_HoldLocalForward = 0;
        m_HoldLocalRight = 0;
        m_HoldLocalUp = 0;
        m_HoldWorldVelocityX = 0;
        m_HoldWorldVelocityY = 0;
        m_HoldWorldVelocityZ = 0;
    }
}

void CFreecamController::SetInputHold(bool enabled) {
    if (!m_bEnabled) {
        return;
    }
    if (enabled) {
        ComputeHoldAngularVelocity();
        if (m_HoldMovementMode == HoldMovementMode::Camera) {
            ComputeHoldMovementBasis();
        } else {
            m_HoldWorldVelocityX = m_VelocityX;
            m_HoldWorldVelocityY = m_VelocityY;
            m_HoldWorldVelocityZ = m_VelocityZ;
        }
    }
    m_bInputHold = enabled;
}

void CFreecamController::SetHoldMovementMode(HoldMovementMode mode) {
    if (m_HoldMovementMode == mode) {
        return;
    }

    m_HoldMovementMode = mode;
    if (m_bInputHold) {
        if (m_HoldMovementMode == HoldMovementMode::Camera) {
            ComputeHoldMovementBasis();
        } else {
            m_HoldWorldVelocityX = m_VelocityX;
            m_HoldWorldVelocityY = m_VelocityY;
            m_HoldWorldVelocityZ = m_VelocityZ;
        }
    }
}

void CFreecamController::Reset(const CameraTransform& transform) {
    m_Transform = transform;
    m_SmoothedTransform = transform;
    m_RawQuat = BuildQuat(transform);
    m_SmoothedQuat = m_RawQuat;
    m_RotVelocity = Afx::Math::Vector3(0.0, 0.0, 0.0);
    m_HoldRotVelocity = Afx::Math::Vector3(0.0, 0.0, 0.0);
    m_VelocityX = m_VelocityY = m_VelocityZ = 0;
    m_MouseVelocityX = m_MouseVelocityY = 0;
    m_HoldYawVelocity = 0;
    m_HoldPitchVelocity = 0;
    m_HoldLocalForward = 0;
    m_HoldLocalRight = 0;
    m_HoldLocalUp = 0;
    m_HoldWorldVelocityX = 0;
    m_HoldWorldVelocityY = 0;
    m_HoldWorldVelocityZ = 0;
    m_TargetRoll = m_CurrentRoll = transform.roll;
    m_RollVelocity = 0.0f;
    m_LastLateralVelocity = 0.0f;
    m_LastSmoothedX = m_SmoothedTransform.x;
    m_LastSmoothedY = m_SmoothedTransform.y;
    m_LastSmoothedZ = m_SmoothedTransform.z;
    m_CurrentHalfRot = m_Config.halfRot;
    m_PlayerLockReturnHalfRot = m_Config.halfRot;
    m_HalfRotTransitionActive = false;
    m_HalfRotTransitionElapsed = 0.0f;
    ResetSpeed();
    m_bInitialized = true;
}

void CFreecamController::SetSmoothedTransform(const CameraTransform& transform) {
    m_SmoothedTransform = transform;
    m_SmoothedQuat = BuildQuat(transform);
    m_RotVelocity = Afx::Math::Vector3(0.0, 0.0, 0.0);
    m_bInitialized = true;
}

void CFreecamController::Reset() {
    CameraTransform origin;
    Reset(origin);
}

void CFreecamController::Update(const InputState& input, float deltaTime) {
    if (!m_bEnabled) {
        return;
    }

    // Clamp deltaTime to prevent large jumps
    deltaTime = (std::min)(deltaTime, 0.1f);

    if (m_bInputHold) {
        ApplyHoldRotation(deltaTime);
        UpdatePlayerLock(InputState{}, deltaTime);
        UpdateRoll(InputState{}, deltaTime);

        // Apply stored velocity without consuming new input.
        ApplyHoldMovement(deltaTime);
    } else {
        // Only process input if input is enabled (gated when right-click released)
        if (m_bInputEnabled) {
            UpdateSpeed(input, deltaTime);
            UpdateMouseLook(input, deltaTime);
        }

        UpdatePlayerLock(input, deltaTime);

        if (m_bInputEnabled) {
            UpdateMovement(input, deltaTime);
        }

        UpdateRoll(m_bInputEnabled ? input : InputState{}, deltaTime);

        if (m_bInputEnabled) {
            UpdateFOV(input);
        }
    }

    m_RawQuat = BuildQuat(m_Transform);

    // Always apply smoothing (even when input is disabled) to maintain camera position
    if (m_Config.smoothEnabled) {
        ApplySmoothing(deltaTime);
    } else {
        m_SmoothedTransform = m_Transform;
        m_SmoothedQuat = m_RawQuat;
        m_RotVelocity = Afx::Math::Vector3(0.0, 0.0, 0.0);
    }
}

void CFreecamController::UpdateMouseLook(const InputState& input, float deltaTime) {
    if (deltaTime <= 0) {
        return;
    }

    // Apply mouse deltas directly to angles
    float deltaYaw = -input.mouseDx * m_Config.mouseSensitivity;
    float deltaPitch = input.mouseDy * m_Config.mouseSensitivity;

    m_Transform.yaw += deltaYaw;
    m_Transform.pitch += deltaPitch;

    // Store velocities for roll calculation
    if (deltaTime > 0) {
        m_MouseVelocityX = deltaYaw / deltaTime;
        m_MouseVelocityY = deltaPitch / deltaTime;
    }

    if (m_Config.clampPitch) {
        m_Transform.pitch = Clamp(m_Transform.pitch, -89.0f, 89.0f);
    }
}

void CFreecamController::ComputeHoldAngularVelocity() {
    m_HoldYawVelocity = 0.0f;
    m_HoldPitchVelocity = 0.0f;
    m_HoldRotVelocity = Afx::Math::Vector3(0.0, 0.0, 0.0);

    if (m_Config.smoothEnabled) {
        m_HoldRotVelocity = m_RotVelocity;
        return;
    }

    m_HoldRotVelocity = GetWorldAngularVelocity(m_MouseVelocityX, m_MouseVelocityY, 0.0f, m_Transform);
}

void CFreecamController::ApplyHoldRotation(float deltaTime) {
    m_RawQuat = IntegrateQuat(m_RawQuat, m_HoldRotVelocity, deltaTime);
    UpdateAnglesFromQuat(m_RawQuat, m_Transform, m_Transform);

    if (m_Config.clampPitch) {
        m_Transform.pitch = Clamp(m_Transform.pitch, -89.0f, 89.0f);
        m_RawQuat = BuildQuat(m_Transform);
    }
}

void CFreecamController::ComputeHoldMovementBasis() {
    float forwardX, forwardY, forwardZ;
    GetForwardVector(m_Transform.pitch, m_Transform.yaw, forwardX, forwardY, forwardZ);

    float rightX, rightY, rightZ;
    GetRightVector(m_Transform.yaw, rightX, rightY, rightZ);

    float upX, upY, upZ;
    GetUpVector(m_Transform.pitch, m_Transform.yaw, upX, upY, upZ);

    m_HoldLocalForward = m_VelocityX * forwardX + m_VelocityY * forwardY + m_VelocityZ * forwardZ;
    m_HoldLocalRight = m_VelocityX * rightX + m_VelocityY * rightY + m_VelocityZ * rightZ;
    m_HoldLocalUp = m_VelocityX * upX + m_VelocityY * upY + m_VelocityZ * upZ;
}

void CFreecamController::ApplyHoldMovement(float deltaTime) {
    if (m_HoldMovementMode == HoldMovementMode::World) {
        m_VelocityX = m_HoldWorldVelocityX;
        m_VelocityY = m_HoldWorldVelocityY;
        m_VelocityZ = m_HoldWorldVelocityZ;

        m_Transform.x += m_VelocityX * deltaTime;
        m_Transform.y += m_VelocityY * deltaTime;
        m_Transform.z += m_VelocityZ * deltaTime;
        return;
    }

    float forwardX, forwardY, forwardZ;
    GetForwardVector(m_Transform.pitch, m_Transform.yaw, forwardX, forwardY, forwardZ);

    float rightX, rightY, rightZ;
    GetRightVector(m_Transform.yaw, rightX, rightY, rightZ);

    float upX, upY, upZ;
    GetUpVector(m_Transform.pitch, m_Transform.yaw, upX, upY, upZ);

    m_VelocityX = forwardX * m_HoldLocalForward + rightX * m_HoldLocalRight + upX * m_HoldLocalUp;
    m_VelocityY = forwardY * m_HoldLocalForward + rightY * m_HoldLocalRight + upY * m_HoldLocalUp;
    m_VelocityZ = forwardZ * m_HoldLocalForward + rightZ * m_HoldLocalRight + upZ * m_HoldLocalUp;

    m_Transform.x += m_VelocityX * deltaTime;
    m_Transform.y += m_VelocityY * deltaTime;
    m_Transform.z += m_VelocityZ * deltaTime;
}

void CFreecamController::UpdateMovement(const InputState& input, float deltaTime) {
    // Calculate movement speed
    float moveSpeed = GetCurrentMoveSpeed();
    float verticalSpeed = GetCurrentVerticalSpeed();
    if (input.analogEnabled) {
        float sprintInput = Clamp(input.analogRX, 0.0f, 1.0f);
        if (sprintInput <= 0.0f && IsShiftDown(input)) {
            sprintInput = 1.0f;
        }
        float sprintFactor = 1.0f + sprintInput * (m_Config.sprintMultiplier - 1.0f);
        moveSpeed *= sprintFactor;
        verticalSpeed *= sprintFactor;
    } else if (IsShiftDown(input)) {
        moveSpeed *= m_Config.sprintMultiplier;
        verticalSpeed *= m_Config.sprintMultiplier;
    }

    // Get direction vectors
    float forwardX, forwardY, forwardZ;
    GetForwardVector(m_Transform.pitch, m_Transform.yaw, forwardX, forwardY, forwardZ);

    float rightX, rightY, rightZ;
    GetRightVector(m_Transform.yaw, rightX, rightY, rightZ);

    float upX, upY, upZ;
    GetUpVector(m_Transform.pitch, m_Transform.yaw, upX, upY, upZ);

    // Calculate desired velocity
    float desiredVelX = 0, desiredVelY = 0, desiredVelZ = 0;

    if (input.analogEnabled) {
        float analogX = Clamp(input.analogLX, -1.0f, 1.0f);
        float analogY = Clamp(input.analogLY, -1.0f, 1.0f);
        float analogZ = Clamp(input.analogRY, -1.0f, 1.0f);

        desiredVelX += forwardX * (moveSpeed * analogY);
        desiredVelY += forwardY * (moveSpeed * analogY);
        desiredVelZ += forwardZ * (moveSpeed * analogY);

        desiredVelX += rightX * (moveSpeed * analogX);
        desiredVelY += rightY * (moveSpeed * analogX);

        desiredVelX += upX * (verticalSpeed * analogZ);
        desiredVelY += upY * (verticalSpeed * analogZ);
        desiredVelZ += upZ * (verticalSpeed * analogZ);
    } else {
        // Forward/backward (W/S)
        if (input.IsKeyDown('W')) {
            desiredVelX += forwardX * moveSpeed;
            desiredVelY += forwardY * moveSpeed;
            desiredVelZ += forwardZ * moveSpeed;
        }
        if (input.IsKeyDown('S')) {
            desiredVelX -= forwardX * moveSpeed;
            desiredVelY -= forwardY * moveSpeed;
            desiredVelZ -= forwardZ * moveSpeed;
        }

        // Strafe (A/D)
        if (input.IsKeyDown('A')) {
            desiredVelX -= rightX * moveSpeed;
            desiredVelY -= rightY * moveSpeed;
        }
        if (input.IsKeyDown('D')) {
            desiredVelX += rightX * moveSpeed;
            desiredVelY += rightY * moveSpeed;
        }

        // Vertical (Space/Ctrl)
        if (input.IsKeyDown(' ')) {
            desiredVelX += upX * verticalSpeed;
            desiredVelY += upY * verticalSpeed;
            desiredVelZ += upZ * verticalSpeed;
        }
        if (IsCtrlDown(input)) {
            desiredVelX -= upX * verticalSpeed;
            desiredVelY -= upY * verticalSpeed;
            desiredVelZ -= upZ * verticalSpeed;
        }
    }

    // Normalize diagonal movement to prevent faster speed
    float desiredSpeed = sqrtf(desiredVelX * desiredVelX +
                               desiredVelY * desiredVelY +
                               desiredVelZ * desiredVelZ);

    // Determine max speed based on what keys are pressed
    float maxSpeed = moveSpeed;
    if ((input.analogEnabled && fabsf(input.analogRY) > 0.0001f) || (!input.analogEnabled && (input.IsKeyDown(' ') || IsCtrlDown(input)))) {
        // If vertical movement, use the larger of the two speeds
        maxSpeed = (verticalSpeed > moveSpeed) ? verticalSpeed : moveSpeed;
    }

    // Clamp to max speed if diagonal movement exceeds it
    if (desiredSpeed > maxSpeed && desiredSpeed > 0.001f) {
        float scale = maxSpeed / desiredSpeed;
        desiredVelX *= scale;
        desiredVelY *= scale;
        desiredVelZ *= scale;
    }

    // No acceleration filter - instant velocity change
    // Smoothing is handled by exponential half-time in ApplySmoothing()
    m_VelocityX = desiredVelX;
    m_VelocityY = desiredVelY;
    m_VelocityZ = desiredVelZ;

    // Apply velocity to position
    m_Transform.x += m_VelocityX * deltaTime;
    m_Transform.y += m_VelocityY * deltaTime;
    m_Transform.z += m_VelocityZ * deltaTime;
}

void CFreecamController::UpdateRoll(const InputState& input, float deltaTime) {
    // Manual roll (Q/E) only when smoothing disabled
    if (!m_Config.smoothEnabled) {
        if (input.IsKeyDown('Q')) {
            m_TargetRoll += m_Config.rollSpeed * deltaTime;
        }
        if (input.IsKeyDown('E')) {
            m_TargetRoll -= m_Config.rollSpeed * deltaTime;
        }
    } else {
        m_TargetRoll = 0;
    }

    // Dynamic roll (only when smoothing enabled)
    float dynamicRoll = 0;
    if (m_Config.smoothEnabled) {
        const CameraTransform& view = m_Config.smoothEnabled ? m_SmoothedTransform : m_Transform;

        float posBlend = (m_Config.halfVec > 0.0f)
            ? 1.0f - expf((-logf(2.0f) * deltaTime) / m_Config.halfVec)
            : 1.0f;

        float smoothedX = Lerp(m_SmoothedTransform.x, m_Transform.x, posBlend);
        float smoothedY = Lerp(m_SmoothedTransform.y, m_Transform.y, posBlend);
        float smoothedZ = Lerp(m_SmoothedTransform.z, m_Transform.z, posBlend);

        float velX = 0.0f;
        float velY = 0.0f;
        float velZ = 0.0f;
        if (deltaTime > 0.0f) {
            velX = (smoothedX - m_LastSmoothedX) / deltaTime;
            velY = (smoothedY - m_LastSmoothedY) / deltaTime;
            velZ = (smoothedZ - m_LastSmoothedZ) / deltaTime;
        }

        m_LastSmoothedX = smoothedX;
        m_LastSmoothedY = smoothedY;
        m_LastSmoothedZ = smoothedZ;

        // Use smoothed orientation for lateral basis to match what the user sees.
        float rightX, rightY, rightZ;
        GetRightVector(view.yaw, rightX, rightY, rightZ);

        float lateralVelocity = velX * rightX + velY * rightY + velZ * rightZ;
        float lateralAccel = 0.0f;
        if (deltaTime > 0.0f) {
            lateralAccel = (lateralVelocity - m_LastLateralVelocity) / deltaTime;
        }
        m_LastLateralVelocity = lateralVelocity;

        float rawLean = lateralAccel * m_Config.leanAccelScale +
                        lateralVelocity * m_Config.leanVelocityScale;
        rawLean *= m_Config.leanStrength;

        if (m_Config.leanMaxAngle > 0.0f) {
            float curved = std::tanh(rawLean / m_Config.leanMaxAngle);
            dynamicRoll = curved * m_Config.leanMaxAngle;
        }
    } else {
        m_LastLateralVelocity = 0.0f;
        m_LastSmoothedX = m_Transform.x;
        m_LastSmoothedY = m_Transform.y;
        m_LastSmoothedZ = m_Transform.z;
    }

    // Combine manual and dynamic roll
    float combinedRoll = m_TargetRoll + dynamicRoll;

    // Apply roll smoothing
    if (m_Config.smoothEnabled && m_Config.leanHalfTime > 0.0f) {
        m_CurrentRoll = SmoothDamp(m_CurrentRoll, combinedRoll, m_RollVelocity, m_Config.leanHalfTime, deltaTime);
    } else if (m_Config.smoothEnabled) {
        m_CurrentRoll = combinedRoll;
        m_RollVelocity = 0.0f;
    } else {
        m_CurrentRoll = Lerp(m_CurrentRoll, combinedRoll, 1.0f - m_Config.rollSmoothing);
        m_RollVelocity = 0.0f;
    }
    m_Transform.roll = m_CurrentRoll;
}

void CFreecamController::UpdateFOV(const InputState& input) {
    if (input.mouseWheel != 0 && !IsAltDown(input)) {
        m_Transform.fov += input.mouseWheel * m_Config.fovStep;
        m_Transform.fov = Clamp(m_Transform.fov, m_Config.fovMin, m_Config.fovMax);
    }
}

void CFreecamController::UpdateSpeed(const InputState& input, float deltaTime) {
    if (deltaTime <= 0.0f) {
        return;
    }

    const float clickWindow = 0.12f; // seconds that count as a single tap
    bool held4 = input.mouseButton4;
    bool held5 = input.mouseButton5;

    // If both are held, cancel out adjustments and reset hold accumulators.
    if (held4 && held5) {
        m_MouseButton4Hold = 0.0f;
        m_MouseButton5Hold = 0.0f;
        m_LastMouseButton4 = held4;
        m_LastMouseButton5 = held5;
        return;
    }

    float prevHold4 = m_MouseButton4Hold;
    float prevHold5 = m_MouseButton5Hold;
    if (held4) m_MouseButton4Hold += deltaTime; else m_MouseButton4Hold = 0.0f;
    if (held5) m_MouseButton5Hold += deltaTime; else m_MouseButton5Hold = 0.0f;

    auto extraTime = [clickWindow](float prevHold, float curHold) {
        float prevOver = (prevHold > clickWindow) ? (prevHold - clickWindow) : 0.0f;
        float curOver = (curHold > clickWindow) ? (curHold - clickWindow) : 0.0f;
        float deltaOver = curOver - prevOver;
        return (deltaOver > 0.0f) ? deltaOver : 0.0f;
    };

    float adjustment = 0.0f;
    if (held5) {
        if (!m_LastMouseButton5) {
            // Initial tap: one step
            adjustment += m_Config.speedAdjustRate * clickWindow;
        }
        // Continuous hold past the click window
        adjustment += m_Config.speedAdjustRate * extraTime(prevHold5, m_MouseButton5Hold);
    } else if (held4) {
        if (!m_LastMouseButton4) {
            adjustment -= m_Config.speedAdjustRate * clickWindow;
        }
        adjustment -= m_Config.speedAdjustRate * extraTime(prevHold4, m_MouseButton4Hold);
    }
    if (IsAltDown(input) && input.mouseWheel != 0) {
        adjustment += input.mouseWheel * 0.05;
    }

    m_LastMouseButton4 = held4;
    m_LastMouseButton5 = held5;

    if (adjustment != 0.0f) {
        float newScalar = m_SpeedScalar + adjustment;
        newScalar = Clamp(newScalar, m_Config.speedMinMultiplier, m_Config.speedMaxMultiplier);
        if (fabsf(newScalar - m_SpeedScalar) > 0.0001f) {
            m_SpeedScalar = newScalar;
            m_SpeedDirty = true;
        }
    }
}

void CFreecamController::ResetSpeed() {
    m_SpeedScalar = Clamp(1.0f, m_Config.speedMinMultiplier, m_Config.speedMaxMultiplier);
    m_SpeedDirty = true;
    m_LastMouseButton4 = false;
    m_LastMouseButton5 = false;
    m_MouseButton4Hold = 0.0f;
    m_MouseButton5Hold = 0.0f;
}

void CFreecamController::SetSpeedScalar(float value) {
    m_SpeedScalar = Clamp(value, m_Config.speedMinMultiplier, m_Config.speedMaxMultiplier);
    m_SpeedDirty = true;
}

void CFreecamController::ApplySmoothing(float deltaTime) {
    // Exponential smoothing with half-time (like mirv_input)
    float posBlend = (m_Config.halfVec > 0) ?
        1.0f - expf((-logf(2.0f) * deltaTime) / m_Config.halfVec) : 1.0f;

    float fovBlend = (m_Config.halfFov > 0) ?
        1.0f - expf((-logf(2.0f) * deltaTime) / m_Config.halfFov) : 1.0f;

    // Position smoothing
    m_SmoothedTransform.x = Lerp(m_SmoothedTransform.x, m_Transform.x, posBlend);
    m_SmoothedTransform.y = Lerp(m_SmoothedTransform.y, m_Transform.y, posBlend);
    m_SmoothedTransform.z = Lerp(m_SmoothedTransform.z, m_Transform.z, posBlend);

    // Rotation smoothing (critical damping or long-path slerp)
    Afx::Math::Quaternion targetQuat = m_RawQuat;
    if (m_CurrentHalfRot > 0.0f) {
        if (m_Config.rotCriticalDamping) {
            const double omega = log(2.0) / m_CurrentHalfRot;
            const double damping = 1.0;

            Afx::Math::Quaternion qErr = targetQuat * m_SmoothedQuat.Conjugate();

            double w = qErr.W;
            w = (std::max)(-1.0, (std::min)(1.0, w));
            double angle = 2.0 * acos(w);
            double sinHalf = sqrt((std::max)(0.0, 1.0 - w * w));
            Afx::Math::Vector3 axis = sinHalf < 1e-6
                ? Afx::Math::Vector3(1.0, 0.0, 0.0)
                : Afx::Math::Vector3(qErr.X / sinHalf, qErr.Y / sinHalf, qErr.Z / sinHalf);

            Afx::Math::Vector3 error = axis * angle;
            Afx::Math::Vector3 wdot =
                (omega * omega) * error -
                (2.0 * damping * omega) * m_RotVelocity;
            m_RotVelocity += wdot * deltaTime;
            m_SmoothedQuat = IntegrateQuat(m_SmoothedQuat, m_RotVelocity, deltaTime);
        } else {
            const double t = deltaTime / m_CurrentHalfRot;
            Afx::Math::Vector3 axis;
            double targetAngle = m_SmoothedQuat.GetAng(targetQuat, axis);
            double stepAngle = CalcDeltaExpSmooth(t, targetAngle);

            if (fabs(stepAngle) > AFX_MATH_EPS && fabs(targetAngle) > AFX_MATH_EPS) {
                m_SmoothedQuat = m_SmoothedQuat.Slerp(targetQuat, stepAngle / targetAngle).Normalized();
                if (deltaTime > 0.0f) {
                    m_RotVelocity = axis * (stepAngle / deltaTime);
                } else {
                    m_RotVelocity = Afx::Math::Vector3(0.0, 0.0, 0.0);
                }
            } else {
                m_RotVelocity = Afx::Math::Vector3(0.0, 0.0, 0.0);
            }
        }
    } else {
        m_SmoothedQuat = targetQuat;
        m_RotVelocity = Afx::Math::Vector3(0.0, 0.0, 0.0);
    }

    UpdateAnglesFromQuat(m_SmoothedQuat, m_SmoothedTransform, m_SmoothedTransform);

    // FOV smoothing
    m_SmoothedTransform.fov = Lerp(m_SmoothedTransform.fov, m_Transform.fov, fovBlend);
}

void CFreecamController::UpdatePlayerLock(const InputState& input, float deltaTime) {
    bool vDown = input.IsKeyDown(kVkV);
    if (vDown && !m_LastKeyVDown) {
        if (!m_PlayerLockActive) {
            float eyeX = 0.0f, eyeY = 0.0f, eyeZ = 0.0f;
            int handle = -1;
            if (TryAcquirePlayerLockTarget(eyeX, eyeY, eyeZ, handle)) {
                m_PlayerLockActive = true;
                m_PlayerLockHandle = handle;
                m_PlayerLockReturnHalfRot = m_Config.halfRot;
                StartHalfRotTransition(m_Config.lockHalfRot);
            }
        } else {
            m_PlayerLockActive = false;
            m_PlayerLockHandle = -1;
            StartHalfRotTransition(m_PlayerLockReturnHalfRot);
        }
    }
    m_LastKeyVDown = vDown;

    UpdateHalfRotTransition(deltaTime);

    if (m_PlayerLockActive) {
        float eyeX = 0.0f, eyeY = 0.0f, eyeZ = 0.0f;
        if (GetLockedTargetEye(eyeX, eyeY, eyeZ)) {
            ApplyLockAngles(eyeX, eyeY, eyeZ);
        } else {
            m_PlayerLockActive = false;
            m_PlayerLockHandle = -1;
            StartHalfRotTransition(m_PlayerLockReturnHalfRot);
        }
    }
}

bool CFreecamController::TryAcquirePlayerLockTarget(float& outX, float& outY, float& outZ, int& outHandle) {
    if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) {
        return false;
    }

    const CameraTransform& view = m_Config.smoothEnabled ? m_SmoothedTransform : m_Transform;
    float forwardX = 0.0f, forwardY = 0.0f, forwardZ = 0.0f;
    GetForwardVector(view.pitch, view.yaw, forwardX, forwardY, forwardZ);

    float bestDot = 0.0f;
    float bestDistSq = 0.0f;
    bool found = false;

    int highestIndex = GetHighestEntityIndex();
    for (int i = 0; i <= highestIndex; ++i) {
        auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, i);
        if (!ent || !ent->IsPlayerPawn() || !ent->GetHealth() > 0) {
            continue;
        }

        float eye[3];
        ent->GetRenderEyeOrigin(eye);

        float dx = eye[0] - view.x;
        float dy = eye[1] - view.y;
        float dz = eye[2] - view.z;
        float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq < 0.0001f) {
            continue;
        }

        float invLen = 1.0f / sqrtf(distSq);
        float dirX = dx * invLen;
        float dirY = dy * invLen;
        float dirZ = dz * invLen;
        float dot = dirX * forwardX + dirY * forwardY + dirZ * forwardZ;
        if (dot <= 0.0f) {
            continue;
        }

        if (!found || dot > bestDot + 0.0001f ||
            (fabsf(dot - bestDot) < 0.0001f && distSq < bestDistSq)) {
            found = true;
            bestDot = dot;
            bestDistSq = distSq;
            outX = eye[0];
            outY = eye[1];
            outZ = eye[2];
            outHandle = ent->GetHandle().ToInt();
        }
    }

    return found;
}

bool CFreecamController::GetLockedTargetEye(float& outX, float& outY, float& outZ) {
    if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) {
        return false;
    }

    SOURCESDK::CS2::CBaseHandle handle(m_PlayerLockHandle);
    if (!handle.IsValid()) {
        return false;
    }

    int index = handle.GetEntryIndex();
    if (index < 0) {
        return false;
    }

    auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
    if (!ent || !ent->IsPlayerPawn()) {
        return false;
    }

    if (ent->GetHandle().ToInt() != m_PlayerLockHandle) {
        return false;
    }

    float eye[3];
    ent->GetRenderEyeOrigin(eye);
    outX = eye[0];
    outY = eye[1];
    outZ = eye[2];
    return true;
}

void CFreecamController::StartHalfRotTransition(float targetHalfRot) {
    if (fabsf(m_CurrentHalfRot - targetHalfRot) < 0.0001f) {
        m_CurrentHalfRot = targetHalfRot;
        m_HalfRotTransitionActive = false;
        m_HalfRotTransitionElapsed = 0.0f;
        return;
    }

    m_HalfRotTransitionStart = m_CurrentHalfRot;
    m_HalfRotTransitionTarget = targetHalfRot;
    m_HalfRotTransitionElapsed = 0.0f;
    m_HalfRotTransitionActive = true;
}

void CFreecamController::UpdateHalfRotTransition(float deltaTime) {
    if (!m_HalfRotTransitionActive) {
        float desired = m_PlayerLockActive ? m_Config.lockHalfRot : m_Config.halfRot;
        if (fabsf(m_CurrentHalfRot - desired) > 0.0001f) {
            m_CurrentHalfRot = desired;
        }
        return;
    }

    m_HalfRotTransitionElapsed += deltaTime;
    float transitionSeconds = m_Config.lockHalfRotTransition;
    float t = (transitionSeconds > 0.0f)
        ? Clamp(m_HalfRotTransitionElapsed / transitionSeconds, 0.0f, 1.0f)
        : 1.0f;
    m_CurrentHalfRot = Lerp(m_HalfRotTransitionStart, m_HalfRotTransitionTarget, t);
    if (t >= 1.0f) {
        m_HalfRotTransitionActive = false;
    }
}

void CFreecamController::ApplyLockAngles(float targetX, float targetY, float targetZ) {
    const CameraTransform& view = m_Config.smoothEnabled ? m_SmoothedTransform : m_Transform;

    float dx = targetX - view.x;
    float dy = targetY - view.y;
    float dz = targetZ - view.z;
    float distSq = dx * dx + dy * dy + dz * dz;
    if (distSq < 0.0001f) {
        return;
    }

    float yaw = atan2f(dy, dx) * (180.0f / M_PI);
    float hyp = sqrtf(dx * dx + dy * dy);
    float pitch = -atan2f(dz, hyp) * (180.0f / M_PI);

    // Normalize yaw to [-180, 180]
    while (yaw > 180.0f) yaw -= 360.0f;
    while (yaw < -180.0f) yaw += 360.0f;

    m_Transform.pitch = m_Config.clampPitch ? Clamp(pitch, -89.0f, 89.0f) : pitch;
    m_Transform.yaw = yaw;
}

// Math helpers

float CFreecamController::Clamp(float value, float min, float max) {
    return (value < min) ? min : (value > max) ? max : value;
}

float CFreecamController::Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

float CFreecamController::SmoothDamp(float current, float target, float& currentVelocity, float smoothTime, float deltaTime) {
    if (smoothTime <= 0.0f || deltaTime <= 0.0f) {
        currentVelocity = 0.0f;
        return target;
    }

    float omega = 2.0f / smoothTime;
    float x = omega * deltaTime;
    float exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);

    float change = current - target;
    float temp = (currentVelocity + omega * change) * deltaTime;
    currentVelocity = (currentVelocity - omega * temp) * exp;
    return target + (change + temp) * exp;
}

Afx::Math::Quaternion CFreecamController::BuildQuat(const CameraTransform& transform) const {
    Afx::Math::QEulerAngles euler(transform.pitch, transform.yaw, transform.roll);
    Afx::Math::QREulerAngles qr = Afx::Math::QREulerAngles::FromQEulerAngles(euler);
    return Afx::Math::Quaternion::FromQREulerAngles(qr).Normalized();
}

void CFreecamController::UpdateAnglesFromQuat(const Afx::Math::Quaternion& q, CameraTransform& out, const CameraTransform& hint) const {
    Afx::Math::QEulerAngles angles = q.ToQREulerAngles().ToQEulerAngles();
    auto normalizeNear = [](float value, float target) {
        float delta = target - value;
        float turns = roundf(delta / 360.0f);
        return value + turns * 360.0f;
    };

    out.pitch = normalizeNear((float)angles.Pitch, hint.pitch);
    out.yaw = normalizeNear((float)angles.Yaw, hint.yaw);
    out.roll = normalizeNear((float)angles.Roll, hint.roll);
}

Afx::Math::Vector3 CFreecamController::GetWorldAngularVelocity(float yawRateDeg, float pitchRateDeg, float rollRateDeg, const CameraTransform& transform) const {
    float forwardX, forwardY, forwardZ;
    GetForwardVector(transform.pitch, transform.yaw, forwardX, forwardY, forwardZ);
    float rightX, rightY, rightZ;
    GetRightVector(transform.yaw, rightX, rightY, rightZ);
    float upX, upY, upZ;
    GetUpVector(transform.pitch, transform.yaw, upX, upY, upZ);

    const double degToRad = M_PI / 180.0;
    const double yawRate = yawRateDeg * degToRad;
    const double pitchRate = pitchRateDeg * degToRad;
    const double rollRate = rollRateDeg * degToRad;

    return Afx::Math::Vector3(
        upX * yawRate + rightX * pitchRate + forwardX * rollRate,
        upY * yawRate + rightY * pitchRate + forwardY * rollRate,
        upZ * yawRate + rightZ * pitchRate + forwardZ * rollRate
    );
}

Afx::Math::Quaternion CFreecamController::IntegrateQuat(const Afx::Math::Quaternion& q, const Afx::Math::Vector3& angularVelocity, float deltaTime) const {
    double speed = angularVelocity.Length();
    if (speed <= 1e-8 || deltaTime <= 0.0f) {
        return q;
    }

    double angle = speed * deltaTime;
    double half = 0.5 * angle;
    double sinHalf = sin(half);
    Afx::Math::Vector3 axis = (1.0 / speed) * angularVelocity;
    Afx::Math::Quaternion dq(cos(half), axis.X * sinHalf, axis.Y * sinHalf, axis.Z * sinHalf);
    return (dq * q).Normalized();
}

void CFreecamController::GetForwardVector(float pitch, float yaw, float& outX, float& outY, float& outZ) const {
    float pitchRad = pitch * (M_PI / 180.0f);
    float yawRad = yaw * (M_PI / 180.0f);

    outX = cosf(pitchRad) * cosf(yawRad);
    outY = cosf(pitchRad) * sinf(yawRad);
    outZ = -sinf(pitchRad);
}

void CFreecamController::GetRightVector(float yaw, float& outX, float& outY, float& outZ) const {
    float yawRad = yaw * (M_PI / 180.0f);

    outX = sinf(yawRad);
    outY = -cosf(yawRad);
    outZ = 0;
}

void CFreecamController::GetUpVector(float pitch, float yaw, float& outX, float& outY, float& outZ) const {
    float pitchRad = pitch * (M_PI / 180.0f);
    float yawRad = yaw * (M_PI / 180.0f);

    outX = sinf(pitchRad) * cosf(yawRad);
    outY = sinf(pitchRad) * sinf(yawRad);
    outZ = cosf(pitchRad);
}
