#include "stdafx.h"

#include "Cs2GameRecording.h"

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../deps/release/prop/AfxHookSource/SourceInterfaces.h"

#include "ClientEntitySystem.h"
#include "SchemaSystem.h"
#include "WrpConsole.h"

#include "../shared/AfxConsole.h"
#include "../shared/AfxGameRecord.h"
#include "../shared/StringTools.h"

#define _USE_MATH_DEFINES
#include <math.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

static CEntityInstance* GetPawnFromControllerIndex(int controllerIndex) {
	if (!g_pEntityList || !g_GetEntityFromIndex) return nullptr;

	auto controller = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, controllerIndex);
	if (!controller || !controller->IsPlayerController()) return nullptr;

	auto pawnHandle = controller->GetPlayerPawnHandle();
	if (!pawnHandle.IsValid()) return nullptr;

	int pawnIndex = pawnHandle.GetEntryIndex();
	if (pawnIndex < 0) return nullptr;

	return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, pawnIndex);
}

static bool GetCurrentSpectatedControllerIndex(int& outControllerIndex) {
	if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex || !g_ClientDll_GetSplitScreenPlayer)
		return false;

	CEntityInstance* localController = g_ClientDll_GetSplitScreenPlayer(0);
	if (!localController) return false;

	auto localPawnHandle = localController->GetPlayerPawnHandle();
	if (!localPawnHandle.IsValid()) return false;

	CEntityInstance* localPawn = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, localPawnHandle.GetEntryIndex());
	if (!localPawn) return false;

	CEntityInstance* targetPawn = nullptr;
	if (localPawn->GetObserverMode() > 0) {
		auto observerTargetHandle = localPawn->GetObserverTarget();
		if (observerTargetHandle.IsValid()) {
			targetPawn = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, observerTargetHandle.GetEntryIndex());
		}
	}
	if (!targetPawn) {
		targetPawn = localPawn;
	}

	auto controllerHandle = targetPawn->GetPlayerControllerHandle();
	if (!controllerHandle.IsValid()) return false;

	outControllerIndex = controllerHandle.GetEntryIndex();
	return true;
}

static constexpr std::ptrdiff_t kCSkeletonInstanceModelStateOffset = 0x150;
static constexpr std::ptrdiff_t kCModelStateModelHandleOffset = 0xA0;
static constexpr std::ptrdiff_t kCModelStateModelNameOffset = 0xA8;
static constexpr std::ptrdiff_t kModelBoneNamesArrayOffset = 0x168;
static constexpr std::ptrdiff_t kModelBoneCountOffset = 0x178;
static constexpr std::ptrdiff_t kModelBoneParentArrayOffset = 0x180;
static constexpr std::ptrdiff_t kModelBoneFlagsArrayOffset = 0x1B0;
static constexpr uint32_t kMaxReasonableBoneCount = 4096;
static constexpr uint32_t kCs2KnownBoneFlagsMask =
	0x00000004u // BONE_FLEX_DRIVER
	| 0x00000008u // CLOTH
	| 0x00000010u // PHYSICS
	| 0x00000020u // ATTACHMENT
	| 0x00000040u // ANIMATION
	| 0x00000080u // MESH
	| 0x00000100u // HITBOX
	| 0x00000200u // RETARGET_SRC
	| 0x00000400u // BONE_USED_BY_VERTEX_LOD0
	| 0x00000800u
	| 0x00001000u
	| 0x00002000u
	| 0x00004000u
	| 0x00008000u
	| 0x00010000u
	| 0x00020000u // BONE_USED_BY_VERTEX_LOD7
	| 0x00040000u // BONE_MERGE_READ
	| 0x00080000u // BONE_MERGE_WRITE
	| 0x00100000u // BLEND_PREALIGNED
	| 0x00200000u // RIGID_LENGTH
	| 0x00400000u; // PROCEDURAL
static constexpr uint32_t kCs2BoneUsedByAnythingMask =
	0x00000020u // ATTACHMENT
	| 0x00000040u // ANIMATION
	| 0x00000080u // MESH
	| 0x00000100u // HITBOX
	| 0x00000400u
	| 0x00000800u
	| 0x00001000u
	| 0x00002000u
	| 0x00004000u
	| 0x00008000u
	| 0x00010000u
	| 0x00020000u
	| 0x00040000u
	| 0x00080000u;

static bool IsLikelyPrintableAscii(const char* text, size_t maxLen);

static bool TryGetEntityModelInfo(
	CEntityInstance* entity,
	unsigned char*& outSceneNode,
	unsigned char*& outModelState,
	unsigned char***& outModelHandle,
	unsigned char*& outModelImp,
	const char*& outModelName,
	unsigned char*& outBoneNamesArray,
	int16_t*& outBoneParentArray,
	uint32_t& outBoneCount) {
	outSceneNode = nullptr;
	outModelState = nullptr;
	outModelHandle = nullptr;
	outModelImp = nullptr;
	outModelName = nullptr;
	outBoneNamesArray = nullptr;
	outBoneParentArray = nullptr;
	outBoneCount = 0;
	if (!entity || !g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode) return false;

	outSceneNode = *(unsigned char**)((unsigned char*)entity + g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode);
	if (!outSceneNode) return false;

	outModelState = outSceneNode + kCSkeletonInstanceModelStateOffset;
	outModelHandle = (unsigned char***)(outModelState + kCModelStateModelHandleOffset);
	if (!outModelHandle || !*outModelHandle || !**outModelHandle) return false;

	outModelImp = **outModelHandle;
	outBoneCount = *(uint32_t*)(outModelImp + kModelBoneCountOffset);
	if (outBoneCount == 0 || outBoneCount > kMaxReasonableBoneCount) return false;

	outModelName = *(const char**)(outModelState + kCModelStateModelNameOffset);
	outBoneNamesArray = *(unsigned char**)(outModelImp + kModelBoneNamesArrayOffset);
	outBoneParentArray = *(int16_t**)(outModelImp + kModelBoneParentArrayOffset);
	return outBoneNamesArray != nullptr;
}

static bool TryGetEntityModelBaseInfo(CEntityInstance* entity, unsigned char*& outSceneNode, const char*& outModelName) {
	outSceneNode = nullptr;
	outModelName = nullptr;
	if (!entity || !g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode) return false;

	outSceneNode = *(unsigned char**)((unsigned char*)entity + g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode);
	if (!outSceneNode) return false;

	unsigned char* modelState = outSceneNode + kCSkeletonInstanceModelStateOffset;
	outModelName = *(const char**)(modelState + kCModelStateModelNameOffset);
	return outModelName && IsLikelyPrintableAscii(outModelName, 256);
}

static bool IsLikelyPrintableAscii(const char* text, size_t maxLen) {
	if (!text || IsBadReadPtr(text, 1)) return false;
	size_t len = 0;
	for (; len < maxLen; ++len) {
		if (IsBadReadPtr(text + len, 1)) return false;
		unsigned char c = (unsigned char)text[len];
		if (c == '\0') break;
		if (c < 0x20 || c > 0x7e) return false;
	}
	return len > 0 && len < maxLen;
}

static bool IsValidBoneParentArray(int16_t* boneParentArray, uint32_t boneCount) {
	if (!boneParentArray || boneCount == 0 || boneCount > kMaxReasonableBoneCount) return false;
	if (IsBadReadPtr(boneParentArray, sizeof(int16_t) * boneCount)) return false;
	if (boneParentArray[0] != -1) return false;

	int rootCount = 0;
	for (uint32_t i = 0; i < boneCount; ++i) {
		const int parent = (int)boneParentArray[i];
		if (parent < -1 || parent >= (int)boneCount) return false;
		if (parent != -1 && parent >= (int)i) return false;
		if (parent == -1) ++rootCount;
	}

	return rootCount >= 1 && rootCount <= (int)(boneCount / 4) + 1;
}

static bool IsHudModelEntity(CEntityInstance* entity) {
	if (!entity) return false;
	const char* clientClassName = entity->GetClientClassName();
	if (!clientClassName) return false;
	return 0 == _stricmp(clientClassName, "C_CS2HudModelArms")
		|| 0 == _stricmp(clientClassName, "C_CS2HudModelWeapon")
		|| 0 == _stricmp(clientClassName, "C_CS2HudModelAddon");
}

static bool StringBeginsWithCaseSensitive(const char* value, const char* prefix) {
	if (!value || !prefix) return false;
	const size_t prefixLen = strlen(prefix);
	return 0 == strncmp(value, prefix, prefixLen);
}

static bool StringEndsWithCaseSensitive(const char* value, const char* suffix) {
	if (!value || !suffix) return false;
	const size_t valueLen = strlen(value);
	const size_t suffixLen = strlen(suffix);
	if (valueLen < suffixLen) return false;
	return 0 == strcmp(value + valueLen - suffixLen, suffix);
}

static void CollectHudModelOwnersFromSceneNode(
	unsigned char* node,
	int depth,
	int maxDepth,
	int& visitedCount,
	std::vector<unsigned char*>& visited,
	std::vector<CEntityInstance*>& outOwners) {
	if (!node || depth > maxDepth || visitedCount >= 256) return;
	if (std::find(visited.begin(), visited.end(), node) != visited.end()) return;
	visited.push_back(node);
	++visitedCount;

	if (g_clientDllOffsets.CGameSceneNode.m_pOwner) {
		CEntityInstance* owner = *(CEntityInstance**)(node + g_clientDllOffsets.CGameSceneNode.m_pOwner);
		if (IsHudModelEntity(owner) && std::find(outOwners.begin(), outOwners.end(), owner) == outOwners.end()) {
			outOwners.push_back(owner);
		}
	}

	if (depth >= maxDepth || !g_clientDllOffsets.CGameSceneNode.m_pChild || !g_clientDllOffsets.CGameSceneNode.m_pNextSibling) return;

	unsigned char* child = *(unsigned char**)(node + g_clientDllOffsets.CGameSceneNode.m_pChild);
	for (unsigned char* cur = child; cur && visitedCount < 256; cur = *(unsigned char**)(cur + g_clientDllOffsets.CGameSceneNode.m_pNextSibling)) {
		CollectHudModelOwnersFromSceneNode(cur, depth + 1, maxDepth, visitedCount, visited, outOwners);
	}
}

static void CollectHudModelOwnersForPawn(CEntityInstance* pawn, std::vector<CEntityInstance*>& outOwners) {
	if (!pawn || !g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode || !g_clientDllOffsets.CGameSceneNode.m_pChild || !g_clientDllOffsets.CGameSceneNode.m_pNextSibling) return;
	unsigned char* root = *(unsigned char**)((unsigned char*)pawn + g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode);
	if (!root) return;

	int visitedCount = 0;
	std::vector<unsigned char*> visited;
	CollectHudModelOwnersFromSceneNode(root, 0, 8, visitedCount, visited, outOwners);
}

static bool IsLikelyBoneFlagsArray(uint32_t* flagsArray, uint32_t boneCount, int& outKnownCount, int& outUsedCount, int& outZeroCount, uint32_t& outUnknownMask) {
	outKnownCount = 0;
	outUsedCount = 0;
	outZeroCount = 0;
	outUnknownMask = 0;
	if (!flagsArray || boneCount == 0 || boneCount > kMaxReasonableBoneCount) return false;
	if (IsBadReadPtr(flagsArray, sizeof(uint32_t) * boneCount)) return false;

	int veryLargeCount = 0;
	for (uint32_t i = 0; i < boneCount; ++i) {
		const uint32_t flags = flagsArray[i];
		const uint32_t unknown = flags & ~kCs2KnownBoneFlagsMask;
		outUnknownMask |= unknown;
		if (flags == 0) ++outZeroCount;
		if (unknown == 0) ++outKnownCount;
		if ((flags & kCs2BoneUsedByAnythingMask) != 0) ++outUsedCount;
		if (flags > 0x01000000u) ++veryLargeCount;
	}

	return outKnownCount == (int)boneCount
		&& outUsedCount > 0
		&& outZeroCount < (int)boneCount
		&& veryLargeCount == 0;
}

static void MatrixIdentity(SOURCESDK::matrix3x4_t& out) {
	out[0][0] = 1.0f; out[0][1] = 0.0f; out[0][2] = 0.0f; out[0][3] = 0.0f;
	out[1][0] = 0.0f; out[1][1] = 1.0f; out[1][2] = 0.0f; out[1][3] = 0.0f;
	out[2][0] = 0.0f; out[2][1] = 0.0f; out[2][2] = 1.0f; out[2][3] = 0.0f;
}

static void MatrixFromQuaternionPosition(const SOURCESDK::Quaternion& qIn, const SOURCESDK::Vector& pos, SOURCESDK::matrix3x4_t& out) {
	double x = qIn.x;
	double y = qIn.y;
	double z = qIn.z;
	double w = qIn.w;
	double norm = sqrt(x * x + y * y + z * z + w * w);
	if (norm > 0.0) {
		x /= norm;
		y /= norm;
		z /= norm;
		w /= norm;
	}

	const double xx = x * x;
	const double yy = y * y;
	const double zz = z * z;
	const double xy = x * y;
	const double xz = x * z;
	const double yz = y * z;
	const double wx = w * x;
	const double wy = w * y;
	const double wz = w * z;

	out[0][0] = (float)(1.0 - 2.0 * (yy + zz));
	out[0][1] = (float)(2.0 * (xy - wz));
	out[0][2] = (float)(2.0 * (xz + wy));
	out[0][3] = pos.x;
	out[1][0] = (float)(2.0 * (xy + wz));
	out[1][1] = (float)(1.0 - 2.0 * (xx + zz));
	out[1][2] = (float)(2.0 * (yz - wx));
	out[1][3] = pos.y;
	out[2][0] = (float)(2.0 * (xz - wy));
	out[2][1] = (float)(2.0 * (yz + wx));
	out[2][2] = (float)(1.0 - 2.0 * (xx + yy));
	out[2][3] = pos.z;
}

static void MatrixConcat(const SOURCESDK::matrix3x4_t& a, const SOURCESDK::matrix3x4_t& b, SOURCESDK::matrix3x4_t& out) {
	SOURCESDK::matrix3x4_t tmp;
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
		}
		tmp[i][3] = a[i][0] * b[0][3] + a[i][1] * b[1][3] + a[i][2] * b[2][3] + a[i][3];
	}
	out = tmp;
}

static void MatrixInvertRigid(const SOURCESDK::matrix3x4_t& in, SOURCESDK::matrix3x4_t& out) {
	out[0][0] = in[0][0];
	out[0][1] = in[1][0];
	out[0][2] = in[2][0];
	out[1][0] = in[0][1];
	out[1][1] = in[1][1];
	out[1][2] = in[2][1];
	out[2][0] = in[0][2];
	out[2][1] = in[1][2];
	out[2][2] = in[2][2];

	out[0][3] = -(out[0][0] * in[0][3] + out[0][1] * in[1][3] + out[0][2] * in[2][3]);
	out[1][3] = -(out[1][0] * in[0][3] + out[1][1] * in[1][3] + out[1][2] * in[2][3]);
	out[2][3] = -(out[2][0] * in[0][3] + out[2][1] * in[1][3] + out[2][2] * in[2][3]);
}

static bool TryGetEntityAbsTransform(CEntityInstance* entity, SOURCESDK::matrix3x4_t& out) {
	MatrixIdentity(out);
	if (!entity || !g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode) return false;

	unsigned char* sceneNode = *(unsigned char**)((unsigned char*)entity + g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode);
	if (!sceneNode) return false;

	SOURCESDK::Vector origin;
	origin.x = origin.y = origin.z = 0.0f;
	if (g_clientDllOffsets.CGameSceneNode.m_vecAbsOrigin && !IsBadReadPtr(sceneNode + g_clientDllOffsets.CGameSceneNode.m_vecAbsOrigin, sizeof(float) * 3)) {
		float* rawOrigin = (float*)(sceneNode + g_clientDllOffsets.CGameSceneNode.m_vecAbsOrigin);
		origin.x = rawOrigin[0];
		origin.y = rawOrigin[1];
		origin.z = rawOrigin[2];
	}

	if (g_clientDllOffsets.CGameSceneNode.m_angAbsRotation && !IsBadReadPtr(sceneNode + g_clientDllOffsets.CGameSceneNode.m_angAbsRotation, sizeof(float) * 3)) {
		float* rawAngles = (float*)(sceneNode + g_clientDllOffsets.CGameSceneNode.m_angAbsRotation);
		SOURCESDK::QAngle angles(rawAngles[0], rawAngles[1], rawAngles[2]);
		SOURCESDK::AngleMatrix(angles, origin, out);
	} else {
		out[0][3] = origin.x;
		out[1][3] = origin.y;
		out[2][3] = origin.z;
	}

	return true;
}

static bool ReadBoolField(unsigned char* base, ptrdiff_t offset, bool& outValue) {
	if (!base || offset <= 0 || IsBadReadPtr(base + offset, sizeof(bool))) return false;
	outValue = *(bool*)(base + offset);
	return true;
}

static bool TryGetEntityRenderEnabled(CEntityInstance* entity, bool& outRenderEnabled) {
	outRenderEnabled = true;
	if (!entity || g_clientDllOffsets.C_BaseModelEntity.m_CRenderComponent <= 0) return false;
	if (g_clientDllOffsets.CRenderComponent.m_bEnableRendering <= 0) return false;
	if (IsBadReadPtr((unsigned char*)entity + g_clientDllOffsets.C_BaseModelEntity.m_CRenderComponent, sizeof(void*))) return false;

	void* renderComponent = *(void**)((unsigned char*)entity + g_clientDllOffsets.C_BaseModelEntity.m_CRenderComponent);
	return ReadBoolField((unsigned char*)renderComponent, g_clientDllOffsets.CRenderComponent.m_bEnableRendering, outRenderEnabled);
}

class Cs2AgrRecorder {
public:
	bool Start(const wchar_t* fileName) {
		Stop();
		m_NextId = 1;
		m_EntityIds.clear();
		m_VisibleLastFrame.clear();
		return m_Record.StartRecording(fileName, 6);
	}

	void Stop() {
		m_Record.EndRecording();
		m_EntityIds.clear();
		m_VisibleLastFrame.clear();
		m_FpsAccumulator = 0.0;
		m_HasWrittenFrame = false;
		m_HasPendingSetupView = false;
	}

	bool IsRecording() {
		return m_Record.GetRecording();
	}

	bool GetOverrideFps() const {
		return m_OverrideFps;
	}

	float GetOverrideFpsValue() const {
		return m_OverrideFpsValue;
	}

	bool GetRecordCamera() const {
		return m_RecordCamera;
	}

	void SetRecordCamera(bool value) {
		m_RecordCamera = value;
	}

	bool GetRecordPlayers() const {
		return m_RecordPlayers;
	}

	void SetRecordPlayers(bool value) {
		m_RecordPlayers = value;
	}

	bool GetRecordWeapons() const {
		return m_RecordWeapons;
	}

	void SetRecordWeapons(bool value) {
		m_RecordWeapons = value;
	}

	bool GetRecordViewModel() const {
		return m_RecordViewModel;
	}

	void SetRecordViewModel(bool value) {
		m_RecordViewModel = value;
	}

	bool GetRecordProjectiles() const {
		return m_RecordProjectiles;
	}

	void SetRecordProjectiles(bool value) {
		m_RecordProjectiles = value;
	}

	void SetOverrideFps(bool value) {
		m_OverrideFps = value;
		m_FpsAccumulator = 0.0;
		m_HasWrittenFrame = false;
	}

	void SetOverrideFpsValue(float value) {
		m_OverrideFpsValue = value;
		m_FpsAccumulator = 0.0;
		m_HasWrittenFrame = false;
	}

	void OnSetupView(float frameTime, float x, float y, float z, float rx, float ry, float rz, float fov) {
		if (!m_Record.GetRecording()) return;

		m_PendingFrameTime = frameTime;
		m_PendingCameraX = x;
		m_PendingCameraY = y;
		m_PendingCameraZ = z;
		m_PendingCameraRx = rx;
		m_PendingCameraRy = ry;
		m_PendingCameraRz = rz;
		m_PendingCameraFov = fov;
		m_HasPendingSetupView = true;
	}

	void OnMainRenderFrame() {
		if (!m_Record.GetRecording()) return;
		if (!m_HasPendingSetupView) return;
		m_HasPendingSetupView = false;

		int controllerIndex = -1;
		if (!GetCurrentSpectatedControllerIndex(controllerIndex)) return;

		CEntityInstance* pawn = GetPawnFromControllerIndex(controllerIndex);
		if (!pawn) return;

		const float recordFrameTime = GetRecordFrameTime(m_PendingFrameTime);
		if (recordFrameTime <= 0.0f) return;

		std::set<int> visibleThisFrame;
		m_Record.BeginFrame(recordFrameTime);

		if (m_RecordPlayers || m_RecordWeapons || m_RecordProjectiles) {
			int highestIndex = GetHighestEntityIndex();
			for (int i = 0; i <= highestIndex; ++i) {
				CEntityInstance* entity = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, i);
				if (!entity) continue;

				const char* debugName = entity->GetDebugName();
				if (m_RecordPlayers && IsPlayerPawnForAgr(entity)) {
					RecordEntity(entity, false, visibleThisFrame);
				} else if (m_RecordWeapons && debugName && StringBeginsWithCaseSensitive(debugName, "weapon_")) {
					RecordEntity(entity, false, visibleThisFrame);
				} else if (m_RecordProjectiles && debugName && StringEndsWithCaseSensitive(debugName, "_projectile")) {
					RecordEntity(entity, false, visibleThisFrame);
				}
			}
		}

		if (m_RecordViewModel) {
			std::vector<CEntityInstance*> hudModels;
			CollectHudModelOwnersForPawn(pawn, hudModels);
			for (CEntityInstance* hudModel : hudModels) {
				RecordEntity(hudModel, true, visibleThisFrame);
			}
		}

		if (m_RecordCamera) {
			m_Record.WriteDictionary("afxCam");
			m_Record.Write(m_PendingCameraX);
			m_Record.Write(m_PendingCameraY);
			m_Record.Write(m_PendingCameraZ);
			m_Record.Write(m_PendingCameraRx);
			m_Record.Write(m_PendingCameraRy);
			m_Record.Write(m_PendingCameraRz);
			m_Record.Write(m_PendingCameraFov);
		}

		for (std::set<int>::iterator it = m_VisibleLastFrame.begin(); it != m_VisibleLastFrame.end(); ++it) {
			if (visibleThisFrame.find(*it) == visibleThisFrame.end()) {
				m_Record.MarkHidden(*it);
			}
		}
		m_VisibleLastFrame.swap(visibleThisFrame);

		m_Record.EndFrame();
	}

private:
	advancedfx::CAfxGameRecord m_Record;
	bool m_RecordPlayers = true;
	bool m_RecordWeapons = true;
	bool m_RecordViewModel = true;
	bool m_RecordProjectiles = true;
	bool m_RecordCamera = true;
	bool m_OverrideFps = false;
	float m_OverrideFpsValue = 60.0f;
	double m_FpsAccumulator = 0.0;
	bool m_HasWrittenFrame = false;
	bool m_HasPendingSetupView = false;
	float m_PendingFrameTime = 0.0f;
	float m_PendingCameraX = 0.0f;
	float m_PendingCameraY = 0.0f;
	float m_PendingCameraZ = 0.0f;
	float m_PendingCameraRx = 0.0f;
	float m_PendingCameraRy = 0.0f;
	float m_PendingCameraRz = 0.0f;
	float m_PendingCameraFov = 0.0f;
	int m_NextId = 1;
	std::map<CEntityInstance*, int> m_EntityIds;
	std::set<int> m_VisibleLastFrame;

	float GetRecordFrameTime(float sourceFrameTime) {
		if (!m_OverrideFps) return sourceFrameTime;
		if (m_OverrideFpsValue <= 0.0f) return 0.0f;

		const double interval = 1.0 / (double)m_OverrideFpsValue;
		if (!m_HasWrittenFrame) {
			m_HasWrittenFrame = true;
			m_FpsAccumulator = 0.0;
			return (float)interval;
		}

		if (sourceFrameTime > 0.0f) {
			m_FpsAccumulator += (double)sourceFrameTime;
		}

		if (m_FpsAccumulator + 0.000001 < interval) return 0.0f;

		m_FpsAccumulator -= interval;
		if (m_FpsAccumulator >= interval) {
			m_FpsAccumulator = fmod(m_FpsAccumulator, interval);
		}

		return (float)interval;
	}

	bool IsPlayerPawnForAgr(CEntityInstance* entity) {
		const char* clientClassName = entity ? entity->GetClientClassName() : nullptr;
		return clientClassName && 0 == _stricmp(clientClassName, "C_CSPlayerPawn");
	}

	int GetEntityId(CEntityInstance* entity) {
		std::map<CEntityInstance*, int>::iterator it = m_EntityIds.find(entity);
		if (it != m_EntityIds.end()) return it->second;

		int result = m_NextId++;
		m_EntityIds[entity] = result;
		return result;
	}

	void WriteMatrix3x4(const SOURCESDK::matrix3x4_t& value) {
		m_Record.Write(value[0][0]);
		m_Record.Write(value[0][1]);
		m_Record.Write(value[0][2]);
		m_Record.Write(value[0][3]);
		m_Record.Write(value[1][0]);
		m_Record.Write(value[1][1]);
		m_Record.Write(value[1][2]);
		m_Record.Write(value[1][3]);
		m_Record.Write(value[2][0]);
		m_Record.Write(value[2][1]);
		m_Record.Write(value[2][2]);
		m_Record.Write(value[2][3]);
	}

	bool RecordEntity(CEntityInstance* entity, bool viewModel, std::set<int>& visibleThisFrame) {
		unsigned char* baseSceneNode = nullptr;
		const char* baseModelName = nullptr;
		if (!TryGetEntityModelBaseInfo(entity, baseSceneNode, baseModelName)) return false;

		bool visible = true;
		TryGetEntityRenderEnabled(entity, visible);

		SOURCESDK::matrix3x4_t entityTransform;
		if (!TryGetEntityAbsTransform(entity, entityTransform)) {
			MatrixIdentity(entityTransform);
		}

		const int id = GetEntityId(entity);
		visibleThisFrame.insert(id);

		m_Record.WriteDictionary("entity_state");
		m_Record.Write(id);

		m_Record.WriteDictionary("baseentity");
		m_Record.WriteDictionary(baseModelName);
		m_Record.Write(visible);
		WriteMatrix3x4(entityTransform);

		unsigned char* sceneNode = nullptr;
		unsigned char* modelState = nullptr;
		unsigned char*** modelHandle = nullptr;
		unsigned char* modelImp = nullptr;
		const char* modelName = nullptr;
		unsigned char* boneNamesArray = nullptr;
		int16_t* boneParentArray = nullptr;
		uint32_t boneCount = 0;
		bool hasBones = TryGetEntityModelInfo(entity, sceneNode, modelState, modelHandle, modelImp, modelName, boneNamesArray, boneParentArray, boneCount)
			&& modelName
			&& IsLikelyPrintableAscii(modelName, 256)
			&& IsValidBoneParentArray(boneParentArray, boneCount);

		uint32_t* boneFlagsArray = nullptr;
		bool hasBoneFlags = false;
		if (hasBones && modelImp && !IsBadReadPtr(modelImp + kModelBoneFlagsArrayOffset, sizeof(uint32_t*))) {
			boneFlagsArray = *(uint32_t**)(modelImp + kModelBoneFlagsArrayOffset);
			int knownCount = 0;
			int usedCount = 0;
			int zeroCount = 0;
			uint32_t unknownMask = 0;
			hasBoneFlags = IsLikelyBoneFlagsArray(boneFlagsArray, boneCount, knownCount, usedCount, zeroCount, unknownMask);
		}

		std::vector<SOURCESDK::matrix3x4_t> boneWorld;
		if (hasBones) {
			boneWorld.resize(boneCount);
			for (uint32_t i = 0; i < boneCount; ++i) {
				SOURCESDK::Vector origin;
				SOURCESDK::Quaternion angles;
				if (!entity->GetBone((int)i, origin, angles)) {
					hasBones = false;
					boneWorld.clear();
					break;
				}
				MatrixFromQuaternionPosition(angles, origin, boneWorld[i]);
			}
		}

		m_Record.WriteDictionary("baseanimating");
		m_Record.Write(hasBones);
		if (!hasBones) {
			m_Record.WriteDictionary("/");
			m_Record.Write(viewModel);
			return true;
		}

		m_Record.Write((int)boneCount);

		for (uint32_t i = 0; i < boneCount; ++i) {
			SOURCESDK::matrix3x4_t parentInverse;
			SOURCESDK::matrix3x4_t localBone;
			const bool boneIsUsed = !hasBoneFlags || ((boneFlagsArray[i] & kCs2BoneUsedByAnythingMask) != 0);
			if (!boneIsUsed) {
				MatrixIdentity(localBone);
				WriteMatrix3x4(localBone);
				continue;
			}

			int parentIndex = (int)boneParentArray[i];
			if (parentIndex >= 0 && (uint32_t)parentIndex < boneCount) {
				MatrixInvertRigid(boneWorld[(uint32_t)parentIndex], parentInverse);
			} else {
				MatrixInvertRigid(entityTransform, parentInverse);
			}
			MatrixConcat(parentInverse, boneWorld[i], localBone);
			WriteMatrix3x4(localBone);
		}

		m_Record.WriteDictionary("/");
		m_Record.Write(viewModel);
		return true;
	}
};

static Cs2AgrRecorder g_Cs2AgrRecorder;

void Cs2Agr_OnSetupView(float frameTime, float x, float y, float z, float rx, float ry, float rz, float fov) {
	g_Cs2AgrRecorder.OnSetupView(frameTime, x, y, z, rx, ry, rz, fov);
}

void Cs2Agr_OnMainRenderFrame() {
	g_Cs2AgrRecorder.OnMainRenderFrame();
}

CON_COMMAND(mirv_agr, "Source 2 AGR recording") {
	int argc = args->ArgC();
	if (argc < 2) {
		advancedfx::Message(
			"mirv_agr start <fileName>\n"
			"mirv_agr stop\n"
			"mirv_agr fps default|<fValue>\n"
			"mirv_agr recordCamera 0|1\n"
			"mirv_agr recordPlayers 0|1\n"
			"mirv_agr recordWeapons 0|1\n"
			"mirv_agr recordViewmodel 0|1\n"
			"mirv_agr recordProjectiles 0|1\n"
			"mirv_agr status\n"
		);
		return;
	}

	const char* cmd = args->ArgV(1);
	auto handleBoolSetting = [&](const char* name, bool currentValue, void (Cs2AgrRecorder::*setter)(bool)) -> bool {
		if (0 != _stricmp(cmd, name)) return false;
		if (argc >= 3) {
			int value = atoi(args->ArgV(2));
			(g_Cs2AgrRecorder.*setter)(value != 0);
			return true;
		}

		advancedfx::Message("mirv_agr %s 0|1\nCurrent value: %d\n", name, currentValue ? 1 : 0);
		return true;
	};

	if (0 == _stricmp(cmd, "start")) {
		if (argc < 3) {
			advancedfx::Warning("mirv_agr: start requires a file name.\n");
			return;
		}

		std::wstring wideFilePath;
		if (!UTF8StringToWideString(args->ArgV(2), wideFilePath)) {
			advancedfx::Warning("mirv_agr: failed to convert file name to wide string.\n");
			return;
		}

		if (g_Cs2AgrRecorder.Start(wideFilePath.c_str())) {
			advancedfx::Message("mirv_agr: started recording \"%s\".\n", args->ArgV(2));
		} else {
			advancedfx::Warning("mirv_agr: failed to open \"%s\" for writing.\n", args->ArgV(2));
		}
		return;
	}

	if (0 == _stricmp(cmd, "stop")) {
		g_Cs2AgrRecorder.Stop();
		advancedfx::Message("mirv_agr: stopped.\n");
		return;
	}

	if (handleBoolSetting("recordCamera", g_Cs2AgrRecorder.GetRecordCamera(), &Cs2AgrRecorder::SetRecordCamera)) return;
	if (handleBoolSetting("recordPlayers", g_Cs2AgrRecorder.GetRecordPlayers(), &Cs2AgrRecorder::SetRecordPlayers)) return;
	if (handleBoolSetting("recordWeapons", g_Cs2AgrRecorder.GetRecordWeapons(), &Cs2AgrRecorder::SetRecordWeapons)) return;
	if (handleBoolSetting("recordViewmodel", g_Cs2AgrRecorder.GetRecordViewModel(), &Cs2AgrRecorder::SetRecordViewModel)) return;
	if (handleBoolSetting("recordViewModel", g_Cs2AgrRecorder.GetRecordViewModel(), &Cs2AgrRecorder::SetRecordViewModel)) return;
	if (handleBoolSetting("recordProjectiles", g_Cs2AgrRecorder.GetRecordProjectiles(), &Cs2AgrRecorder::SetRecordProjectiles)) return;

	if (0 == _stricmp(cmd, "fps")) {
		if (argc >= 3) {
			const char* valueArg = args->ArgV(2);
			if (0 == _stricmp(valueArg, "default")) {
				g_Cs2AgrRecorder.SetOverrideFps(false);
				return;
			}

			char* endPtr = nullptr;
			double value = strtod(valueArg, &endPtr);
			if (endPtr && *endPtr == '\0' && value > 0.0 && value <= 1000.0) {
				g_Cs2AgrRecorder.SetOverrideFpsValue((float)value);
				g_Cs2AgrRecorder.SetOverrideFps(true);
				return;
			}

			advancedfx::Warning("mirv_agr: fps must be default or a value greater than 0 and at most 1000.\n");
			return;
		}

		advancedfx::Message("mirv_agr fps default|<fValue>\n");
		if (g_Cs2AgrRecorder.GetOverrideFps()) {
			advancedfx::Message("Current value: %f\n", g_Cs2AgrRecorder.GetOverrideFpsValue());
		} else {
			advancedfx::Message("Current value: default\n");
		}
		return;
	}

	if (0 == _stricmp(cmd, "status")) {
		if (g_Cs2AgrRecorder.GetOverrideFps()) {
			advancedfx::Message("mirv_agr: recording %d fps %f.\n", g_Cs2AgrRecorder.IsRecording() ? 1 : 0, g_Cs2AgrRecorder.GetOverrideFpsValue());
		} else {
			advancedfx::Message("mirv_agr: recording %d fps default.\n", g_Cs2AgrRecorder.IsRecording() ? 1 : 0);
		}
		advancedfx::Message(
			"mirv_agr: recordCamera %d recordPlayers %d recordWeapons %d recordViewmodel %d recordProjectiles %d.\n",
			g_Cs2AgrRecorder.GetRecordCamera() ? 1 : 0,
			g_Cs2AgrRecorder.GetRecordPlayers() ? 1 : 0,
			g_Cs2AgrRecorder.GetRecordWeapons() ? 1 : 0,
			g_Cs2AgrRecorder.GetRecordViewModel() ? 1 : 0,
			g_Cs2AgrRecorder.GetRecordProjectiles() ? 1 : 0);
		return;
	}

	advancedfx::Warning("mirv_agr: unknown command \"%s\".\n", cmd);
}

#ifdef _DEBUG

static bool TryParseLongArg(const char* text, long& outValue, int base = 10) {
	if (!text) return false;
	char* endPtr = nullptr;
	long value = strtol(text, &endPtr, base);
	if (!(endPtr && *endPtr == '\0')) return false;
	outValue = value;
	return true;
}

static const char* TryGetBoneNameFromArray(unsigned char* boneNamesArray, uint32_t boneIndex) {
	if (!boneNamesArray) return nullptr;
	unsigned char* entry = boneNamesArray + (size_t)boneIndex * sizeof(void*);
	if (IsBadReadPtr(entry, sizeof(const char*))) return nullptr;

	const char* name = *(const char**)entry;
	return IsLikelyPrintableAscii(name, 128) ? name : nullptr;
}

static bool IsInterestingModelString(const char* text) {
	if (!text) return false;
	return nullptr != strstr(text, ".vmdl")
		|| nullptr != strstr(text, "models/")
		|| nullptr != strstr(text, "characters/")
		|| nullptr != strstr(text, "weapons/")
		|| nullptr != strstr(text, "ctm_")
		|| nullptr != strstr(text, "tm_")
		|| nullptr != strstr(text, "hud")
		|| nullptr != strstr(text, "arms");
}

static void PrintSceneNodeTree(unsigned char* node, int depth, int maxDepth, int& visitedCount, std::vector<unsigned char*>& visited) {
	if (!node || depth > maxDepth || visitedCount >= 256) return;
	if (std::find(visited.begin(), visited.end(), node) != visited.end()) {
		advancedfx::Message("mirv_scenechildren: depth %d node %p already visited\n", depth, node);
		return;
	}
	visited.push_back(node);
	++visitedCount;

	CEntityInstance* owner = nullptr;
	if (g_clientDllOffsets.CGameSceneNode.m_pOwner) {
		owner = *(CEntityInstance**)(node + g_clientDllOffsets.CGameSceneNode.m_pOwner);
	}

	float* origin = nullptr;
	if (g_clientDllOffsets.CGameSceneNode.m_vecAbsOrigin) {
		origin = (float*)(node + g_clientDllOffsets.CGameSceneNode.m_vecAbsOrigin);
	}

	const char* debugName = owner ? owner->GetDebugName() : nullptr;
	const char* className = owner ? owner->GetClassName() : nullptr;
	const char* clientClassName = owner ? owner->GetClientClassName() : nullptr;
	if (origin) {
		advancedfx::Message(
			"mirv_scenechildren: depth %d node %p owner %p debug \"%s\" class \"%s\" client \"%s\" origin [%.2f %.2f %.2f]\n",
			depth, node, owner,
			debugName ? debugName : "",
			className ? className : "",
			clientClassName ? clientClassName : "",
			origin[0], origin[1], origin[2]);
	} else {
		advancedfx::Message(
			"mirv_scenechildren: depth %d node %p owner %p debug \"%s\" class \"%s\" client \"%s\"\n",
			depth, node, owner,
			debugName ? debugName : "",
			className ? className : "",
			clientClassName ? clientClassName : "");
	}

	if (depth >= maxDepth || !g_clientDllOffsets.CGameSceneNode.m_pChild || !g_clientDllOffsets.CGameSceneNode.m_pNextSibling) return;

	unsigned char* child = *(unsigned char**)(node + g_clientDllOffsets.CGameSceneNode.m_pChild);
	for (unsigned char* cur = child; cur && visitedCount < 256; cur = *(unsigned char**)(cur + g_clientDllOffsets.CGameSceneNode.m_pNextSibling)) {
		PrintSceneNodeTree(cur, depth + 1, maxDepth, visitedCount, visited);
	}
}

static void DumpModelInfoForEntity(CEntityInstance* entity, const char* label, size_t scanBytes) {
	unsigned char* sceneNode = nullptr;
	unsigned char* modelState = nullptr;
	unsigned char*** modelHandle = nullptr;
	unsigned char* modelImp = nullptr;
	const char* modelName = nullptr;
	unsigned char* boneNamesArray = nullptr;
	int16_t* boneParentArray = nullptr;
	uint32_t boneCount = 0;
	const bool ok = TryGetEntityModelInfo(entity, sceneNode, modelState, modelHandle, modelImp, modelName, boneNamesArray, boneParentArray, boneCount);

	const char* debugName = entity ? entity->GetDebugName() : nullptr;
	const char* className = entity ? entity->GetClassName() : nullptr;
	const char* clientClassName = entity ? entity->GetClientClassName() : nullptr;
	advancedfx::Message(
		"mirv_modelinfo: %s entity %p debug \"%s\" class \"%s\" client \"%s\" sceneNode %p modelState %p modelHandlePtr %p modelImp %p modelName \"%s\" boneCount %u boneNamesArray %p boneParentArray %p scanBytes 0x%Ix\n",
		label ? label : "",
		entity,
		debugName ? debugName : "",
		className ? className : "",
		clientClassName ? clientClassName : "",
		sceneNode,
		modelState,
		modelHandle,
		modelImp,
		modelName && IsLikelyPrintableAscii(modelName, 192) ? modelName : "",
		boneCount,
		boneNamesArray,
		boneParentArray,
		scanBytes);

	if (!ok) {
		advancedfx::Warning("mirv_modelinfo: failed to resolve model info for %s.\n", label ? label : "entity");
		return;
	}

	unsigned char* modelNameField = modelState + kCModelStateModelNameOffset;
	uint64_t q0 = 0;
	uint64_t q1 = 0;
	uint64_t q2 = 0;
	uint64_t q3 = 0;
	if (!IsBadReadPtr(modelNameField, sizeof(q0))) q0 = *(uint64_t*)modelNameField;
	if (!IsBadReadPtr(modelNameField + 8, sizeof(q1))) q1 = *(uint64_t*)(modelNameField + 8);
	if (!IsBadReadPtr(modelNameField + 16, sizeof(q2))) q2 = *(uint64_t*)(modelNameField + 16);
	if (!IsBadReadPtr(modelNameField + 24, sizeof(q3))) q3 = *(uint64_t*)(modelNameField + 24);

	const char* directModelName = IsLikelyPrintableAscii((const char*)modelNameField, 192) ? (const char*)modelNameField : nullptr;
	const char* ptr0ModelName = nullptr;
	if (q0 > 0x10000) {
		const char* ptrCandidate = (const char*)(uintptr_t)q0;
		if (IsLikelyPrintableAscii(ptrCandidate, 192)) {
			ptr0ModelName = ptrCandidate;
		}
	}

	advancedfx::Message(
		"mirv_modelinfo: %s m_ModelName modelState+0x%Ix addr %p qwords [%016llX %016llX %016llX %016llX] direct \"%s\" ptr0 \"%s\"\n",
		label ? label : "",
		(size_t)kCModelStateModelNameOffset,
		modelNameField,
		(unsigned long long)q0,
		(unsigned long long)q1,
		(unsigned long long)q2,
		(unsigned long long)q3,
		directModelName ? directModelName : "",
		ptr0ModelName ? ptr0ModelName : "");

	if (scanBytes < sizeof(void*)) scanBytes = sizeof(void*);
	if (scanBytes > 0x4000) scanBytes = 0x4000;

	int printed = 0;
	for (size_t offset = 0; offset + sizeof(void*) <= scanBytes && printed < 64; offset += sizeof(void*)) {
		unsigned char* field = modelImp + offset;
		if (IsBadReadPtr(field, sizeof(void*))) continue;

		const char* directText = IsLikelyPrintableAscii((const char*)field, 192) ? (const char*)field : nullptr;
		if (directText && IsInterestingModelString(directText)) {
			advancedfx::Message("mirv_modelinfo: %s modelImp+0x%Ix direct \"%s\"\n", label ? label : "", offset, directText);
			++printed;
		}

		unsigned char* pointerValue = *(unsigned char**)field;
		if (pointerValue && (uintptr_t)pointerValue > 0x10000 && IsLikelyPrintableAscii((const char*)pointerValue, 192)) {
			const char* ptrText = (const char*)pointerValue;
			if (IsInterestingModelString(ptrText)) {
				advancedfx::Message("mirv_modelinfo: %s modelImp+0x%Ix ptr %p \"%s\"\n", label ? label : "", offset, pointerValue, ptrText);
				++printed;
			}
		}
	}

	if (printed == 0) {
		advancedfx::Message("mirv_modelinfo: %s found no interesting model strings in modelImp scan.\n", label ? label : "");
	}
}

static void DumpBoneNameEntriesForEntity(CEntityInstance* entity, const char* label, int maxEntries, size_t entryStride) {
	unsigned char* sceneNode = nullptr;
	unsigned char* modelState = nullptr;
	unsigned char*** modelHandle = nullptr;
	unsigned char* modelImp = nullptr;
	const char* modelName = nullptr;
	unsigned char* boneNamesArray = nullptr;
	int16_t* boneParentArray = nullptr;
	uint32_t boneCount = 0;
	const bool ok = TryGetEntityModelInfo(entity, sceneNode, modelState, modelHandle, modelImp, modelName, boneNamesArray, boneParentArray, boneCount);

	const char* debugName = entity ? entity->GetDebugName() : nullptr;
	const char* className = entity ? entity->GetClassName() : nullptr;
	const char* clientClassName = entity ? entity->GetClientClassName() : nullptr;
	advancedfx::Message(
		"mirv_bonenames: %s entity %p debug \"%s\" class \"%s\" client \"%s\" sceneNode %p modelState %p modelHandlePtr %p modelImp %p modelName \"%s\" boneCount %u boneNamesArray %p boneParentArray %p stride 0x%Ix\n",
		label ? label : "",
		entity,
		debugName ? debugName : "",
		className ? className : "",
		clientClassName ? clientClassName : "",
		sceneNode,
		modelState,
		modelHandle,
		modelImp,
		modelName && IsLikelyPrintableAscii(modelName, 192) ? modelName : "",
		boneCount,
		boneNamesArray,
		boneParentArray,
		entryStride);

	if (!ok) {
		advancedfx::Warning("mirv_bonenames: failed to resolve model info for %s.\n", label ? label : "entity");
		return;
	}

	int count = maxEntries;
	if (count < 0 || (uint32_t)count > boneCount) count = (int)boneCount;
	for (int i = 0; i < count; ++i) {
		unsigned char* entry = boneNamesArray + (size_t)i * entryStride;
		uint64_t q0 = 0;
		uint64_t q1 = 0;
		uint64_t q2 = 0;
		uint64_t q3 = 0;
		if (!IsBadReadPtr(entry, sizeof(q0))) q0 = *(uint64_t*)(entry);
		if (!IsBadReadPtr(entry + 8, sizeof(q1))) q1 = *(uint64_t*)(entry + 8);
		if (!IsBadReadPtr(entry + 16, sizeof(q2))) q2 = *(uint64_t*)(entry + 16);
		if (!IsBadReadPtr(entry + 24, sizeof(q3))) q3 = *(uint64_t*)(entry + 24);

		const char* directText = IsLikelyPrintableAscii((const char*)entry, 64) ? (const char*)entry : nullptr;
		const char* ptrText = nullptr;
		const char* ptrCandidate = (const char*)(uintptr_t)q0;
		if (q0 > 0x10000 && IsLikelyPrintableAscii(ptrCandidate, 64)) {
			ptrText = ptrCandidate;
		}

		advancedfx::Message(
			"mirv_bonenames: %s[%d] entry %p qwords [%016llX %016llX %016llX %016llX] direct \"%s\" ptr0 \"%s\"\n",
			label ? label : "",
			i,
			entry,
			(unsigned long long)q0,
			(unsigned long long)q1,
			(unsigned long long)q2,
			(unsigned long long)q3,
			directText ? directText : "",
			ptrText ? ptrText : "");
	}
}

struct BoneParentArrayCandidate {
	const char* source;
	size_t offset;
	unsigned char* address;
	const char* type;
	size_t stride;
	int validCount;
	int parentBeforeChildCount;
	int rootCount;
	std::vector<int> firstParents;
};

static bool TryReadBoneParentValue(unsigned char* address, uint32_t index, size_t stride, bool int16Values, int& outValue) {
	unsigned char* valueAddress = address + (size_t)index * stride;
	const size_t valueSize = int16Values ? sizeof(int16_t) : sizeof(int32_t);
	if (!address || stride < valueSize || IsBadReadPtr(valueAddress, valueSize)) return false;

	outValue = int16Values ? (int)*(int16_t*)valueAddress : (int)*(int32_t*)valueAddress;
	return true;
}

static bool EvaluateBoneParentArrayCandidate(
	unsigned char* address,
	uint32_t boneCount,
	const char* source,
	size_t offset,
	const char* type,
	size_t stride,
	bool int16Values,
	BoneParentArrayCandidate& outCandidate) {
	if (!address || boneCount == 0 || boneCount > kMaxReasonableBoneCount) return false;

	int firstValue = 0;
	if (!TryReadBoneParentValue(address, 0, stride, int16Values, firstValue)) return false;

	int validCount = 0;
	int parentBeforeChildCount = 0;
	int rootCount = 0;
	std::vector<int> firstParents;
	const uint32_t previewCount = boneCount < 32 ? boneCount : 32;
	firstParents.reserve(previewCount);

	for (uint32_t i = 0; i < boneCount; ++i) {
		int value = 0;
		if (!TryReadBoneParentValue(address, i, stride, int16Values, value)) return false;

		if (i < previewCount) firstParents.push_back(value);

		if (value >= -1 && value < (int)boneCount) {
			++validCount;
			if (value == -1) ++rootCount;
			if (value == -1 || value < (int)i) ++parentBeforeChildCount;
		}
	}

	if (firstValue != -1) return false;
	if (validCount < (int)boneCount) return false;
	if (parentBeforeChildCount < (int)boneCount - 1) return false;
	if (rootCount < 1 || rootCount > (int)(boneCount / 4) + 1) return false;

	outCandidate.source = source;
	outCandidate.offset = offset;
	outCandidate.address = address;
	outCandidate.type = type;
	outCandidate.stride = stride;
	outCandidate.validCount = validCount;
	outCandidate.parentBeforeChildCount = parentBeforeChildCount;
	outCandidate.rootCount = rootCount;
	outCandidate.firstParents = firstParents;
	return true;
}

static void AddBoneParentArrayCandidatesForAddress(
	std::vector<BoneParentArrayCandidate>& candidates,
	unsigned char* address,
	uint32_t boneCount,
	const char* source,
	size_t offset) {
	BoneParentArrayCandidate candidate;
	if (EvaluateBoneParentArrayCandidate(address, boneCount, source, offset, "int16", sizeof(int16_t), true, candidate)) candidates.push_back(candidate);
	if (EvaluateBoneParentArrayCandidate(address, boneCount, source, offset, "int16", sizeof(int32_t), true, candidate)) candidates.push_back(candidate);
	if (EvaluateBoneParentArrayCandidate(address, boneCount, source, offset, "int32", sizeof(int32_t), false, candidate)) candidates.push_back(candidate);
	if (EvaluateBoneParentArrayCandidate(address, boneCount, source, offset, "int32", sizeof(int64_t), false, candidate)) candidates.push_back(candidate);
}

static void ScanBoneParentArraysForEntity(CEntityInstance* entity, const char* label, size_t scanBytes) {
	unsigned char* sceneNode = nullptr;
	unsigned char* modelState = nullptr;
	unsigned char*** modelHandle = nullptr;
	unsigned char* modelImp = nullptr;
	const char* modelName = nullptr;
	unsigned char* boneNamesArray = nullptr;
	int16_t* boneParentArray = nullptr;
	uint32_t boneCount = 0;
	const bool ok = TryGetEntityModelInfo(entity, sceneNode, modelState, modelHandle, modelImp, modelName, boneNamesArray, boneParentArray, boneCount);

	const char* debugName = entity ? entity->GetDebugName() : nullptr;
	const char* className = entity ? entity->GetClassName() : nullptr;
	const char* clientClassName = entity ? entity->GetClientClassName() : nullptr;
	advancedfx::Message(
		"mirv_boneparents: %s entity %p debug \"%s\" class \"%s\" client \"%s\" modelImp %p modelName \"%s\" boneCount %u scanBytes 0x%Ix\n",
		label ? label : "",
		entity,
		debugName ? debugName : "",
		className ? className : "",
		clientClassName ? clientClassName : "",
		modelImp,
		modelName && IsLikelyPrintableAscii(modelName, 192) ? modelName : "",
		boneCount,
		scanBytes);

	if (!ok) {
		advancedfx::Warning("mirv_boneparents: failed to resolve model info for %s.\n", label ? label : "entity");
		return;
	}

	if (scanBytes < sizeof(void*)) scanBytes = sizeof(void*);
	if (scanBytes > 0x4000) scanBytes = 0x4000;

	std::vector<BoneParentArrayCandidate> candidates;
	for (size_t offset = 0; offset + sizeof(void*) <= scanBytes; offset += sizeof(void*)) {
		unsigned char* field = modelImp + offset;
		if (IsBadReadPtr(field, sizeof(void*))) continue;

		AddBoneParentArrayCandidatesForAddress(candidates, field, boneCount, "inline", offset);

		unsigned char* pointerValue = *(unsigned char**)field;
		if (pointerValue && (uintptr_t)pointerValue > 0x10000) {
			AddBoneParentArrayCandidatesForAddress(candidates, pointerValue, boneCount, "ptr", offset);
		}
	}

	std::sort(candidates.begin(), candidates.end(), [](const BoneParentArrayCandidate& a, const BoneParentArrayCandidate& b) {
		if (a.parentBeforeChildCount != b.parentBeforeChildCount) return a.parentBeforeChildCount > b.parentBeforeChildCount;
		if (a.validCount != b.validCount) return a.validCount > b.validCount;
		if (0 != strcmp(a.source, b.source)) return strcmp(a.source, b.source) < 0;
		if (a.offset != b.offset) return a.offset < b.offset;
		return a.stride < b.stride;
	});

	if (candidates.empty()) {
		advancedfx::Message("mirv_boneparents: %s found no plausible parent arrays.\n", label ? label : "");
		return;
	}

	for (size_t i = 0; i < candidates.size(); ++i) {
		const BoneParentArrayCandidate& candidate = candidates[i];
		std::string parents;
		for (size_t j = 0; j < candidate.firstParents.size(); ++j) {
			if (!parents.empty()) parents += " ";
			parents += std::to_string(candidate.firstParents[j]);
		}

		advancedfx::Message(
			"mirv_boneparents: %s candidate %zu source %s modelImp+0x%Ix addr %p type %s stride 0x%Ix valid %d/%u parentBeforeChild %d/%u roots %d first [%s]\n",
			label ? label : "",
			i,
			candidate.source,
			candidate.offset,
			candidate.address,
			candidate.type,
			candidate.stride,
			candidate.validCount,
			boneCount,
			candidate.parentBeforeChildCount,
			boneCount,
			candidate.rootCount,
			parents.c_str());
	}
}

static void PrintBoneFlagsCandidate(const char* commandName, const char* label, const char* source, uint32_t* flagsArray, uint32_t boneCount) {
	int knownCount = 0;
	int usedCount = 0;
	int zeroCount = 0;
	uint32_t unknownMask = 0;
	if (!IsLikelyBoneFlagsArray(flagsArray, boneCount, knownCount, usedCount, zeroCount, unknownMask)) return;

	advancedfx::Message(
		"%s: %s candidate source %s addr %p type uint32 stride 0x4 known %d/%u used %d/%u zero %d/%u unknownMask 0x%08X first [",
		commandName,
		label ? label : "",
		source ? source : "",
		flagsArray,
		knownCount,
		boneCount,
		usedCount,
		boneCount,
		zeroCount,
		boneCount,
		unknownMask);

	uint32_t printCount = boneCount < 32 ? boneCount : 32;
	for (uint32_t i = 0; i < printCount; ++i) {
		advancedfx::Message("%s0x%08X", i ? " " : "", flagsArray[i]);
	}
	advancedfx::Message("]\n");
}

static void ScanBoneFlagsArraysForEntity(CEntityInstance* entity, const char* label, size_t scanBytes) {
	unsigned char* sceneNode = nullptr;
	unsigned char* modelState = nullptr;
	unsigned char*** modelHandle = nullptr;
	unsigned char* modelImp = nullptr;
	const char* modelName = nullptr;
	unsigned char* boneNamesArray = nullptr;
	int16_t* boneParentArray = nullptr;
	uint32_t boneCount = 0;
	const bool ok = TryGetEntityModelInfo(entity, sceneNode, modelState, modelHandle, modelImp, modelName, boneNamesArray, boneParentArray, boneCount);

	const char* debugName = entity ? entity->GetDebugName() : nullptr;
	const char* className = entity ? entity->GetClassName() : nullptr;
	const char* clientClassName = entity ? entity->GetClientClassName() : nullptr;
	advancedfx::Message(
		"mirv_boneflags: %s entity %p debug \"%s\" class \"%s\" client \"%s\" modelImp %p boneCount %u scanBytes 0x%Ix\n",
		label ? label : "",
		entity,
		debugName ? debugName : "",
		className ? className : "",
		clientClassName ? clientClassName : "",
		modelImp,
		boneCount,
		scanBytes);

	if (!ok) {
		advancedfx::Warning("mirv_boneflags: failed to resolve model info for %s.\n", label ? label : "entity");
		return;
	}

	if (scanBytes < sizeof(void*)) scanBytes = sizeof(void*);
	if (scanBytes > 0x4000) scanBytes = 0x4000;

	int printed = 0;
	std::vector<uint32_t*> seenArrays;
	for (size_t offset = 0; offset + sizeof(void*) <= scanBytes && printed < 32; offset += sizeof(void*)) {
		unsigned char* field = modelImp + offset;
		if (IsBadReadPtr(field, sizeof(void*))) continue;

		uint32_t* flagsArray = *(uint32_t**)field;
		if (!flagsArray || (uintptr_t)flagsArray < 0x10000) continue;
		if (std::find(seenArrays.begin(), seenArrays.end(), flagsArray) != seenArrays.end()) continue;
		seenArrays.push_back(flagsArray);

		int knownCount = 0;
		int usedCount = 0;
		int zeroCount = 0;
		uint32_t unknownMask = 0;
		if (!IsLikelyBoneFlagsArray(flagsArray, boneCount, knownCount, usedCount, zeroCount, unknownMask)) continue;

		char source[64];
		sprintf_s(source, "ptr modelImp+0x%Ix", offset);
		PrintBoneFlagsCandidate("mirv_boneflags", label, source, flagsArray, boneCount);
		++printed;
	}

	if (printed == 0) {
		advancedfx::Message("mirv_boneflags: %s found no plausible uint32 bone flags arrays.\n", label ? label : "");
	}
}

static void ListBonesForEntity(CEntityInstance* entity, const char* label, int maxBones) {
	unsigned char* sceneNode = nullptr;
	unsigned char* modelState = nullptr;
	unsigned char*** modelHandle = nullptr;
	unsigned char* modelImp = nullptr;
	const char* modelName = nullptr;
	unsigned char* boneNamesArray = nullptr;
	int16_t* boneParentArray = nullptr;
	uint32_t boneCount = 0;
	const bool ok = TryGetEntityModelInfo(entity, sceneNode, modelState, modelHandle, modelImp, modelName, boneNamesArray, boneParentArray, boneCount);

	const char* debugName = entity ? entity->GetDebugName() : nullptr;
	const char* className = entity ? entity->GetClassName() : nullptr;
	const char* clientClassName = entity ? entity->GetClientClassName() : nullptr;
	advancedfx::Message(
		"mirv_listbones: %s entity %p debug \"%s\" class \"%s\" client \"%s\" modelName \"%s\" boneCount %u boneNamesArray %p boneParentArray %p\n",
		label ? label : "",
		entity,
		debugName ? debugName : "",
		className ? className : "",
		clientClassName ? clientClassName : "",
		modelName && IsLikelyPrintableAscii(modelName, 192) ? modelName : "",
		boneCount,
		boneNamesArray,
		boneParentArray);

	if (!ok) {
		advancedfx::Warning("mirv_listbones: failed to resolve model info for %s.\n", label ? label : "entity");
		return;
	}

	const bool hasValidParentArray = IsValidBoneParentArray(boneParentArray, boneCount);
	if (boneParentArray && !hasValidParentArray) {
		advancedfx::Warning("mirv_listbones: %s bone parent array failed validation.\n", label ? label : "entity");
	}

	uint32_t count = boneCount;
	if (maxBones >= 0 && (uint32_t)maxBones < count) count = (uint32_t)maxBones;

	for (uint32_t i = 0; i < count; ++i) {
		const char* boneName = TryGetBoneNameFromArray(boneNamesArray, i);
		int parentIndex = -2;
		const char* parentName = nullptr;
		if (hasValidParentArray) {
			parentIndex = (int)boneParentArray[i];
			if (parentIndex >= 0 && (uint32_t)parentIndex < boneCount) {
				parentName = TryGetBoneNameFromArray(boneNamesArray, (uint32_t)parentIndex);
			}
		}

		SOURCESDK::Vector origin;
		SOURCESDK::Quaternion angles;
		const bool hasTransform = entity && entity->GetBone((int)i, origin, angles);
		if (hasTransform) {
			advancedfx::Message(
				"mirv_listbones: %s[%u] \"%s\" parent %d \"%s\" pos [%.3f %.3f %.3f] quat [%.6f %.6f %.6f %.6f]\n",
				label ? label : "",
				i,
				boneName ? boneName : "",
				parentIndex,
				parentName ? parentName : "",
				origin.x, origin.y, origin.z,
				angles.x, angles.y, angles.z, angles.w);
		} else {
			advancedfx::Message(
				"mirv_listbones: %s[%u] \"%s\" parent %d \"%s\" transform failed\n",
				label ? label : "",
				i,
				boneName ? boneName : "",
				parentIndex,
				parentName ? parentName : "");
		}
	}

	if (count < boneCount) {
		advancedfx::Message("mirv_listbones: %s truncated %u/%u bones.\n", label ? label : "", count, boneCount);
	}
}

CON_COMMAND(mirv_scenechildren, "Debug scene-node children of a player pawn") {
	if (args->ArgC() < 2) {
		advancedfx::Message("mirv_scenechildren <playerControllerIndex|current> [maxDepth]\nExample: mirv_scenechildren current 4\n");
		return;
	}

	int controllerIndex = -1;
	const char* controllerArg = args->ArgV(1);
	if (0 == _stricmp("current", controllerArg)) {
		if (!GetCurrentSpectatedControllerIndex(controllerIndex)) {
			advancedfx::Warning("mirv_scenechildren: could not resolve 'current' (no spectated/local controller).\n");
			return;
		}
	} else {
		long controllerVal = 0;
		if (!TryParseLongArg(controllerArg, controllerVal)) {
			advancedfx::Warning("mirv_scenechildren: controller index must be a number or 'current'.\n");
			return;
		}
		controllerIndex = (int)controllerVal;
	}

	int maxDepth = 4;
	if (args->ArgC() >= 3) {
		long depthVal = 0;
		if (!TryParseLongArg(args->ArgV(2), depthVal) || depthVal < 0 || depthVal > 16) {
			advancedfx::Warning("mirv_scenechildren: maxDepth must be between 0 and 16.\n");
			return;
		}
		maxDepth = (int)depthVal;
	}

	CEntityInstance* pawn = GetPawnFromControllerIndex(controllerIndex);
	if (!pawn) {
		advancedfx::Warning("mirv_scenechildren: failed to resolve pawn for controller %d.\n", controllerIndex);
		return;
	}

	if (!g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode || !g_clientDllOffsets.CGameSceneNode.m_pChild || !g_clientDllOffsets.CGameSceneNode.m_pNextSibling) {
		advancedfx::Warning("mirv_scenechildren: scene node child/sibling offsets are not initialized.\n");
		return;
	}

	unsigned char* root = *(unsigned char**)((unsigned char*)pawn + g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode);
	if (!root) {
		advancedfx::Warning("mirv_scenechildren: pawn scene node is null for controller %d.\n", controllerIndex);
		return;
	}

	advancedfx::Message("mirv_scenechildren: controller %d pawn %p root %p maxDepth %d\n", controllerIndex, pawn, root, maxDepth);
	int visitedCount = 0;
	std::vector<unsigned char*> visited;
	PrintSceneNodeTree(root, 0, maxDepth, visitedCount, visited);
	advancedfx::Message("mirv_scenechildren: visited %d scene nodes.\n", visitedCount);
}

static bool ResolveEntityEntryIndexArg(const char* entityArg, const char* commandName, CEntityInstance*& outEntity, int& outEntryIndex) {
	outEntity = nullptr;
	outEntryIndex = -1;
	long entryVal = 0;
	if (!TryParseLongArg(entityArg, entryVal) || entryVal < 0 || entryVal > 32766) {
		advancedfx::Warning("%s: entity entry index must be a number between 0 and 32766.\n", commandName);
		return false;
	}
	if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) {
		advancedfx::Warning("%s: entity system is not initialized.\n", commandName);
		return false;
	}

	outEntryIndex = (int)entryVal;
	outEntity = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, outEntryIndex);
	if (!outEntity) {
		advancedfx::Warning("%s: failed to resolve entity at entry index %d.\n", commandName, outEntryIndex);
		return false;
	}
	return true;
}

static int FindEntityEntryIndex(CEntityInstance* entity) {
	if (!entity || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) return -1;
	int highestIndex = GetHighestEntityIndex();
	for (int i = 0; i <= highestIndex; ++i) {
		if ((CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, i) == entity) return i;
	}
	return -1;
}

static void PrintEntityVisibilityInfo(CEntityInstance* entity, int entryIndex, const char* label) {
	const char* debugName = entity ? entity->GetDebugName() : nullptr;
	const char* className = entity ? entity->GetClassName() : nullptr;
	const char* clientClassName = entity ? entity->GetClientClassName() : nullptr;

	unsigned char* sceneNode = nullptr;
	const char* modelName = nullptr;
	TryGetEntityModelBaseInfo(entity, sceneNode, modelName);

	void* renderComponent = nullptr;
	bool renderEnabled = false;
	bool hasRenderEnabled = false;
	if (
		entity
		&& g_clientDllOffsets.C_BaseModelEntity.m_CRenderComponent > 0
		&& !IsBadReadPtr((unsigned char*)entity + g_clientDllOffsets.C_BaseModelEntity.m_CRenderComponent, sizeof(void*))
	) {
		renderComponent = *(void**)((unsigned char*)entity + g_clientDllOffsets.C_BaseModelEntity.m_CRenderComponent);
		hasRenderEnabled = TryGetEntityRenderEnabled(entity, renderEnabled);
	}

	advancedfx::Message(
		"mirv_entityvis: %s entry %d entity %p debug \"%s\" class \"%s\" client \"%s\" model \"%s\" sceneNode %p renderComponent %p renderEnable%s %d offsets renderComp 0x%Ix renderEnable 0x%Ix\n",
		label ? label : "",
		entryIndex,
		entity,
		debugName ? debugName : "",
		className ? className : "",
		clientClassName ? clientClassName : "",
		modelName ? modelName : "",
		sceneNode,
		renderComponent,
		hasRenderEnabled ? "" : "(unread)",
		hasRenderEnabled ? (renderEnabled ? 1 : 0) : -1,
		(size_t)g_clientDllOffsets.C_BaseModelEntity.m_CRenderComponent,
		(size_t)g_clientDllOffsets.CRenderComponent.m_bEnableRendering);
}

static bool ParseModelDebugCommandTarget(IWrpCommandArgs* args, const char* commandName, int& outControllerIndex, const char*& outTargetArg) {
	if (args->ArgC() < 3) {
		return false;
	}

	const char* controllerArg = args->ArgV(1);
	if (0 == _stricmp("current", controllerArg)) {
		if (!GetCurrentSpectatedControllerIndex(outControllerIndex)) {
			advancedfx::Warning("%s: could not resolve 'current' (no spectated/local controller).\n", commandName);
			return false;
		}
	} else {
		long controllerVal = 0;
		if (!TryParseLongArg(controllerArg, controllerVal)) {
			advancedfx::Warning("%s: controller index must be a number or 'current'.\n", commandName);
			return false;
		}
		outControllerIndex = (int)controllerVal;
	}

	outTargetArg = args->ArgV(2);
	return true;
}

CON_COMMAND(mirv_entityvis, "Debug entity render visibility fields") {
	if (args->ArgC() < 2) {
		advancedfx::Message(
			"mirv_entityvis <entityEntryIndex>\n"
			"mirv_entityvis current viewmodel\n"
			"mirv_entityvis weapons\n"
			"mirv_entityvis projectiles\n");
		return;
	}

	const char* mode = args->ArgV(1);
	if (0 == _stricmp(mode, "current")) {
		if (args->ArgC() < 3 || 0 != _stricmp(args->ArgV(2), "viewmodel")) {
			advancedfx::Warning("mirv_entityvis: expected current viewmodel.\n");
			return;
		}

		int controllerIndex = -1;
		if (!GetCurrentSpectatedControllerIndex(controllerIndex)) {
			advancedfx::Warning("mirv_entityvis: could not resolve current controller.\n");
			return;
		}

		CEntityInstance* pawn = GetPawnFromControllerIndex(controllerIndex);
		if (!pawn) {
			advancedfx::Warning("mirv_entityvis: failed to resolve pawn for controller %d.\n", controllerIndex);
			return;
		}

		std::vector<CEntityInstance*> hudModels;
		CollectHudModelOwnersForPawn(pawn, hudModels);
		advancedfx::Message("mirv_entityvis: found %zu hud model entities.\n", hudModels.size());
		for (size_t i = 0; i < hudModels.size(); ++i) {
			std::string label = "viewmodel" + std::to_string(i);
			PrintEntityVisibilityInfo(hudModels[i], FindEntityEntryIndex(hudModels[i]), label.c_str());
		}
		return;
	}

	if (0 == _stricmp(mode, "weapons") || 0 == _stricmp(mode, "projectiles")) {
		if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) {
			advancedfx::Warning("mirv_entityvis: entity system is not initialized.\n");
			return;
		}

		const bool weapons = 0 == _stricmp(mode, "weapons");
		int count = 0;
		int highestIndex = GetHighestEntityIndex();
		for (int i = 0; i <= highestIndex; ++i) {
			CEntityInstance* entity = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, i);
			if (!entity) continue;
			const char* debugName = entity->GetDebugName();
			if (!debugName) continue;
			if (weapons) {
				if (!StringBeginsWithCaseSensitive(debugName, "weapon_")) continue;
			} else {
				if (!StringEndsWithCaseSensitive(debugName, "_projectile")) continue;
			}

			PrintEntityVisibilityInfo(entity, i, mode);
			++count;
		}
		advancedfx::Message("mirv_entityvis: printed %d %s entities.\n", count, mode);
		return;
	}

	CEntityInstance* entity = nullptr;
	int entryIndex = -1;
	if (!ResolveEntityEntryIndexArg(mode, "mirv_entityvis", entity, entryIndex)) return;
	PrintEntityVisibilityInfo(entity, entryIndex, "entity");
}

CON_COMMAND(mirv_bonenames, "Debug raw model bone-name array entries") {
	if (args->ArgC() < 3) {
		advancedfx::Message("mirv_bonenames <playerControllerIndex|current> <player|viewmodel|all> [maxEntries] [entryStride]\nExample: mirv_bonenames current player 16 0x8\n");
		return;
	}

	int controllerIndex = -1;
	const char* targetArg = nullptr;
	if (!ParseModelDebugCommandTarget(args, "mirv_bonenames", controllerIndex, targetArg)) return;

	int maxEntries = 16;
	if (args->ArgC() >= 4) {
		long maxEntriesVal = 0;
		if (!TryParseLongArg(args->ArgV(3), maxEntriesVal) || maxEntriesVal < 1 || maxEntriesVal > 512) {
			advancedfx::Warning("mirv_bonenames: maxEntries must be between 1 and 512.\n");
			return;
		}
		maxEntries = (int)maxEntriesVal;
	}

	size_t entryStride = sizeof(void*);
	if (args->ArgC() >= 5) {
		long strideVal = 0;
		if (!TryParseLongArg(args->ArgV(4), strideVal, 0) || strideVal < 1 || strideVal > 256) {
			advancedfx::Warning("mirv_bonenames: entryStride must be between 1 and 256.\n");
			return;
		}
		entryStride = (size_t)strideVal;
	}

	CEntityInstance* pawn = GetPawnFromControllerIndex(controllerIndex);
	if (!pawn) {
		advancedfx::Warning("mirv_bonenames: failed to resolve pawn for controller %d.\n", controllerIndex);
		return;
	}

	const bool dumpPlayer = 0 == _stricmp(targetArg, "player") || 0 == _stricmp(targetArg, "all");
	const bool dumpViewmodel = 0 == _stricmp(targetArg, "viewmodel") || 0 == _stricmp(targetArg, "all");
	if (!dumpPlayer && !dumpViewmodel) {
		advancedfx::Warning("mirv_bonenames: target must be player, viewmodel, or all.\n");
		return;
	}

	if (dumpPlayer) DumpBoneNameEntriesForEntity(pawn, "player", maxEntries, entryStride);
	if (dumpViewmodel) {
		std::vector<CEntityInstance*> hudModels;
		CollectHudModelOwnersForPawn(pawn, hudModels);
		advancedfx::Message("mirv_bonenames: found %zu hud model entities.\n", hudModels.size());
		for (size_t i = 0; i < hudModels.size(); ++i) {
			std::string label = "viewmodel" + std::to_string(i);
			DumpBoneNameEntriesForEntity(hudModels[i], label.c_str(), maxEntries, entryStride);
		}
	}
}

CON_COMMAND(mirv_modelinfo, "Debug model state and model implementation strings") {
	if (args->ArgC() < 3) {
		advancedfx::Message("mirv_modelinfo <playerControllerIndex|current> <player|viewmodel|all> [scanBytes]\nExample: mirv_modelinfo current viewmodel 0x1000\n");
		return;
	}

	int controllerIndex = -1;
	const char* targetArg = nullptr;
	if (!ParseModelDebugCommandTarget(args, "mirv_modelinfo", controllerIndex, targetArg)) return;

	size_t scanBytes = 0x400;
	if (args->ArgC() >= 4) {
		long scanBytesVal = 0;
		if (!TryParseLongArg(args->ArgV(3), scanBytesVal, 0) || scanBytesVal < 0x20 || scanBytesVal > 0x4000) {
			advancedfx::Warning("mirv_modelinfo: scanBytes must be between 0x20 and 0x4000.\n");
			return;
		}
		scanBytes = (size_t)scanBytesVal;
	}

	CEntityInstance* pawn = GetPawnFromControllerIndex(controllerIndex);
	if (!pawn) {
		advancedfx::Warning("mirv_modelinfo: failed to resolve pawn for controller %d.\n", controllerIndex);
		return;
	}

	const bool dumpPlayer = 0 == _stricmp(targetArg, "player") || 0 == _stricmp(targetArg, "all");
	const bool dumpViewmodel = 0 == _stricmp(targetArg, "viewmodel") || 0 == _stricmp(targetArg, "all");
	if (!dumpPlayer && !dumpViewmodel) {
		advancedfx::Warning("mirv_modelinfo: target must be player, viewmodel, or all.\n");
		return;
	}

	if (dumpPlayer) DumpModelInfoForEntity(pawn, "player", scanBytes);
	if (dumpViewmodel) {
		std::vector<CEntityInstance*> hudModels;
		CollectHudModelOwnersForPawn(pawn, hudModels);
		advancedfx::Message("mirv_modelinfo: found %zu hud model entities.\n", hudModels.size());
		for (size_t i = 0; i < hudModels.size(); ++i) {
			std::string label = "viewmodel" + std::to_string(i);
			DumpModelInfoForEntity(hudModels[i], label.c_str(), scanBytes);
		}
	}
}

CON_COMMAND(mirv_boneparents, "Scan model data for plausible bone parent arrays") {
	if (args->ArgC() < 3) {
		advancedfx::Message("mirv_boneparents <playerControllerIndex|current> <player|viewmodel|all> [scanBytes]\nExample: mirv_boneparents current player 0x400\n");
		return;
	}

	int controllerIndex = -1;
	const char* targetArg = nullptr;
	if (!ParseModelDebugCommandTarget(args, "mirv_boneparents", controllerIndex, targetArg)) return;

	size_t scanBytes = 0x400;
	if (args->ArgC() >= 4) {
		long scanBytesVal = 0;
		if (!TryParseLongArg(args->ArgV(3), scanBytesVal, 0) || scanBytesVal < 0x20 || scanBytesVal > 0x4000) {
			advancedfx::Warning("mirv_boneparents: scanBytes must be between 0x20 and 0x4000.\n");
			return;
		}
		scanBytes = (size_t)scanBytesVal;
	}

	CEntityInstance* pawn = GetPawnFromControllerIndex(controllerIndex);
	if (!pawn) {
		advancedfx::Warning("mirv_boneparents: failed to resolve pawn for controller %d.\n", controllerIndex);
		return;
	}

	const bool scanPlayer = 0 == _stricmp(targetArg, "player") || 0 == _stricmp(targetArg, "all");
	const bool scanViewmodel = 0 == _stricmp(targetArg, "viewmodel") || 0 == _stricmp(targetArg, "all");
	if (!scanPlayer && !scanViewmodel) {
		advancedfx::Warning("mirv_boneparents: target must be player, viewmodel, or all.\n");
		return;
	}

	if (scanPlayer) ScanBoneParentArraysForEntity(pawn, "player", scanBytes);
	if (scanViewmodel) {
		std::vector<CEntityInstance*> hudModels;
		CollectHudModelOwnersForPawn(pawn, hudModels);
		advancedfx::Message("mirv_boneparents: found %zu hud model entities.\n", hudModels.size());
		for (size_t i = 0; i < hudModels.size(); ++i) {
			std::string label = "viewmodel" + std::to_string(i);
			ScanBoneParentArraysForEntity(hudModels[i], label.c_str(), scanBytes);
		}
	}
}

CON_COMMAND(mirv_boneflags, "Scan model data for plausible Source 2 bone flag arrays") {
	if (args->ArgC() < 3) {
		advancedfx::Message(
			"mirv_boneflags <playerControllerIndex|current> <player|viewmodel|all> [scanBytes]\n"
			"mirv_boneflags entity <entityEntryIndex> [scanBytes]\n"
			"Example: mirv_boneflags current player 0x400\n"
			"Example: mirv_boneflags entity 123 0x1000\n");
		return;
	}

	if (0 == _stricmp(args->ArgV(1), "entity")) {
		if (args->ArgC() < 3) {
			advancedfx::Warning("mirv_boneflags: entity requires an entity entry index.\n");
			return;
		}

		size_t scanBytes = 0x400;
		if (args->ArgC() >= 4) {
			long scanBytesVal = 0;
			if (!TryParseLongArg(args->ArgV(3), scanBytesVal, 0) || scanBytesVal < 0x20 || scanBytesVal > 0x4000) {
				advancedfx::Warning("mirv_boneflags: scanBytes must be between 0x20 and 0x4000.\n");
				return;
			}
			scanBytes = (size_t)scanBytesVal;
		}

		CEntityInstance* entity = nullptr;
		int entryIndex = -1;
		if (!ResolveEntityEntryIndexArg(args->ArgV(2), "mirv_boneflags", entity, entryIndex)) return;

		std::string label = "entity" + std::to_string(entryIndex);
		ScanBoneFlagsArraysForEntity(entity, label.c_str(), scanBytes);
		return;
	}

	int controllerIndex = -1;
	const char* targetArg = nullptr;
	if (!ParseModelDebugCommandTarget(args, "mirv_boneflags", controllerIndex, targetArg)) return;

	size_t scanBytes = 0x400;
	if (args->ArgC() >= 4) {
		long scanBytesVal = 0;
		if (!TryParseLongArg(args->ArgV(3), scanBytesVal, 0) || scanBytesVal < 0x20 || scanBytesVal > 0x4000) {
			advancedfx::Warning("mirv_boneflags: scanBytes must be between 0x20 and 0x4000.\n");
			return;
		}
		scanBytes = (size_t)scanBytesVal;
	}

	CEntityInstance* pawn = GetPawnFromControllerIndex(controllerIndex);
	if (!pawn) {
		advancedfx::Warning("mirv_boneflags: failed to resolve pawn for controller %d.\n", controllerIndex);
		return;
	}

	const bool scanPlayer = 0 == _stricmp(targetArg, "player") || 0 == _stricmp(targetArg, "all");
	const bool scanViewmodel = 0 == _stricmp(targetArg, "viewmodel") || 0 == _stricmp(targetArg, "all");
	if (!scanPlayer && !scanViewmodel) {
		advancedfx::Warning("mirv_boneflags: target must be player, viewmodel, or all.\n");
		return;
	}

	if (scanPlayer) ScanBoneFlagsArraysForEntity(pawn, "player", scanBytes);
	if (scanViewmodel) {
		std::vector<CEntityInstance*> hudModels;
		CollectHudModelOwnersForPawn(pawn, hudModels);
		advancedfx::Message("mirv_boneflags: found %zu hud model entities.\n", hudModels.size());
		for (size_t i = 0; i < hudModels.size(); ++i) {
			std::string label = "viewmodel" + std::to_string(i);
			ScanBoneFlagsArraysForEntity(hudModels[i], label.c_str(), scanBytes);
		}
	}
}

CON_COMMAND(mirv_listbones, "List model bones and current transforms") {
	if (args->ArgC() < 3) {
		advancedfx::Message("mirv_listbones <playerControllerIndex|current> <player|viewmodel|all> [maxBones]\nExample: mirv_listbones current viewmodel 32\n");
		return;
	}

	int controllerIndex = -1;
	const char* targetArg = nullptr;
	if (!ParseModelDebugCommandTarget(args, "mirv_listbones", controllerIndex, targetArg)) return;

	int maxBones = -1;
	if (args->ArgC() >= 4) {
		long maxBonesVal = 0;
		if (!TryParseLongArg(args->ArgV(3), maxBonesVal) || maxBonesVal < 1 || maxBonesVal > 4096) {
			advancedfx::Warning("mirv_listbones: maxBones must be between 1 and 4096.\n");
			return;
		}
		maxBones = (int)maxBonesVal;
	}

	CEntityInstance* pawn = GetPawnFromControllerIndex(controllerIndex);
	if (!pawn) {
		advancedfx::Warning("mirv_listbones: failed to resolve pawn for controller %d.\n", controllerIndex);
		return;
	}

	const bool listPlayer = 0 == _stricmp(targetArg, "player") || 0 == _stricmp(targetArg, "all");
	const bool listViewmodel = 0 == _stricmp(targetArg, "viewmodel") || 0 == _stricmp(targetArg, "all");
	if (!listPlayer && !listViewmodel) {
		advancedfx::Warning("mirv_listbones: target must be player, viewmodel, or all.\n");
		return;
	}

	if (listPlayer) ListBonesForEntity(pawn, "player", maxBones);
	if (listViewmodel) {
		std::vector<CEntityInstance*> hudModels;
		CollectHudModelOwnersForPawn(pawn, hudModels);
		advancedfx::Message("mirv_listbones: found %zu hud model entities.\n", hudModels.size());
		for (size_t i = 0; i < hudModels.size(); ++i) {
			std::string label = "viewmodel" + std::to_string(i);
			ListBonesForEntity(hudModels[i], label.c_str(), maxBones);
		}
	}
}

#endif