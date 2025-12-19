#pragma once

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../shared/AfxMath.h"

#include <cstdint>
#include <string>

struct AttachmentCameraState {
	bool active = false;
	int controllerIndex = -1;
	bool useAttachmentIndex = true;
	uint8_t attachmentIndex = 0;
	std::string attachmentName;
	SOURCESDK::Vector offsetPos = {0.0f, 0.0f, 0.0f};
	Afx::Math::QEulerAngles offsetAngles = Afx::Math::QEulerAngles(0.0, 0.0, 0.0);
	float fov = 90.0f;
};

extern AttachmentCameraState g_AttachmentCamera;
extern bool g_AttachmentCameraHadError;
