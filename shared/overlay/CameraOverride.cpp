#include "CameraOverride.h"
#include "../AfxConsole.h"
namespace advancedfx {
namespace overlay {

static std::mutex g_CameraOverrideMutex;
static CameraOverrideState g_CameraOverrideState;

void CameraOverride_SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> _l(g_CameraOverrideMutex);
    g_CameraOverrideState.enabled = enabled;
}

void CameraOverride_SetPosition(const float pos[3]) {
    std::lock_guard<std::mutex> _l(g_CameraOverrideMutex);
    g_CameraOverrideState.pos[0] = pos[0];
    g_CameraOverrideState.pos[1] = pos[1];
    g_CameraOverrideState.pos[2] = pos[2];
}

void CameraOverride_SetAngles(const float ang[3]) {
    std::lock_guard<std::mutex> _l(g_CameraOverrideMutex);
    g_CameraOverrideState.ang[0] = ang[0];
    g_CameraOverrideState.ang[1] = ang[1];
    g_CameraOverrideState.ang[2] = ang[2];
}

void CameraOverride_GetState(CameraOverrideState &out) {
    std::lock_guard<std::mutex> _l(g_CameraOverrideMutex);
    out = g_CameraOverrideState;
}

} // namespace overlay
} // namespace advancedfx
