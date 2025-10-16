#include "BirdCamera.h"
#include "CameraOverride.h"
#include "../AfxConsole.h"
#include <cmath>
#include <algorithm>

namespace advancedfx {
namespace overlay {

// Bird camera phases
enum class BirdPhase {
    Idle = 0,
    AscendA = 1,    // Ascending from source player
    HoldA = 2,      // Holding above source player
    ToB = 3,        // Traveling to destination player
    HoldB = 4,      // Holding above destination player
    DescendB = 5    // Descending to destination player POV
};

enum class BirdMode {
    Goto = 0,       // Full transition from A to B
    Player = 1      // Just go to bird's eye of current player
};

// Internal state for bird camera
struct BirdCameraState {
    bool active = false;
    BirdMode mode = BirdMode::Goto;
    BirdPhase phase = BirdPhase::Idle;

    int fromControllerIndex = -1;
    int toControllerIndex = -1;
    float height = 1000.0f;

    // Timing
    float phaseStartTime = 0.0f;
    float holdTimeA = 1.0f;
    float holdTimeB = 1.0f;

    // Kinematics
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float vel[3] = {0.0f, 0.0f, 0.0f};
    float ang[3] = {0.0f, 0.0f, 0.0f}; // pitch, yaw, roll
    float angVel[3] = {0.0f, 0.0f, 0.0f};

    bool initialized = false;
    float downYaw = 0.0f;
    bool downYawInit = false;
    bool forceSnapAngles = false;

    // Smoothing parameters
    float linSpeed = 1000.0f;
    float minSpeed = 300.0f;
    float velSmoothTime = 0.5f;
    float angSmoothTime = 0.25f;
    float margin = 5.0f;

};

static BirdCameraState g_BirdState;

// Pending spec target (set when we need to execute spec_player)
static int g_PendingSpecTarget = -1;

// Target data set externally each frame
static bool g_TargetAValid = false;
static float g_TargetAPos[3] = {0, 0, 0};
static float g_TargetAAng[3] = {0, 0, 0};
static bool g_TargetBValid = false;
static float g_TargetBPos[3] = {0, 0, 0};
static float g_TargetBAng[3] = {0, 0, 0};

// Helper functions
namespace {

inline float angleNormalize(float a) {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

inline float angleDelta(float a, float b) {
    return angleNormalize(a - b);
}

inline float snapYawToCardinal(float yaw) {
    float n = angleNormalize(yaw);
    float snapped = roundf(n / 90.0f) * 90.0f;
    return angleNormalize(snapped);
}

inline float clampf(float v, float minv, float maxv) {
    return (v < minv) ? minv : ((v > maxv) ? maxv : v);
}

// Critically damped smoothing
struct SmoothResult { float value; float vel; };

SmoothResult smoothDamp1D(float current, float target, float vel, float smoothTime, float maxSpeed, float dt) {
    smoothTime = std::max(0.0001f, smoothTime);
    float omega = 2.0f / smoothTime;
    float x = omega * dt;
    float exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    float change = current - target;
    float originalTo = target;
    float maxChange = maxSpeed * smoothTime;
    if (maxChange > 0.0f) {
        change = clampf(change, -maxChange, maxChange);
    }
    target = current - change;
    float temp = (vel + omega * change) * dt;
    float newVel = (vel - omega * temp) * exp;
    float output = target + (change + temp) * exp;
    if (((originalTo - current) > 0.0f) == (output > originalTo)) {
        output = originalTo;
        float outVel = (dt > 0) ? ((output - originalTo) / dt) : 0.0f;
        return {output, outVel};
    }
    return {output, newVel};
}

SmoothResult smoothDampAngle1D(float current, float target, float vel, float smoothTime, float maxSpeed, float dt) {
    float delta = angleDelta(target, current);
    SmoothResult r = smoothDamp1D(current, current + delta, vel, smoothTime, maxSpeed, dt);
    return {angleNormalize(r.value), r.vel};
}

} // anonymous namespace

void BirdCamera_StartGoto(int fromControllerIdx, int toControllerIdx, float height) {
    g_BirdState.active = true;
    g_BirdState.mode = BirdMode::Goto;
    g_BirdState.fromControllerIndex = fromControllerIdx;
    g_BirdState.toControllerIndex = toControllerIdx;
    g_BirdState.height = height;
    g_BirdState.phase = BirdPhase::AscendA;
    g_BirdState.initialized = false;
    g_BirdState.downYawInit = false;
    g_BirdState.forceSnapAngles = false;
    g_BirdState.holdTimeA = 1.0f;
    g_BirdState.holdTimeB = 1.0f;

    // Request spec to source player at start
    g_PendingSpecTarget = fromControllerIdx;

}

void BirdCamera_StartPlayer(int controllerIdx, float height) {
    g_BirdState.active = true;
    g_BirdState.mode = BirdMode::Player;
    g_BirdState.toControllerIndex = controllerIdx;
    g_BirdState.fromControllerIndex = controllerIdx;
    g_BirdState.height = height;
    g_BirdState.phase = BirdPhase::AscendA;
    g_BirdState.initialized = false;
    g_BirdState.downYawInit = false;
    g_BirdState.forceSnapAngles = false;
    g_BirdState.holdTimeA = 0.0f; // Skip hold on ascend for player mode

    // Request spec to target player at start
    g_PendingSpecTarget = controllerIdx;

}

void BirdCamera_StartPlayerReturn() {
    if (g_BirdState.active && g_BirdState.mode == BirdMode::Player) {
        g_BirdState.phase = BirdPhase::DescendB;
    }
}

void BirdCamera_Stop() {
    g_BirdState.active = false;
    g_BirdState.initialized = false;
    CameraOverride_SetEnabled(false);
}

bool BirdCamera_IsActive() {
    return g_BirdState.active;
}

bool BirdCamera_GetControllerIndices(int& outFromIdx, int& outToIdx) {
    if (!g_BirdState.active) {
        return false;
    }
    outFromIdx = g_BirdState.fromControllerIndex;
    outToIdx = g_BirdState.toControllerIndex;
    return true;
}

void BirdCamera_SetTargetA(const float pos[3], const float ang[3], bool valid) {
    g_TargetAValid = valid;
    if (valid) {
        g_TargetAPos[0] = pos[0]; g_TargetAPos[1] = pos[1]; g_TargetAPos[2] = pos[2];
        g_TargetAAng[0] = ang[0]; g_TargetAAng[1] = ang[1]; g_TargetAAng[2] = ang[2];
    }
}

void BirdCamera_SetTargetB(const float pos[3], const float ang[3], bool valid) {
    g_TargetBValid = valid;
    if (valid) {
        g_TargetBPos[0] = pos[0]; g_TargetBPos[1] = pos[1]; g_TargetBPos[2] = pos[2];
        g_TargetBAng[0] = ang[0]; g_TargetBAng[1] = ang[1]; g_TargetBAng[2] = ang[2];
    }
}

bool BirdCamera_Update(float deltaTime, float curTime) {

    if (!g_BirdState.active) {
        CameraOverride_SetEnabled(false);
        return false;
    }

    // Get current camera position if not initialized
    if (!g_BirdState.initialized) {
        // Initialize from targetA (current player position)
        if (g_TargetAValid) {
            g_BirdState.pos[0] = g_TargetAPos[0];
            g_BirdState.pos[1] = g_TargetAPos[1];
            g_BirdState.pos[2] = g_TargetAPos[2];
            g_BirdState.ang[0] = g_TargetAAng[0];
            g_BirdState.ang[1] = g_TargetAAng[1];
            g_BirdState.ang[2] = g_TargetAAng[2];

            if (!g_BirdState.downYawInit) {
                g_BirdState.downYaw = snapYawToCardinal(g_TargetAAng[1]);
                g_BirdState.downYawInit = true;
            }

            g_BirdState.vel[0] = g_BirdState.vel[1] = g_BirdState.vel[2] = 0.0f;
            g_BirdState.angVel[0] = g_BirdState.angVel[1] = g_BirdState.angVel[2] = 0.0f;
            g_BirdState.initialized = true;
            g_BirdState.phaseStartTime = curTime;
        } else {
            // Can't initialize, abort
            advancedfx::Message("BirdCamera_Update: Failed to initialize - no valid target A, stopping\n");
            BirdCamera_Stop();
            return false;
        }
    }

    float dt = deltaTime;
    dt = clampf(dt, 1.0f / 128.0f, 0.25f);

    // Use target positions based on phase
    float targetPos[3] = {0, 0, 0};
    float targetAngles[3] = {89.9f, g_BirdState.downYaw, 0.0f}; // Default to top-down
    bool hasTarget = false;

    if (g_BirdState.phase == BirdPhase::AscendA || g_BirdState.phase == BirdPhase::HoldA) {
        if (g_TargetAValid) {
            // For bird phases, add height above the player's eye position
            targetPos[0] = g_TargetAPos[0];
            targetPos[1] = g_TargetAPos[1];
            targetPos[2] = g_TargetAPos[2] + g_BirdState.height;
            targetAngles[0] = 89.9f;
            targetAngles[1] = g_BirdState.downYaw;
            targetAngles[2] = 0.0f;
            hasTarget = true;
        } else if ((curTime - g_BirdState.phaseStartTime) > 3.0f) {
            BirdCamera_Stop();
            return false;
        }
    } else if (g_BirdState.phase == BirdPhase::ToB || g_BirdState.phase == BirdPhase::HoldB) {
        if (g_TargetBValid) {
            // For bird phases, add height above the player's eye position
            targetPos[0] = g_TargetBPos[0];
            targetPos[1] = g_TargetBPos[1];
            targetPos[2] = g_TargetBPos[2] + g_BirdState.height;
            targetAngles[0] = 89.9f;
            targetAngles[1] = g_BirdState.downYaw;
            targetAngles[2] = 0.0f;
            hasTarget = true;
        } else if ((curTime - g_BirdState.phaseStartTime) > 3.0f) {
            BirdCamera_Stop();
            return false;
        }
    } else if (g_BirdState.phase == BirdPhase::DescendB) {
        if (g_TargetBValid) {
            targetPos[0] = g_TargetBPos[0];
            targetPos[1] = g_TargetBPos[1];
            targetPos[2] = g_TargetBPos[2];

            // Blend angles from top-down to player POV as we descend
            float topDown[3] = {89.9f, g_BirdState.downYaw, 0.0f};
            float dx = targetPos[0] - g_BirdState.pos[0];
            float dy = targetPos[1] - g_BirdState.pos[1];
            float dz = targetPos[2] - g_BirdState.pos[2];
            float distD = sqrtf(dx*dx + dy*dy + dz*dz);
            float blendRadius = std::max(g_BirdState.height * 0.5f, 1.0f);
            float tBlend = clampf(1.0f - (distD / blendRadius), 0.0f, 1.0f);

            targetAngles[0] = angleNormalize(topDown[0] + angleDelta(g_TargetBAng[0], topDown[0]) * tBlend);
            targetAngles[1] = angleNormalize(topDown[1] + angleDelta(g_TargetBAng[1], topDown[1]) * tBlend);
            targetAngles[2] = angleNormalize(topDown[2] + angleDelta(g_TargetBAng[2], topDown[2]) * tBlend);
            hasTarget = true;
        } else if ((curTime - g_BirdState.phaseStartTime) > 3.0f) {
            BirdCamera_Stop();
            return false;
        }
    }

    if (!hasTarget) {
        CameraOverride_SetEnabled(false);
        return false;
    }

    // Update position based on phase
    if (g_BirdState.phase == BirdPhase::HoldA || g_BirdState.phase == BirdPhase::HoldB) {
        // Hard lock
        g_BirdState.pos[0] = targetPos[0];
        g_BirdState.pos[1] = targetPos[1];
        g_BirdState.pos[2] = targetPos[2];
        g_BirdState.vel[0] = g_BirdState.vel[1] = g_BirdState.vel[2] = 0.0f;
    } else if (g_BirdState.phase == BirdPhase::AscendA) {
        // Lock XY, smooth Z
        auto sz = smoothDamp1D(g_BirdState.pos[2], targetPos[2], g_BirdState.vel[2], g_BirdState.velSmoothTime, g_BirdState.linSpeed, dt);
        g_BirdState.pos[0] = targetPos[0];
        g_BirdState.pos[1] = targetPos[1];
        g_BirdState.pos[2] = sz.value;
        g_BirdState.vel[0] = g_BirdState.vel[1] = 0.0f;
        g_BirdState.vel[2] = sz.vel;
    } else if (g_BirdState.phase == BirdPhase::ToB) {
        // Straight-line path with easing
        float dx = targetPos[0] - g_BirdState.pos[0];
        float dy = targetPos[1] - g_BirdState.pos[1];
        float dz = targetPos[2] - g_BirdState.pos[2];
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);

        if (dist > 1e-6f) {
            float dirX = dx / dist, dirY = dy / dist, dirZ = dz / dist;
            float speedMag = sqrtf(g_BirdState.vel[0]*g_BirdState.vel[0] + g_BirdState.vel[1]*g_BirdState.vel[1] + g_BirdState.vel[2]*g_BirdState.vel[2]);
            float a = 1.0f - expf(-dt / g_BirdState.velSmoothTime);
            float goal = g_BirdState.linSpeed;
            float decelRadius = std::max(g_BirdState.height * 0.5f, 50.0f);
            if (dist < decelRadius) {
                float baseSpeed = g_BirdState.linSpeed * (dist / decelRadius);
                goal = std::max(baseSpeed, g_BirdState.minSpeed);
            }
            speedMag = speedMag + (goal - speedMag) * a;
            speedMag = std::max(speedMag, g_BirdState.minSpeed);
            float step = std::min(speedMag * dt, dist);
            g_BirdState.pos[0] += dirX * step;
            g_BirdState.pos[1] += dirY * step;
            g_BirdState.pos[2] += dirZ * step;
            float v = (step > 0 && dt > 0) ? (step / dt) : 0.0f;
            g_BirdState.vel[0] = dirX * v;
            g_BirdState.vel[1] = dirY * v;
            g_BirdState.vel[2] = dirZ * v;
        }
    } else if (g_BirdState.phase == BirdPhase::DescendB) {
        if (g_BirdState.mode == BirdMode::Player) {
            // Player return: lock XY, smooth Z
            auto sz = smoothDamp1D(g_BirdState.pos[2], targetPos[2], g_BirdState.vel[2], g_BirdState.velSmoothTime, g_BirdState.linSpeed, dt);
            g_BirdState.pos[0] = targetPos[0];
            g_BirdState.pos[1] = targetPos[1];
            g_BirdState.pos[2] = sz.value;
            g_BirdState.vel[0] = g_BirdState.vel[1] = 0.0f;
            g_BirdState.vel[2] = sz.vel;
        } else {
            // Goto: constant-speed linear approach
            float dx = targetPos[0] - g_BirdState.pos[0];
            float dy = targetPos[1] - g_BirdState.pos[1];
            float dz = targetPos[2] - g_BirdState.pos[2];
            float dist = sqrtf(dx*dx + dy*dy + dz*dz);
            if (dist > 1e-6f) {
                float dirX = dx / dist, dirY = dy / dist, dirZ = dz / dist;
                float speedMag = std::max(g_BirdState.linSpeed, g_BirdState.minSpeed);
                if (dt > 1e-6f && dist / dt < speedMag) speedMag = dist / dt;
                float step = std::min(speedMag * dt, dist);
                g_BirdState.pos[0] += dirX * step;
                g_BirdState.pos[1] += dirY * step;
                g_BirdState.pos[2] += dirZ * step;
                float v = (step > 0 && dt > 0) ? (step / dt) : 0.0f;
                g_BirdState.vel[0] = dirX * v;
                g_BirdState.vel[1] = dirY * v;
                g_BirdState.vel[2] = dirZ * v;
            }
        }
    }

    // Angular smoothing
    float maxAngSpeed = 720.0f;
    float angSmooth = g_BirdState.angSmoothTime;
    if (g_BirdState.phase == BirdPhase::DescendB) {
        float dx = targetPos[0] - g_BirdState.pos[0];
        float dy = targetPos[1] - g_BirdState.pos[1];
        float dz = targetPos[2] - g_BirdState.pos[2];
        float distForAng = sqrtf(dx*dx + dy*dy + dz*dz);
        float blendRadius = std::max(g_BirdState.height * 0.5f, 1.0f);
        float factor = clampf(distForAng / blendRadius, 0.15f, 1.0f);
        angSmooth = std::max(g_BirdState.angSmoothTime * factor, 0.06f);
    }

    if (g_BirdState.forceSnapAngles) {
        g_BirdState.ang[0] = targetAngles[0];
        g_BirdState.ang[1] = targetAngles[1];
        g_BirdState.ang[2] = targetAngles[2];
        g_BirdState.angVel[0] = g_BirdState.angVel[1] = g_BirdState.angVel[2] = 0.0f;
        g_BirdState.forceSnapAngles = false;
    } else {
        auto pr = smoothDampAngle1D(g_BirdState.ang[0], targetAngles[0], g_BirdState.angVel[0], angSmooth, maxAngSpeed, dt);
        auto yr = smoothDampAngle1D(g_BirdState.ang[1], targetAngles[1], g_BirdState.angVel[1], angSmooth, maxAngSpeed, dt);
        auto rr = smoothDampAngle1D(g_BirdState.ang[2], targetAngles[2], g_BirdState.angVel[2], angSmooth, maxAngSpeed, dt);
        g_BirdState.ang[0] = pr.value;
        g_BirdState.ang[1] = yr.value;
        g_BirdState.ang[2] = rr.value;
        g_BirdState.angVel[0] = pr.vel;
        g_BirdState.angVel[1] = yr.vel;
        g_BirdState.angVel[2] = rr.vel;
    }

    // Phase progression
    if (g_BirdState.phase == BirdPhase::AscendA) {
        float dx = g_BirdState.pos[0] - targetPos[0];
        float dy = g_BirdState.pos[1] - targetPos[1];
        float dz = g_BirdState.pos[2] - targetPos[2];
        float horiz = sqrtf(dx*dx + dy*dy);
        if (horiz <= g_BirdState.margin && fabsf(dz) <= g_BirdState.margin) {
            g_BirdState.phase = BirdPhase::HoldA;
            g_BirdState.phaseStartTime = curTime;
        }
    } else if (g_BirdState.phase == BirdPhase::HoldA) {
        if ((curTime - g_BirdState.phaseStartTime) >= g_BirdState.holdTimeA) {
            g_BirdState.downYaw = snapYawToCardinal(g_BirdState.ang[1]);
            g_BirdState.phase = BirdPhase::ToB;
            g_BirdState.phaseStartTime = curTime;
            // Request spec to destination player when transitioning to ToB
            g_PendingSpecTarget = g_BirdState.toControllerIndex;
        }
    } else if (g_BirdState.phase == BirdPhase::ToB) {
        float dx = g_BirdState.pos[0] - targetPos[0];
        float dy = g_BirdState.pos[1] - targetPos[1];
        float dz = g_BirdState.pos[2] - targetPos[2];
        float horiz = sqrtf(dx*dx + dy*dy);
        if (horiz <= g_BirdState.margin && fabsf(dz) <= g_BirdState.margin) {
            g_BirdState.phase = BirdPhase::HoldB;
            g_BirdState.phaseStartTime = curTime;
        }
    } else if (g_BirdState.phase == BirdPhase::HoldB) {
        if (g_BirdState.mode == BirdMode::Goto) {
            if ((curTime - g_BirdState.phaseStartTime) >= g_BirdState.holdTimeB) {
                g_BirdState.phase = BirdPhase::DescendB;
                g_BirdState.phaseStartTime = curTime;
            }
        }
        // If Player mode, stay in HoldB until user requests return
    } else if (g_BirdState.phase == BirdPhase::DescendB) {
        float dx = g_BirdState.pos[0] - targetPos[0];
        float dy = g_BirdState.pos[1] - targetPos[1];
        float dz = g_BirdState.pos[2] - targetPos[2];
        float d = sqrtf(dx*dx + dy*dy + dz*dz);
        if (d <= g_BirdState.margin) {
            BirdCamera_Stop();
            return false;
        }
    }

    // Apply camera override
    CameraOverride_SetPosition(g_BirdState.pos);
    CameraOverride_SetAngles(g_BirdState.ang);
    CameraOverride_SetEnabled(true);


    return true;
}

int BirdCamera_GetPendingSpecTarget() {
    int target = g_PendingSpecTarget;
    g_PendingSpecTarget = -1; // Clear after reading
    return target;
}

} // namespace overlay
} // namespace advancedfx
