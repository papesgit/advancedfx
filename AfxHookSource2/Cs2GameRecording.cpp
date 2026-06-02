#include "stdafx.h"

#include "Cs2GameRecording.h"

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../deps/release/prop/AfxHookSource/SourceInterfaces.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "SchemaSystem.h"
#include "WrpConsole.h"

#include "../shared/AfxConsole.h"
#include "../shared/AfxGameRecord.h"
#include "../shared/binutils.h"
#include "../shared/StringTools.h"

#include <winsock.h>
#include "../deps/release/Detours/src/detours.h"

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
static constexpr std::ptrdiff_t kCEffectDataOriginOffset = 0x08;
static constexpr std::ptrdiff_t kCEffectDataNormalOffset = 0x20;
static constexpr std::ptrdiff_t kCEffectDataEntityOffset = 0x38;
static constexpr std::ptrdiff_t kCEffectDataMagnitudeOffset = 0x44;
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

static bool IsRecordableWeaponEntity(CEntityInstance* entity) {
	if (!entity) return false;

	const char* debugName = entity->GetDebugName();
	if (debugName && StringBeginsWithCaseSensitive(debugName, "weapon_")) return true;

	const char* clientClassName = entity->GetClientClassName();
	if (clientClassName && 0 == strcmp(clientClassName, "C_PlantedC4")) return true;

	unsigned char* sceneNode = nullptr;
	const char* modelName = nullptr;
	if (TryGetEntityModelBaseInfo(entity, sceneNode, modelName)) {
		if (0 == strcmp(modelName, "weapons/models/defuser/defuser.vmdl")) return true;
	}

	return false;
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

static bool IsEntityRenderEnabled(CEntityInstance* entity) {
	bool renderEnabled = true;
	TryGetEntityRenderEnabled(entity, renderEnabled);
	return renderEnabled;
}

static bool TryReadEntityHandleField(CEntityInstance* entity, std::ptrdiff_t offset, SOURCESDK::CS2::CBaseHandle& outHandle) {
	outHandle = SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	if (!entity || offset <= 0) return false;
	unsigned char* address = (unsigned char*)entity + offset;
	if (IsBadReadPtr(address, sizeof(uint32_t))) return false;
	outHandle = SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(uint32_t*)address);
	return true;
}

static const char* TryGetRecordingBoneNameFromArray(unsigned char* boneNamesArray, uint32_t boneIndex) {
	if (!boneNamesArray) return nullptr;
	unsigned char* entry = boneNamesArray + (size_t)boneIndex * sizeof(void*);
	if (IsBadReadPtr(entry, sizeof(const char*))) return nullptr;

	const char* name = *(const char**)entry;
	return IsLikelyPrintableAscii(name, 128) ? name : nullptr;
}

struct Cs2RecordedCamera {
	float X = 0.0f;
	float Y = 0.0f;
	float Z = 0.0f;
	float Rx = 0.0f;
	float Ry = 0.0f;
	float Rz = 0.0f;
	float Fov = 0.0f;
};

struct Cs2SkeletonMetadata {
	unsigned char* ModelImp = nullptr;
	std::string ModelName;
	uint32_t BoneCount = 0;
	bool HasBoneFlags = false;
	std::vector<std::string> BoneNames;
	std::vector<int> BoneParents;
	std::vector<uint8_t> BoneUsed;
};

struct Cs2RecordedEntity {
	int Id = 0;
	int OwnerId = -1;
	std::string ClientClassName;
	std::string ModelName;
	bool Visible = true;
	bool ViewModel = false;
	bool Projectile = false;
	SOURCESDK::matrix3x4_t Transform;
	bool HasBones = false;
	const Cs2SkeletonMetadata* Skeleton = nullptr;
	std::vector<SOURCESDK::matrix3x4_t> LocalBoneTransforms;
};

struct Cs2RecordedBloodEvent {
	int VictimEntityId = -1;
	SOURCESDK::Vector Origin;
	SOURCESDK::Vector Normal;
	float Magnitude = 0.0f;
};

struct Cs2RecordedShotPellet {
	SOURCESDK::Vector Direction;
};

struct Cs2RecordedShotEvent {
	int ShooterEntityId = -1;
	int WeaponEntityId = -1;
	SOURCESDK::Vector Origin;
	std::vector<Cs2RecordedShotPellet> Pellets;
};

struct Cs2RecordedFrame {
	float FrameTime = 0.0f;
	bool HasCamera = false;
	Cs2RecordedCamera Camera;
	std::vector<Cs2RecordedEntity> Entities;
	std::vector<int> HiddenEntityIds;
	std::vector<Cs2RecordedBloodEvent> BloodEvents;
	std::vector<Cs2RecordedShotEvent> ShotEvents;
};

class ICs2FrameSink {
public:
	virtual ~ICs2FrameSink() {}
	virtual void OnFrame(const Cs2RecordedFrame& frame) = 0;
};

class Cs2AgrSink : public ICs2FrameSink {
public:
	bool Start(const wchar_t* fileName) {
		return m_Record.StartRecording(fileName, 6);
	}

	void Stop() {
		m_Record.EndRecording();
	}

	bool IsRecording() {
		return m_Record.GetRecording();
	}

	virtual void OnFrame(const Cs2RecordedFrame& frame) override {
		if (!m_Record.GetRecording()) return;

		m_Record.BeginFrame(frame.FrameTime);

		for (std::vector<Cs2RecordedEntity>::const_iterator it = frame.Entities.begin(); it != frame.Entities.end(); ++it) {
			WriteEntity(*it);
		}

		if (frame.HasCamera) {
			m_Record.WriteDictionary("afxCam");
			m_Record.Write(frame.Camera.X);
			m_Record.Write(frame.Camera.Y);
			m_Record.Write(frame.Camera.Z);
			m_Record.Write(frame.Camera.Rx);
			m_Record.Write(frame.Camera.Ry);
			m_Record.Write(frame.Camera.Rz);
			m_Record.Write(frame.Camera.Fov);
		}

		for (std::vector<int>::const_iterator it = frame.HiddenEntityIds.begin(); it != frame.HiddenEntityIds.end(); ++it) {
			m_Record.MarkHidden(*it);
		}

		m_Record.EndFrame();
	}

private:
	advancedfx::CAfxGameRecord m_Record;

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

	void WriteEntity(const Cs2RecordedEntity& entity) {
		m_Record.WriteDictionary("entity_state");
		m_Record.Write(entity.Id);

		m_Record.WriteDictionary("baseentity");
		m_Record.WriteDictionary(entity.ModelName.c_str());
		m_Record.Write(entity.Visible);
		WriteMatrix3x4(entity.Transform);

		m_Record.WriteDictionary("baseanimating");
		m_Record.Write(entity.HasBones);
		if (!entity.HasBones) {
			m_Record.WriteDictionary("/");
			m_Record.Write(entity.ViewModel);
			return;
		}

		m_Record.Write((int)entity.LocalBoneTransforms.size());
		for (std::vector<SOURCESDK::matrix3x4_t>::const_iterator it = entity.LocalBoneTransforms.begin(); it != entity.LocalBoneTransforms.end(); ++it) {
			WriteMatrix3x4(*it);
		}

		m_Record.WriteDictionary("/");
		m_Record.Write(entity.ViewModel);
	}
};

class Cs2DebugFrameSink : public ICs2FrameSink {
public:
	bool IsEnabled() const {
		return m_Enabled;
	}

	void SetEnabled(bool value) {
		m_Enabled = value;
		m_FrameIndex = 0;
	}

	int GetPrintInterval() const {
		return m_PrintInterval;
	}

	void SetPrintInterval(int value) {
		m_PrintInterval = value < 1 ? 1 : value;
	}

	virtual void OnFrame(const Cs2RecordedFrame& frame) override {
		if (!m_Enabled) return;
		++m_FrameIndex;
		if (m_PrintInterval > 1 && (m_FrameIndex % m_PrintInterval) != 0) return;

		advancedfx::Message(
			"mirv_livelink debug: frame %d dt %.6f camera %d entities %zu hidden %zu\n",
			m_FrameIndex,
			frame.FrameTime,
			frame.HasCamera ? 1 : 0,
			frame.Entities.size(),
			frame.HiddenEntityIds.size());

		if (!frame.Entities.empty()) {
			const Cs2RecordedEntity& entity = frame.Entities.front();
			advancedfx::Message(
				"mirv_livelink debug: first entity id %d model \"%s\" visible %d viewmodel %d bones %zu transform [%.3f %.3f %.3f]\n",
				entity.Id,
				entity.ModelName.c_str(),
				entity.Visible ? 1 : 0,
				entity.ViewModel ? 1 : 0,
				entity.LocalBoneTransforms.size(),
				entity.Transform[0][3],
				entity.Transform[1][3],
				entity.Transform[2][3]);
		}
	}

private:
	bool m_Enabled = false;
	int m_FrameIndex = 0;
	int m_PrintInterval = 60;
};

class Cs2UdpFrameSink : public ICs2FrameSink {
public:
	~Cs2UdpFrameSink() {
		CloseSocket();
	}

	bool IsEnabled() const {
		return m_Enabled;
	}

	bool SetEnabled(bool value) {
		if (value == m_Enabled) return true;
		if (!value) {
			m_Enabled = false;
			CloseSocket();
			return true;
		}

		if (!OpenSocket()) return false;
		m_Enabled = true;
		m_Sequence = 0;
		m_FrameId = 0;
		m_SentSkeletons.clear();
		return true;
	}

	const std::string& GetTargetHost() const {
		return m_TargetHost;
	}

	int GetTargetPort() const {
		return m_TargetPort;
	}

	bool SetTarget(const char* host, int port) {
		if (!host || !host[0] || port < 1 || port > 65535) return false;

		unsigned long address = inet_addr(host);
		if (address == INADDR_NONE) return false;

		m_TargetHost = host;
		m_TargetPort = port;
		m_TargetAddress = address;
		ConfigureTargetAddress();
		return true;
	}

	virtual void OnFrame(const Cs2RecordedFrame& frame) override {
		if (!m_Enabled) return;
		if (m_Socket == INVALID_SOCKET && !OpenSocket()) return;

		SendSkeletonPackets(frame);
		SendFramePackets(frame);
	}

private:
	enum { kMaxPacketBytes = 60000 };
	enum { kPacketTypeFrame = 1 };
	enum { kPacketTypeSkeleton = 2 };

	bool m_Enabled = false;
	bool m_WsaStarted = false;
	SOCKET m_Socket = INVALID_SOCKET;
	std::string m_TargetHost = "127.0.0.1";
	int m_TargetPort = 31237;
	unsigned long m_TargetAddress = 0x0100007f; // 127.0.0.1 in network byte order.
	sockaddr_in m_TargetSockAddr = {};
	uint32_t m_Sequence = 0;
	uint32_t m_FrameId = 0;
	uint32_t m_LastLargePacketWarningSequence = 0;
	uint32_t m_LastSocketWarningSequence = 0;
	std::map<int, const Cs2SkeletonMetadata*> m_SentSkeletons;
	std::vector<unsigned char> m_PacketScratch;

	void SendSkeletonPackets(const Cs2RecordedFrame& frame) {
		for (std::vector<Cs2RecordedEntity>::const_iterator it = frame.Entities.begin(); it != frame.Entities.end(); ++it) {
			if (!it->HasBones || !it->Skeleton) continue;

			std::map<int, const Cs2SkeletonMetadata*>::iterator knownIt = m_SentSkeletons.find(it->Id);
			if (knownIt != m_SentSkeletons.end() && knownIt->second == it->Skeleton) continue;

			std::vector<unsigned char>& packet = m_PacketScratch;
			packet.clear();
			packet.reserve(4096);
			AppendHeader(packet, kPacketTypeSkeleton, m_Sequence);
			AppendI32(packet, it->Id);
			AppendString(packet, it->ModelName);
			AppendU32(packet, (uint32_t)it->Skeleton->BoneNames.size());

			for (size_t i = 0; i < it->Skeleton->BoneNames.size(); ++i) {
				AppendString(packet, it->Skeleton->BoneNames[i]);
				AppendI32(packet, i < it->Skeleton->BoneParents.size() ? it->Skeleton->BoneParents[i] : -1);
			}

			if (SendPacket(packet, "skeleton")) {
				m_SentSkeletons[it->Id] = it->Skeleton;
			}
		}
	}

	void SendFramePackets(const Cs2RecordedFrame& frame) {
		const uint32_t frameId = m_FrameId++;
		const size_t basePacketBytes = 12 + sizeof(float) + sizeof(uint32_t) + 2 * sizeof(uint16_t) + 2 * sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint8_t);
		const size_t finalOnlyBytes = frame.HiddenEntityIds.size() * sizeof(int32_t)
			+ sizeof(uint16_t)
			+ frame.BloodEvents.size() * GetBloodEventPacketSize()
			+ sizeof(uint16_t)
			+ GetShotEventsPacketSize(frame)
			+ (frame.HasCamera ? 7 * sizeof(float) : 0);
		if (basePacketBytes + finalOnlyBytes > kMaxPacketBytes) {
			WarnPacketTooLarge(basePacketBytes + finalOnlyBytes);
			return;
		}

		size_t entityIndex = 0;
		uint16_t chunkIndex = 0;
		do {
			const size_t chunkStartEntityIndex = entityIndex;
			size_t chunkEntityBytes = 0;
			uint32_t chunkEntityCount = 0;

			while (entityIndex < frame.Entities.size()) {
				const size_t entityBytes = GetFrameEntityPacketSize(frame.Entities[entityIndex]);
				const size_t targetMaxSize = basePacketBytes + finalOnlyBytes + chunkEntityBytes + entityBytes;
				if (targetMaxSize > kMaxPacketBytes) {
					if (chunkEntityCount > 0) break;
					WarnPacketTooLarge(targetMaxSize);
					++entityIndex;
					continue;
				}

				chunkEntityBytes += entityBytes;
				++chunkEntityCount;
				++entityIndex;
			}

			const bool isFinalChunk = entityIndex >= frame.Entities.size();
			std::vector<unsigned char>& packet = m_PacketScratch;
			packet.clear();
			packet.reserve(4096 + chunkEntityBytes);
			BeginFramePacket(packet, frame, frameId, chunkIndex, isFinalChunk ? kFrameChunkFlagFinal : 0);
			AppendU32(packet, chunkEntityCount);
			for (size_t i = 0; i < chunkEntityCount; ++i) {
				AppendFrameEntity(packet, frame.Entities[chunkStartEntityIndex + i]);
			}
			if (isFinalChunk) {
				AppendHiddenIds(packet, frame);
				AppendBloodEvents(packet, frame);
				AppendShotEvents(packet, frame);
				AppendCamera(packet, frame);
			} else {
				AppendU32(packet, 0);
				AppendU16(packet, 0);
				AppendU16(packet, 0);
				AppendU8(packet, 0);
			}
			SendPacket(packet, "frame");

			if (isFinalChunk) break;
			if (chunkIndex == UINT16_MAX) {
				advancedfx::Warning("mirv_livelink: too many chunks for frame %u.\n", frameId);
				break;
			}
			++chunkIndex;
		} while (entityIndex < frame.Entities.size());

		ForgetHiddenSkeletons(frame);
	}

	enum { kFrameChunkFlagFinal = 1 };

	void BeginFramePacket(std::vector<unsigned char>& packet, const Cs2RecordedFrame& frame, uint32_t frameId, uint16_t chunkIndex, uint16_t chunkFlags) {
		uint32_t frameRateNumerator = 30;
		uint32_t frameRateDenominator = 1;
		GetFrameRateFromDeltaTime(frame.FrameTime, frameRateNumerator, frameRateDenominator);

		AppendHeader(packet, kPacketTypeFrame, m_Sequence++);
		AppendFloat(packet, frame.FrameTime);
		AppendU32(packet, frameId);
		AppendU16(packet, chunkIndex);
		AppendU16(packet, chunkFlags);
		AppendU32(packet, frameRateNumerator);
		AppendU32(packet, frameRateDenominator);
	}

	static void GetFrameRateFromDeltaTime(float deltaTime, uint32_t& outNumerator, uint32_t& outDenominator) {
		outNumerator = 30;
		outDenominator = 1;
		if (deltaTime <= 0.0f) return;

		const double framesPerSecond = 1.0 / (double)deltaTime;
		const uint32_t roundedFps = (uint32_t)llround(framesPerSecond);
		if (roundedFps >= 1 && roundedFps <= 10000 && fabs(framesPerSecond - (double)roundedFps) < 0.01) {
			outNumerator = roundedFps;
			outDenominator = 1;
			return;
		}

		const uint32_t scale = 1000000;
		uint32_t denominator = (uint32_t)llround((double)deltaTime * (double)scale);
		if (denominator == 0) return;
		uint32_t numerator = scale;
		const uint32_t divisor = Gcd(numerator, denominator);
		outNumerator = numerator / divisor;
		outDenominator = denominator / divisor;
	}

	static uint32_t Gcd(uint32_t a, uint32_t b) {
		while (b != 0) {
			uint32_t t = b;
			b = a % b;
			a = t;
		}
		return a == 0 ? 1 : a;
	}

	static void AppendCamera(std::vector<unsigned char>& packet, const Cs2RecordedFrame& frame) {
		AppendU8(packet, frame.HasCamera ? 1 : 0);
		if (frame.HasCamera) {
			AppendFloat(packet, frame.Camera.X);
			AppendFloat(packet, frame.Camera.Y);
			AppendFloat(packet, frame.Camera.Z);
			AppendFloat(packet, frame.Camera.Rx);
			AppendFloat(packet, frame.Camera.Ry);
			AppendFloat(packet, frame.Camera.Rz);
			AppendFloat(packet, frame.Camera.Fov);
		}
	}

	static void AppendFrameEntity(std::vector<unsigned char>& packet, const Cs2RecordedEntity& entity) {
		AppendI32(packet, entity.Id);
		AppendI32(packet, entity.OwnerId);
		AppendU8(packet, entity.Projectile ? 1 : 0);
		AppendU8(packet, entity.Visible ? 1 : 0);
		AppendU8(packet, entity.ViewModel ? 1 : 0);
		AppendString(packet, entity.ClientClassName);
		AppendMatrix3x4(packet, entity.Transform);
		AppendU8(packet, entity.HasBones ? 1 : 0);
		AppendU32(packet, (uint32_t)entity.LocalBoneTransforms.size());

		for (std::vector<SOURCESDK::matrix3x4_t>::const_iterator it = entity.LocalBoneTransforms.begin(); it != entity.LocalBoneTransforms.end(); ++it) {
			AppendMatrix3x4(packet, *it);
		}
	}

	static size_t GetFrameEntityPacketSize(const Cs2RecordedEntity& entity) {
		return sizeof(int32_t) // Id
			+ sizeof(int32_t) // OwnerId
			+ sizeof(uint8_t) // Projectile
			+ sizeof(uint8_t) // Visible
			+ sizeof(uint8_t) // ViewModel
			+ sizeof(uint16_t) + entity.ClientClassName.size()
			+ 12 * sizeof(float) // Transform
			+ sizeof(uint8_t) // HasBones
			+ sizeof(uint32_t) // BoneCount
			+ entity.LocalBoneTransforms.size() * 12 * sizeof(float);
	}

	static void AppendHiddenIds(std::vector<unsigned char>& packet, const Cs2RecordedFrame& frame) {
		AppendU32(packet, (uint32_t)frame.HiddenEntityIds.size());
		for (std::vector<int>::const_iterator it = frame.HiddenEntityIds.begin(); it != frame.HiddenEntityIds.end(); ++it) {
			AppendI32(packet, *it);
		}
	}

	static void AppendBloodEvents(std::vector<unsigned char>& packet, const Cs2RecordedFrame& frame) {
		const size_t cappedSize = std::min<size_t>(frame.BloodEvents.size(), 65535);
		AppendU16(packet, (uint16_t)cappedSize);
		for (size_t i = 0; i < cappedSize; ++i) {
			const Cs2RecordedBloodEvent& bloodEvent = frame.BloodEvents[i];
			AppendI32(packet, bloodEvent.VictimEntityId);
			AppendFloat(packet, bloodEvent.Origin.x);
			AppendFloat(packet, bloodEvent.Origin.y);
			AppendFloat(packet, bloodEvent.Origin.z);
			AppendFloat(packet, bloodEvent.Normal.x);
			AppendFloat(packet, bloodEvent.Normal.y);
			AppendFloat(packet, bloodEvent.Normal.z);
			AppendFloat(packet, bloodEvent.Magnitude);
		}
	}

	static size_t GetBloodEventPacketSize() {
		return sizeof(int32_t)
			+ 7 * sizeof(float);
	}

	static void AppendShotEvents(std::vector<unsigned char>& packet, const Cs2RecordedFrame& frame) {
		const size_t cappedSize = std::min<size_t>(frame.ShotEvents.size(), 65535);
		AppendU16(packet, (uint16_t)cappedSize);
		for (size_t i = 0; i < cappedSize; ++i) {
			const Cs2RecordedShotEvent& shotEvent = frame.ShotEvents[i];
			const size_t cappedPelletCount = std::min<size_t>(shotEvent.Pellets.size(), 65535);
			AppendI32(packet, shotEvent.ShooterEntityId);
			AppendI32(packet, shotEvent.WeaponEntityId);
			AppendFloat(packet, shotEvent.Origin.x);
			AppendFloat(packet, shotEvent.Origin.y);
			AppendFloat(packet, shotEvent.Origin.z);
			AppendU16(packet, (uint16_t)cappedPelletCount);
			for (size_t pelletIndex = 0; pelletIndex < cappedPelletCount; ++pelletIndex) {
				const SOURCESDK::Vector& direction = shotEvent.Pellets[pelletIndex].Direction;
				AppendFloat(packet, direction.x);
				AppendFloat(packet, direction.y);
				AppendFloat(packet, direction.z);
			}
		}
	}

	static size_t GetShotEventsPacketSize(const Cs2RecordedFrame& frame) {
		size_t result = 0;
		const size_t cappedSize = std::min<size_t>(frame.ShotEvents.size(), 65535);
		for (size_t i = 0; i < cappedSize; ++i) {
			result += sizeof(int32_t) // ShooterEntityId
				+ sizeof(int32_t) // WeaponEntityId
				+ 3 * sizeof(float) // Origin
				+ sizeof(uint16_t); // PelletCount
			result += std::min<size_t>(frame.ShotEvents[i].Pellets.size(), 65535) * 3 * sizeof(float);
		}
		return result;
	}

	void ForgetHiddenSkeletons(const Cs2RecordedFrame& frame) {
		for (std::vector<int>::const_iterator it = frame.HiddenEntityIds.begin(); it != frame.HiddenEntityIds.end(); ++it) {
			m_SentSkeletons.erase(*it);
		}
	}

	bool SendPacket(const std::vector<unsigned char>& packet, const char* label) {
		if (packet.size() > kMaxPacketBytes) {
			WarnPacketTooLarge(packet.size());
			return false;
		}

		const int result = sendto(
			m_Socket,
			(const char*)packet.data(),
			(int)packet.size(),
			0,
			(const sockaddr*)&m_TargetSockAddr,
			sizeof(m_TargetSockAddr));
		if (result == SOCKET_ERROR) {
			WarnSocketError(label ? label : "sendto", WSAGetLastError());
			return false;
		}
		return true;
	}

	static void AppendHeader(std::vector<unsigned char>& packet, uint16_t packetType, uint32_t sequence) {
		AppendU8(packet, 'A');
		AppendU8(packet, 'F');
		AppendU8(packet, 'X');
		AppendU8(packet, 'L');
		AppendU16(packet, 12);
		AppendU16(packet, packetType);
		AppendU32(packet, sequence);
	}

	bool OpenSocket() {
		if (!m_WsaStarted) {
			WSADATA wsaData;
			int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
			if (wsaResult != 0) {
				WarnSocketError("WSAStartup", wsaResult);
				return false;
			}
			m_WsaStarted = true;
		}

		if (m_Socket == INVALID_SOCKET) {
			m_Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (m_Socket == INVALID_SOCKET) {
				WarnSocketError("socket", WSAGetLastError());
				return false;
			}
		}

		ConfigureTargetAddress();
		return true;
	}

	void CloseSocket() {
		if (m_Socket != INVALID_SOCKET) {
			closesocket(m_Socket);
			m_Socket = INVALID_SOCKET;
		}

		if (m_WsaStarted) {
			WSACleanup();
			m_WsaStarted = false;
		}
	}

	void ConfigureTargetAddress() {
		memset(&m_TargetSockAddr, 0, sizeof(m_TargetSockAddr));
		m_TargetSockAddr.sin_family = AF_INET;
		m_TargetSockAddr.sin_addr.s_addr = m_TargetAddress;
		m_TargetSockAddr.sin_port = htons((u_short)m_TargetPort);
	}

	static void AppendU8(std::vector<unsigned char>& packet, uint8_t value) {
		packet.push_back(value);
	}

	static void AppendU16(std::vector<unsigned char>& packet, uint16_t value) {
		AppendBytes(packet, &value, sizeof(value));
	}

	static void AppendU32(std::vector<unsigned char>& packet, uint32_t value) {
		AppendBytes(packet, &value, sizeof(value));
	}

	static void AppendI32(std::vector<unsigned char>& packet, int32_t value) {
		AppendBytes(packet, &value, sizeof(value));
	}

	static void AppendFloat(std::vector<unsigned char>& packet, float value) {
		AppendBytes(packet, &value, sizeof(value));
	}

	static void AppendString(std::vector<unsigned char>& packet, const std::string& value) {
		const size_t cappedSize = std::min<size_t>(value.size(), 65535);
		AppendU16(packet, (uint16_t)cappedSize);
		if (cappedSize > 0) {
			AppendBytes(packet, value.data(), cappedSize);
		}
	}

	static void AppendMatrix3x4(std::vector<unsigned char>& packet, const SOURCESDK::matrix3x4_t& value) {
		for (int row = 0; row < 3; ++row) {
			for (int col = 0; col < 4; ++col) {
				AppendFloat(packet, value[row][col]);
			}
		}
	}

	static void AppendBytes(std::vector<unsigned char>& packet, const void* data, size_t size) {
		const unsigned char* bytes = (const unsigned char*)data;
		packet.insert(packet.end(), bytes, bytes + size);
	}

	void WarnPacketTooLarge(size_t packetSize) {
		if (m_Sequence - m_LastLargePacketWarningSequence < 120) return;
		m_LastLargePacketWarningSequence = m_Sequence;
		advancedfx::Warning(
			"mirv_livelink: UDP packet too large (%zu bytes, max %zu). Try recordSpectated 1 and disable broad entity groups.\n",
			packetSize,
			(size_t)kMaxPacketBytes);
	}

	void WarnSocketError(const char* operation, int errorCode) {
		if (m_Sequence - m_LastSocketWarningSequence < 120) return;
		m_LastSocketWarningSequence = m_Sequence;
		advancedfx::Warning("mirv_livelink: UDP %s failed with error %d.\n", operation ? operation : "operation", errorCode);
	}
};

class Cs2AgrRecorder {
public:
	bool Start(const wchar_t* fileName) {
		Stop();
		m_NextId = 1;
		m_EntityIds.clear();
		m_VisibleLastFrame.clear();
		m_PendingBloodEvents.clear();
		m_PendingShotEvents.clear();
		return m_AgrSink.Start(fileName);
	}

	void Stop() {
		m_AgrSink.Stop();
		m_EntityIds.clear();
		m_VisibleLastFrame.clear();
		m_FpsAccumulator = 0.0;
		m_HasWrittenFrame = false;
		m_HasPendingSetupView = false;
		m_PendingBloodEvents.clear();
		m_PendingShotEvents.clear();
	}

	bool IsRecording() {
		return m_AgrSink.IsRecording();
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

	bool GetLiveDebug() const {
		return m_DebugSink.IsEnabled();
	}

	void SetLiveDebug(bool value) {
		m_DebugSink.SetEnabled(value);
	}

	int GetLiveDebugInterval() const {
		return m_DebugSink.GetPrintInterval();
	}

	void SetLiveDebugInterval(int value) {
		m_DebugSink.SetPrintInterval(value);
	}

	bool GetLiveUdp() const {
		return m_UdpSink.IsEnabled();
	}

	bool SetLiveUdp(bool value) {
		if (!value) {
			m_PendingBloodEvents.clear();
			m_PendingShotEvents.clear();
		}
		bool result = m_UdpSink.SetEnabled(value);
		if (result && value) {
			m_PendingBloodEvents.clear();
			m_PendingShotEvents.clear();
		}
		return result;
	}

	const std::string& GetLiveTargetHost() const {
		return m_UdpSink.GetTargetHost();
	}

	int GetLiveTargetPort() const {
		return m_UdpSink.GetTargetPort();
	}

	bool SetLiveTarget(const char* host, int port) {
		return m_UdpSink.SetTarget(host, port);
	}

	bool GetLiveRecordCamera() const {
		return m_LiveRecordCamera;
	}

	void SetLiveRecordCamera(bool value) {
		m_LiveRecordCamera = value;
	}

	bool GetLiveRecordPlayers() const {
		return m_LiveRecordPlayers;
	}

	void SetLiveRecordPlayers(bool value) {
		m_LiveRecordPlayers = value;
	}

	bool GetLiveRecordWeapons() const {
		return m_LiveRecordWeapons;
	}

	void SetLiveRecordWeapons(bool value) {
		m_LiveRecordWeapons = value;
	}

	bool GetLiveRecordViewModel() const {
		return m_LiveRecordViewModel;
	}

	void SetLiveRecordViewModel(bool value) {
		m_LiveRecordViewModel = value;
	}

	bool GetLiveRecordProjectiles() const {
		return m_LiveRecordProjectiles;
	}

	void SetLiveRecordProjectiles(bool value) {
		m_LiveRecordProjectiles = value;
	}

	bool GetLiveRecordSpectated() const {
		return m_LiveRecordSpectated;
	}

	void SetLiveRecordSpectated(bool value) {
		m_LiveRecordSpectated = value;
	}

	bool GetLiveOverrideFps() const {
		return m_LiveOverrideFps;
	}

	float GetLiveOverrideFpsValue() const {
		return m_LiveOverrideFpsValue;
	}

	void SetLiveOverrideFps(bool value) {
		m_LiveOverrideFps = value;
		m_LiveFpsAccumulator = 0.0;
		m_LiveHasWrittenFrame = false;
	}

	void SetLiveOverrideFpsValue(float value) {
		m_LiveOverrideFpsValue = value;
		m_LiveFpsAccumulator = 0.0;
		m_LiveHasWrittenFrame = false;
	}

	void OnSetupView(float frameTime, float x, float y, float z, float rx, float ry, float rz, float fov) {
		if (!HasActiveSink()) return;

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

	void QueueBloodEffect(unsigned char* effectData) {
		if (!m_UdpSink.IsEnabled() || !effectData) return;
		if (IsBadReadPtr(effectData + kCEffectDataOriginOffset, sizeof(SOURCESDK::Vector))) return;
		if (IsBadReadPtr(effectData + kCEffectDataNormalOffset, sizeof(SOURCESDK::Vector))) return;
		if (IsBadReadPtr(effectData + kCEffectDataEntityOffset, sizeof(uint32_t))) return;
		if (IsBadReadPtr(effectData + kCEffectDataMagnitudeOffset, sizeof(float))) return;

		const uint32_t rawEntityHandle = *(uint32_t*)(effectData + kCEffectDataEntityOffset);
		SOURCESDK::CS2::CBaseHandle entityHandle = SOURCESDK::CS2::CEntityHandle::CEntityHandle(rawEntityHandle);
		if (!entityHandle.IsValid()) return;

		const int entityEntryIndex = entityHandle.GetEntryIndex();
		CEntityInstance* entity = entityEntryIndex >= 0 && g_pEntityList && *g_pEntityList && g_GetEntityFromIndex
			? (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, entityEntryIndex)
			: nullptr;
		if (!entity) return;

		Cs2RecordedBloodEvent event;
		event.VictimEntityId = GetEntityId(entity);
		event.Origin = *(SOURCESDK::Vector*)(effectData + kCEffectDataOriginOffset);
		event.Normal = *(SOURCESDK::Vector*)(effectData + kCEffectDataNormalOffset);
		event.Magnitude = *(float*)(effectData + kCEffectDataMagnitudeOffset);
		if (!(event.Magnitude == event.Magnitude)) event.Magnitude = 0.0f;
		m_PendingBloodEvents.push_back(event);
	}

	void QueueShotTrace(CEntityInstance* shooterEntity, CEntityInstance* weaponEntity, const SOURCESDK::Vector& origin, const SOURCESDK::Vector& angles, float spreadX, float spreadY) {
		QueueShotEvent(shooterEntity, weaponEntity, origin, GetShotDirectionFromAnglesAndSpread(angles, spreadX, spreadY));
	}

	void OnMainRenderFrame() {
		if (!HasActiveSink()) return;
		if (!m_HasPendingSetupView) return;
		m_HasPendingSetupView = false;

		int controllerIndex = -1;
		if (!GetCurrentSpectatedControllerIndex(controllerIndex)) return;

		CEntityInstance* pawn = GetPawnFromControllerIndex(controllerIndex);
		if (!pawn) return;

		const bool agrActive = m_AgrSink.IsRecording();
		const float recordFrameTime = agrActive ? GetRecordFrameTime(m_PendingFrameTime) : GetLiveFrameTime(m_PendingFrameTime);
		if (recordFrameTime <= 0.0f) return;

		Cs2RecordedFrame frame;
		frame.FrameTime = recordFrameTime;
		std::set<int> visibleThisFrame;

		if (!agrActive && m_LiveRecordSpectated) {
			SampleEntity(pawn, false, false, visibleThisFrame, frame.Entities);
		}

		if (m_RecordPlayers || m_RecordWeapons || m_RecordProjectiles || (!agrActive && (m_LiveRecordPlayers || m_LiveRecordWeapons || m_LiveRecordProjectiles))) {
			const bool samplePlayers = agrActive ? m_RecordPlayers : m_LiveRecordPlayers;
			const bool sampleWeapons = agrActive ? m_RecordWeapons : m_LiveRecordWeapons;
			const bool sampleProjectiles = agrActive ? m_RecordProjectiles : m_LiveRecordProjectiles;
			int highestIndex = GetHighestEntityIndex();
			for (int i = 0; i <= highestIndex; ++i) {
				CEntityInstance* entity = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, i);
				if (!entity) continue;

				const char* debugName = entity->GetDebugName();
				if (samplePlayers && IsPlayerPawnForAgr(entity)) {
					SampleEntity(entity, false, false, visibleThisFrame, frame.Entities);
				} else if (sampleWeapons && IsRecordableWeaponEntity(entity)) {
					SampleEntity(entity, false, false, visibleThisFrame, frame.Entities);
				} else if (sampleProjectiles && debugName && StringEndsWithCaseSensitive(debugName, "_projectile")) {
					SampleEntity(entity, false, true, visibleThisFrame, frame.Entities);
				}
			}
		}

		if (agrActive ? m_RecordViewModel : m_LiveRecordViewModel) {
			std::vector<CEntityInstance*> hudModels;
			CollectHudModelOwnersForPawn(pawn, hudModels);
			for (CEntityInstance* hudModel : hudModels) {
				SampleEntity(hudModel, true, false, visibleThisFrame, frame.Entities, pawn);
			}
		}

		if (agrActive ? m_RecordCamera : m_LiveRecordCamera) {
			frame.HasCamera = true;
			frame.Camera.X = m_PendingCameraX;
			frame.Camera.Y = m_PendingCameraY;
			frame.Camera.Z = m_PendingCameraZ;
			frame.Camera.Rx = m_PendingCameraRx;
			frame.Camera.Ry = m_PendingCameraRy;
			frame.Camera.Rz = m_PendingCameraRz;
			frame.Camera.Fov = m_PendingCameraFov;
		}

		for (std::set<int>::iterator it = m_VisibleLastFrame.begin(); it != m_VisibleLastFrame.end(); ++it) {
			if (visibleThisFrame.find(*it) == visibleThisFrame.end()) {
				frame.HiddenEntityIds.push_back(*it);
			}
		}
		m_VisibleLastFrame.swap(visibleThisFrame);

		frame.BloodEvents.swap(m_PendingBloodEvents);
		frame.ShotEvents.swap(m_PendingShotEvents);

		DispatchFrame(frame);
	}

private:
	Cs2AgrSink m_AgrSink;
	Cs2DebugFrameSink m_DebugSink;
	Cs2UdpFrameSink m_UdpSink;
	bool m_RecordPlayers = true;
	bool m_RecordWeapons = true;
	bool m_RecordViewModel = true;
	bool m_RecordProjectiles = true;
	bool m_RecordCamera = true;
	bool m_LiveRecordCamera = true;
	bool m_LiveRecordPlayers = false;
	bool m_LiveRecordWeapons = false;
	bool m_LiveRecordViewModel = false;
	bool m_LiveRecordProjectiles = false;
	bool m_LiveRecordSpectated = true;
	bool m_OverrideFps = false;
	float m_OverrideFpsValue = 60.0f;
	bool m_LiveOverrideFps = true;
	float m_LiveOverrideFpsValue = 30.0f;
	double m_FpsAccumulator = 0.0;
	double m_LiveFpsAccumulator = 0.0;
	bool m_HasWrittenFrame = false;
	bool m_LiveHasWrittenFrame = false;
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
	std::map<unsigned char*, Cs2SkeletonMetadata> m_SkeletonCache;
	std::vector<SOURCESDK::matrix3x4_t> m_BoneWorldScratch;
	std::vector<Cs2RecordedBloodEvent> m_PendingBloodEvents;
	std::vector<Cs2RecordedShotEvent> m_PendingShotEvents;

	bool HasActiveSink() {
		return m_AgrSink.IsRecording() || m_DebugSink.IsEnabled() || m_UdpSink.IsEnabled();
	}

	void DispatchFrame(const Cs2RecordedFrame& frame) {
		if (m_AgrSink.IsRecording()) {
			m_AgrSink.OnFrame(frame);
		}
		if (m_DebugSink.IsEnabled()) {
			m_DebugSink.OnFrame(frame);
		}
		if (m_UdpSink.IsEnabled()) {
			m_UdpSink.OnFrame(frame);
		}
	}

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

	float GetLiveFrameTime(float sourceFrameTime) {
		if (!m_LiveOverrideFps) return sourceFrameTime;
		if (m_LiveOverrideFpsValue <= 0.0f) return 0.0f;

		const double interval = 1.0 / (double)m_LiveOverrideFpsValue;
		if (!m_LiveHasWrittenFrame) {
			m_LiveHasWrittenFrame = true;
			m_LiveFpsAccumulator = 0.0;
			return (float)interval;
		}

		if (sourceFrameTime > 0.0f) {
			m_LiveFpsAccumulator += (double)sourceFrameTime;
		}

		if (m_LiveFpsAccumulator + 0.000001 < interval) return 0.0f;

		m_LiveFpsAccumulator -= interval;
		if (m_LiveFpsAccumulator >= interval) {
			m_LiveFpsAccumulator = fmod(m_LiveFpsAccumulator, interval);
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

	static bool NearlySameVector(const SOURCESDK::Vector& a, const SOURCESDK::Vector& b, float epsilon) {
		return fabsf(a.x - b.x) <= epsilon
			&& fabsf(a.y - b.y) <= epsilon
			&& fabsf(a.z - b.z) <= epsilon;
	}

	static SOURCESDK::Vector NormalizeVector(const SOURCESDK::Vector& value) {
		const float length = sqrtf(value.x * value.x + value.y * value.y + value.z * value.z);
		if (length <= 0.000001f || !(length == length)) return SOURCESDK::Vector(1.0f, 0.0f, 0.0f);
		const float invLength = 1.0f / length;
		return SOURCESDK::Vector(value.x * invLength, value.y * invLength, value.z * invLength);
	}

	static SOURCESDK::Vector GetShotDirectionFromAnglesAndSpread(const SOURCESDK::Vector& angles, float spreadX, float spreadY) {
		const float pitch = angles.x * (float)(M_PI / 180.0);
		const float yaw = angles.y * (float)(M_PI / 180.0);
		const float roll = angles.z * (float)(M_PI / 180.0);
		const float sp = sinf(pitch);
		const float cp = cosf(pitch);
		const float sy = sinf(yaw);
		const float cy = cosf(yaw);
		const float sr = sinf(roll);
		const float cr = cosf(roll);

		const SOURCESDK::Vector forward(cp * cy, cp * sy, -sp);
		const SOURCESDK::Vector right((-sr * sp * cy) + (-cr * -sy), (-sr * sp * sy) + (-cr * cy), -sr * cp);
		const SOURCESDK::Vector up((cr * sp * cy) + (-sr * -sy), (cr * sp * sy) + (-sr * cy), cr * cp);
		return NormalizeVector(SOURCESDK::Vector(
			forward.x + right.x * spreadX + up.x * spreadY,
			forward.y + right.y * spreadX + up.y * spreadY,
			forward.z + right.z * spreadX + up.z * spreadY));
	}

	void QueueShotEvent(CEntityInstance* shooterEntity, CEntityInstance* weaponEntity, const SOURCESDK::Vector& origin, const SOURCESDK::Vector& direction) {
		if (!m_UdpSink.IsEnabled() || !shooterEntity) return;

		const int shooterEntityId = GetEntityId(shooterEntity);
		const int weaponEntityId = weaponEntity ? GetEntityId(weaponEntity) : -1;

		for (std::vector<Cs2RecordedShotEvent>::iterator it = m_PendingShotEvents.begin(); it != m_PendingShotEvents.end(); ++it) {
			if (it->ShooterEntityId == shooterEntityId
				&& it->WeaponEntityId == weaponEntityId
				&& NearlySameVector(it->Origin, origin, 0.001f)) {
				Cs2RecordedShotPellet pellet;
				pellet.Direction = direction;
				it->Pellets.push_back(pellet);
				return;
			}
		}

		Cs2RecordedShotEvent event;
		event.ShooterEntityId = shooterEntityId;
		event.WeaponEntityId = weaponEntityId;
		event.Origin = origin;
		Cs2RecordedShotPellet pellet;
		pellet.Direction = direction;
		event.Pellets.push_back(pellet);
		m_PendingShotEvents.push_back(event);
	}

	const Cs2SkeletonMetadata* GetSkeletonMetadata(
		unsigned char* modelImp,
		const char* modelName,
		unsigned char* boneNamesArray,
		int16_t* boneParentArray,
		uint32_t boneCount) {
		if (!modelImp || !modelName || !boneNamesArray || !boneParentArray || boneCount == 0 || boneCount > kMaxReasonableBoneCount) return nullptr;

		std::map<unsigned char*, Cs2SkeletonMetadata>::iterator it = m_SkeletonCache.find(modelImp);
		if (it != m_SkeletonCache.end()
			&& it->second.BoneCount == boneCount
			&& 0 == strcmp(it->second.ModelName.c_str(), modelName)) {
			return &it->second;
		}

		if (!IsLikelyPrintableAscii(modelName, 256) || !IsValidBoneParentArray(boneParentArray, boneCount)) return nullptr;

		Cs2SkeletonMetadata metadata;
		metadata.ModelImp = modelImp;
		metadata.ModelName = modelName;
		metadata.BoneCount = boneCount;
		metadata.BoneNames.reserve(boneCount);
		metadata.BoneParents.reserve(boneCount);
		metadata.BoneUsed.assign(boneCount, 1);

		for (uint32_t i = 0; i < boneCount; ++i) {
			const char* boneName = TryGetRecordingBoneNameFromArray(boneNamesArray, i);
			metadata.BoneNames.push_back(boneName ? boneName : "");
			metadata.BoneParents.push_back((int)boneParentArray[i]);
		}

		uint32_t* boneFlagsArray = nullptr;
		if (!IsBadReadPtr(modelImp + kModelBoneFlagsArrayOffset, sizeof(uint32_t*))) {
			boneFlagsArray = *(uint32_t**)(modelImp + kModelBoneFlagsArrayOffset);
			int knownCount = 0;
			int usedCount = 0;
			int zeroCount = 0;
			uint32_t unknownMask = 0;
			metadata.HasBoneFlags = IsLikelyBoneFlagsArray(boneFlagsArray, boneCount, knownCount, usedCount, zeroCount, unknownMask);
		}

		if (metadata.HasBoneFlags) {
			for (uint32_t i = 0; i < boneCount; ++i) {
				metadata.BoneUsed[i] = (boneFlagsArray[i] & kCs2BoneUsedByAnythingMask) != 0 ? 1 : 0;
			}

			for (uint32_t i = 0; i < boneCount; ++i) {
				if (!metadata.BoneUsed[i]) continue;
				int parent = metadata.BoneParents[i];
				while (parent >= 0 && parent < (int)boneCount && !metadata.BoneUsed[(uint32_t)parent]) {
					metadata.BoneUsed[(uint32_t)parent] = 1;
					parent = metadata.BoneParents[(uint32_t)parent];
				}
			}
		}

		m_SkeletonCache[modelImp] = metadata;
		return &m_SkeletonCache[modelImp];
	}

	bool SampleEntity(CEntityInstance* entity, bool viewModel, bool projectile, std::set<int>& visibleThisFrame, std::vector<Cs2RecordedEntity>& outEntities, CEntityInstance* ownerOverride = nullptr) {
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
		if (!visibleThisFrame.insert(id).second) return true;

		Cs2RecordedEntity sampledEntity;
		sampledEntity.Id = id;
		const char* clientClassName = entity ? entity->GetClientClassName() : nullptr;
		sampledEntity.ClientClassName = clientClassName ? clientClassName : "";
		sampledEntity.ModelName = baseModelName ? baseModelName : "";
		sampledEntity.Visible = visible;
		sampledEntity.ViewModel = viewModel;
		sampledEntity.Projectile = projectile;
		sampledEntity.Transform = entityTransform;

		if (ownerOverride && ownerOverride != entity) {
			sampledEntity.OwnerId = GetEntityId(ownerOverride);
		} else {
			SOURCESDK::CS2::CBaseHandle ownerHandle;
			if (TryReadEntityHandleField(entity, g_clientDllOffsets.C_BaseEntity.m_hOwnerEntity, ownerHandle) && ownerHandle.IsValid()) {
				int ownerEntryIndex = ownerHandle.GetEntryIndex();
				CEntityInstance* ownerEntity = ownerEntryIndex >= 0 && g_pEntityList && *g_pEntityList && g_GetEntityFromIndex
					? (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, ownerEntryIndex)
					: nullptr;
				if (ownerEntity && ownerEntity != entity) {
					sampledEntity.OwnerId = GetEntityId(ownerEntity);
				}
			}
		}

		unsigned char* sceneNode = nullptr;
		unsigned char* modelState = nullptr;
		unsigned char*** modelHandle = nullptr;
		unsigned char* modelImp = nullptr;
		const char* modelName = nullptr;
		unsigned char* boneNamesArray = nullptr;
		int16_t* boneParentArray = nullptr;
		uint32_t boneCount = 0;
		const Cs2SkeletonMetadata* skeleton = nullptr;
		bool hasBones = TryGetEntityModelInfo(entity, sceneNode, modelState, modelHandle, modelImp, modelName, boneNamesArray, boneParentArray, boneCount);
		if (hasBones) {
			skeleton = GetSkeletonMetadata(modelImp, modelName, boneNamesArray, boneParentArray, boneCount);
			hasBones = skeleton != nullptr;
		}

		if (hasBones) {
			m_BoneWorldScratch.resize(skeleton->BoneCount);
			for (uint32_t i = 0; i < boneCount; ++i) {
				if (skeleton->HasBoneFlags && !skeleton->BoneUsed[i]) {
					MatrixIdentity(m_BoneWorldScratch[i]);
					continue;
				}

				SOURCESDK::Vector origin;
				SOURCESDK::Quaternion angles;
				if (!entity->GetBone((int)i, origin, angles)) {
					hasBones = false;
					skeleton = nullptr;
					m_BoneWorldScratch.clear();
					break;
				}
				MatrixFromQuaternionPosition(angles, origin, m_BoneWorldScratch[i]);
			}
		}

		if (!hasBones) {
			outEntities.push_back(sampledEntity);
			return true;
		}

		sampledEntity.HasBones = true;
		sampledEntity.Skeleton = skeleton;
		sampledEntity.LocalBoneTransforms.reserve(skeleton->BoneCount);

		for (uint32_t i = 0; i < skeleton->BoneCount; ++i) {
			SOURCESDK::matrix3x4_t parentInverse;
			SOURCESDK::matrix3x4_t localBone;

			const bool boneIsUsed = !skeleton->HasBoneFlags || skeleton->BoneUsed[i] != 0;
			if (!boneIsUsed) {
				MatrixIdentity(localBone);
				sampledEntity.LocalBoneTransforms.push_back(localBone);
				continue;
			}

			int parentIndex = i < skeleton->BoneParents.size() ? skeleton->BoneParents[i] : -1;
			if (parentIndex >= 0 && (uint32_t)parentIndex < skeleton->BoneCount) {
				MatrixInvertRigid(m_BoneWorldScratch[(uint32_t)parentIndex], parentInverse);
			} else {
				MatrixInvertRigid(entityTransform, parentInverse);
			}
			MatrixConcat(parentInverse, m_BoneWorldScratch[i], localBone);
			sampledEntity.LocalBoneTransforms.push_back(localBone);
		}

		outEntities.push_back(sampledEntity);
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

static bool g_Cs2BloodEffectHookTried = false;

typedef void(__fastcall* Cs2BloodEffect_t)(unsigned char* effectData);
static Cs2BloodEffect_t g_Old_Cs2BloodEffect = nullptr;

static void __fastcall New_Cs2BloodEffect(unsigned char* effectData) {
	g_Cs2AgrRecorder.QueueBloodEffect(effectData);

	if (g_Old_Cs2BloodEffect) {
		g_Old_Cs2BloodEffect(effectData);
	}
}

void Cs2BloodEffect_Init(void* clientDll) {
	if (g_Cs2BloodEffectHookTried) return;
	g_Cs2BloodEffectHookTried = true;

	if (!clientDll) {
		advancedfx::Warning("cs2_bloodeffect: clientDll missing, hook disabled.\n");
		return;
	}

	Afx::BinUtils::ImageSectionsReader sections((HMODULE)clientDll);
	Afx::BinUtils::MemRange textRange = sections.GetMemRange();
	// CS blood impact wrapper from cstrike15/fx_cs_blood.cpp:
	//   void wrapper(CEffectData* data) {
	//     FX_CSBloodSpray(data->m_vStart, data->m_vOrigin, data->m_hEntity, data->m_vNormal, data->m_flMagnitude);
	//   }
	// Wrapper calls function showing strings like "particles/blood_impact/blood_impact_high.vpcf"
	// In Ghidra this is the tiny function that loads RCX+0x14, RCX+0x08, RCX+0x20,
	// [RCX+0x38], and [RCX+0x44], then tail-calls the larger blood particle selector.
	Afx::BinUtils::MemRange result = Afx::BinUtils::FindPatternString(
		textRange,
		"48 83 EC 38 4C 8B C1 4C 8D 49 20 48 8D 51 08 48 83 C1 14 F3 41 0F 10 40 44 45 8B 40 38 F3 0F 11 44 24 20 E8 ?? ?? ?? ?? 48 83 C4 38 C3");
	if (result.IsEmpty()) {
		ErrorBox(MkErrStr(__FILE__, __LINE__));
		return;
	}

	g_Old_Cs2BloodEffect = (Cs2BloodEffect_t)result.Start;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_Old_Cs2BloodEffect, New_Cs2BloodEffect);
	if (NO_ERROR != DetourTransactionCommit()) {
		g_Old_Cs2BloodEffect = nullptr;
		ErrorBox(MkErrStr(__FILE__, __LINE__));
		return;
	}
}

static bool g_Cs2FireBulletsHookTried = false;

typedef uint64_t(__fastcall* Cs2FXFireBulletTrace_t)(
	void* param1,
	const SOURCESDK::Vector* origin,
	void* angles,
	float param4,
	uint64_t param5,
	uint32_t param6,
	uint8_t param7,
	uint32_t param8,
	uint32_t bulletIndex,
	uint32_t param10,
	void* player,
	void* traceVector,
	float spreadX,
	float spreadY,
	void* weaponEntity,
	void* weaponData,
	uint32_t param17,
	int param18,
	uint64_t param19);
static Cs2FXFireBulletTrace_t g_Old_Cs2FXFireBulletTrace = nullptr;

static bool TryReadVectorPointer(const SOURCESDK::Vector* value, SOURCESDK::Vector& outValue) {
	outValue = SOURCESDK::Vector();
	if (!value || IsBadReadPtr(value, sizeof(SOURCESDK::Vector))) return false;
	outValue = *value;
	return true;
}

static uint64_t __fastcall New_Cs2FXFireBulletTrace(
	void* param1,
	const SOURCESDK::Vector* origin,
	void* angles,
	float param4,
	uint64_t param5,
	uint32_t param6,
	uint8_t param7,
	uint32_t param8,
	uint32_t bulletIndex,
	uint32_t param10,
	void* player,
	void* traceVector,
	float spreadX,
	float spreadY,
	void* weaponEntity,
	void* weaponData,
	uint32_t param17,
	int param18,
	uint64_t param19) {
	SOURCESDK::Vector originValue;
	SOURCESDK::Vector anglesValue;
	if (TryReadVectorPointer(origin, originValue) && TryReadVectorPointer((const SOURCESDK::Vector*)angles, anglesValue)) {
		g_Cs2AgrRecorder.QueueShotTrace((CEntityInstance*)player, (CEntityInstance*)weaponEntity, originValue, anglesValue, spreadX, spreadY);
	}

	if (g_Old_Cs2FXFireBulletTrace) {
		return g_Old_Cs2FXFireBulletTrace(
			param1,
			origin,
			angles,
			param4,
			param5,
			param6,
			param7,
			param8,
			bulletIndex,
			param10,
			player,
			traceVector,
			spreadX,
			spreadY,
			weaponEntity,
			weaponData,
			param17,
			param18,
			param19);
	}

	return 0;
}

void Cs2FireBullets_Init(void* clientDll) {
	if (g_Cs2FireBulletsHookTried) return;
	g_Cs2FireBulletsHookTried = true;

	if (!clientDll) {
		advancedfx::Warning("cs2_firebullets: clientDll missing, hook disabled.\n");
		return;
	}

	Afx::BinUtils::ImageSectionsReader sections((HMODULE)clientDll);
	Afx::BinUtils::MemRange textRange = sections.GetMemRange();
	// Per-bullet trace/effect routine called from FX_FireBullets after spread offsets are generated.
	// FX_FireBullets has "FX_FireBullets: " strings, with our function being called near the bottom:
	// FUN_180806ab0(plVar9[0x28d],&local_528,&local_4f8,uVar4,uVar1,4,uVar3,local_518,
	//               uVar19,uVar2,plVar9,local_408,
	//               *(undefined4 *)((longlong)local_448 + uVar17),
	//               *(undefined4 *)((longlong)local_488 + uVar17),param_2,lVar10,param_18,
	//               param_6,0,param_20);
	// It receives the shot origin, view angles, bullet index, and
	// per-bullet spread offsets before tracing/applying effects.
	Afx::BinUtils::MemRange traceResult = Afx::BinUtils::FindPatternString(
		textRange,
		"4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 56 48 8D AC 24 18 DB FF FF B8 E8 25 00 00 E8 ?? ?? ?? ?? 48 2B E0 F2 0F 10 02 4C 8D 4D 38 48 89 9C 24 E0 25 00 00");
	if (traceResult.IsEmpty()) {
		advancedfx::Warning("cs2_firebullets: failed to find per-bullet trace pattern.\n");
	}
	else {
		g_Old_Cs2FXFireBulletTrace = (Cs2FXFireBulletTrace_t)traceResult.Start;
	}

	if (!g_Old_Cs2FXFireBulletTrace) return;

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_Old_Cs2FXFireBulletTrace, New_Cs2FXFireBulletTrace);
	if (NO_ERROR != DetourTransactionCommit()) {
		g_Old_Cs2FXFireBulletTrace = nullptr;
		advancedfx::Warning("cs2_firebullets: failed to attach fire bullets hook.\n");
		return;
	}
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

CON_COMMAND(mirv_livelink, "Source 2 Live Link streaming") {
	int argc = args->ArgC();
	if (argc < 2) {
		advancedfx::Message(
			"mirv_livelink debug 0|1\n"
			"mirv_livelink udp 0|1\n"
			"mirv_livelink target <ip> <port>\n"
			"mirv_livelink recordCamera 0|1\n"
			"mirv_livelink recordPlayers 0|1\n"
			"mirv_livelink recordWeapons 0|1\n"
			"mirv_livelink recordViewmodel 0|1\n"
			"mirv_livelink recordProjectiles 0|1\n"
			"mirv_livelink recordSpectated 0|1\n"
			"mirv_livelink recordAll 0|1\n"
			"mirv_livelink debugInterval <iFrames>\n"
			"mirv_livelink fps default|<fValue>\n"
			"mirv_livelink status\n"
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

		advancedfx::Message("mirv_livelink %s 0|1\nCurrent value: %d\n", name, currentValue ? 1 : 0);
		return true;
	};

	if (handleBoolSetting("debug", g_Cs2AgrRecorder.GetLiveDebug(), &Cs2AgrRecorder::SetLiveDebug)) return;
	if (handleBoolSetting("recordCamera", g_Cs2AgrRecorder.GetLiveRecordCamera(), &Cs2AgrRecorder::SetLiveRecordCamera)) return;
	if (handleBoolSetting("recordPlayers", g_Cs2AgrRecorder.GetLiveRecordPlayers(), &Cs2AgrRecorder::SetLiveRecordPlayers)) return;
	if (handleBoolSetting("recordWeapons", g_Cs2AgrRecorder.GetLiveRecordWeapons(), &Cs2AgrRecorder::SetLiveRecordWeapons)) return;
	if (handleBoolSetting("recordViewmodel", g_Cs2AgrRecorder.GetLiveRecordViewModel(), &Cs2AgrRecorder::SetLiveRecordViewModel)) return;
	if (handleBoolSetting("recordViewModel", g_Cs2AgrRecorder.GetLiveRecordViewModel(), &Cs2AgrRecorder::SetLiveRecordViewModel)) return;
	if (handleBoolSetting("recordProjectiles", g_Cs2AgrRecorder.GetLiveRecordProjectiles(), &Cs2AgrRecorder::SetLiveRecordProjectiles)) return;
	if (handleBoolSetting("recordSpectated", g_Cs2AgrRecorder.GetLiveRecordSpectated(), &Cs2AgrRecorder::SetLiveRecordSpectated)) return;

	if (0 == _stricmp(cmd, "recordAll")) {
		if (argc >= 3) {
			const bool value = atoi(args->ArgV(2)) != 0;
			g_Cs2AgrRecorder.SetLiveRecordPlayers(value);
			g_Cs2AgrRecorder.SetLiveRecordWeapons(value);
			g_Cs2AgrRecorder.SetLiveRecordViewModel(value);
			g_Cs2AgrRecorder.SetLiveRecordProjectiles(value);
			g_Cs2AgrRecorder.SetLiveRecordSpectated(value);
			return;
		}

		advancedfx::Message(
			"mirv_livelink recordAll 0|1\n"
			"Current values: recordPlayers %d recordWeapons %d recordViewmodel %d recordProjectiles %d recordSpectated %d\n",
			g_Cs2AgrRecorder.GetLiveRecordPlayers() ? 1 : 0,
			g_Cs2AgrRecorder.GetLiveRecordWeapons() ? 1 : 0,
			g_Cs2AgrRecorder.GetLiveRecordViewModel() ? 1 : 0,
			g_Cs2AgrRecorder.GetLiveRecordProjectiles() ? 1 : 0,
			g_Cs2AgrRecorder.GetLiveRecordSpectated() ? 1 : 0);
		return;
	}

	if (0 == _stricmp(cmd, "udp")) {
		if (argc >= 3) {
			int value = atoi(args->ArgV(2));
			if (!g_Cs2AgrRecorder.SetLiveUdp(value != 0)) {
				advancedfx::Warning(
					"mirv_livelink: failed to enable UDP target %s:%d.\n",
					g_Cs2AgrRecorder.GetLiveTargetHost().c_str(),
					g_Cs2AgrRecorder.GetLiveTargetPort());
			}
			return;
		}

		advancedfx::Message("mirv_livelink udp 0|1\nCurrent value: %d\n", g_Cs2AgrRecorder.GetLiveUdp() ? 1 : 0);
		return;
	}

	if (0 == _stricmp(cmd, "target")) {
		if (argc >= 4) {
			int port = atoi(args->ArgV(3));
			if (!g_Cs2AgrRecorder.SetLiveTarget(args->ArgV(2), port)) {
				advancedfx::Warning("mirv_livelink: target must be an IPv4 address and port between 1 and 65535.\n");
			}
			return;
		}

		advancedfx::Message(
			"mirv_livelink target <ip> <port>\nCurrent value: %s:%d\n",
			g_Cs2AgrRecorder.GetLiveTargetHost().c_str(),
			g_Cs2AgrRecorder.GetLiveTargetPort());
		return;
	}

	if (0 == _stricmp(cmd, "fps")) {
		if (argc >= 3) {
			const char* valueArg = args->ArgV(2);
			if (0 == _stricmp(valueArg, "default")) {
				g_Cs2AgrRecorder.SetLiveOverrideFps(false);
				return;
			}

			char* endPtr = nullptr;
			double value = strtod(valueArg, &endPtr);
			if (endPtr && *endPtr == '\0' && value > 0.0 && value <= 1000.0) {
				g_Cs2AgrRecorder.SetLiveOverrideFpsValue((float)value);
				g_Cs2AgrRecorder.SetLiveOverrideFps(true);
				return;
			}

			advancedfx::Warning("mirv_livelink: fps must be default or a value greater than 0 and at most 1000.\n");
			return;
		}

		advancedfx::Message("mirv_livelink fps default|<fValue>\n");
		if (g_Cs2AgrRecorder.GetLiveOverrideFps()) {
			advancedfx::Message("Current value: %f\n", g_Cs2AgrRecorder.GetLiveOverrideFpsValue());
		} else {
			advancedfx::Message("Current value: default\n");
		}
		return;
	}

	if (0 == _stricmp(cmd, "debugInterval")) {
		if (argc >= 3) {
			int value = atoi(args->ArgV(2));
			if (value < 1 || value > 10000) {
				advancedfx::Warning("mirv_livelink: debugInterval must be between 1 and 10000.\n");
				return;
			}

			g_Cs2AgrRecorder.SetLiveDebugInterval(value);
			return;
		}

		advancedfx::Message("mirv_livelink debugInterval <iFrames>\nCurrent value: %d\n", g_Cs2AgrRecorder.GetLiveDebugInterval());
		return;
	}

	if (0 == _stricmp(cmd, "status")) {
		if (g_Cs2AgrRecorder.GetLiveOverrideFps()) {
			advancedfx::Message("mirv_livelink: fps %f.\n", g_Cs2AgrRecorder.GetLiveOverrideFpsValue());
		} else {
			advancedfx::Message("mirv_livelink: fps default.\n");
		}
		advancedfx::Message(
			"mirv_livelink: debug %d udp %d target %s:%d debugInterval %d.\n",
			g_Cs2AgrRecorder.GetLiveDebug() ? 1 : 0,
			g_Cs2AgrRecorder.GetLiveUdp() ? 1 : 0,
			g_Cs2AgrRecorder.GetLiveTargetHost().c_str(),
			g_Cs2AgrRecorder.GetLiveTargetPort(),
			g_Cs2AgrRecorder.GetLiveDebugInterval());
		advancedfx::Message(
			"mirv_livelink: recordCamera %d recordPlayers %d recordWeapons %d recordViewmodel %d recordProjectiles %d recordSpectated %d.\n",
			g_Cs2AgrRecorder.GetLiveRecordCamera() ? 1 : 0,
			g_Cs2AgrRecorder.GetLiveRecordPlayers() ? 1 : 0,
			g_Cs2AgrRecorder.GetLiveRecordWeapons() ? 1 : 0,
			g_Cs2AgrRecorder.GetLiveRecordViewModel() ? 1 : 0,
			g_Cs2AgrRecorder.GetLiveRecordProjectiles() ? 1 : 0,
			g_Cs2AgrRecorder.GetLiveRecordSpectated() ? 1 : 0);
		return;
	}

	advancedfx::Warning("mirv_livelink: unknown command \"%s\".\n", cmd);
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
			if (weapons) {
				if (!IsRecordableWeaponEntity(entity)) continue;
			} else {
				if (!debugName) continue;
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
