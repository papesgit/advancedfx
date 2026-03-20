#include "ObsWebSocketActions.h"

#include "../shared/MirvCampath.h"
#include "MirvTime.h"
#include "ObsSpectatorBindings.h"

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../shared/AfxConsole.h"
#include "../shared/StringTools.h"

#include <cmath>
#include <deque>
#include <mutex>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;
extern CFreecamController* g_pFreecam;
extern CamPath g_CamPath;

AttachmentCameraState g_AttachmentCamera;
bool g_AttachmentCameraHadError = false;

namespace {
	enum class ActionType {
		FreecamEnable,
		FreecamDisable,
		FreecamHold,
		FreecamConfig,
		FreecamHandoff,
		AttachCamera,
		RefreshBinds,
		SpectatorBindingsMode,
		ExecCommand,
		CampathPlay
	};

	struct PendingAction {
		ActionType type;
		FreecamInitMode freecamInitMode = FreecamInitMode::InheritMotion;
		FreecamConfigDelta configDelta;
		FreecamHandoffPayload handoff;
		AttachmentCameraState attachment;
		bool hasHoldMode = false;
		FreecamHoldMode holdMode = FreecamHoldMode::Camera;
		bool useAltBindings = false;
		std::string cmd;
		double offset = 0.0;
		std::string message;
	};

	std::mutex g_ActionMutex;
	std::deque<PendingAction> g_ActionQueue;
	constexpr float kInheritInitMaxTargetDelta = 5000.0f;

	bool TryComputeInheritInitTargetDelta(const FreecamConfig& config, const CameraTransformSamples& samples, float& outTargetDelta) {
		outTargetDelta = 0.0f;
		if (!samples.hasPrevious) return false;
		if (!(samples.deltaTime > 0.0f) || !std::isfinite(samples.deltaTime)) return false;

		const float halfVec = config.halfVec;
		const float blend = halfVec > 0.0f
			? 1.0f - expf((-logf(2.0f) * samples.deltaTime) / halfVec)
			: 1.0f;
		if (!(blend > 1.0e-3f) || !std::isfinite(blend)) return false;

		const float targetDeltaX = (samples.current.x - samples.previous.x) / blend;
		const float targetDeltaY = (samples.current.y - samples.previous.y) / blend;
		const float targetDeltaZ = (samples.current.z - samples.previous.z) / blend;

		outTargetDelta = sqrtf(
			targetDeltaX * targetDeltaX +
			targetDeltaY * targetDeltaY +
			targetDeltaZ * targetDeltaZ);
		return std::isfinite(outTargetDelta);
	}

	void ApplyFreecamConfigDelta(FreecamConfig& config, const FreecamConfigDelta& delta) {
		if (delta.hasMouseSensitivity) config.mouseSensitivity = delta.mouseSensitivity;
		if (delta.hasMoveSpeed) config.moveSpeed = delta.moveSpeed;
		if (delta.hasSprintMultiplier) config.sprintMultiplier = delta.sprintMultiplier;
		if (delta.hasVerticalSpeed) config.verticalSpeed = delta.verticalSpeed;
		if (delta.hasSpeedAdjustRate) config.speedAdjustRate = delta.speedAdjustRate;
		if (delta.hasSpeedMinMultiplier) config.speedMinMultiplier = delta.speedMinMultiplier;
		if (delta.hasSpeedMaxMultiplier) config.speedMaxMultiplier = delta.speedMaxMultiplier;
		if (delta.hasRollSpeed) config.rollSpeed = delta.rollSpeed;
		if (delta.hasRollSmoothing) config.rollSmoothing = delta.rollSmoothing;
		if (delta.hasLeanStrength) config.leanStrength = delta.leanStrength;
		if (delta.hasLeanAccelScale) config.leanAccelScale = delta.leanAccelScale;
		if (delta.hasLeanVelocityScale) config.leanVelocityScale = delta.leanVelocityScale;
		if (delta.hasLeanMaxAngle) config.leanMaxAngle = delta.leanMaxAngle;
		if (delta.hasLeanHalfTime) config.leanHalfTime = delta.leanHalfTime;
		if (delta.hasClampPitch) config.clampPitch = delta.clampPitch;
		if (delta.hasFovMin) config.fovMin = delta.fovMin;
		if (delta.hasFovMax) config.fovMax = delta.fovMax;
		if (delta.hasFovStep) config.fovStep = delta.fovStep;
		if (delta.hasDefaultFov) config.defaultFov = delta.defaultFov;
		if (delta.hasSmoothEnabled) config.smoothEnabled = delta.smoothEnabled;
		if (delta.hasHalfVec) config.halfVec = delta.halfVec;
		if (delta.hasHalfRot) config.halfRot = delta.halfRot;
		if (delta.hasLockHalfRot) config.lockHalfRot = delta.lockHalfRot;
		if (delta.hasLockHalfRotTransition) config.lockHalfRotTransition = delta.lockHalfRotTransition;
		if (delta.hasHalfFov) config.halfFov = delta.halfFov;
		if (delta.hasRotCriticalDamping) config.rotCriticalDamping = delta.rotCriticalDamping;
		if (delta.hasRotDampingRatio) config.rotDampingRatio = delta.rotDampingRatio;
		if (delta.hasWalkMoveSpeed) config.walkMoveSpeed = delta.walkMoveSpeed;
		if (delta.hasWalkMoveAcceleration) config.walkMoveAcceleration = delta.walkMoveAcceleration;
		if (delta.hasWalkMoveDeceleration) config.walkMoveDeceleration = delta.walkMoveDeceleration;
		if (delta.hasWalkRunMultiplier) config.walkRunMultiplier = delta.walkRunMultiplier;
		if (delta.hasWalkCrouchSpeedMultiplier) config.walkCrouchSpeedMultiplier = delta.walkCrouchSpeedMultiplier;
		if (delta.hasWalkLookHalfTime) config.walkLookHalfTime = delta.walkLookHalfTime;
		if (delta.hasWalkFovHalfTime) config.walkFovHalfTime = delta.walkFovHalfTime;
		if (delta.hasWalkGravity) config.walkGravity = delta.walkGravity;
		if (delta.hasWalkJumpSpeed) config.walkJumpSpeed = delta.walkJumpSpeed;
		if (delta.hasWalkHullRadius) config.walkHullRadius = delta.walkHullRadius;
		if (delta.hasWalkHullHalfHeight) config.walkHullHalfHeight = delta.walkHullHalfHeight;
		if (delta.hasWalkCrouchHullHalfHeight) config.walkCrouchHullHalfHeight = delta.walkCrouchHullHalfHeight;
		if (delta.hasWalkCameraTopInset) config.walkCameraTopInset = delta.walkCameraTopInset;
		if (delta.hasWalkStepHeight) config.walkStepHeight = delta.walkStepHeight;
		if (delta.hasWalkGroundProbe) config.walkGroundProbe = delta.walkGroundProbe;
		if (delta.hasWalkMinGroundNormalZ) config.walkMinGroundNormalZ = delta.walkMinGroundNormalZ;
		if (delta.hasWalkTraceMask) config.walkTraceMask = delta.walkTraceMask;
		if (delta.hasWalkModeDefaultEnabled) config.walkModeDefaultEnabled = delta.walkModeDefaultEnabled;
		if (delta.hasHandheldDefaultEnabled) config.handheldDefaultEnabled = delta.handheldDefaultEnabled;
		if (delta.hasWalkBobAmplitudeZ) config.walkBobAmplitudeZ = delta.walkBobAmplitudeZ;
		if (delta.hasWalkBobAmplitudeSide) config.walkBobAmplitudeSide = delta.walkBobAmplitudeSide;
		if (delta.hasWalkBobAmplitudeRoll) config.walkBobAmplitudeRoll = delta.walkBobAmplitudeRoll;
		if (delta.hasWalkBobFrequency) config.walkBobFrequency = delta.walkBobFrequency;
		if (delta.hasHandheldShakePosAmplitude) config.handheldShakePosAmplitude = delta.handheldShakePosAmplitude;
		if (delta.hasHandheldShakeAngAmplitude) config.handheldShakeAngAmplitude = delta.handheldShakeAngAmplitude;
		if (delta.hasHandheldShakeFrequency) config.handheldShakeFrequency = delta.handheldShakeFrequency;
		if (delta.hasHandheldDriftPosAmplitude) config.handheldDriftPosAmplitude = delta.handheldDriftPosAmplitude;
		if (delta.hasHandheldDriftAngAmplitude) config.handheldDriftAngAmplitude = delta.handheldDriftAngAmplitude;
		if (delta.hasHandheldDriftFrequency) config.handheldDriftFrequency = delta.handheldDriftFrequency;
	}
}

void ObsWebSocket_QueueFreecamEnable(FreecamInitMode initMode) {
	PendingAction action;
	action.type = ActionType::FreecamEnable;
	action.freecamInitMode = initMode;
	std::lock_guard<std::mutex> lock(g_ActionMutex);
	g_ActionQueue.push_back(std::move(action));
}

void ObsWebSocket_QueueFreecamDisable() {
	std::lock_guard<std::mutex> lock(g_ActionMutex);
	g_ActionQueue.push_back({ActionType::FreecamDisable});
}

void ObsWebSocket_QueueFreecamHold(bool hasMode, FreecamHoldMode mode) {
	std::lock_guard<std::mutex> lock(g_ActionMutex);
	PendingAction action;
	action.type = ActionType::FreecamHold;
	action.hasHoldMode = hasMode;
	action.holdMode = mode;
	g_ActionQueue.push_back(std::move(action));
}

void ObsWebSocket_QueueFreecamConfig(const FreecamConfigDelta& delta, const std::string& message) {
	PendingAction action;
	action.type = ActionType::FreecamConfig;
	action.configDelta = delta;
	action.message = message;
	std::lock_guard<std::mutex> lock(g_ActionMutex);
	g_ActionQueue.push_back(std::move(action));
}

void ObsWebSocket_QueueFreecamHandoff(const FreecamHandoffPayload& payload) {
	PendingAction action;
	action.type = ActionType::FreecamHandoff;
	action.handoff = payload;
	std::lock_guard<std::mutex> lock(g_ActionMutex);
	g_ActionQueue.push_back(std::move(action));
}

void ObsWebSocket_QueueAttachCamera(const AttachmentCameraState& state) {
	PendingAction action;
	action.type = ActionType::AttachCamera;
	action.attachment = state;
	std::lock_guard<std::mutex> lock(g_ActionMutex);
	g_ActionQueue.push_back(std::move(action));
}

void ObsWebSocket_QueueRefreshBinds() {
	std::lock_guard<std::mutex> lock(g_ActionMutex);
	g_ActionQueue.push_back({ActionType::RefreshBinds});
}

void ObsWebSocket_QueueSetAltSpectatorBindings(bool enabled) {
	PendingAction action;
	action.type = ActionType::SpectatorBindingsMode;
	action.useAltBindings = enabled;
	std::lock_guard<std::mutex> lock(g_ActionMutex);
	g_ActionQueue.push_back(std::move(action));
}

void ObsWebSocket_QueueExecCommand(const std::string& cmd) {
	PendingAction action;
	action.type = ActionType::ExecCommand;
	action.cmd = cmd;
	std::lock_guard<std::mutex> lock(g_ActionMutex);
	g_ActionQueue.push_back(std::move(action));
}

void ObsWebSocket_QueueCampathPlay(const std::string& cmd, double offset) {
	PendingAction action;
	action.type = ActionType::CampathPlay;
	action.cmd = cmd;
	action.offset = offset;
	std::lock_guard<std::mutex> lock(g_ActionMutex);
	g_ActionQueue.push_back(std::move(action));
}

void ObsWebSocket_ProcessActions() {
	std::deque<PendingAction> pending;
	{
		std::lock_guard<std::mutex> lock(g_ActionMutex);
		pending.swap(g_ActionQueue);
	}

	for (const auto& action : pending) {
		switch (action.type) {
		case ActionType::FreecamEnable:
			if (!g_pFreecam) break;
			if (!g_pFreecam->IsEnabled()) {
				CameraTransformSamples samples = Obs_GetRecentCameraTransforms();
				if (action.freecamInitMode == FreecamInitMode::InheritMotion) {
					float targetDelta = 0.0f;
					const bool canCompute = TryComputeInheritInitTargetDelta(g_pFreecam->GetConfig(), samples, targetDelta);
					if (!canCompute || targetDelta > kInheritInitMaxTargetDelta) {
						// TODO: Wire websocket error result
						advancedfx::Warning(
							"Freecam enable rejected: unstable inherit-motion sample (targetDelta=%.3f, threshold=%.1f)\n",
							targetDelta,
							kInheritInitMaxTargetDelta
						);
						break;
					}
				}

				g_pFreecam->SetEnabled(true);
				if (action.freecamInitMode == FreecamInitMode::InheritMotion && samples.hasPrevious)
					g_pFreecam->ResetWithInheritedMotion(samples.previous, samples.current, samples.deltaTime);
				else
					g_pFreecam->Reset(samples.current);
			}
			g_pFreecam->SetInputEnabled(true);
			if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, "spec_mode 4", true);
			if (g_CamPath.Enabled_get()) g_CamPath.Enabled_set(false);
			g_AttachmentCamera.active = false;
			g_AttachmentCameraHadError = false;
			advancedfx::Message("Freecam enabled\n");
			break;
		case ActionType::FreecamDisable:
			if (!g_pFreecam) break;
			g_pFreecam->SetInputEnabled(false);
			advancedfx::Message("Freecam input disabled\n");
			break;
		case ActionType::FreecamHold:
			if (!g_pFreecam || !g_pFreecam->IsEnabled()) break;
			if (action.hasHoldMode) {
				g_pFreecam->SetHoldMovementMode(
					action.holdMode == FreecamHoldMode::World
						? CFreecamController::HoldMovementMode::World
						: CFreecamController::HoldMovementMode::Camera);
			}
			g_pFreecam->SetInputHold(true);
			advancedfx::Message("Freecam input hold enabled\n");
			break;
		case ActionType::FreecamConfig:
			if (!g_pFreecam) break;
			ApplyFreecamConfigDelta(g_pFreecam->GetConfig(), action.configDelta);
			if (!action.message.empty()) {
				advancedfx::Message((action.message + "\n").c_str());
			}
			break;
		case ActionType::FreecamHandoff:
			if (!g_pFreecam) break;
			ApplyFreecamConfigDelta(g_pFreecam->GetConfig(), action.handoff.configDelta);
			{
				const bool wasEnabled = g_pFreecam->IsEnabled();
				if (!wasEnabled) {
					g_pFreecam->SetEnabled(true);
				}
				CameraTransform transform = action.handoff.transform;
				if (action.handoff.hasFov) {
					transform.fov = action.handoff.fov;
				} else {
					transform.fov = g_pFreecam->GetConfig().defaultFov;
				}
				g_pFreecam->Reset(transform);

				if (action.handoff.hasSmoothTransform) {
					CameraTransform smoothTransform = action.handoff.smoothTransform;
					smoothTransform.fov = action.handoff.hasSmoothFov ? action.handoff.smoothFov : transform.fov;
					if (action.handoff.hasSmoothQuat) {
						g_pFreecam->SetSmoothedTransformWithQuat(smoothTransform, action.handoff.smoothQuat);
					} else {
						g_pFreecam->SetSmoothedTransform(smoothTransform);
					}
				}
			}
			g_pFreecam->SetInputEnabled(true);
			if (action.handoff.hasSpeedScalar) {
				g_pFreecam->SetSpeedScalar(action.handoff.speedScalar);
			}
			if (action.handoff.hasWalkRuntimeState) {
				g_pFreecam->ApplyWalkRuntimeState(action.handoff.walkRuntimeState);
			}
			if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, "spec_mode 4", true);
			if (g_CamPath.Enabled_get()) g_CamPath.Enabled_set(false);
			g_AttachmentCamera.active = false;
			g_AttachmentCameraHadError = false;
			advancedfx::Message("Freecam handoff applied\n");
			break;
		case ActionType::AttachCamera:
			g_AttachmentCamera = action.attachment;
			g_AttachmentCamera.animation.startTime = g_AttachmentCamera.animation.enabled
				? g_MirvTime.curtime_get()
				: 0.0;
			g_AttachmentCamera.animation.transitionApplied = false;
			g_AttachmentCamera.animation.transitionMode4Applied = false;
			g_AttachmentCameraHadError = false;
			if (g_pFreecam && g_pFreecam->IsEnabled()) g_pFreecam->SetEnabled(false);
			if (g_pEngineToClient) {
				std::string cmd = "spec_mode 2; spec_player " + std::to_string(action.attachment.controllerIndex);
				g_pEngineToClient->ExecuteClientCmd(0, cmd.c_str(), true);
			}
			break;
		case ActionType::RefreshBinds:
			RefreshSpectatorBindings();
			break;
		case ActionType::SpectatorBindingsMode:
			SetAltSpectatorBindings(action.useAltBindings);
			break;
		case ActionType::ExecCommand:
			if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, action.cmd.c_str(), true);
			break;
		case ActionType::CampathPlay: {
			if (action.cmd.empty()) break;
			float curTime = g_MirvTime.curtime_get();
			std::wstring wcmd;
			UTF8StringToWideString(action.cmd.c_str(), wcmd);
			const bool loaded = g_CamPath.Load(wcmd.c_str());
			g_CamPath.SetStart(curTime - action.offset);
			if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, "spec_mode 4", true);
			if(!g_CamPath.Enabled_get()) g_CamPath.Enabled_set(true);
			if(loaded && g_CamPath.CanEval() && g_pFreecam && g_pFreecam->IsEnabled()) g_pFreecam->SetEnabled(false);
			g_AttachmentCamera.active = false;
			g_AttachmentCameraHadError = false;
			break;
		}
		default:
			break;
		}
	}
}
