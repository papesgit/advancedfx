#pragma once

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../shared/AfxMath.h"

#include <cstdint>
#include <array>
#include <vector>
#include <string>

enum class AttachmentCameraTransitionEasing {
	Linear,
	Smoothstep,
	EaseInOutCubic
};

enum class AttachmentCameraKeyframeEasingCurve {
	Linear,
	Smoothstep,
	Cubic
};

enum class AttachmentCameraKeyframeEase {
	EaseIn,
	EaseOut,
	EaseInOut
};

enum class AttachmentCameraKeyframeRotationSampling {
	Live,
	FreezeAtSegmentStart
};

enum class AttachmentCameraRotationReference {
	Attachment,
	OffsetLocal
};

enum class AttachmentCameraRotationBasis {
	Attachment,
	World
};

struct AttachmentCameraKeyframe {
	double time = 0.0;
	int order = 0;
	SOURCESDK::Vector deltaPos = { 0.0f, 0.0f, 0.0f };
	Afx::Math::QEulerAngles deltaAngles = Afx::Math::QEulerAngles(0.0, 0.0, 0.0);
	bool hasFov = false;
	float fov = 90.0f;
	AttachmentCameraKeyframeRotationSampling rotationSampling = AttachmentCameraKeyframeRotationSampling::Live;
	bool followAttachmentPitch = true;
	bool followAttachmentYaw = true;
	bool followAttachmentRoll = true;
	AttachmentCameraKeyframeEasingCurve easingCurve = AttachmentCameraKeyframeEasingCurve::Linear;
	AttachmentCameraKeyframeEase easingMode = AttachmentCameraKeyframeEase::EaseInOut;
};

struct AttachmentCameraAnimationState {
	struct FrozenRotationState {
		bool valid = false;
		int controllerIndex = -1;
		double segmentStartTime = 0.0;
		int segmentStartOrder = 0;
		double segmentEndTime = 0.0;
		int segmentEndOrder = 0;
		Afx::Math::Quaternion quat = Afx::Math::Quaternion(1.0, 0.0, 0.0, 0.0);
	};

	bool enabled = false;
	double startTime = 0.0;
	std::vector<AttachmentCameraKeyframe> keyframes;
	bool hasTransition = false;
	double transitionTime = 0.0;
	double transitionDuration = 0.0;
	AttachmentCameraTransitionEasing transitionEasing = AttachmentCameraTransitionEasing::Smoothstep;
	int targetControllerIndex = -1;
	bool transitionApplied = false;
	bool transitionMode4Applied = false;
	std::array<FrozenRotationState, 2> frozenRotationStates;
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
	AttachmentCameraRotationReference rotationReference = AttachmentCameraRotationReference::Attachment;
	AttachmentCameraRotationBasis rotationBasisPitch = AttachmentCameraRotationBasis::Attachment;
	AttachmentCameraRotationBasis rotationBasisYaw = AttachmentCameraRotationBasis::Attachment;
	AttachmentCameraRotationBasis rotationBasisRoll = AttachmentCameraRotationBasis::Attachment;
	bool rotationLockPitch = false;
	bool rotationLockYaw = false;
	bool rotationLockRoll = false;
	AttachmentCameraAnimationState animation;
};

extern AttachmentCameraState g_AttachmentCamera;
extern bool g_AttachmentCameraHadError;
