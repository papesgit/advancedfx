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
}

CFreecamController::CFreecamController()
    : m_bEnabled(false)
    , m_bInputEnabled(false)
    , m_bInitialized(false)
    , m_VelocityX(0), m_VelocityY(0), m_VelocityZ(0)
    , m_MouseVelocityX(0), m_MouseVelocityY(0)
    , m_SpeedScalar(1.0f), m_SpeedDirty(false)
    , m_LastMouseButton4(false), m_LastMouseButton5(false)
    , m_MouseButton4Hold(0.0f), m_MouseButton5Hold(0.0f)
    , m_TargetRoll(0), m_CurrentRoll(0)
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
        m_TargetRoll = m_CurrentRoll = 0;
        m_PlayerLockActive = false;
        m_LastKeyVDown = false;
        m_PlayerLockHandle = -1;
        m_PlayerLockReturnHalfRot = m_Config.halfRot;
        m_CurrentHalfRot = m_Config.halfRot;
        m_HalfRotTransitionActive = false;
        m_HalfRotTransitionElapsed = 0.0f;
        ResetSpeed();
        m_LastUpdateTime = std::chrono::steady_clock::now();
    }
}

void CFreecamController::Reset(const CameraTransform& transform) {
    m_Transform = transform;
    m_SmoothedTransform = transform;
    m_VelocityX = m_VelocityY = m_VelocityZ = 0;
    m_MouseVelocityX = m_MouseVelocityY = 0;
    m_TargetRoll = m_CurrentRoll = transform.roll;
    m_CurrentHalfRot = m_Config.halfRot;
    m_PlayerLockReturnHalfRot = m_Config.halfRot;
    m_HalfRotTransitionActive = false;
    m_HalfRotTransitionElapsed = 0.0f;
    ResetSpeed();
    m_bInitialized = true;
}

void CFreecamController::SetSmoothedTransform(const CameraTransform& transform) {
    m_SmoothedTransform = transform;
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

    // Only process input if input is enabled (gated when right-click released)
    if (m_bInputEnabled) {
        UpdateSpeed(input, deltaTime);
        UpdateMouseLook(input, deltaTime);
    }

    UpdatePlayerLock(input, deltaTime);

    if (m_bInputEnabled) {
        UpdateMovement(input, deltaTime);
        UpdateRoll(input, deltaTime);
        UpdateFOV(input);
    }

    // Always apply smoothing (even when input is disabled) to maintain camera position
    if (m_Config.smoothEnabled) {
        ApplySmoothing(deltaTime);
    } else {
        m_SmoothedTransform = m_Transform;
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

    // Clamp pitch
    m_Transform.pitch = Clamp(m_Transform.pitch, -89.0f, 89.0f);

    // Wrap yaw to [-180, 180]
    while (m_Transform.yaw > 180.0f) m_Transform.yaw -= 360.0f;
    while (m_Transform.yaw < -180.0f) m_Transform.yaw += 360.0f;
}

void CFreecamController::UpdateMovement(const InputState& input, float deltaTime) {
    // Calculate movement speed
    float moveSpeed = GetCurrentMoveSpeed();
    if (IsShiftDown(input)) {
        moveSpeed *= m_Config.sprintMultiplier;
    }

    float verticalSpeed = GetCurrentVerticalSpeed();
    if (IsShiftDown(input)) {
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

    // Normalize diagonal movement to prevent faster speed
    float desiredSpeed = sqrtf(desiredVelX * desiredVelX +
                               desiredVelY * desiredVelY +
                               desiredVelZ * desiredVelZ);

    // Determine max speed based on what keys are pressed
    float maxSpeed = moveSpeed;
    if (input.IsKeyDown(' ') || IsCtrlDown(input)) {
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
        // Yaw rate from mouse velocity
        float yawRate = m_MouseVelocityX; // degrees/sec

        // Get right vector for lateral speed calculation
        float rightX, rightY, rightZ;
        GetRightVector(m_Transform.yaw, rightX, rightY, rightZ);

        // Lateral speed (projection of velocity onto right vector)
        float lateralSpeed = m_VelocityX * rightX + m_VelocityY * rightY;

        // Total speed magnitude
        float speedMag = sqrtf(m_VelocityX * m_VelocityX +
                              m_VelocityY * m_VelocityY +
                              m_VelocityZ * m_VelocityZ);

        // Calculate lean
        const float maxLeanSpeed = 1000.0f; // units/s for full lean
        const float maxLeanDeg = 30.0f;

        float speedFactor = Clamp(speedMag / maxLeanSpeed, 0.0f, 1.0f);
        float lateralFactor = Clamp(lateralSpeed / maxLeanSpeed, -1.0f, 1.0f);

        float moveLean = lateralFactor * maxLeanDeg * speedFactor;

        // Require higher yaw speed for full lean by reducing yaw sensitivity
        float yawLean = Clamp(yawRate * 0.04f, -maxLeanDeg, maxLeanDeg);

        float combined = (moveLean + yawLean) * m_Config.leanStrength;
        dynamicRoll = Clamp(combined, -maxLeanDeg, maxLeanDeg);
    }

    // Combine manual and dynamic roll
    float combinedRoll = m_TargetRoll + dynamicRoll;

    // Apply roll smoothing
    m_CurrentRoll = Lerp(m_CurrentRoll, combinedRoll, 1.0f - m_Config.rollSmoothing);
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

    float rotBlend = (m_CurrentHalfRot > 0) ?
        1.0f - expf((-logf(2.0f) * deltaTime) / m_CurrentHalfRot) : 1.0f;

    float fovBlend = (m_Config.halfFov > 0) ?
        1.0f - expf((-logf(2.0f) * deltaTime) / m_Config.halfFov) : 1.0f;

    // Position smoothing
    m_SmoothedTransform.x = Lerp(m_SmoothedTransform.x, m_Transform.x, posBlend);
    m_SmoothedTransform.y = Lerp(m_SmoothedTransform.y, m_Transform.y, posBlend);
    m_SmoothedTransform.z = Lerp(m_SmoothedTransform.z, m_Transform.z, posBlend);

    // Rotation smoothing (handle yaw wrap)
    float targetYaw = m_Transform.yaw;
    float currentYaw = m_SmoothedTransform.yaw;

    // Wrap yaw to avoid long way around
    while (targetYaw - currentYaw > 180.0f) targetYaw -= 360.0f;
    while (targetYaw - currentYaw < -180.0f) targetYaw += 360.0f;

    m_SmoothedTransform.pitch = Lerp(m_SmoothedTransform.pitch, m_Transform.pitch, rotBlend);
    m_SmoothedTransform.yaw = Lerp(currentYaw, targetYaw, rotBlend);
    m_SmoothedTransform.roll = Lerp(m_SmoothedTransform.roll, m_Transform.roll, rotBlend);

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

    m_Transform.pitch = Clamp(pitch, -89.0f, 89.0f);
    m_Transform.yaw = yaw;
}

// Math helpers

float CFreecamController::Clamp(float value, float min, float max) {
    return (value < min) ? min : (value > max) ? max : value;
}

float CFreecamController::Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

void CFreecamController::GetForwardVector(float pitch, float yaw, float& outX, float& outY, float& outZ) {
    float pitchRad = pitch * (M_PI / 180.0f);
    float yawRad = yaw * (M_PI / 180.0f);

    outX = cosf(pitchRad) * cosf(yawRad);
    outY = cosf(pitchRad) * sinf(yawRad);
    outZ = -sinf(pitchRad);
}

void CFreecamController::GetRightVector(float yaw, float& outX, float& outY, float& outZ) {
    float yawRad = yaw * (M_PI / 180.0f);

    outX = sinf(yawRad);
    outY = -cosf(yawRad);
    outZ = 0;
}

void CFreecamController::GetUpVector(float pitch, float yaw, float& outX, float& outY, float& outZ) {
    float pitchRad = pitch * (M_PI / 180.0f);
    float yawRad = yaw * (M_PI / 180.0f);

    outX = sinf(pitchRad) * cosf(yawRad);
    outY = sinf(pitchRad) * sinf(yawRad);
    outZ = cosf(pitchRad);
}
