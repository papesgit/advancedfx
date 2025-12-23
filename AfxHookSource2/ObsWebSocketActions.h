#pragma once

#include "FreecamController.h"
#include "ObsObserverState.h"

#include <string>

struct FreecamConfigDelta {
	bool hasMouseSensitivity = false;
	float mouseSensitivity = 0.0f;

	bool hasMoveSpeed = false;
	float moveSpeed = 0.0f;
	bool hasSprintMultiplier = false;
	float sprintMultiplier = 0.0f;
	bool hasVerticalSpeed = false;
	float verticalSpeed = 0.0f;
	bool hasSpeedAdjustRate = false;
	float speedAdjustRate = 0.0f;
	bool hasSpeedMinMultiplier = false;
	float speedMinMultiplier = 0.0f;
	bool hasSpeedMaxMultiplier = false;
	float speedMaxMultiplier = 0.0f;

	bool hasRollSpeed = false;
	float rollSpeed = 0.0f;
	bool hasRollSmoothing = false;
	float rollSmoothing = 0.0f;
	bool hasLeanStrength = false;
	float leanStrength = 0.0f;

	bool hasFovMin = false;
	float fovMin = 0.0f;
	bool hasFovMax = false;
	float fovMax = 0.0f;
	bool hasFovStep = false;
	float fovStep = 0.0f;
	bool hasDefaultFov = false;
	float defaultFov = 0.0f;

	bool hasSmoothEnabled = false;
	bool smoothEnabled = false;
	bool hasHalfVec = false;
	float halfVec = 0.0f;
	bool hasHalfRot = false;
	float halfRot = 0.0f;
	bool hasLockHalfRot = false;
	float lockHalfRot = 0.0f;
	bool hasLockHalfRotTransition = false;
	float lockHalfRotTransition = 0.0f;
	bool hasHalfFov = false;
	float halfFov = 0.0f;
};

struct FreecamHandoffPayload {
	CameraTransform transform;
	bool hasFov = false;
	float fov = 90.0f;
	bool hasSmoothTransform = false;
	CameraTransform smoothTransform;
	bool hasSmoothFov = false;
	float smoothFov = 90.0f;
	bool hasSpeedScalar = false;
	float speedScalar = 1.0f;
	FreecamConfigDelta configDelta;
	std::string message;
};

enum class FreecamHoldMode {
	Camera,
	World
};

void ObsWebSocket_QueueFreecamEnable();
void ObsWebSocket_QueueFreecamDisable();
void ObsWebSocket_QueueFreecamHold(bool hasMode, FreecamHoldMode mode);
void ObsWebSocket_QueueFreecamConfig(const FreecamConfigDelta& delta, const std::string& message);
void ObsWebSocket_QueueFreecamHandoff(const FreecamHandoffPayload& payload);
void ObsWebSocket_QueueAttachCamera(const AttachmentCameraState& state);
void ObsWebSocket_QueueRefreshBinds();
void ObsWebSocket_QueueSetAltSpectatorBindings(bool enabled);
void ObsWebSocket_QueueExecCommand(const std::string& cmd);
void ObsWebSocket_QueueCampathPlay(const std::string& cmd, double offset);
void ObsWebSocket_ProcessActions();

CameraTransform Obs_GetLastCameraTransform();
