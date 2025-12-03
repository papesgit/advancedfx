#include "FreecamController.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

CFreecamController::CFreecamController()
    : m_bEnabled(false)
    , m_bInputEnabled(false)
    , m_bInitialized(false)
    , m_VelocityX(0), m_VelocityY(0), m_VelocityZ(0)
    , m_MouseVelocityX(0), m_MouseVelocityY(0)
    , m_TargetRoll(0), m_CurrentRoll(0)
{
    m_LastUpdateTime = std::chrono::steady_clock::now();
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
        m_Transform.fov = m_Config.defaultFov;
        m_LastUpdateTime = std::chrono::steady_clock::now();
    }
}

void CFreecamController::Reset(const CameraTransform& transform) {
    m_Transform = transform;
    m_SmoothedTransform = transform;
    m_VelocityX = m_VelocityY = m_VelocityZ = 0;
    m_MouseVelocityX = m_MouseVelocityY = 0;
    m_TargetRoll = m_CurrentRoll = transform.roll;
    m_bInitialized = true;
}

void CFreecamController::Reset() {
    CameraTransform origin;
    origin.fov = m_Config.defaultFov;
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
        UpdateMouseLook(input, deltaTime);
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

    // Convert mouse deltas to angular velocity
    float invDt = 1.0f / deltaTime;
    float targetVelX = -input.mouseDx * m_Config.mouseSensitivity * invDt;
    float targetVelY = input.mouseDy * m_Config.mouseSensitivity * invDt;

    // Apply mouse smoothing (exponential decay to target velocity)
    float mouseLerpRaw = 1.0f - expf(-m_Config.mouseAcceleration * deltaTime);
    float mouseLerp = Clamp(mouseLerpRaw * (1.0f - m_Config.mouseSmoothing), 0.0f, 1.0f);

    m_MouseVelocityX = Lerp(m_MouseVelocityX, targetVelX, mouseLerp);
    m_MouseVelocityY = Lerp(m_MouseVelocityY, targetVelY, mouseLerp);

    // Apply velocity to angles
    m_Transform.yaw += m_MouseVelocityX * deltaTime;
    m_Transform.pitch += m_MouseVelocityY * deltaTime;

    // Clamp pitch
    m_Transform.pitch = Clamp(m_Transform.pitch, -89.0f, 89.0f);

    // Wrap yaw to [-180, 180]
    while (m_Transform.yaw > 180.0f) m_Transform.yaw -= 360.0f;
    while (m_Transform.yaw < -180.0f) m_Transform.yaw += 360.0f;
}

void CFreecamController::UpdateMovement(const InputState& input, float deltaTime) {
    // Calculate movement speed
    float moveSpeed = m_Config.moveSpeed;
    if (input.keyShift) {
        moveSpeed *= m_Config.sprintMultiplier;
    }

    float verticalSpeed = m_Config.verticalSpeed;
    if (input.keyShift) {
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
    if (input.keyW) {
        desiredVelX += forwardX * moveSpeed;
        desiredVelY += forwardY * moveSpeed;
        desiredVelZ += forwardZ * moveSpeed;
    }
    if (input.keyS) {
        desiredVelX -= forwardX * moveSpeed;
        desiredVelY -= forwardY * moveSpeed;
        desiredVelZ -= forwardZ * moveSpeed;
    }

    // Strafe (A/D)
    if (input.keyA) {
        desiredVelX -= rightX * moveSpeed;
        desiredVelY -= rightY * moveSpeed;
    }
    if (input.keyD) {
        desiredVelX += rightX * moveSpeed;
        desiredVelY += rightY * moveSpeed;
    }

    // Vertical (Space/Ctrl)
    if (input.keySpace) {
        desiredVelX += upX * verticalSpeed;
        desiredVelY += upY * verticalSpeed;
        desiredVelZ += upZ * verticalSpeed;
    }
    if (input.keyCtrl) {
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
    if (input.keySpace || input.keyCtrl) {
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
        if (input.keyQ) {
            m_TargetRoll += m_Config.rollSpeed * deltaTime;
        }
        if (input.keyE) {
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
    if (input.mouseWheel != 0) {
        m_Transform.fov += input.mouseWheel * m_Config.fovStep;
        m_Transform.fov = Clamp(m_Transform.fov, m_Config.fovMin, m_Config.fovMax);
    }
}

void CFreecamController::ApplySmoothing(float deltaTime) {
    // Exponential smoothing with half-time (like mirv_input)
    float posBlend = (m_Config.halfVec > 0) ?
        1.0f - expf((-logf(2.0f) * deltaTime) / m_Config.halfVec) : 1.0f;

    float rotBlend = (m_Config.halfRot > 0) ?
        1.0f - expf((-logf(2.0f) * deltaTime) / m_Config.halfRot) : 1.0f;

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
