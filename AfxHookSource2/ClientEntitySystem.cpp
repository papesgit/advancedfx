#include "stdafx.h"

#include "ClientEntitySystem.h"
#include "DeathMsg.h"
#include "WrpConsole.h"

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"
#include "../shared/FFITools.h"
#include "../shared/StringTools.h"

#include "AfxHookSource2Rs.h"
#include "SchemaSystem.h"

#define WIN32_LEAN_AND_MEAN
#include "../deps/release/Detours/src/detours.h"

#include <map>
#include <algorithm>

void ** g_pEntityList = nullptr;
GetHighestEntityIndex_t  g_GetHighestEntityIndex = nullptr;
GetEntityFromIndex_t g_GetEntityFromIndex = nullptr;

/*
cl_track_render_eye_angles 1
cl_ent_absbox 192
cl_ent_viewoffset 192
*/

// CEntityInstance: Root class for all entities
// Retrieved from script function.
const char * CEntityInstance::GetName() {
	const char * pszName = (const char*)*(unsigned char**)(*(unsigned char**)((unsigned char*)this + 0x10) + 0x18);
	if(pszName) return pszName;
	return "";
}

// Retrieved from script function.
// can return nullptr!
const char * CEntityInstance::GetDebugName() {
	const char * pszName = (const char*)*(unsigned char**)(*(unsigned char**)((unsigned char*)this + 0x10) + 0x18);
	if(pszName) return pszName;
	return **(const char***)(*(unsigned char**)(*(unsigned char**)((unsigned char*)this + 0x10) + 0x8)+0x30);
}

// Retrieved from script function.
const char * CEntityInstance::GetClassName() {
	const char * pszName = (const char*)*(unsigned char**)(*(unsigned char**)((unsigned char*)this + 0x10) + 0x20);
	if(pszName) return pszName;
	return "";
}

extern HMODULE g_H_ClientDll;

// Retrieved from script function.
const char * CEntityInstance::GetClientClassName() {
    // GetClientClass function.
    // find it by searching for 4th full-ptr ref to "C_PlantedC4" subtract sizeof(void*) (0x8) and search function that references this struct.
    // you need to search for raw bytes, GiHidra doesn't seem to find the reference.
    void * pClientClass = ((void * (__fastcall *)(void *)) (*(void***)this)[40]) (this);

    if(pClientClass) {
        return *(const char**)((unsigned char*)pClientClass + 0x10);
    }
    return nullptr;
}

// Retrieved from script function.
// GetEntityHandle ...

bool CEntityInstance::IsPlayerPawn() {
	// See cl_ent_text drawing function.
	return ((bool (__fastcall *)(void *)) (*(void***)this)[155]) (this);
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetPlayerPawnHandle() {
	// See cl_ent_text drawing function.
	if(!IsPlayerController())  return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int *)((unsigned char *)this + g_clientDllOffsets.CBasePlayerController.m_hPawn));
}

bool CEntityInstance::IsPlayerController() {
	// See cl_ent_text drawing function. Near "Pawn: (%d) Name: %s".
	return ((bool (__fastcall *)(void *)) (*(void***)this)[156]) (this);    
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetPlayerControllerHandle() {
	// See cl_ent_text drawing function.
	if(!IsPlayerPawn())  return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int *)((unsigned char *)this + g_clientDllOffsets.C_BasePlayerPawn.m_hController));
}

unsigned int CEntityInstance::GetHealth() {
	// See cl_ent_text drawing function. Near "Health: %d\n".
	return *(unsigned int *)((unsigned char *)this + 0x34c);
}

int CEntityInstance::GetTeam() {
    return *(int*)((u_char*)(this) + g_clientDllOffsets.C_BaseEntity.m_iTeamNum);
}


/**
 * @remarks FLOAT_MAX if invalid
 */
void CEntityInstance::GetOrigin(float & x, float & y, float & z) {
    void * ptr = *(void **)((unsigned char *)this + 0x330);
	// See cl_ent_text drawing function. Near "Position: %0.3f, %0.3f, %0.3f\n" or cl_ent_viewoffset related function.
	x =  (*(float *)((unsigned char *)ptr + 0xd0));
	y =  (*(float *)((unsigned char *)ptr + 0xd4));
	z =  (*(float *)((unsigned char *)ptr + 0xd8));
}

void CEntityInstance::GetRenderEyeOrigin(float outOrigin[3]) {
	// GetRenderEyeAngles vtable offset minus 2
	((void (__fastcall *)(void *,float outOrigin[3])) (*(void***)this)[170]) (this,outOrigin);
}

void CEntityInstance::GetRenderEyeAngles(float outAngles[3]) {
	// See cl_track_render_eye_angles. Near "Render eye angles: %.7f, %.7f, %.7f\n".
	((void (__fastcall *)(void *,float outAngles[3])) (*(void***)this)[171]) (this,outAngles);
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetViewEntityHandle() {
	if (!IsPlayerPawn())  return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
    void * pCameraServices = *(void**)((unsigned char*)this + g_clientDllOffsets.C_BasePlayerPawn.m_pCameraServices);
    if(nullptr == pCameraServices) return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int*)((unsigned char*)pCameraServices + g_clientDllOffsets.CPlayer_CameraServices.m_hViewEntity));
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetActiveWeaponHandle() {
	if (!IsPlayerPawn())  return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
    void * pWeaponServices = *(void**)((unsigned char*)this + g_clientDllOffsets.C_BasePlayerPawn.m_pWeaponServices);
    if(nullptr == pWeaponServices) return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int*)((unsigned char*)pWeaponServices + g_clientDllOffsets.CPlayer_WeaponServices.m_hActiveWeapon));
}

const char * CEntityInstance::GetPlayerName(){
    if (!IsPlayerController()) return nullptr;
    return *(const char **)((u_char*)(this) + g_clientDllOffsets.CBasePlayerController.m_iszPlayerName);
}

uint64_t CEntityInstance::GetSteamId(){
    if (!IsPlayerController())  return 0;
    return *(uint64_t*)((u_char*)(this) + g_clientDllOffsets.CBasePlayerController.m_steamID);
}

const char * CEntityInstance::GetSanitizedPlayerName() {
   if (!IsPlayerController()) return nullptr;
    return *(const char **)((u_char*)(this) + g_clientDllOffsets.CCSPlayerController.m_sSanitizedPlayerName);

}

uint8_t CEntityInstance::GetObserverMode() {
	if (!IsPlayerPawn()) return 0;
    void * pObserverServices = *(void**)((unsigned char*)this + g_clientDllOffsets.C_BasePlayerPawn.m_pObserverServices);
    if(nullptr == pObserverServices) return 0;
	return *(uint8_t*)((unsigned char*)pObserverServices + g_clientDllOffsets.CPlayer_ObserverServices.m_iObserverMode);    
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetObserverTarget() {
	if (!IsPlayerPawn())  return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
    void * pObserverServices = *(void**)((unsigned char*)this + g_clientDllOffsets.C_BasePlayerPawn.m_pObserverServices);
    if(nullptr == pObserverServices) return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int*)((unsigned char*)pObserverServices + g_clientDllOffsets.CPlayer_ObserverServices.m_hObserverTarget));    
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetHandle() {
	if (auto pEntityIdentity = *(u_char**)((u_char*)this + g_clientDllOffsets.CEntityInstance.m_pEntity)) {
		return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(uint32_t*)(pEntityIdentity + 0x10));
	}

	return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
}

class CAfxEntityInstanceRef {
public:
    static CAfxEntityInstanceRef * Aquire(CEntityInstance * pInstance) {
        CAfxEntityInstanceRef * pRef;
        auto it = m_Map.find(pInstance);
        if(it != m_Map.end()) {    
            pRef = it->second;
        } else {
            pRef = new CAfxEntityInstanceRef(pInstance);
            m_Map[pInstance] = pRef;
        }
        pRef->AddRef();
        return pRef;
    }

    static void Invalidate(CEntityInstance * pInstance) {
        if(m_Map.empty()) return;
        auto it = m_Map.find(pInstance);
        if(it != m_Map.end()) {
            auto & pInstance = it->second;
            pInstance->m_pInstance = nullptr;
            m_Map.erase(it);
        }        
    }

    CEntityInstance * GetInstance() {
        return m_pInstance;
    }

    bool IsValid() {
        return nullptr != m_pInstance;
    }

    void AddRef() {
        m_RefCount++;
    }

    void Release() {
        m_RefCount--;
        if(0 == m_RefCount) {
            delete this;
        }
    }

protected:
    CAfxEntityInstanceRef(class CEntityInstance * pInstance)
    : m_pInstance(pInstance)
    {
    }

    ~CAfxEntityInstanceRef() {
        m_Map.erase(m_pInstance);
    }

private:
    int m_RefCount = 0;
    class CEntityInstance * m_pInstance;
    static std::map<CEntityInstance *,CAfxEntityInstanceRef *> m_Map;
};

std::map<CEntityInstance *,CAfxEntityInstanceRef *> CAfxEntityInstanceRef::m_Map;


typedef void* (__fastcall * OnAddEntity_t)(void* This, CEntityInstance* pInstance, SOURCESDK::uint32 handle);
OnAddEntity_t g_Org_OnAddEntity = nullptr;


void* __fastcall New_OnAddEntity(void* This, CEntityInstance* pInstance, SOURCESDK::uint32 handle) {

    void * result =  g_Org_OnAddEntity(This,pInstance,handle);

    if(g_b_on_add_entity && pInstance) {
        auto pRef = CAfxEntityInstanceRef::Aquire(pInstance);
        AfxHookSource2Rs_Engine_OnAddEntity(pRef,handle);
        pRef->Release();
    }

    return result;
}

typedef void* (__fastcall * OnRemoveEntity_t)(void* This, CEntityInstance* inst, SOURCESDK::uint32 handle);
OnRemoveEntity_t g_Org_OnRemoveEntity = nullptr;

void* __fastcall New_OnRemoveEntity(void* This, CEntityInstance* pInstance, SOURCESDK::uint32 handle) {

    if(g_b_on_remove_entity && pInstance) {
        auto pRef = CAfxEntityInstanceRef::Aquire(pInstance);
        AfxHookSource2Rs_Engine_OnRemoveEntity(pRef,handle);
        pRef->Release();
    }

    CAfxEntityInstanceRef::Invalidate(pInstance);

    void * result =  g_Org_OnRemoveEntity(This,pInstance,handle);
    return result;
}

#define STRINGIZE(x) STRINGIZE2(x)
#define STRINGIZE2(x) #x
#define MkErrStr(file,line) "Problem in " file ":" STRINGIZE(line)
extern void ErrorBox(char const * messageText);

bool Hook_ClientEntitySystem( void* pEntityList, void * pFnGetHighestEntityIterator, void * pFnGetEntityFromIndex ) {
    static bool firstResult = false;
    static bool firstRun = true;

    if(firstRun) {
        firstRun = false;
        g_pEntityList = (void**)pEntityList;
        g_GetHighestEntityIndex = (GetHighestEntityIndex_t)pFnGetHighestEntityIterator;
        g_GetEntityFromIndex = (GetEntityFromIndex_t)pFnGetEntityFromIndex;
        firstResult = true;
    }

    return firstResult;
}

bool Hook_ClientEntitySystem2() {
    static bool firstResult = false;
    static bool firstRun = true;

    if(g_pEntityList && *g_pEntityList) {
        // https://github.com/bruhmoment21/cs2-sdk
        void ** vtable = **(void****)g_pEntityList;
        g_Org_OnAddEntity = (OnAddEntity_t)vtable[15];
        g_Org_OnRemoveEntity = (OnRemoveEntity_t)vtable[16];
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_Org_OnAddEntity, New_OnAddEntity);
        DetourAttach(&(PVOID&)g_Org_OnRemoveEntity, New_OnRemoveEntity);
        firstResult = NO_ERROR == DetourTransactionCommit();
    }

    return firstResult;    
}

int GetHighestEntityIndex() {
    return 2048; // Hardcoded for now, because the function we have is the count, not the index and we need to change mirv-script API to support that better.
    //return g_pEntityList && g_GetHighestEntityIndex ? g_GetHighestEntityIndex(*g_pEntityList, false) : -1;
}

struct MirvEntityEntry {
	int entryIndex;
	int handle;
	std::string debugName;
	std::string className;
	std::string clientClassName;
	SOURCESDK::Vector origin;
	SOURCESDK::QAngle angles;
};

CON_COMMAND(mirv_listentities, "List entities.")
{
	auto argC = args->ArgC();
	auto arg0 = args->ArgV(0);

	bool filterPlayers = false;
	bool sortByDistance = false;
	int printCount = -1;

	if (2 <= argC && 0 == _stricmp(args->ArgV(1), "help")) {
		advancedfx::Message(
			"%s help - Print this help.\n"
			"%s <option1> <option2> ... - Customize printed output with options.\n"
			"Where <option> is (you don't have to use all):\n"
			"\t\"isPlayer=1\" - Show only player related entities. Unless you need handles, the \"mirv_deathmsg help players\" might be more useful.\n"
			"\t\"sort=distance\" - Sort entities by distance relative to current position, from closest to most distant.\n"
			"\t\"limit=<i>\" - Limit number of printed entries.\n"
			"Example:\n"
			"%s sort=distance limit=10\n" 
			, arg0, arg0, arg0
		);
		return;
	} else {
		for (int i = 1; i < argC; i++) {
			const char * argI = args->ArgV(i);
			if (StringIBeginsWith(argI, "limit=")) {
				printCount = atoi(argI + strlen("limit="));
			} 
			else if (StringIBeginsWith(argI, "sort=")) {
				if (0 == _stricmp(argI + strlen("sort="), "distance")) sortByDistance = true;
			}
			else if (0 == _stricmp(argI, "isPlayer=1")) {
				filterPlayers = true;
			}
		}
	}

	std::vector<MirvEntityEntry> entries;

    int highestIndex = GetHighestEntityIndex();
    for(int i = 0; i < highestIndex + 1; i++) {
        if(auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList,i)) {
			if (filterPlayers && !ent->IsPlayerController() && !ent->IsPlayerPawn()) continue;
			
            float render_origin[3];
            float render_angles[3];
            ent->GetRenderEyeOrigin(render_origin);
            ent->GetRenderEyeAngles(render_angles);

			auto debugName = ent->GetDebugName();
			auto className = ent->GetClassName();
			auto clientClassName = ent->GetClientClassName();

			entries.emplace_back(
				MirvEntityEntry {
					i, ent->GetHandle().ToInt(), 
					debugName ? debugName : "", className ? className : "", clientClassName ? clientClassName : "",
					SOURCESDK::Vector {render_origin[0], render_origin[1], render_origin[2]},
					SOURCESDK::QAngle {render_angles[0], render_angles[1], render_angles[2]} 
				}
			);

        }
    }

	if (sortByDistance) {
		SOURCESDK::Vector curPos = {(float)g_CurrentGameCamera.origin[0], (float)g_CurrentGameCamera.origin[1], (float)g_CurrentGameCamera.origin[2]};

		std::sort(entries.begin(), entries.end(), [&](MirvEntityEntry & a, MirvEntityEntry & b) {
			auto distA = (curPos - a.origin).LengthSqr();
			auto distB = (curPos - b.origin).LengthSqr();
			return distA < distB;
		});
	}

	advancedfx::Message("entryIndex / handle / debugName / className / clientClassName / [ x , y , z , rX , rY , rZ ]\n");
	if (printCount == -1) printCount = entries.size();
	for (int i = 0; i < printCount; i++) {
		auto e = entries[i];
		advancedfx::Message("%i / %i / %s / %s / %s / [ %f , %f , %f , %f , %f , %f ]\n"
			, e.entryIndex, e.handle
			, e.debugName.c_str(), e.className.c_str(), e.clientClassName.c_str()
			, e.origin.x, e.origin.y, e.origin.z 
			, e.angles.x, e.angles.y, e.angles.z
		);
	}
}

extern "C" int afx_hook_source2_get_highest_entity_index() {
    int highestIndex = GetHighestEntityIndex();
    return highestIndex;
}

extern "C" void * afx_hook_source2_get_entity_ref_from_index(int index) {
    if(CEntityInstance * result = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList,index)) {
        return CAfxEntityInstanceRef::Aquire(result);
    }
    return nullptr;
}

extern "C" void afx_hook_source2_add_ref_entity_ref(void * pRef) {
    ((CAfxEntityInstanceRef *)pRef)->AddRef();
}

extern "C" void afx_hook_source2_release_entity_ref(void * pRef) {
    ((CAfxEntityInstanceRef *)pRef)->Release();
}

extern "C" FFIBool afx_hook_source2_get_entity_ref_is_valid(void * pRef) {
    return BOOL_TO_FFIBOOL(((CAfxEntityInstanceRef *)pRef)->IsValid());
}

extern "C" const char * afx_hook_source2_get_entity_ref_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetName();
    }
    return "";
}

extern "C" const char * afx_hook_source2_get_entity_ref_debug_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetDebugName();
    }
    return nullptr;
}

extern "C" const char * afx_hook_source2_get_entity_ref_class_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetClassName();
    }
    return "";
}

extern "C" const char * afx_hook_source2_get_entity_ref_client_class_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetClientClassName();
    }
    return "";
}

extern "C" FFIBool afx_hook_source2_get_entity_ref_is_player_pawn(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return BOOL_TO_FFIBOOL(pInstance->IsPlayerPawn());
    }
    return FFIBOOL_FALSE;
}

extern "C" int afx_hook_source2_get_entity_ref_player_pawn_handle(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetPlayerPawnHandle().ToInt();
    }
    return SOURCESDK_CS2_INVALID_EHANDLE_INDEX;    
}

extern "C" FFIBool afx_hook_source2_get_entity_ref_is_player_controller(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return BOOL_TO_FFIBOOL(pInstance->IsPlayerController());
    }
    return FFIBOOL_FALSE;    
}

extern "C" int afx_hook_source2_get_entity_ref_player_controller_handle(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetPlayerControllerHandle().ToInt();
    }
    return SOURCESDK_CS2_INVALID_EHANDLE_INDEX;  
}

extern "C" int afx_hook_source2_get_entity_ref_health(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetHealth();
    }
    return 0;    
}

extern "C" int afx_hook_source2_get_entity_ref_team(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetTeam();
    }
    return 0;    
}


extern "C" void afx_hook_source2_get_entity_ref_origin(void * pRef, float & x, float & y, float & z) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       pInstance->GetOrigin(x,y,z);
    }    
}

extern "C" void afx_hook_source2_get_entity_ref_render_eye_origin(void * pRef, float & x, float & y, float & z) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        float tmp[3];
       pInstance->GetRenderEyeOrigin(tmp);
       x = tmp[0];
       y = tmp[1];
       z = tmp[2];
    }    
}

extern "C" void afx_hook_source2_get_entity_ref_render_eye_angles(void * pRef, float & x, float & y, float & z) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        float tmp[3];
       pInstance->GetRenderEyeAngles(tmp);
       x = tmp[0];
       y = tmp[1];
       z = tmp[2];
    }    
}

extern "C" int afx_hook_source2_get_entity_ref_view_entity_handle(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetViewEntityHandle().ToInt();
    }
    return SOURCESDK_CS2_INVALID_EHANDLE_INDEX;
}

extern "C" int afx_hook_source2_get_entity_ref_active_weapon_handle(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetActiveWeaponHandle().ToInt();
    }
    return SOURCESDK_CS2_INVALID_EHANDLE_INDEX;
}

extern "C" const char* afx_hook_source2_get_entity_ref_player_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetPlayerName();
    }
    return nullptr;
}

extern "C" uint64_t afx_hook_source2_get_entity_ref_steam_id(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetSteamId();
    }
    return 0;
}

extern "C" const char* afx_hook_source2_get_entity_ref_sanitized_player_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetSanitizedPlayerName();
    }
    return nullptr;
}

typedef CEntityInstance *  (__fastcall * ClientDll_GetSplitScreenPlayer_t)(int slot);
ClientDll_GetSplitScreenPlayer_t g_ClientDll_GetSplitScreenPlayer = nullptr;

bool Hook_GetSplitScreenPlayer( void* pAddr) {
    g_ClientDll_GetSplitScreenPlayer = (ClientDll_GetSplitScreenPlayer_t)pAddr;
    return true;
}

extern "C" void * afx_hook_source2_get_entity_ref_from_split_screen_player(int index) {
    if(0 == index && g_ClientDll_GetSplitScreenPlayer) {
        if(CEntityInstance * result = g_ClientDll_GetSplitScreenPlayer(index)) {
            return CAfxEntityInstanceRef::Aquire(result);
        }
    }
    return nullptr;
}

extern "C" uint8_t afx_hook_source2_get_entity_ref_observer_mode(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetObserverMode();
    }
    return 0;
}

extern "C" int afx_hook_source2_get_entity_ref_observer_target_handle(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetObserverTarget().ToInt();
    }
    return SOURCESDK_CS2_INVALID_EHANDLE_INDEX;
}

// Attachment helpers

namespace {
    // Helper: check if a pointer lies in client.dll executable sections.
    static bool IsClientCodePtr(void* p) {
        if(!p) return false;
        if(!g_H_ClientDll) return false;
        Afx::BinUtils::ImageSectionsReader reader(g_H_ClientDll);
        while(!reader.Eof()) {
            reader.Next(IMAGE_SCN_MEM_EXECUTE);
            if(reader.Eof()) break;
            auto r = reader.GetMemRange();
            if(r.Start <= (size_t)p && (size_t)p < r.End) return true;
        }
        return false;
    }
    typedef char  (__fastcall * GetAttachment_t)(void* pCtx, unsigned char idx1Based, void* pOut, void* pTmp);
    typedef void (__fastcall * ResolveAttachmentName_t)(void* pCtxOrEnt, unsigned char* pOutIdx, const char* name);

    static GetAttachment_t                    g_pGetAttachment   = nullptr;    // FUN_180615a30
    static ResolveAttachmentName_t            g_pResolveName     = nullptr;    // FUN_1806243d0 (optional)

    // Calls owner vfunc at +0x1B8 to get the attachment context.
    static void* GetAttachmentContext(void* pEnt) {
        if(!pEnt) return nullptr; void** vtbl = *(void***)pEnt;
        // 0x1B8 / 8 = 55
        void* candidate = vtbl[55];
        if(!IsClientCodePtr(candidate)) return nullptr;
        auto fn = (void* (__fastcall*)(void*))candidate;
        return fn ? fn(pEnt) : nullptr;
    }

    // Helper to print a one-time message for missing resolver pieces.
    static void PrintResolverHintOnce(const char* which) {
        static bool once = false;
        if(once) return; once = true;
        advancedfx::Message("[CS2 Attachments] Resolver missing: %s\n", which);
    }
}

bool CS2_GetAttachmentPosAngByIndex(void* pEntity, int idx1Based, float outPos[3], float outAng[3])
{
    if(!g_pGetAttachment) return false;
    if(!pEntity || idx1Based <= 0 || idx1Based > 255 || !outPos || !outAng) return false;
    void* ctx = GetAttachmentContext(pEntity);
    if(!ctx) return false;
    struct OutBuf { float x; float y; float z; } outBuf = {0};
    float eul[4] = {0,0,0,0};
    char ok = g_pGetAttachment(ctx, (unsigned char)idx1Based, &outBuf, eul);
    if(!ok) return false;
    outPos[0] = outBuf.x; outPos[1] = outBuf.y; outPos[2] = outBuf.z;
    outAng[0] = eul[0]; outAng[1] = eul[1]; outAng[2] = eul[2];
    return true;
}

int CS2_LookupAttachmentIndex(void* pEntity, const char* name)
{
    if(!pEntity || !name || !*name) return 0;

    void* ctx = GetAttachmentContext(pEntity);
    if(!ctx) return 0;

    if(g_pResolveName) {
        unsigned char outIdx = 0;
        g_pResolveName(ctx, &outIdx, name);
        if(outIdx) {
            return (int)outIdx;
        } else {
            advancedfx::Message("[CS2 Attachments] g_pResolveName present but returned 0 for '%s', falling back.\n", name);
        }
    }
    unsigned char outIdx = 0;
    return (int)outIdx; // 1-based; 0 means not found
}

void CS2_Attachments_SetupResolvers(
    void* pGetAttachment,
    void* pResolveName
)
{
    g_pGetAttachment   = (GetAttachment_t)pGetAttachment;
    g_pResolveName     = (ResolveAttachmentName_t)pResolveName;
}
CON_COMMAND(_mirv_debug_attachment_addrs, "Print CS2 attachment resolver addresses")
{
    advancedfx::Message(
        "[CS2 Attachments] get=%p resolveName=%p\n",
        g_pGetAttachment, g_pResolveName
    );
}

// Debug console commands
CON_COMMAND(_mirv_debug_attachment_index, "Print world pos+angles for entity attachment index (CS2)")
{
    int argc = args->ArgC();
    if(argc < 3) {
        advancedfx::Message("Usage: %s <entIndex> <idx1Based>\n", args->ArgV(0));
        return;
    }
    int entIndex = atoi(args->ArgV(1));
    int idx = atoi(args->ArgV(2));

    if(!g_pEntityList || !g_GetEntityFromIndex) {
        advancedfx::Message("Entity system not initialized.\n");
        return;
    }
    auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, entIndex);
    if(!ent) { advancedfx::Message("Entity %d not found.\n", entIndex); return; }

    float pos[3]; float ang[3];
    if(CS2_GetAttachmentPosAngByIndex(ent, idx, pos, ang)) {
        advancedfx::Message("ent=%d idx=%d pos=(%g,%g,%g) angles=(%g,%g,%g)\n", entIndex, idx, pos[0], pos[1], pos[2], ang[0], ang[1], ang[2]);
    } else {
        advancedfx::Message("Failed to resolve attachment transform.\n");
    }
}

CON_COMMAND(_mirv_debug_attachment_name, "Print world pos+angles for entity attachment name (CS2)")
{
    int argc = args->ArgC();
    if(argc < 3) {
        advancedfx::Message("Usage: %s <entIndex> <name>\n", args->ArgV(0));
        return;
    }
    int entIndex = atoi(args->ArgV(1));
    const char* name = args->ArgV(2);

    if(!g_pEntityList || !g_GetEntityFromIndex) {
        advancedfx::Message("Entity system not initialized.\n");
        return;
    }
    auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, entIndex);
    if(!ent) { advancedfx::Message("Entity %d not found.\n", entIndex); return; }

    int idx = CS2_LookupAttachmentIndex(ent, name);
    if(idx <= 0) { advancedfx::Message("Attachment '%s' not found. [entity=%p]\n", name, ent); return; }

    float pos[3]; float ang[3];
    if(CS2_GetAttachmentPosAngByIndex(ent, idx, pos, ang)) {
        advancedfx::Message("ent=%d name=%s idx=%d pos=(%g,%g,%g) angles=(%g,%g,%g)\n",
            entIndex, name, idx, pos[0], pos[1], pos[2], ang[0], ang[1], ang[2]);
    } else {
        advancedfx::Message("Resolved index=%d for '%s' but transform failed (check resolver).\n", idx, name);
    }
}

CON_COMMAND(_mirv_debug_attachment_list, "List attachment indices and tokens for entity (CS2)")
{
    int argc = args->ArgC();
    if(argc < 2) {
        advancedfx::Message("Usage: %s <entIndex>\n", args->ArgV(0));
        return;
    }
    int entIndex = atoi(args->ArgV(1));
    if(!g_pEntityList || !g_GetEntityFromIndex) {
        advancedfx::Message("Entity system not initialized.\n");
        return;
    }
    auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, entIndex);
    if(!ent) { advancedfx::Message("Entity %d not found.\n", entIndex); return; }

    void* ctx = GetAttachmentContext(ent);
    if(!ctx) { advancedfx::Message("No ctx for ent=%d.\n", entIndex); return; }

    //probe indices by attempting transform. Print indices that resolve.
    int found = 0;
    for(int i=1;i<=64;++i) {
        float pos[3]; float ang[3];
        if(CS2_GetAttachmentPosAngByIndex(ent, i, pos, ang)) {
            if(0 == found) advancedfx::Message("Usable attachments (index => name):\n");
            found++;
            const char* resolved = nullptr;
            if(ctx && g_pResolveName) {
                static const char* kCandidates[] = {
                    "knife","eholster","pistol","leg_l_iktarget","leg_r_iktarget","defusekit","grenade0","grenade1",
                    "grenade2","grenade3","grenade4","primary","primary_smg","c4","look_straight_ahead_stand",
                    "clip_limit","weapon_hand_l","weapon_hand_r","gun_accurate","weaponhier_l_iktarget",
                    "weaponhier_r_iktarget","look_straight_ahead_crouch","axis_of_intent","muzzle_flash","muzzle_flash2",
                    "camera_inventory","shell_eject","stattrak","weapon_holster_center","stattrak_legacy","nametag",
                    "nametag_legacy","keychain","keychain_legacy"

                };
                for(size_t k=0; k<sizeof(kCandidates)/sizeof(kCandidates[0]); ++k) {
                    unsigned char idx2 = 0;
                    g_pResolveName(ctx, &idx2, kCandidates[k]);
                    if(idx2 == (unsigned char)i) { resolved = kCandidates[k]; break; }
                }
            }
            if(resolved) advancedfx::Message("  %d => %s\n", i, resolved);
            else advancedfx::Message("  %d\n", i);
        }
    }
    if(0 == found) advancedfx::Message("No attachments found by probing 1..64.\n");
}
