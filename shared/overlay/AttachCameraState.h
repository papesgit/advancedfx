#pragma once

#include <cstdint>

namespace advancedfx {
namespace overlay {

struct AttachCamSettings {
    bool  enabled = false;
    int   entityHandle = -1;       // Source2 handle (ToInt())
    int   attachmentIndex = 0;     // 1-based; 0 = invalid
    float offsetPos[3] = {0.0f, 0.0f, 0.0f}; // forward, right, up
    float offsetRot[3] = {0.0f, 0.0f, 0.0f}; // pitch, yaw, roll (deg)
};

// Thread-safe setters/getter for overlay <-> hook communication
void AttachCam_SetEnabled(bool enabled);
void AttachCam_SetEntityHandle(int handle);
void AttachCam_SetAttachmentIndex(int idx);
void AttachCam_SetOffsetPos(const float pos[3]);
void AttachCam_SetOffsetRot(const float rot[3]);

void AttachCam_GetSettings(AttachCamSettings &out);

} // namespace overlay
} // namespace advancedfx

