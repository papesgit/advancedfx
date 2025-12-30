#pragma once

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../shared/AfxMath.h"

#include <cstdint>
#include <vector>
#include <string>

struct AttachmentCameraKeyframe {
	double time = 0.0;
	int order = 0;
	SOURCESDK::Vector deltaPos = { 0.0f, 0.0f, 0.0f };
	Afx::Math::QEulerAngles deltaAngles = Afx::Math::QEulerAngles(0.0, 0.0, 0.0);
	bool hasFov = false;
	float fov = 90.0f;
};

struct AttachmentCameraAnimationState {
	bool enabled = false;
	double startTime = 0.0;
	std::vector<AttachmentCameraKeyframe> keyframes;
	bool hasTransition = false;
	double transitionTime = 0.0;
	int targetControllerIndex = -1;
	bool transitionApplied = false;
};

struct AttachmentCameraState {
	bool active = false;
	int controllerIndex = -1;
	bool useAttachmentIndex = true;
	uint8_t attachmentIndex = 0;
	std::string attachmentName;
	SOURCESDK::Vector offsetPos = {0.0f, 0.0f, 0.0f};
	Afx::Math::QEulerAngles offsetAngles = Afx::Math::QEulerAngles(0.0, 0.0, 0.0);
	float fov = 90.0f;
	AttachmentCameraAnimationState animation;
};

extern AttachmentCameraState g_AttachmentCamera;
extern bool g_AttachmentCameraHadError;
