#include "ObsWebSocketHandlers.h"

#include "ObsSpectatorBindings.h"
#include "ObsWebSocketActions.h"

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../shared/AfxConsole.h"

#include <sstream>

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

		return updated;
	}
}

void RegisterObsWebSocketHandlers() {
	static bool initialized = false;
	if (initialized) return;
	initialized = true;

	g_ObsWebSocketProtocol.RegisterCommandHandler("freecam_enable", [](const json& /*args*/, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!g_pFreecam) {
			respond(MakeCommandResult("freecam_enable", false, "Freecam controller not ready"));
			return;
		}

		ObsWebSocket_QueueFreecamEnable();
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

	g_ObsWebSocketProtocol.RegisterCommandHandler("freecam_hold", [](const json& /*args*/, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!g_pFreecam) {
			respond(MakeCommandResult("freecam_hold", false, "Freecam controller not ready"));
			return;
		}
		if (!g_pFreecam->IsEnabled()) {
			respond(MakeCommandResult("freecam_hold", false, "Freecam is not enabled"));
			return;
		}

		ObsWebSocket_QueueFreecamHold();
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

		payload.hasSmoothTransform = args.contains("smoothPosX") && args.contains("smoothPosY") && args.contains("smoothPosZ")
			&& args.contains("smoothPitch") && args.contains("smoothYaw") && args.contains("smoothRoll");
		if (payload.hasSmoothTransform) {
			payload.smoothTransform.x = args["smoothPosX"].get<float>();
			payload.smoothTransform.y = args["smoothPosY"].get<float>();
			payload.smoothTransform.z = args["smoothPosZ"].get<float>();
			payload.smoothTransform.pitch = args["smoothPitch"].get<float>();
			payload.smoothTransform.yaw = args["smoothYaw"].get<float>();
			payload.smoothTransform.roll = args["smoothRoll"].get<float>();
			if (args.contains("smoothFov")) {
				payload.hasSmoothFov = true;
				payload.smoothFov = args["smoothFov"].get<float>();
			}
		}

		if (args.contains("speedScalar")) {
			payload.hasSpeedScalar = true;
			payload.speedScalar = args["speedScalar"].get<float>();
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

	g_ObsWebSocketProtocol.RegisterCommandHandler("spectator_bindings_mode", [](const json& args, const CObsWebSocketProtocol::JsonResponder& respond) {
		if (!args.contains("useAlt") || !args["useAlt"].is_boolean()) {
			respond(MakeCommandResult("spectator_bindings_mode", false, "Missing or invalid useAlt flag"));
			return;
		}

		bool useAlt = args["useAlt"].get<bool>();
		ObsWebSocket_QueueSetAltSpectatorBindings(useAlt);
		respond(MakeCommandResult("spectator_bindings_mode", true, useAlt ? "Alt spectator bindings enabled" : "Alt spectator bindings disabled"));
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
