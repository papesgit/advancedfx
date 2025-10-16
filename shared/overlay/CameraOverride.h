#pragma once

#include <mutex>

namespace advancedfx {
namespace overlay {

// Generic camera override state that can be used by any overlay feature
struct CameraOverrideState {
    bool enabled = false;
    float pos[3] = {0.0f, 0.0f, 0.0f};  // X, Y, Z
    float ang[3] = {0.0f, 0.0f, 0.0f};  // Pitch, Yaw, Roll
};

// Thread-safe setters/getter for overlay <-> hook communication
void CameraOverride_SetEnabled(bool enabled);
void CameraOverride_SetPosition(const float pos[3]);
void CameraOverride_SetAngles(const float ang[3]);
void CameraOverride_GetState(CameraOverrideState &out);

} // namespace overlay
} // namespace advancedfx
