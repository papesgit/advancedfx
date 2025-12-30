#include "ObsWebSocketActions.h"

#include "../shared/MirvCampath.h"
#include "MirvTime.h"
#include "ObsSpectatorBindings.h"

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../shared/AfxConsole.h"
#include "../shared/StringTools.h"

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
	}
}

void ObsWebSocket_QueueFreecamEnable() {
	std::lock_guard<std::mutex> lock(g_ActionMutex);
	g_ActionQueue.push_back({ActionType::FreecamEnable});
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
				CameraTransform initTransform = Obs_GetLastCameraTransform();
				g_pFreecam->Reset(initTransform);
			}
			g_pFreecam->SetInputEnabled(true);
			g_pFreecam->SetEnabled(true);
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
			g_CamPath.Load(wcmd.c_str());
			g_CamPath.SetStart(curTime - g_CamPath.GetOffset() - action.offset);
			if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, "spec_mode 4", true);
			if(!g_CamPath.Enabled_get()) g_CamPath.Enabled_set(true);
			if(g_pFreecam && g_pFreecam->IsEnabled()) g_pFreecam->SetEnabled(false);
			g_AttachmentCamera.active = false;
			g_AttachmentCameraHadError = false;
			break;
		}
		default:
			break;
		}
	}
}
