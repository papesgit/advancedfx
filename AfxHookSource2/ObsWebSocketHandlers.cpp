#include "ObsWebSocketHandlers.h"

#include "ObsSpectatorBindings.h"
#include "ObsWebSocketActions.h"
#include "RenderSystemDX11Hooks.h"
#include "MirvImage.h"
#include "MirvTime.h"
#include "hlaeFolder.h"
#include "DeathMsg.h"

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../shared/AfxConsole.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <vector>

using json = nlohmann::json;

extern CFreecamController* g_pFreecam;
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace {
	CObsWebSocketProtocol g_ObsWebSocketProtocol;

	json MakeCommandResult(const std::string& command, bool ok, const std::string& message = std::string()) {
		json result{
			{"type", "command_result"},
			{"command", command},
			{"ok", ok}
		};

		if (!message.empty()) {
			if (ok) result["message"] = message;
			else result["error"] = message;
		}

		return result;
	}

	json MakeExecCmdResult(const std::string& cmd, bool ok, const std::string& message = std::string(), const json& output = json::array()) {
		json result{
			{"type", "exec_cmd_result"},
			{"cmd", cmd},
			{"ok", ok},
			{"output", output.is_array() ? output : json::array()}
		};

		if (!message.empty()) {
			if (ok) result["message"] = message;
			else result["error"] = message;
		}

		return result;
	}

	json MakeCampathPlayResult(const std::string& cmd, bool ok, const std::string& message = std::string()) {
		json result{
			{"type", "campath_play_result"},
			{"cmd", cmd},
			{"ok", ok}
		};

		if (!message.empty()) {
			if (ok) result["message"] = message;
			else result["error"] = message;
		}

		return result;
	}

	bool IsSupportedMirvImageFileExtension(const std::filesystem::path& path) {
		std::string ext = path.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });

		return ext == ".png"
			|| ext == ".jpg"
			|| ext == ".jpeg"
			|| ext == ".bmp"
			|| ext == ".gif"
			|| ext == ".tif"
			|| ext == ".tiff"
			|| ext == ".webp";
	}

	std::vector<std::string> ListMirvImageFiles() {
		std::vector<std::string> result;

		std::filesystem::path root = std::filesystem::path(GetHlaeFolderW()) / L"resources" / L"AfxHookSource2" / L"images";
		std::error_code ec;
		if (!std::filesystem::exists(root, ec) || ec) {
			return result;
		}

		for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
			if (ec) break;
			if (!it->is_regular_file(ec) || ec) continue;

			const auto& path = it->path();
			if (!IsSupportedMirvImageFileExtension(path)) continue;

			auto relative = std::filesystem::relative(path, root, ec);
			if (ec) {
				ec.clear();
				continue;
			}

			std::string value = relative.generic_string();
			if (!value.empty()) {
				result.push_back(std::move(value));
			}
		}

		std::sort(result.begin(), result.end());
		return result;
	}

	bool UpdateFreecamConfigDeltaFromJson(const json& args, FreecamConfigDelta& delta, std::string& message) {
		bool updated = false;
		message = "Freecam config updated: ";

		if (args.contains("mouseSensitivity")) {
			delta.hasMouseSensitivity = true;
			delta.mouseSensitivity = args["mouseSensitivity"].get<float>();
			message += "mouseSensitivity=" + std::to_string(delta.mouseSensitivity) + " ";
			updated = true;
		}

		if (args.contains("moveSpeed")) {
			delta.hasMoveSpeed = true;
			delta.moveSpeed = args["moveSpeed"].get<float>();
			message += "moveSpeed=" + std::to_string(delta.moveSpeed) + " ";
			updated = true;
		}
		if (args.contains("sprintMultiplier")) {
			delta.hasSprintMultiplier = true;
			delta.sprintMultiplier = args["sprintMultiplier"].get<float>();
			message += "sprintMultiplier=" + std::to_string(delta.sprintMultiplier) + " ";
			updated = true;
		}
		if (args.contains("verticalSpeed")) {
			delta.hasVerticalSpeed = true;
			delta.verticalSpeed = args["verticalSpeed"].get<float>();
			message += "verticalSpeed=" + std::to_string(delta.verticalSpeed) + " ";
			updated = true;
		}
		if (args.contains("speedAdjustRate")) {
			delta.hasSpeedAdjustRate = true;
			delta.speedAdjustRate = args["speedAdjustRate"].get<float>();
			message += "speedAdjustRate=" + std::to_string(delta.speedAdjustRate) + " ";
			updated = true;
		}
		if (args.contains("speedMinMultiplier")) {
			delta.hasSpeedMinMultiplier = true;
			delta.speedMinMultiplier = args["speedMinMultiplier"].get<float>();
			message += "speedMinMultiplier=" + std::to_string(delta.speedMinMultiplier) + " ";
			updated = true;
		}
		if (args.contains("speedMaxMultiplier")) {
			delta.hasSpeedMaxMultiplier = true;
			delta.speedMaxMultiplier = args["speedMaxMultiplier"].get<float>();
			message += "speedMaxMultiplier=" + std::to_string(delta.speedMaxMultiplier) + " ";
			updated = true;
		}

		if (args.contains("rollSpeed")) {
			delta.hasRollSpeed = true;
			delta.rollSpeed = args["rollSpeed"].get<float>();
			message += "rollSpeed=" + std::to_string(delta.rollSpeed) + " ";
			updated = true;
		}
		if (args.contains("rollSmoothing")) {
			delta.hasRollSmoothing = true;
			delta.rollSmoothing = args["rollSmoothing"].get<float>();
			message += "rollSmoothing=" + std::to_string(delta.rollSmoothing) + " ";
			updated = true;
		}
		if (args.contains("leanStrength")) {
			delta.hasLeanStrength = true;
			delta.leanStrength = args["leanStrength"].get<float>();
			message += "leanStrength=" + std::to_string(delta.leanStrength) + " ";
			updated = true;
		}
		if (args.contains("leanAccelScale")) {
			delta.hasLeanAccelScale = true;
			delta.leanAccelScale = args["leanAccelScale"].get<float>();
			message += "leanAccelScale=" + std::to_string(delta.leanAccelScale) + " ";
			updated = true;
		}
		if (args.contains("leanVelocityScale")) {
			delta.hasLeanVelocityScale = true;
			delta.leanVelocityScale = args["leanVelocityScale"].get<float>();
			message += "leanVelocityScale=" + std::to_string(delta.leanVelocityScale) + " ";
			updated = true;
		}
		if (args.contains("leanMaxAngle")) {
			delta.hasLeanMaxAngle = true;
			delta.leanMaxAngle = args["leanMaxAngle"].get<float>();
			message += "leanMaxAngle=" + std::to_string(delta.leanMaxAngle) + " ";
			updated = true;
		}
		if (args.contains("leanHalfTime")) {
			delta.hasLeanHalfTime = true;
			delta.leanHalfTime = args["leanHalfTime"].get<float>();
			message += "leanHalfTime=" + std::to_string(delta.leanHalfTime) + " ";
			updated = true;
		}
		if (args.contains("clampPitch")) {
			delta.hasClampPitch = true;
			delta.clampPitch = args["clampPitch"].get<bool>();
			message += "clampPitch=" + std::string(delta.clampPitch ? "true" : "false") + " ";
			updated = true;
		}

		if (args.contains("fovMin")) {
			delta.hasFovMin = true;
			delta.fovMin = args["fovMin"].get<float>();
			message += "fovMin=" + std::to_string(delta.fovMin) + " ";
			updated = true;
		}
		if (args.contains("fovMax")) {
			delta.hasFovMax = true;
			delta.fovMax = args["fovMax"].get<float>();
			message += "fovMax=" + std::to_string(delta.fovMax) + " ";
			updated = true;
		}
		if (args.contains("fovStep")) {
			delta.hasFovStep = true;
			delta.fovStep = args["fovStep"].get<float>();
			message += "fovStep=" + std::to_string(delta.fovStep) + " ";
			updated = true;
		}
		if (args.contains("defaultFov")) {
			delta.hasDefaultFov = true;
			delta.defaultFov = args["defaultFov"].get<float>();
			message += "defaultFov=" + std::to_string(delta.defaultFov) + " ";
			updated = true;
		}

		if (args.contains("smoothEnabled")) {
			delta.hasSmoothEnabled = true;
			delta.smoothEnabled = args["smoothEnabled"].get<bool>();
			message += "smoothEnabled=" + std::string(delta.smoothEnabled ? "true" : "false") + " ";
			updated = true;
		}
		if (args.contains("halfVec")) {
			delta.hasHalfVec = true;
			delta.halfVec = args["halfVec"].get<float>();
			message += "halfVec=" + std::to_string(delta.halfVec) + " ";
			updated = true;
		}
		if (args.contains("halfRot")) {
			delta.hasHalfRot = true;
			delta.halfRot = args["halfRot"].get<float>();
			message += "halfRot=" + std::to_string(delta.halfRot) + " ";
			updated = true;
		}
		if (args.contains("lockHalfRot")) {
			delta.hasLockHalfRot = true;
			delta.lockHalfRot = args["lockHalfRot"].get<float>();
			message += "lockHalfRot=" + std::to_string(delta.lockHalfRot) + " ";
			updated = true;
		}
		if (args.contains("lockHalfRotTransition")) {
			delta.hasLockHalfRotTransition = true;
			delta.lockHalfRotTransition = args["lockHalfRotTransition"].get<float>();
			message += "lockHalfRotTransition=" + std::to_string(delta.lockHalfRotTransition) + " ";
			updated = true;
		}
		if (args.contains("halfFov")) {
			delta.hasHalfFov = true;
			delta.halfFov = args["halfFov"].get<float>();
			message += "halfFov=" + std::to_string(delta.halfFov) + " ";
			updated = true;
		}
		if (args.contains("rotCriticalDamping")) {
			delta.hasRotCriticalDamping = true;
			delta.rotCriticalDamping = args["rotCriticalDamping"].get<bool>();
			message += "rotCriticalDamping=" + std::string(delta.rotCriticalDamping ? "true" : "false") + " ";
			updated = true;
		}
		if (args.contains("rotDampingRatio")) {
			delta.hasRotDampingRatio = true;
			delta.rotDampingRatio = args["rotDampingRatio"].get<float>();
			message += "rotDampingRatio=" + std::to_string(delta.rotDampingRatio) + " ";
			updated = true;
		}

		if (args.contains("walkMoveSpeed")) {
			delta.hasWalkMoveSpeed = true;
			delta.walkMoveSpeed = args["walkMoveSpeed"].get<float>();
			message += "walkMoveSpeed=" + std::to_string(delta.walkMoveSpeed) + " ";
			updated = true;
		}
		if (args.contains("walkMoveAcceleration")) {
			delta.hasWalkMoveAcceleration = true;
			delta.walkMoveAcceleration = args["walkMoveAcceleration"].get<float>();
			message += "walkMoveAcceleration=" + std::to_string(delta.walkMoveAcceleration) + " ";
			updated = true;
		}
		if (args.contains("walkMoveDeceleration")) {
			delta.hasWalkMoveDeceleration = true;
			delta.walkMoveDeceleration = args["walkMoveDeceleration"].get<float>();
			message += "walkMoveDeceleration=" + std::to_string(delta.walkMoveDeceleration) + " ";
			updated = true;
		}
		if (args.contains("walkRunMultiplier")) {
			delta.hasWalkRunMultiplier = true;
			delta.walkRunMultiplier = args["walkRunMultiplier"].get<float>();
			message += "walkRunMultiplier=" + std::to_string(delta.walkRunMultiplier) + " ";
			updated = true;
		}
		if (args.contains("walkCrouchSpeedMultiplier")) {
			delta.hasWalkCrouchSpeedMultiplier = true;
			delta.walkCrouchSpeedMultiplier = args["walkCrouchSpeedMultiplier"].get<float>();
			message += "walkCrouchSpeedMultiplier=" + std::to_string(delta.walkCrouchSpeedMultiplier) + " ";
			updated = true;
		}
		if (args.contains("walkLookHalfTime")) {
			delta.hasWalkLookHalfTime = true;
			delta.walkLookHalfTime = args["walkLookHalfTime"].get<float>();
			message += "walkLookHalfTime=" + std::to_string(delta.walkLookHalfTime) + " ";
			updated = true;
		}
		if (args.contains("walkFovHalfTime")) {
			delta.hasWalkFovHalfTime = true;
			delta.walkFovHalfTime = args["walkFovHalfTime"].get<float>();
			message += "walkFovHalfTime=" + std::to_string(delta.walkFovHalfTime) + " ";
			updated = true;
		}
		if (args.contains("walkGravity")) {
			delta.hasWalkGravity = true;
			delta.walkGravity = args["walkGravity"].get<float>();
			message += "walkGravity=" + std::to_string(delta.walkGravity) + " ";
			updated = true;
		}
		if (args.contains("walkJumpSpeed")) {
			delta.hasWalkJumpSpeed = true;
			delta.walkJumpSpeed = args["walkJumpSpeed"].get<float>();
			message += "walkJumpSpeed=" + std::to_string(delta.walkJumpSpeed) + " ";
			updated = true;
		}
		if (args.contains("walkHullRadius")) {
			delta.hasWalkHullRadius = true;
			delta.walkHullRadius = args["walkHullRadius"].get<float>();
			message += "walkHullRadius=" + std::to_string(delta.walkHullRadius) + " ";
			updated = true;
		}
		if (args.contains("walkHullHalfHeight")) {
			delta.hasWalkHullHalfHeight = true;
			delta.walkHullHalfHeight = args["walkHullHalfHeight"].get<float>();
			message += "walkHullHalfHeight=" + std::to_string(delta.walkHullHalfHeight) + " ";
			updated = true;
		}
		if (args.contains("walkCrouchHullHalfHeight")) {
			delta.hasWalkCrouchHullHalfHeight = true;
			delta.walkCrouchHullHalfHeight = args["walkCrouchHullHalfHeight"].get<float>();
			message += "walkCrouchHullHalfHeight=" + std::to_string(delta.walkCrouchHullHalfHeight) + " ";
			updated = true;
		}
		if (args.contains("walkCameraTopInset")) {
			delta.hasWalkCameraTopInset = true;
			delta.walkCameraTopInset = args["walkCameraTopInset"].get<float>();
			message += "walkCameraTopInset=" + std::to_string(delta.walkCameraTopInset) + " ";
			updated = true;
		}
		if (args.contains("walkStepHeight")) {
			delta.hasWalkStepHeight = true;
			delta.walkStepHeight = args["walkStepHeight"].get<float>();
			message += "walkStepHeight=" + std::to_string(delta.walkStepHeight) + " ";
			updated = true;
		}
		if (args.contains("walkGroundProbe")) {
			delta.hasWalkGroundProbe = true;
			delta.walkGroundProbe = args["walkGroundProbe"].get<float>();
			message += "walkGroundProbe=" + std::to_string(delta.walkGroundProbe) + " ";
			updated = true;
		}
		if (args.contains("walkMinGroundNormalZ")) {
			delta.hasWalkMinGroundNormalZ = true;
			delta.walkMinGroundNormalZ = args["walkMinGroundNormalZ"].get<float>();
			message += "walkMinGroundNormalZ=" + std::to_string(delta.walkMinGroundNormalZ) + " ";
			updated = true;
		}
		if (args.contains("walkTraceMask")) {
			delta.hasWalkTraceMask = true;
			delta.walkTraceMask = args["walkTraceMask"].get<uint32_t>();
			message += "walkTraceMask=" + std::to_string(delta.walkTraceMask) + " ";
			updated = true;
		}
		if (args.contains("walkModeDefaultEnabled")) {
			delta.hasWalkModeDefaultEnabled = true;
			delta.walkModeDefaultEnabled = args["walkModeDefaultEnabled"].get<bool>();
			message += "walkModeDefaultEnabled=" + std::string(delta.walkModeDefaultEnabled ? "true" : "false") + " ";
			updated = true;
		}
		if (args.contains("handheldDefaultEnabled")) {
			delta.hasHandheldDefaultEnabled = true;
			delta.handheldDefaultEnabled = args["handheldDefaultEnabled"].get<bool>();
			message += "handheldDefaultEnabled=" + std::string(delta.handheldDefaultEnabled ? "true" : "false") + " ";
			updated = true;
		}
		if (args.contains("walkBobAmplitudeZ")) {
			delta.hasWalkBobAmplitudeZ = true;
			delta.walkBobAmplitudeZ = args["walkBobAmplitudeZ"].get<float>();
			message += "walkBobAmplitudeZ=" + std::to_string(delta.walkBobAmplitudeZ) + " ";
			updated = true;
		}
		if (args.contains("walkBobAmplitudeSide")) {
			delta.hasWalkBobAmplitudeSide = true;
			delta.walkBobAmplitudeSide = args["walkBobAmplitudeSide"].get<float>();
			message += "walkBobAmplitudeSide=" + std::to_string(delta.walkBobAmplitudeSide) + " ";
			updated = true;
		}
		if (args.contains("walkBobAmplitudeRoll")) {
			delta.hasWalkBobAmplitudeRoll = true;
			delta.walkBobAmplitudeRoll = args["walkBobAmplitudeRoll"].get<float>();
			message += "walkBobAmplitudeRoll=" + std::to_string(delta.walkBobAmplitudeRoll) + " ";
			updated = true;
		}
		if (args.contains("walkBobFrequency")) {
			delta.hasWalkBobFrequency = true;
			delta.walkBobFrequency = args["walkBobFrequency"].get<float>();
			message += "walkBobFrequency=" + std::to_string(delta.walkBobFrequency) + " ";
			updated = true;
		}
		if (args.contains("handheldShakePosAmplitude")) {
			delta.hasHandheldShakePosAmplitude = true;
			delta.handheldShakePosAmplitude = args["handheldShakePosAmplitude"].get<float>();
			message += "handheldShakePosAmplitude=" + std::to_string(delta.handheldShakePosAmplitude) + " ";
			updated = true;
		}
		if (args.contains("handheldShakeAngAmplitude")) {
			delta.hasHandheldShakeAngAmplitude = true;
			delta.handheldShakeAngAmplitude = args["handheldShakeAngAmplitude"].get<float>();
			message += "handheldShakeAngAmplitude=" + std::to_string(delta.handheldShakeAngAmplitude) + " ";
			updated = true;
		}
		if (args.contains("handheldShakeFrequency")) {
			delta.hasHandheldShakeFrequency = true;
			delta.handheldShakeFrequency = args["handheldShakeFrequency"].get<float>();
			message += "handheldShakeFrequency=" + std::to_string(delta.handheldShakeFrequency) + " ";
			updated = true;
		}
		if (args.contains("handheldDriftPosAmplitude")) {
			delta.hasHandheldDriftPosAmplitude = true;
			delta.handheldDriftPosAmplitude = args["handheldDriftPosAmplitude"].get<float>();
			message += "handheldDriftPosAmplitude=" + std::to_string(delta.handheldDriftPosAmplitude) + " ";
			updated = true;
		}
		if (args.contains("handheldDriftAngAmplitude")) {
			delta.hasHandheldDriftAngAmplitude = true;
			delta.handheldDriftAngAmplitude = args["handheldDriftAngAmplitude"].get<float>();
			message += "handheldDriftAngAmplitude=" + std::to_string(delta.handheldDriftAngAmplitude) + " ";
			updated = true;
		}
		if (args.contains("handheldDriftFrequency")) {
			delta.hasHandheldDriftFrequency = true;
			delta.handheldDriftFrequency = args["handheldDriftFrequency"].get<float>();
			message += "handheldDriftFrequency=" + std::to_string(delta.handheldDriftFrequency) + " ";
			updated = true;
		}

		return updated;
	}
}

void RegisterObsWebSocketHandlers() {
	static bool initialized = false;
	if (initialized) return;
	initialized = true;

	g_ObsWebSocketProtocol.RegisterCommandHandler("freecam_enable", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!g_pFreecam) {
			respond(MakeCommandResult("freecam_enable", false, "Freecam controller not ready"));
			return;
		}

		FreecamInitMode initMode = FreecamInitMode::InheritMotion;
		if (args.contains("initMode")) {
			if (!args["initMode"].is_string()) {
				respond(MakeCommandResult("freecam_enable", false, "initMode must be 'inherit_motion' or 'static'"));
				return;
			}
			const auto mode = args["initMode"].get<std::string>();
			if (mode == "inherit_motion") {
				initMode = FreecamInitMode::InheritMotion;
			} else if (mode == "static") {
				initMode = FreecamInitMode::Static;
			} else {
				respond(MakeCommandResult("freecam_enable", false, "initMode must be 'inherit_motion' or 'static'"));
				return;
			}
		}

		ObsWebSocket_QueueFreecamEnable(initMode);
		respond(MakeCommandResult("freecam_enable", true, "Freecam enabled"));
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("freecam_disable", [](const json& /*args*/, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!g_pFreecam) {
			respond(MakeCommandResult("freecam_disable", false, "Freecam controller not ready"));
			return;
		}

		ObsWebSocket_QueueFreecamDisable();
		respond(MakeCommandResult("freecam_disable", true, "Freecam input disabled"));
	});

g_ObsWebSocketProtocol.RegisterCommandHandler("freecam_hold", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!g_pFreecam) {
			respond(MakeCommandResult("freecam_hold", false, "Freecam controller not ready"));
			return;
		}
		if (!g_pFreecam->IsEnabled()) {
			respond(MakeCommandResult("freecam_hold", false, "Freecam is not enabled"));
			return;
		}

		bool hasMode = false;
		FreecamHoldMode mode = FreecamHoldMode::Camera;
		if (args.contains("mode")) {
			if (!args["mode"].is_string()) {
				respond(MakeCommandResult("freecam_hold", false, "mode must be 'camera' or 'world'"));
				return;
			}
			auto modeStr = args["mode"].get<std::string>();
			if (modeStr == "camera") {
				mode = FreecamHoldMode::Camera;
				hasMode = true;
			} else if (modeStr == "world") {
				mode = FreecamHoldMode::World;
				hasMode = true;
			} else {
				respond(MakeCommandResult("freecam_hold", false, "mode must be 'camera' or 'world'"));
				return;
			}
		}

		ObsWebSocket_QueueFreecamHold(hasMode, mode);
		respond(MakeCommandResult("freecam_hold", true, "Freecam input hold enabled"));
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("freecam_config", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!g_pFreecam) {
			respond(MakeCommandResult("freecam_config", false, "Freecam controller not ready"));
			return;
		}

		FreecamConfigDelta delta;
		std::string message;
		bool updated = UpdateFreecamConfigDeltaFromJson(args, delta, message);

		if (updated) {
			ObsWebSocket_QueueFreecamConfig(delta, message);
			respond(MakeCommandResult("freecam_config", true, message));
		} else {
			respond(MakeCommandResult("freecam_config", false, "No valid config parameters provided"));
		}
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("freecam_handoff", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!g_pFreecam) {
			respond(MakeCommandResult("freecam_handoff", false, "Freecam controller not ready"));
			return;
		}

		if (!args.contains("posX") || !args.contains("posY") || !args.contains("posZ")
			|| !args.contains("pitch") || !args.contains("yaw") || !args.contains("roll")) {
			respond(MakeCommandResult("freecam_handoff", false, "Missing position or rotation"));
			return;
		}

		FreecamConfigDelta delta;
		std::string message;
		UpdateFreecamConfigDeltaFromJson(args, delta, message);

		FreecamHandoffPayload payload;
		payload.configDelta = delta;
		payload.transform.x = args["posX"].get<float>();
		payload.transform.y = args["posY"].get<float>();
		payload.transform.z = args["posZ"].get<float>();
		payload.transform.pitch = args["pitch"].get<float>();
		payload.transform.yaw = args["yaw"].get<float>();
		payload.transform.roll = args["roll"].get<float>();
		if (args.contains("fov")) {
			payload.hasFov = true;
			payload.fov = args["fov"].get<float>();
		}

		payload.hasSmoothTransform = args.contains("smoothPosX") && args.contains("smoothPosY") && args.contains("smoothPosZ");
		if (payload.hasSmoothTransform) {
			payload.smoothTransform = payload.transform;
			payload.smoothTransform.x = args["smoothPosX"].get<float>();
			payload.smoothTransform.y = args["smoothPosY"].get<float>();
			payload.smoothTransform.z = args["smoothPosZ"].get<float>();
			if (args.contains("smoothFov")) {
				payload.hasSmoothFov = true;
				payload.smoothFov = args["smoothFov"].get<float>();
			}
		}
		payload.hasSmoothQuat = args.contains("smoothQuatW") && args.contains("smoothQuatX")
			&& args.contains("smoothQuatY") && args.contains("smoothQuatZ");
		if (payload.hasSmoothQuat) {
			payload.smoothQuat.W = args["smoothQuatW"].get<float>();
			payload.smoothQuat.X = args["smoothQuatX"].get<float>();
			payload.smoothQuat.Y = args["smoothQuatY"].get<float>();
			payload.smoothQuat.Z = args["smoothQuatZ"].get<float>();
		}

		if (args.contains("speedScalar")) {
			payload.hasSpeedScalar = true;
			payload.speedScalar = args["speedScalar"].get<float>();
		}
		if (args.contains("walkModeEnabled")) {
			payload.hasWalkRuntimeState = true;
			payload.walkRuntimeState.walkModeEnabled = args["walkModeEnabled"].get<bool>();
		}
		if (args.contains("handheldEffectsEnabled")) {
			payload.hasWalkRuntimeState = true;
			payload.walkRuntimeState.handheldEffectsEnabled = args["handheldEffectsEnabled"].get<bool>();
		}
		if (args.contains("walkVelocityX")) {
			payload.hasWalkRuntimeState = true;
			payload.walkRuntimeState.velocityX = args["walkVelocityX"].get<float>();
		}
		if (args.contains("walkVelocityY")) {
			payload.hasWalkRuntimeState = true;
			payload.walkRuntimeState.velocityY = args["walkVelocityY"].get<float>();
		}
		if (args.contains("walkVerticalVelocity")) {
			payload.hasWalkRuntimeState = true;
			payload.walkRuntimeState.velocityZ = args["walkVerticalVelocity"].get<float>();
		}
		if (args.contains("walkOnGround")) {
			payload.hasWalkRuntimeState = true;
			payload.walkRuntimeState.onGround = args["walkOnGround"].get<bool>();
		}
		if (args.contains("walkCrouchAmount")) {
			payload.hasWalkRuntimeState = true;
			payload.walkRuntimeState.crouchAmount = args["walkCrouchAmount"].get<float>();
		}
		if (args.contains("walkBobPhase")) {
			payload.hasWalkRuntimeState = true;
			payload.walkRuntimeState.bobPhase = args["walkBobPhase"].get<float>();
		}
		if (args.contains("walkEffectTime")) {
			payload.hasWalkRuntimeState = true;
			payload.walkRuntimeState.effectTime = args["walkEffectTime"].get<float>();
		}
		if (args.contains("walkTargetPitch")) {
			payload.hasWalkRuntimeState = true;
			payload.walkRuntimeState.targetPitch = args["walkTargetPitch"].get<float>();
		}
		if (args.contains("walkTargetYaw")) {
			payload.hasWalkRuntimeState = true;
			payload.walkRuntimeState.targetYaw = args["walkTargetYaw"].get<float>();
		}
		if (args.contains("walkTargetFov")) {
			payload.hasWalkRuntimeState = true;
			payload.walkRuntimeState.targetFov = args["walkTargetFov"].get<float>();
		}
		
		g_pFreecam->m_PlayerLockActive = false;

		ObsWebSocket_QueueFreecamHandoff(payload);
		respond(MakeCommandResult("freecam_handoff", true, "Freecam handoff applied"));
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("attach_camera", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!args.contains("observer_slot")) {
			respond(MakeCommandResult("attach_camera", false, "Missing observer_slot"));
			return;
		}
		int slot = args["observer_slot"].get<int>();
		if (slot < 0 || slot > 9) {
			respond(MakeCommandResult("attach_camera", false, "observer_slot must be 0-9"));
			return;
		}

		if (!args.contains("attachment")) {
			respond(MakeCommandResult("attach_camera", false, "Missing attachment"));
			return;
		}

		if (!args.contains("offset_pos") || !args.contains("offset_angles")) {
			respond(MakeCommandResult("attach_camera", false, "Missing offset_pos or offset_angles"));
			return;
		}

		if (slot < 0 || slot > 9 || g_SpectatorBindings[slot] == -1) {
			respond(MakeCommandResult("attach_camera", false, "observer_slot not mapped to a controller"));
			return;
		}

		AttachmentCameraState state;
		state.active = true;
		state.controllerIndex = g_SpectatorBindings[slot];

		if (args["attachment"].is_number_integer()) {
			int idx = args["attachment"].get<int>();
			if (idx < 0 || idx > 255) {
				respond(MakeCommandResult("attach_camera", false, "attachment index must be 0-255"));
				return;
			}
			state.useAttachmentIndex = true;
			state.attachmentIndex = (uint8_t)idx;
		} else if (args["attachment"].is_string()) {
			state.useAttachmentIndex = false;
			state.attachmentName = args["attachment"].get<std::string>();
		} else {
			respond(MakeCommandResult("attach_camera", false, "attachment must be index or name"));
			return;
		}

		try {
			state.offsetPos.x = (float)args["offset_pos"].at("x").get<double>();
			state.offsetPos.y = (float)args["offset_pos"].at("y").get<double>();
			state.offsetPos.z = (float)args["offset_pos"].at("z").get<double>();

			state.offsetAngles = Afx::Math::QEulerAngles(
				args["offset_angles"].at("pitch").get<double>(),
				args["offset_angles"].at("yaw").get<double>(),
				args["offset_angles"].at("roll").get<double>()
			);

			state.fov = args.contains("fov")
				? (float)args["fov"].get<double>()
				: 90.0f;
		} catch (...) {
			respond(MakeCommandResult("attach_camera", false, "Invalid offset_pos or offset_angles payload"));
			return;
		}

		state.rotationReference = AttachmentCameraRotationReference::Attachment;
		state.rotationBasisPitch = AttachmentCameraRotationBasis::Attachment;
		state.rotationBasisYaw = AttachmentCameraRotationBasis::Attachment;
		state.rotationBasisRoll = AttachmentCameraRotationBasis::Attachment;

		if (args.contains("rotation_reference") && args["rotation_reference"].is_string()) {
			const auto reference = args["rotation_reference"].get<std::string>();
			if (0 == _stricmp(reference.c_str(), "offset_local")) {
				state.rotationReference = AttachmentCameraRotationReference::OffsetLocal;
			}
		}

		if (args.contains("rotation_basis") && args["rotation_basis"].is_object()) {
			const auto& basis = args["rotation_basis"];
			auto parseBasis = [](const json& obj, const char* key, AttachmentCameraRotationBasis fallback) {
				if (!obj.contains(key) || !obj[key].is_string()) return fallback;
				const auto value = obj[key].get<std::string>();
				if (0 == _stricmp(value.c_str(), "world")) {
					return AttachmentCameraRotationBasis::World;
				}
				return AttachmentCameraRotationBasis::Attachment;
			};
			state.rotationBasisPitch = parseBasis(basis, "pitch", state.rotationBasisPitch);
			state.rotationBasisYaw = parseBasis(basis, "yaw", state.rotationBasisYaw);
			state.rotationBasisRoll = parseBasis(basis, "roll", state.rotationBasisRoll);
		}

		state.rotationLockPitch = false;
		state.rotationLockYaw = false;
		state.rotationLockRoll = false;
		if (args.contains("rotation_axis_lock") && args["rotation_axis_lock"].is_object()) {
			const auto& locks = args["rotation_axis_lock"];
			auto parseLock = [](const json& obj, const char* key) {
				return obj.contains(key) && obj[key].is_boolean() ? obj[key].get<bool>() : false;
			};
			state.rotationLockPitch = parseLock(locks, "pitch");
			state.rotationLockYaw = parseLock(locks, "yaw");
			state.rotationLockRoll = parseLock(locks, "roll");
		}

		// Optional attach animation.
		if (args.contains("animation") && args["animation"].is_object()) {
			const auto& anim = args["animation"];
			state.animation.enabled = anim.contains("enabled") && anim["enabled"].is_boolean()
				? anim["enabled"].get<bool>()
				: true;

			state.animation.keyframes.clear();
			state.animation.hasTransition = false;
			state.animation.transitionTime = 0.0;
			state.animation.transitionDuration = 0.0;
			state.animation.transitionEasing = AttachmentCameraTransitionEasing::Smoothstep;
			state.animation.targetControllerIndex = -1;
			state.animation.transitionApplied = false;
			state.animation.transitionMode4Applied = false;

			if (state.animation.enabled) {
				if (anim.contains("events") && anim["events"].is_array()) {
					for (const auto& ev : anim["events"]) {
						if (!ev.is_object()) continue;
						const std::string type = ev.contains("type") && ev["type"].is_string()
							? ev["type"].get<std::string>()
							: "keyframe";
						const double time = ev.contains("time") && ev["time"].is_number()
							? ev["time"].get<double>()
							: 0.0;
						const int order = ev.contains("order") && ev["order"].is_number_integer()
							? ev["order"].get<int>()
							: 0;

						if (0 == _stricmp(type.c_str(), "transition")) {
							if (state.animation.hasTransition) {
								respond(MakeCommandResult("attach_camera", false, "Only one transition event is supported"));
								return;
							}
							state.animation.hasTransition = true;
							state.animation.transitionTime = time;
							state.animation.transitionDuration = ev.contains("duration") && ev["duration"].is_number()
								? ev["duration"].get<double>()
								: 0.0;
							if (state.animation.transitionDuration < 0.0) state.animation.transitionDuration = 0.0;
							state.animation.transitionEasing = AttachmentCameraTransitionEasing::Smoothstep;
							if (ev.contains("easing") && ev["easing"].is_string()) {
								const auto easing = ev["easing"].get<std::string>();
								if (0 == _stricmp(easing.c_str(), "linear")) {
									state.animation.transitionEasing = AttachmentCameraTransitionEasing::Linear;
								} else if (0 == _stricmp(easing.c_str(), "easeinoutcubic")) {
									state.animation.transitionEasing = AttachmentCameraTransitionEasing::EaseInOutCubic;
								} else {
									state.animation.transitionEasing = AttachmentCameraTransitionEasing::Smoothstep;
								}
							}
							continue;
						}

						AttachmentCameraKeyframe kf;
						kf.time = time;
						kf.order = order;
						kf.easingCurve = AttachmentCameraKeyframeEasingCurve::Linear;
						kf.easingMode = AttachmentCameraKeyframeEase::EaseInOut;
						kf.rotationSampling = AttachmentCameraKeyframeRotationSampling::Live;
						kf.followAttachmentPitch = true;
						kf.followAttachmentYaw = true;
						kf.followAttachmentRoll = true;

						if (ev.contains("easing_curve") && ev["easing_curve"].is_string()) {
							const auto curve = ev["easing_curve"].get<std::string>();
							if (0 == _stricmp(curve.c_str(), "smoothstep")) {
								kf.easingCurve = AttachmentCameraKeyframeEasingCurve::Smoothstep;
							} else if (0 == _stricmp(curve.c_str(), "cubic")) {
								kf.easingCurve = AttachmentCameraKeyframeEasingCurve::Cubic;
							} else {
								kf.easingCurve = AttachmentCameraKeyframeEasingCurve::Linear;
							}
						}

						if (ev.contains("easing_mode") && ev["easing_mode"].is_string()) {
							const auto mode = ev["easing_mode"].get<std::string>();
							if (0 == _stricmp(mode.c_str(), "easein")) {
								kf.easingMode = AttachmentCameraKeyframeEase::EaseIn;
							} else if (0 == _stricmp(mode.c_str(), "easeout")) {
								kf.easingMode = AttachmentCameraKeyframeEase::EaseOut;
							} else {
								kf.easingMode = AttachmentCameraKeyframeEase::EaseInOut;
							}
						}

						if (ev.contains("delta_pos") && ev["delta_pos"].is_object()) {
							const auto& dp = ev["delta_pos"];
							if (dp.contains("x") && dp["x"].is_number()) kf.deltaPos.x = (float)dp["x"].get<double>();
							if (dp.contains("y") && dp["y"].is_number()) kf.deltaPos.y = (float)dp["y"].get<double>();
							if (dp.contains("z") && dp["z"].is_number()) kf.deltaPos.z = (float)dp["z"].get<double>();
						}

						if (ev.contains("delta_angles") && ev["delta_angles"].is_object()) {
							const auto& da = ev["delta_angles"];
							const double pitch = da.contains("pitch") && da["pitch"].is_number() ? da["pitch"].get<double>() : 0.0;
							const double yaw = da.contains("yaw") && da["yaw"].is_number() ? da["yaw"].get<double>() : 0.0;
							const double roll = da.contains("roll") && da["roll"].is_number() ? da["roll"].get<double>() : 0.0;
							kf.deltaAngles = Afx::Math::QEulerAngles(pitch, yaw, roll);
						}

						if (ev.contains("fov") && ev["fov"].is_number()) {
							kf.hasFov = true;
							kf.fov = (float)ev["fov"].get<double>();
						}

						if (ev.contains("rotation_sampling") && ev["rotation_sampling"].is_string()) {
							const auto sampling = ev["rotation_sampling"].get<std::string>();
							if (0 == _stricmp(sampling.c_str(), "freeze_at_segment_start")) {
								kf.rotationSampling = AttachmentCameraKeyframeRotationSampling::FreezeAtSegmentStart;
							}
						}

						if (ev.contains("follow_attachment") && ev["follow_attachment"].is_object()) {
							const auto& follow = ev["follow_attachment"];
							auto parseFollow = [](const json& obj, const char* key) {
								return obj.contains(key) && obj[key].is_boolean() ? obj[key].get<bool>() : true;
							};
							kf.followAttachmentPitch = parseFollow(follow, "pitch");
							kf.followAttachmentYaw = parseFollow(follow, "yaw");
							kf.followAttachmentRoll = parseFollow(follow, "roll");
						}

						state.animation.keyframes.push_back(std::move(kf));
					}
				}

				// Ensure implicit base keyframe at t=0, order=0.
				bool hasBase = false;
				for (const auto& kf : state.animation.keyframes) {
					if (kf.time == 0.0 && kf.order == 0) {
						hasBase = true;
						break;
					}
				}
				if (!hasBase) {
					state.animation.keyframes.push_back(AttachmentCameraKeyframe{});
				}

				std::sort(state.animation.keyframes.begin(), state.animation.keyframes.end(), [](const AttachmentCameraKeyframe& a, const AttachmentCameraKeyframe& b) {
					if (a.time < b.time) return true;
					if (a.time > b.time) return false;
					return a.order < b.order;
				});

				if (state.animation.hasTransition) {
					if (!args.contains("target_observer_slot") || !args["target_observer_slot"].is_number_integer()) {
						respond(MakeCommandResult("attach_camera", false, "Missing target_observer_slot for transition"));
						return;
					}
					int targetSlot = args["target_observer_slot"].get<int>();
					if (targetSlot < 0 || targetSlot > 9) {
						respond(MakeCommandResult("attach_camera", false, "target_observer_slot must be 0-9"));
						return;
					}
					if (g_SpectatorBindings[targetSlot] == -1) {
						respond(MakeCommandResult("attach_camera", false, "target_observer_slot not mapped to a controller"));
						return;
					}
					state.animation.targetControllerIndex = g_SpectatorBindings[targetSlot];
				}
			}
		}

		ObsWebSocket_QueueAttachCamera(state);

		std::string attachmentDesc = state.useAttachmentIndex ? std::to_string(state.attachmentIndex) : state.attachmentName;
		std::ostringstream oss;
		oss << "Attached to controller " << state.controllerIndex
			<< " attachment (" << (state.useAttachmentIndex ? "index " : "name ") << attachmentDesc << ")"
			<< " offset pos [" << state.offsetPos.x << " " << state.offsetPos.y << " " << state.offsetPos.z << "]"
			<< " angles [" << state.offsetAngles.Pitch << " " << state.offsetAngles.Yaw << " " << state.offsetAngles.Roll << "]"
			<< " fov [" << state.fov << "]";

		respond(MakeCommandResult("attach_camera", true, oss.str()));
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("refresh_binds", [](const json& /*args*/, const CObsWebSocketProtocol::JsonResponder& respond) {
		ObsWebSocket_QueueRefreshBinds();
		respond(MakeCommandResult("refresh_binds", true, "Spectator bindings refreshed"));
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("curtime_get", [](const json& /*args*/, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!g_pEngineToClient) {
			respond(json{
				{"type", "curtime"},
				{"ok", false},
				{"error", "Engine not ready"}
			});
			return;
		}

		const double curTime = g_MirvTime.curtime_get();
		respond(json{
			{"type", "curtime"},
			{"ok", true},
			{"value", curTime}
		});
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("gfx.camera.get", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		json result{
			{"type", "gfx.camera.get"},
			{"ok", true},
			{"pos", { g_CurrentGameCamera.origin[0], g_CurrentGameCamera.origin[1], g_CurrentGameCamera.origin[2] }},
			{"ang", { g_CurrentGameCamera.angles[0], g_CurrentGameCamera.angles[1], g_CurrentGameCamera.angles[2] }}
		};

		if (args.contains("requestId") && args["requestId"].is_string()) {
			result["requestId"] = args["requestId"].get<std::string>();
		}

		respond(result);
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("spectator_bindings_mode", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!args.contains("useAlt") || !args["useAlt"].is_boolean()) {
			respond(MakeCommandResult("spectator_bindings_mode", false, "Missing or invalid useAlt flag"));
			return;
		}

		bool useAlt = args["useAlt"].get<bool>();
		ObsWebSocket_QueueSetAltSpectatorBindings(useAlt);
		respond(MakeCommandResult("spectator_bindings_mode", true, useAlt ? "Alt spectator bindings enabled" : "Alt spectator bindings disabled"));
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("sharedtex_register", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!args.contains("pid") || !args["pid"].is_number_integer()) {
			respond(MakeCommandResult("sharedtex_register", false, "Missing or invalid pid"));
			return;
		}

		unsigned int pid = args["pid"].get<unsigned int>();
		unsigned long long handleValue = 0;
		std::string error;
		if (!AfxSharedTexture_DuplicateHandleForPid(pid, handleValue, error)) {
			respond(MakeCommandResult("sharedtex_register", false, error.empty() ? "Failed to duplicate shared texture handle" : error));
			return;
		}

		std::ostringstream oss;
		oss << "0x" << std::hex << handleValue;
		json result{
			{"type", "sharedtex_handle"},
			{"ok", true},
			{"pid", pid},
			{"handle", oss.str()}
		};
		respond(result);
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("gfx.register", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!args.contains("name") || !args["name"].is_string()) {
			respond(MakeCommandResult("gfx.register", false, "Missing or invalid name"));
			return;
		}
		if (!args.contains("width") || !args["width"].is_number_integer()) {
			respond(MakeCommandResult("gfx.register", false, "Missing or invalid width"));
			return;
		}
		if (!args.contains("height") || !args["height"].is_number_integer()) {
			respond(MakeCommandResult("gfx.register", false, "Missing or invalid height"));
			return;
		}

		const std::string name = args["name"].get<std::string>();
		const UINT width = static_cast<UINT>(args["width"].get<int>());
		const UINT height = static_cast<UINT>(args["height"].get<int>());

		if (!args.contains("handle") || !args["handle"].is_string()) {
			respond(MakeCommandResult("gfx.register", false, "Missing or invalid handle"));
			return;
		}

		std::string format = "BGRA8";
		if (args.contains("format") && args["format"].is_string()) {
			format = args["format"].get<std::string>();
		}
		std::string alphaMode = "premultiplied";
		if (args.contains("alphaMode") && args["alphaMode"].is_string()) {
			alphaMode = args["alphaMode"].get<std::string>();
		}
		bool keyedMutex = true;
		if (args.contains("keyedMutex") && args["keyedMutex"].is_boolean()) {
			keyedMutex = args["keyedMutex"].get<bool>();
		}

		const std::string handle = args["handle"].get<std::string>();
		g_MirvImageDrawer.RegisterAtlas(name.c_str(), handle.c_str(), width, height, format.c_str(), alphaMode.c_str(), keyedMutex);

		if (args.contains("regions") && args["regions"].is_array()) {
			for (const auto& region : args["regions"]) {
				if (!region.is_object()) continue;
				if (!region.contains("id") || !region["id"].is_string()) continue;
				if (!region.contains("u0") || !region.contains("v0") || !region.contains("u1") || !region.contains("v1")) continue;
				if (!region["u0"].is_number() || !region["v0"].is_number() || !region["u1"].is_number() || !region["v1"].is_number()) continue;

				double defaultW = 1.0;
				double defaultH = 1.0;
				if (region.contains("defaultSize") && region["defaultSize"].is_array() && region["defaultSize"].size() == 2) {
					if (region["defaultSize"][0].is_number()) defaultW = region["defaultSize"][0].get<double>();
					if (region["defaultSize"][1].is_number()) defaultH = region["defaultSize"][1].get<double>();
				}

				g_MirvImageDrawer.SetAtlasRegion(
					name.c_str(),
					region["id"].get<std::string>().c_str(),
					(float)region["u0"].get<double>(),
					(float)region["v0"].get<double>(),
					(float)region["u1"].get<double>(),
					(float)region["v1"].get<double>(),
					defaultW,
					defaultH
				);
			}
		}

		respond(MakeCommandResult("gfx.register", true, "Atlas registered"));
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("gfx.atlas.create", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!args.contains("name") || !args["name"].is_string()) {
			respond(MakeCommandResult("gfx.atlas.create", false, "Missing or invalid name"));
			return;
		}
		if (!args.contains("handle") || !args["handle"].is_string()) {
			respond(MakeCommandResult("gfx.atlas.create", false, "Missing or invalid handle"));
			return;
		}
		if (!args.contains("width") || !args["width"].is_number_integer()) {
			respond(MakeCommandResult("gfx.atlas.create", false, "Missing or invalid width"));
			return;
		}
		if (!args.contains("height") || !args["height"].is_number_integer()) {
			respond(MakeCommandResult("gfx.atlas.create", false, "Missing or invalid height"));
			return;
		}

		const std::string name = args["name"].get<std::string>();
		const std::string handle = args["handle"].get<std::string>();
		const UINT width = static_cast<UINT>(args["width"].get<int>());
		const UINT height = static_cast<UINT>(args["height"].get<int>());

		std::string format = "BGRA8";
		if (args.contains("format") && args["format"].is_string()) {
			format = args["format"].get<std::string>();
		}
		std::string alphaMode = "premultiplied";
		if (args.contains("alphaMode") && args["alphaMode"].is_string()) {
			alphaMode = args["alphaMode"].get<std::string>();
		}
		bool keyedMutex = true;
		if (args.contains("keyedMutex") && args["keyedMutex"].is_boolean()) {
			keyedMutex = args["keyedMutex"].get<bool>();
		}

		g_MirvImageDrawer.RegisterAtlas(name.c_str(), handle.c_str(), width, height, format.c_str(), alphaMode.c_str(), keyedMutex);
		respond(MakeCommandResult("gfx.atlas.create", true, "Atlas created"));
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("gfx.atlas.destroy", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!args.contains("name") || !args["name"].is_string()) {
			respond(MakeCommandResult("gfx.atlas.destroy", false, "Missing or invalid name"));
			return;
		}
		g_MirvImageDrawer.UnregisterAtlas(args["name"].get<std::string>().c_str());
		respond(MakeCommandResult("gfx.atlas.destroy", true, "Atlas destroyed"));
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("gfx.atlas.list", [](const json& /*args*/, const CObsWebSocketProtocol::JsonResponder& respond) {
		std::vector<CMirvImageDrawer::AtlasSnapshot> atlases;
		g_MirvImageDrawer.GetAtlasSnapshot(atlases);

		json list = json::array();
		for (const auto& atlas : atlases) {
			json regions = json::array();
			for (const auto& region : atlas.regions) {
				regions.push_back({
					{"id", region.id},
					{"u0", region.u0},
					{"v0", region.v0},
					{"u1", region.u1},
					{"v1", region.v1},
					{"defaultSize", { region.defaultW, region.defaultH }}
				});
			}
			std::ostringstream handleStream;
			handleStream << "0x" << std::hex << (uintptr_t)atlas.handle;
			list.push_back({
				{"name", atlas.name},
				{"handle", handleStream.str()},
				{"width", atlas.width},
				{"height", atlas.height},
				{"format", atlas.format == CMirvImageDrawer::AtlasFormat::RGBA8 ? "RGBA8" : "BGRA8"},
				{"alphaMode", atlas.alphaMode == CMirvImageDrawer::AlphaMode::Straight ? "straight" : "premultiplied"},
				{"keyedMutex", atlas.keyedMutex},
				{"open", atlas.open},
				{"keyed", atlas.keyed},
				{"regions", regions}
			});
		}

		json result{
			{"type", "gfx.atlas.list"},
			{"ok", true},
			{"atlases", list}
		};
		respond(result);
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("gfx.image.list", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		json result{
			{"type", "gfx.image.list"},
			{"ok", true},
			{"images", ListMirvImageFiles()}
		};

		if (args.contains("requestId") && args["requestId"].is_string()) {
			result["requestId"] = args["requestId"].get<std::string>();
		}

		respond(result);
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("gfx.atlas.region.set", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!args.contains("atlas") || !args["atlas"].is_string()) {
			respond(MakeCommandResult("gfx.atlas.region.set", false, "Missing or invalid atlas"));
			return;
		}
		if (!args.contains("id") || !args["id"].is_string()) {
			respond(MakeCommandResult("gfx.atlas.region.set", false, "Missing or invalid id"));
			return;
		}
		if (!args.contains("u0") || !args.contains("v0") || !args.contains("u1") || !args.contains("v1")) {
			respond(MakeCommandResult("gfx.atlas.region.set", false, "Missing uv coordinates"));
			return;
		}

		const std::string atlas = args["atlas"].get<std::string>();
		const std::string id = args["id"].get<std::string>();
		double defaultW = 1.0;
		double defaultH = 1.0;
		if (args.contains("defaultSize") && args["defaultSize"].is_array() && args["defaultSize"].size() == 2) {
			if (args["defaultSize"][0].is_number()) defaultW = args["defaultSize"][0].get<double>();
			if (args["defaultSize"][1].is_number()) defaultH = args["defaultSize"][1].get<double>();
		}

		g_MirvImageDrawer.SetAtlasRegion(
			atlas.c_str(),
			id.c_str(),
			(float)args["u0"].get<double>(),
			(float)args["v0"].get<double>(),
			(float)args["u1"].get<double>(),
			(float)args["v1"].get<double>(),
			defaultW,
			defaultH
		);
		respond(MakeCommandResult("gfx.atlas.region.set", true, "Region set"));
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("gfx.atlas.region.remove", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!args.contains("atlas") || !args["atlas"].is_string()) {
			respond(MakeCommandResult("gfx.atlas.region.remove", false, "Missing or invalid atlas"));
			return;
		}
		if (!args.contains("id") || !args["id"].is_string()) {
			respond(MakeCommandResult("gfx.atlas.region.remove", false, "Missing or invalid id"));
			return;
		}
		g_MirvImageDrawer.RemoveAtlasRegion(
			args["atlas"].get<std::string>().c_str(),
			args["id"].get<std::string>().c_str()
		);
		respond(MakeCommandResult("gfx.atlas.region.remove", true, "Region removed"));
	});


	g_ObsWebSocketProtocol.RegisterCommandHandler("gfx.updateRegions", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!args.contains("name") || !args["name"].is_string()) {
			respond(MakeCommandResult("gfx.updateRegions", false, "Missing or invalid name"));
			return;
		}
		if (!args.contains("regions") || !args["regions"].is_array()) {
			respond(MakeCommandResult("gfx.updateRegions", false, "Missing or invalid regions"));
			return;
		}

		const std::string name = args["name"].get<std::string>();
		for (const auto& region : args["regions"]) {
			if (!region.is_object()) continue;
			if (!region.contains("id") || !region["id"].is_string()) continue;
			if (!region.contains("u0") || !region.contains("v0") || !region.contains("u1") || !region.contains("v1")) continue;
			if (!region["u0"].is_number() || !region["v0"].is_number() || !region["u1"].is_number() || !region["v1"].is_number()) continue;

			double defaultW = 1.0;
			double defaultH = 1.0;
			if (region.contains("defaultSize") && region["defaultSize"].is_array() && region["defaultSize"].size() == 2) {
				if (region["defaultSize"][0].is_number()) defaultW = region["defaultSize"][0].get<double>();
				if (region["defaultSize"][1].is_number()) defaultH = region["defaultSize"][1].get<double>();
			}

			g_MirvImageDrawer.SetAtlasRegion(
				name.c_str(),
				region["id"].get<std::string>().c_str(),
				(float)region["u0"].get<double>(),
				(float)region["v0"].get<double>(),
				(float)region["u1"].get<double>(),
				(float)region["v1"].get<double>(),
				defaultW,
				defaultH
			);
		}

		respond(MakeCommandResult("gfx.updateRegions", true, "Regions updated"));
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("gfx.unregister", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!args.contains("name") || !args["name"].is_string()) {
			respond(MakeCommandResult("gfx.unregister", false, "Missing or invalid name"));
			return;
		}
		g_MirvImageDrawer.UnregisterAtlas(args["name"].get<std::string>().c_str());
		respond(MakeCommandResult("gfx.unregister", true, "Atlas unregistered"));
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("gfx.instance.create", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!args.contains("name") || !args["name"].is_string()) {
			respond(MakeCommandResult("gfx.instance.create", false, "Missing or invalid name"));
			return;
		}

		const std::string name = args["name"].get<std::string>();
		if (args.contains("imageFile") && args["imageFile"].is_string()) {
			g_MirvImageDrawer.LoadImage(name.c_str(), args["imageFile"].get<std::string>().c_str());
		}
		else {
			if (!args.contains("atlas") || !args["atlas"].is_string()) {
				respond(MakeCommandResult("gfx.instance.create", false, "Missing or invalid atlas"));
				return;
			}
			if (!args.contains("region") || !args["region"].is_string()) {
				respond(MakeCommandResult("gfx.instance.create", false, "Missing or invalid region"));
				return;
			}

			const std::string atlas = args["atlas"].get<std::string>();
			const std::string region = args["region"].get<std::string>();
			g_MirvImageDrawer.UseAtlasRegion(name.c_str(), atlas.c_str(), region.c_str());
		}

		if (args.contains("attach") && args["attach"].is_object()) {
			const auto& attach = args["attach"];
			if (attach.contains("slot") && attach["slot"].is_number_integer()) {
				int slot = attach["slot"].get<int>();
				if (slot < -1 || slot > 9) {
					respond(MakeCommandResult("gfx.instance.create", false, "attach.slot must be -1 or 0-9"));
					return;
				}
				bool useYaw = attach.contains("useYaw") && attach["useYaw"].is_boolean() ? attach["useYaw"].get<bool>() : false;
				bool usePitch = attach.contains("usePitch") && attach["usePitch"].is_boolean() ? attach["usePitch"].get<bool>() : false;
				bool useRoll = attach.contains("useRoll") && attach["useRoll"].is_boolean() ? attach["useRoll"].get<bool>() : false;
				std::string attachmentName;
				if (attach.contains("attachment") && attach["attachment"].is_string()) {
					attachmentName = attach["attachment"].get<std::string>();
				}
				g_MirvImageDrawer.SetAttachment(name.c_str(), slot, useYaw, usePitch, useRoll, attachmentName.c_str());
			}
		}

		if (args.contains("pos") && args["pos"].is_array() && args["pos"].size() == 3) {
			g_MirvImageDrawer.SetPosition(name.c_str(),
				args["pos"][0].get<double>(),
				args["pos"][1].get<double>(),
				args["pos"][2].get<double>());
		}
		if (args.contains("ang") && args["ang"].is_array() && args["ang"].size() == 3) {
			g_MirvImageDrawer.SetAngles(name.c_str(),
				args["ang"][0].get<double>(),
				args["ang"][1].get<double>(),
				args["ang"][2].get<double>());
		}
		if (args.contains("scale") && args["scale"].is_array() && args["scale"].size() == 2) {
			g_MirvImageDrawer.SetScale(name.c_str(),
				args["scale"][0].get<double>(),
				args["scale"][1].get<double>());
		}
		if (args.contains("visible") && args["visible"].is_boolean()) {
			g_MirvImageDrawer.SetVisible(name.c_str(), args["visible"].get<bool>());
		}
		if (args.contains("depthTest") && args["depthTest"].is_boolean()) {
			g_MirvImageDrawer.SetDepthTest(name.c_str(), args["depthTest"].get<bool>());
		}
		if (args.contains("depthWrite") && args["depthWrite"].is_boolean()) {
			g_MirvImageDrawer.SetDepthWrite(name.c_str(), args["depthWrite"].get<bool>());
		}

		respond(MakeCommandResult("gfx.instance.create", true, "Instance created"));
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("gfx.instance.update", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!args.contains("name") || !args["name"].is_string()) {
			respond(MakeCommandResult("gfx.instance.update", false, "Missing or invalid name"));
			return;
		}
		const std::string name = args["name"].get<std::string>();

		if (args.contains("imageFile") && args["imageFile"].is_string()) {
			g_MirvImageDrawer.LoadImage(name.c_str(), args["imageFile"].get<std::string>().c_str());
		}
		else if (args.contains("atlas") && args["atlas"].is_string() && args.contains("region") && args["region"].is_string()) {
			g_MirvImageDrawer.UseAtlasRegion(
				name.c_str(),
				args["atlas"].get<std::string>().c_str(),
				args["region"].get<std::string>().c_str()
			);
		}
		if (args.contains("attach") && args["attach"].is_object()) {
			const auto& attach = args["attach"];
			if (attach.contains("slot") && attach["slot"].is_number_integer()) {
				int slot = attach["slot"].get<int>();
				if (slot < -1 || slot > 9) {
					respond(MakeCommandResult("gfx.instance.update", false, "attach.slot must be -1 or 0-9"));
					return;
				}
				bool useYaw = attach.contains("useYaw") && attach["useYaw"].is_boolean() ? attach["useYaw"].get<bool>() : false;
				bool usePitch = attach.contains("usePitch") && attach["usePitch"].is_boolean() ? attach["usePitch"].get<bool>() : false;
				bool useRoll = attach.contains("useRoll") && attach["useRoll"].is_boolean() ? attach["useRoll"].get<bool>() : false;
				std::string attachmentName;
				if (attach.contains("attachment") && attach["attachment"].is_string()) {
					attachmentName = attach["attachment"].get<std::string>();
				}
				g_MirvImageDrawer.SetAttachment(name.c_str(), slot, useYaw, usePitch, useRoll, attachmentName.c_str());
			}
		}
		if (args.contains("pos") && args["pos"].is_array() && args["pos"].size() == 3) {
			g_MirvImageDrawer.SetPosition(name.c_str(),
				args["pos"][0].get<double>(),
				args["pos"][1].get<double>(),
				args["pos"][2].get<double>());
		}
		if (args.contains("ang") && args["ang"].is_array() && args["ang"].size() == 3) {
			g_MirvImageDrawer.SetAngles(name.c_str(),
				args["ang"][0].get<double>(),
				args["ang"][1].get<double>(),
				args["ang"][2].get<double>());
		}
		if (args.contains("scale") && args["scale"].is_array() && args["scale"].size() == 2) {
			g_MirvImageDrawer.SetScale(name.c_str(),
				args["scale"][0].get<double>(),
				args["scale"][1].get<double>());
		}
		if (args.contains("visible") && args["visible"].is_boolean()) {
			g_MirvImageDrawer.SetVisible(name.c_str(), args["visible"].get<bool>());
		}
		if (args.contains("depthTest") && args["depthTest"].is_boolean()) {
			g_MirvImageDrawer.SetDepthTest(name.c_str(), args["depthTest"].get<bool>());
		}
		if (args.contains("depthWrite") && args["depthWrite"].is_boolean()) {
			g_MirvImageDrawer.SetDepthWrite(name.c_str(), args["depthWrite"].get<bool>());
		}

		respond(MakeCommandResult("gfx.instance.update", true, "Instance updated"));
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("gfx.instance.destroy", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!args.contains("name") || !args["name"].is_string()) {
			respond(MakeCommandResult("gfx.instance.destroy", false, "Missing or invalid name"));
			return;
		}
		g_MirvImageDrawer.UnloadImage(args["name"].get<std::string>().c_str());
		respond(MakeCommandResult("gfx.instance.destroy", true, "Instance destroyed"));
	});

	g_ObsWebSocketProtocol.RegisterCommandHandler("gfx.instance.list", [](const json& /*args*/, const CObsWebSocketProtocol::JsonResponder& respond) {
		std::vector<CMirvImageDrawer::ImageSnapshot> images;
		g_MirvImageDrawer.GetImageSnapshot(images);

		json list = json::array();
		for (const auto& img : images) {
			list.push_back({
				{"name", img.name},
				{"pos", { img.position.X, img.position.Y, img.position.Z }},
				{"ang", { img.pitch, img.yaw, img.roll }},
				{"scale", { img.scaleX, img.scaleY }},
				{"visible", img.visible},
				{"depthTest", img.depthTest},
				{"depthWrite", img.depthWrite},
				{"useAtlas", img.useAtlas},
				{"atlas", img.atlasName},
				{"region", img.regionId},
				{"attach", {
					{"slot", img.attachSlot},
					{"attachment", img.attachAttachmentName},
					{"useYaw", img.attachUseYaw},
					{"usePitch", img.attachUsePitch},
					{"useRoll", img.attachUseRoll}
				}}
			});
		}

		json result{
			{"type", "gfx.instance.list"},
			{"ok", true},
			{"instances", list}
		};
		respond(result);
	});

	g_ObsWebSocketProtocol.SetExecCommandHandler([](const std::string& cmd, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (cmd.empty()) {
			respond(MakeExecCmdResult(cmd, false, "Command string is empty"));
			return;
		}

		if (!g_pEngineToClient) {
			respond(MakeExecCmdResult(cmd, false, "Engine not ready for commands"));
			return;
		}

		ObsWebSocket_QueueExecCommand(cmd);

		json output = json::array();
		output.push_back(cmd + " executed");
		respond(MakeExecCmdResult(cmd, true, "Command executed", output));
	});

	g_ObsWebSocketProtocol.SetCampathPlayHandler([](const std::string& cmd, double offset, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (cmd.empty()) {
			respond(MakeCampathPlayResult(cmd, false, "Campath path is empty"));
			return;
		}

		if (!g_pEngineToClient) {
			respond(MakeCampathPlayResult(cmd, false, "Engine not ready for campath playback"));
			return;
		}

		ObsWebSocket_QueueCampathPlay(cmd, offset);
		respond(MakeCampathPlayResult(cmd, true, "Campath playback started"));
	});
}

void HandleObsWebSocketCommand(const std::string& jsonMessage, const CObsWebSocketProtocol::ResponseSender& respond) {
	g_ObsWebSocketProtocol.HandleMessage(jsonMessage, respond);
}
