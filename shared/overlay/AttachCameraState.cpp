#include "AttachCameraState.h"

#include <mutex>
#include <cstring>

namespace advancedfx {
namespace overlay {

static std::mutex g_AttachCamMutex;
static AttachCamSettings g_AttachCamSettings;

void AttachCam_SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> _l(g_AttachCamMutex);
    g_AttachCamSettings.enabled = enabled;
}

void AttachCam_SetEntityHandle(int handle) {
    std::lock_guard<std::mutex> _l(g_AttachCamMutex);
    g_AttachCamSettings.entityHandle = handle;
}

void AttachCam_SetAttachmentIndex(int idx) {
    std::lock_guard<std::mutex> _l(g_AttachCamMutex);
    g_AttachCamSettings.attachmentIndex = idx;
}

void AttachCam_SetOffsetPos(const float pos[3]) {
    std::lock_guard<std::mutex> _l(g_AttachCamMutex);
    g_AttachCamSettings.offsetPos[0] = pos[0];
    g_AttachCamSettings.offsetPos[1] = pos[1];
    g_AttachCamSettings.offsetPos[2] = pos[2];
}

void AttachCam_SetOffsetRot(const float rot[3]) {
    std::lock_guard<std::mutex> _l(g_AttachCamMutex);
    g_AttachCamSettings.offsetRot[0] = rot[0];
    g_AttachCamSettings.offsetRot[1] = rot[1];
    g_AttachCamSettings.offsetRot[2] = rot[2];
}

void AttachCam_GetSettings(AttachCamSettings &out) {
    std::lock_guard<std::mutex> _l(g_AttachCamMutex);
    out = g_AttachCamSettings;
}

} // namespace overlay
} // namespace advancedfx

