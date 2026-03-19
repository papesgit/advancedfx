#include "stdafx.h"

#include "addresses.h"
#include "CampathDrawer.h"
#include "ClientEntitySystem.h"
#include "GameEvents.h"
#include "hlaeFolder.h"
#include "RenderServiceHooks.h"
#include "RenderSystemDX11Hooks.h"
#include "WrpConsole.h"
#include "AfxHookSource2Rs.h"
#include "ReShadeAdvancedfx.h"
#include "CamIO.h"
#include "ViewModel.h"
#include "Globals.h"
#include "DeathMsg.h"
#include "ReplaceName.h"
#include "SchemaSystem.h"
#include "SceneSystem.h"
#include "MirvCommands.h"
#include "MirvColors.h"
#include "MirvFix.h"
#include "MirvImage.h"
#include "MirvTime.h"
#include "ObsObserverState.h"
#include "ObsWebSocketServer.h"
#include "ObsWebSocketHandlers.h"
#include "ObsInputReceiver.h"
#include "ObsSpectatorBindings.h"
#include "ObsWebSocketActions.h"
#include "FreecamController.h"
#include "NadeCam.h"
#include "PlayerPathDrawer.h"

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../deps/release/prop/AfxHookSource/SourceInterfaces.h"
#include "../deps/release/prop/cs2/Source2Client.h"
#include "../deps/release/prop/cs2/sdk_src/public/tier1/convar.h"
#include "../deps/release/prop/cs2/sdk_src/public/filesystem.h"
#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../deps/release/prop/cs2/sdk_src/public/icvar.h"
#include "../deps/release/prop/cs2/sdk_src/public/igameuiservice.h"

#include "../shared/AfxCommandLine.h"
#include "../shared/AfxConsole.h"
#include "../shared/AfxDetours.h"
#include "../shared/ConsolePrinter.h"
#include "../shared/StringTools.h"
#include "../shared/binutils.h"
#include "../shared/CommandSystem.h"
#include "../shared/AfxMath.h"
#include "../shared/GrowingBufferPoolThreadSafe.h"
#include "../shared/ThreadPool.h"
#include "../shared/MirvCamIO.h"
#include "../shared/MirvCampath.h"
#include "../shared/MirvInput.h"
#include "../shared/MirvSkip.h"

#include "../deps/release/Detours/src/detours.h"

#include <vector>
#include <map>
#include <algorithm>

#define _USE_MATH_DEFINES
#include <math.h>

#include <stdlib.h>
#include <sstream>
#include <fstream>
#include <string>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <iphlpapi.h>

#pragma comment(lib, "iphlpapi.lib")

using json = nlohmann::json;

HMODULE g_h_engine2Dll = 0;
HMODULE g_H_ClientDll = 0;
HMODULE g_H_SchemaSystem = 0;
HMODULE g_H_ResourceSystemDll = 0;
HMODULE g_H_FileSystem_stdio = 0;

SOURCESDK::CS2::IFileSystem* g_pFileSystem = nullptr;

typedef CEntityInstance* (__fastcall* ClientDll_GetSplitScreenPlayer_t)(int slot);
extern ClientDll_GetSplitScreenPlayer_t g_ClientDll_GetSplitScreenPlayer;

advancedfx::CCommandLine  * g_CommandLine = nullptr;

typedef void (__fastcall * AddSearchPath_t)(void* This, const char *pPath, const char *pathID, int addType, int priority, int unk );
AddSearchPath_t org_AddSearchPath = nullptr;

void new_AddSearchPath(void* This, const char *pPath, const char *pathID, int addType, int priority, int unk) {
	if (0 == strcmp(pathID, "USRLOCAL")) {
		const wchar_t* USRLOCALCSGO = _wgetenv(L"USRLOCALCSGO");
		if (nullptr != USRLOCALCSGO) {
			std::string USRLOCALCSGO_copy = "";
			WideStringToUTF8String(USRLOCALCSGO, USRLOCALCSGO_copy);
			if (USRLOCALCSGO_copy.size() > 0) {
				return org_AddSearchPath(This, USRLOCALCSGO_copy.c_str(), pathID, addType, priority, unk);
			} 
		}
	}

	return org_AddSearchPath(This, pPath, pathID, addType, priority, unk);
}

FovScaling GetDefaultFovScaling() {
	return FovScaling_AlienSwarm;
}

void PrintInfo() {
	advancedfx::Message(
		"|" "\n"
		"| AfxHookSource2 (" __DATE__ " " __TIME__ ")" "\n"
		"| https://advancedfx.org/" "\n"
		"|" "\n"
	);
}

void * g_pGameResourceService = nullptr;

// Observer tools for remote control
CObsWebSocketServer* g_pObsWebSocket = nullptr;
CObsInputReceiver* g_pObsInput = nullptr;
CFreecamController* g_pFreecam = nullptr;
CNadeCam* g_pNadeCam = nullptr;
static bool g_NadeCamSuppressUntilAltRelease = false;
static bool g_LastFreecamEnabled = false;

static bool IsLoopback(const in_addr& addr) {
	const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&addr.S_un.S_addr);
	return bytes[0] == 127;
}

static bool IsPrivateIpv4(const in_addr& addr) {
	const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&addr.S_un.S_addr);
	// 10.0.0.0/8
	if (bytes[0] == 10) return true;
	// 172.16.0.0/12
	if (bytes[0] == 172 && (bytes[1] >= 16 && bytes[1] <= 31)) return true;
	// 192.168.0.0/16
	if (bytes[0] == 192 && bytes[1] == 168) return true;
	return false;
}

static std::string InetNtopString(const in_addr& addr) {
	char buffer[INET_ADDRSTRLEN] = {0};
	if (InetNtopA(AF_INET, (PVOID)&addr, buffer, sizeof(buffer))) {
		return std::string(buffer);
	}
	return std::string();
}

static std::string DetectIpv4Address(bool preferPublic) {
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG family = AF_UNSPEC;
	ULONG size = 0;

	// First call to get required buffer size
	if (GetAdaptersAddresses(family, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) {
		return std::string();
	}

	std::vector<BYTE> buffer(size);
	PIP_ADAPTER_ADDRESSES addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
	if (GetAdaptersAddresses(family, flags, nullptr, addresses, &size) != NO_ERROR) {
		return std::string();
	}

	std::string privateCandidate;
	std::string publicCandidate;

	for (auto adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
		if (adapter->OperStatus != IfOperStatusUp) continue;

		for (auto unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
			if (!unicast->Address.lpSockaddr) continue;
			if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;

			auto addr = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr)->sin_addr;
			if (IsLoopback(addr)) continue;

			if (IsPrivateIpv4(addr)) {
				if (privateCandidate.empty()) privateCandidate = InetNtopString(addr);
			} else {
				if (publicCandidate.empty()) publicCandidate = InetNtopString(addr);
			}
		}
	}

	if (preferPublic) {
		if (!publicCandidate.empty()) return publicCandidate;
		if (!privateCandidate.empty()) return privateCandidate;
	} else {
		if (!privateCandidate.empty()) return privateCandidate;
		if (!publicCandidate.empty()) return publicCandidate;
	}

	return std::string();
}

static bool TryGetCommandLineIp(int paramIndex, std::string& outAddress) {
	if (paramIndex > 0 && paramIndex + 1 < g_CommandLine->GetArgC()) {
		auto wArg = g_CommandLine->GetArgV(paramIndex + 1);
		if (wArg && wArg[0] != L'-') {
			std::wstring wstr(wArg);
			if (!wstr.empty()) {
				std::string utf8;
				if (WideStringToUTF8String(wstr.c_str(), utf8)) {
					outAddress = utf8;
					return true;
				}
			}
		}
	}
	return false;
}

static void InstallGsiConfig(const std::string& overrideHost) {
	try {
		std::filesystem::path src = std::filesystem::path(GetHlaeFolderW())
			/ "resources"
			/ "AfxHookSource2"
			/ "gamestate_integration_hot.cfg";

		if (!std::filesystem::exists(src)) {
			advancedfx::Message("GSI install: source missing (%s)\n", src.string().c_str());
			return;
		}

		// CS2 cfg folder is csgo/cfg relative to current working directory
		std::filesystem::path cfgPath = std::filesystem::current_path() / ".." / ".." / "csgo" / "cfg";
		cfgPath = std::filesystem::absolute(cfgPath);

		std::filesystem::create_directories(cfgPath);
		std::filesystem::path dst = cfgPath / "gamestate_integration_hot.cfg";
		advancedfx::Message("GSI install: src=%s dst=%s\n", src.string().c_str(), dst.string().c_str());

		std::ifstream inFile(src, std::ios::binary);
		if (!inFile.is_open()) {
			advancedfx::Message("GSI install: failed to open source\n");
			return;
		}
		std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
		inFile.close();

		if (!overrideHost.empty()) {
			const std::string needle = "http://127.0.0.1:31337/gsi";
			std::string replacement = "http://" + overrideHost + ":31337/gsi";
			if (auto pos = content.find(needle); pos != std::string::npos) {
				content.replace(pos, needle.size(), replacement);
				advancedfx::Message("GSI install: replaced URI with host %s\n", overrideHost.c_str());
			} else {
				advancedfx::Message("GSI install: did not find default URI to replace\n");
			}
		}

		std::ofstream outFile(dst, std::ios::binary | std::ios::trunc);
		if (!outFile.is_open()) {
			advancedfx::Message("GSI install: failed to open destination for write\n");
			return;
		}
		outFile.write(content.data(), static_cast<std::streamsize>(content.size()));
		outFile.close();
		advancedfx::Message("GSI install: wrote config (%zu bytes)\n", content.size());
	}
	catch (...) {
		advancedfx::Message("GSI install: exception, continuing without config\n");
	}
}

/*typedef void (*Unknown_ExecuteClientCommandFromNetChan_t)(void * Ecx, void * Edx, void *R8);
Unknown_ExecuteClientCommandFromNetChan_t g_Old_Unknown_ExecuteClientCommandFromNetChan = nullptr;
void New_Unknown_ExecuteClientCommandFromNetChan(void * Ecx, void * Edx, SOURCESDK::CS2::CCommand *r8Command) {
	//for(int i = 0; i < r8Command->ArgC(); i++) {
	//	advancedfx::Message("Command %i: %s\n",i,r8Command->Arg(i));
	//}
	if(0 == stricmp("connect",r8Command->Arg(0))) {
		if(IDYES != MessageBoxA(0,"YOU ARE TRYING TO CONNECT TO A SERVER - THIS WILL GET YOU VAC BANNED.\nARE YOU SURE?", "HLAE WARNING", MB_YESNOCANCEL|MB_ICONHAND|MB_DEFBUTTON2))
			return;
	}
	g_Old_Unknown_ExecuteClientCommandFromNetChan(Ecx, Edx, r8Command);
}*/


void HookEngineDll(HMODULE engineDll) {

	static bool bFirstCall = true;
	if(!bFirstCall) return;
	bFirstCall = false;
	
	// Unknown_ExecuteClientCommandFromNetChan: // Last checked 2023-07-19
	/*
		The function we hook is called in the function referencing the string
		"Client %s(%d) tried to execute command \"%s\" before being fully connected.\n"
		or the other function referencing "SV: Cheat command '%s' ignored.\n"
		as follows:

		loc_1801842F0:
		mov     r8, rdi
		lea     rdx, [rsp+0D68h+var_D38]
		lea     rcx, [rsp+0D68h+arg_18]
		call    sub_180329DD0 <---
		lea     rcx, [rsp+0D68h+var_D30]
		call    sub_180183A60	
	*/
	/*{
		Afx::BinUtils::ImageSectionsReader sections((HMODULE)engineDll);
		Afx::BinUtils::MemRange textRange = sections.GetMemRange();
		Afx::BinUtils::MemRange result = FindPatternString(textRange, "4C 8B D1 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 13 48 8B 01 4D 8B C8 4C 8B C2 49 8B 12 48 FF A0 90 00 00 00 C3");
		if (!result.IsEmpty()) {
			g_Old_Unknown_ExecuteClientCommandFromNetChan = (Unknown_ExecuteClientCommandFromNetChan_t)result.Start;	
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(&(PVOID&)g_Old_Unknown_ExecuteClientCommandFromNetChan, New_Unknown_ExecuteClientCommandFromNetChan);
			if(NO_ERROR != DetourTransactionCommit())
				ErrorBox("Failed to detour Unknown_ExecuteClientCommandFromNetChan.");
		}
		else
			ErrorBox(MkErrStr(__FILE__, __LINE__));
	}*/
}

typedef void (__fastcall * HostStateRequest_Start_t)(void * This);
HostStateRequest_Start_t g_Old_HostStateRequest_Start = nullptr;
void __fastcall New_HostStateRequest_Start(void * This) {
	if(4 == *(int *)This) {
		// "HostStateRequest::Start(HSR_QUIT)\n"
		AfxStreams_ShutDown();
	}
	g_Old_HostStateRequest_Start(This);
}

void Hook_Engine__HostStateRequest_Start() {
	static bool bFirstRun = true;
	if(bFirstRun) {
		bFirstRun = false;
		if(AFXADDR_GET(cs2_engine_HostStateRequest_Start)) {
			g_Old_HostStateRequest_Start = (HostStateRequest_Start_t)AFXADDR_GET(cs2_engine_HostStateRequest_Start);
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(&(PVOID&)g_Old_HostStateRequest_Start,New_HostStateRequest_Start);
			if(NO_ERROR != DetourTransactionCommit()) ErrorBox(MkErrStr(__FILE__, __LINE__));
		}
	}
}


SOURCESDK::CS2::ISource2EngineToClient * g_pEngineToClient = nullptr;

////////////////////////////////////////////////////////////////////////////////

//TODO: Some bellow here might be not accurate yet.

typedef void * Cs2Gloabls_t;
Cs2Gloabls_t g_pGlobals = nullptr;

CON_COMMAND(__mirv_info,"") {
	PrintInfo();
}

CON_COMMAND(__mirv_test,"") {
	static int offset = 13;

	if(2 <= args->ArgC()) offset = atoi(args->ArgV(1));

	advancedfx::Message("g_pGlobals[%i]: int: %i , float: %f\n",offset,(g_pGlobals ? *(int *)((unsigned char *)g_pGlobals +offset*4) : 0),(g_pGlobals ? *(float *)((unsigned char *)g_pGlobals +offset*4) : 0));
}

extern const char * GetStringForSymbol(int value);

CON_COMMAND(__mirv_get_string_for_symbol,"") {
	if (2<= args->ArgC()) {
		advancedfx::Message("%i: %s\n",atoi(args->ArgV(1)),GetStringForSymbol(atoi(args->ArgV(1))));
	}
}

CON_COMMAND(__mirv_find_vtable,"") {
	if(args->ArgC()<5) return;

	HMODULE hModule = GetModuleHandleA(args->ArgV(1));
	size_t addr = hModule != 0 ? Afx::BinUtils::FindClassVtable(hModule,args->ArgV(2),atoi(args->ArgV(3)),atoi(args->ArgV(4))) : 0;
	DWORD offset = (DWORD)(addr-(size_t)hModule);
	advancedfx::Message("Result: 0x%016llx (Offset: 0x%08x)\n",addr,offset);
}

/*CON_COMMAND(mirv_exec,"") {
    std::ostringstream oss;

	for(int i=1; i < args->ArgC(); i++) {
		if(1 < i ) oss << " ";
		std::string strArg(args->ArgV(i));

		// Escape quotes:
		for (size_t pos = strArg.find('\"', 0); std::string::npos != pos; pos = strArg.find('\"', pos + 2 ) ) strArg.replace(pos, 1, "\\\"");

		oss << "\"" << strArg << "\"";
	}

    if(g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, oss.str().c_str(), false);	
}*/

CON_COMMAND(mirv_loadlibrary, "Load a DLL.")
{
	int argc = args->ArgC();

	if (2 <= argc)
	{
		char const * cmd1 = args->ArgV(1);

		std::wstring wCmd1;
		if (UTF8StringToWideString(cmd1, wCmd1))
		{

			if (0 != LoadLibraryW(wCmd1.c_str()))
			{
				advancedfx::Message("LoadLibraryA OK.\n");
			}
			else
			{
				advancedfx::Message("LoadLibraryA failed.\n");
			}
		}
		else
		{
			advancedfx::Message("Failed to convert \"%s\" from UFT8 to UTF-16.\n", cmd1);
		}

		return;
	}

	advancedfx::Message(
		"mirv_loadlibrary <sDllFilePath> - Load DLL at given path.\n"
	);
}

////////////////////////////////////////////////////////////////////////////////

SOURCESDK::CS2::IGameUIService * g_pGameUIService = nullptr;

class MirvInputEx : private IMirvInputDependencies
{
public:
	MirvInputEx() {
		LastWidth = 1920;
		LastHeight = 1080;

		LastCameraOrigin[0] = 0.0;
		LastCameraOrigin[1] = 0.0;
		LastCameraOrigin[2] = 0.0;
		LastCameraAngles[0] = 0.0;
		LastCameraAngles[1] = 0.0;
		LastCameraAngles[2] = 0.0;
		LastCameraFov = 90.0;

		GameCameraOrigin[0] = 0.0;
		GameCameraOrigin[1] = 0.0;
		GameCameraOrigin[2] = 0.0;
		GameCameraAngles[0] = 0.0;
		GameCameraAngles[1] = 0.0;
		GameCameraAngles[2] = 0.0;
		GameCameraFov = 90.0;

		LastFrameTime = 0;

		m_MirvInput = new MirvInput(this);
	}

	~MirvInputEx() {
		delete m_MirvInput;
	}

	MirvInput * m_MirvInput;

	double LastCameraOrigin[3];
	double LastCameraAngles[3];
	double LastCameraFov;

	double GameCameraOrigin[3];
	double GameCameraAngles[3];
	double GameCameraFov;

	double LastFrameTime;

	int LastWidth;
	int LastHeight;

private:
	virtual bool GetSuspendMirvInput() override {
		return g_pGameUIService && g_pGameUIService->Con_IsVisible();
	}

	virtual void GetLastCameraData(double & x, double & y, double & z, double & rX, double & rY, double & rZ, double & fov) override {
		x = LastCameraOrigin[0];
		y = LastCameraOrigin[1];
		z = LastCameraOrigin[2];
		rX = LastCameraAngles[0];
		rY = LastCameraAngles[1];
		rZ = LastCameraAngles[2];
		fov = LastCameraFov;
	}

	virtual void GetGameCameraData(double & x, double & y, double & z, double & rX, double & rY, double & rZ, double & fov) override {
		x = GameCameraOrigin[0];
		y = GameCameraOrigin[1];
		z = GameCameraOrigin[2];
		rX = GameCameraAngles[0];
		rY = GameCameraAngles[1];
		rZ = GameCameraAngles[2];
		fov = GameCameraFov;
	}

	virtual double GetInverseScaledFov(double fov) override {
		return ScaleFovInverse(LastWidth, LastHeight, fov);
	}

private:

	double ScaleFovInverse(double width, double height, double fov) {
		if (!height) return fov;

		double engineAspectRatio = width / height;
		double defaultAscpectRatio = 4.0 / 3.0;
		double ratio = engineAspectRatio / defaultAscpectRatio;
		double t = tan(0.5 * fov * (2.0 * M_PI / 360.0));
		double halfAngle = atan(t / ratio);
		return 2.0 * halfAngle / (2.0 * M_PI / 360.0);
	}

} g_MirvInputEx;

float GetLastCameraFov() {
	return (float)g_MirvInputEx.LastCameraFov;
}

static CameraTransform g_ObsCurrentCameraTransform;
static CameraTransform g_ObsPreviousCameraTransform;
static float g_ObsCurrentCameraDeltaTime = 0.0f;
static bool g_ObsHasCurrentCameraTransform = false;
static bool g_ObsHasPreviousCameraTransform = false;
static std::mutex g_ObsCameraTransformMutex;

struct RenderedSetupCandidate {
	uint64_t serial = 0;
	CameraTransform transform;
	float deltaTime = 0.0f;
	bool hasWorldToScreen = false;
	SOURCESDK::VMatrix worldToScreen;
};

static const size_t RENDERED_SETUP_CANDIDATE_COUNT = 256;
static RenderedSetupCandidate g_RenderedSetupCandidates[RENDERED_SETUP_CANDIDATE_COUNT];
static std::mutex g_RenderedSetupCandidatesMutex;
static std::atomic<uint64_t> g_RenderedSetupCandidateSerialCounter{ 0 };
static std::atomic<uint64_t> g_RenderedSetupLatestCandidateSerial{ 0 };
static std::atomic<uint64_t> g_RenderedSetupConfirmedSerial{ 0 };
static std::atomic<uint64_t> g_RenderedSetupLastPublishedSerial{ 0 };
static SOURCESDK::VMatrix g_RenderedSetupWorldToScreenMatrix;
static std::atomic<uint64_t> g_RenderedSetupWorldToScreenSerial{ 0 };
static std::mutex g_RenderedSetupWorldToScreenMutex;

static void RenderedSetup_TryPublishWorldToScreenForSerial(uint64_t serial) {
	if (0 == serial) return;

	SOURCESDK::VMatrix matrix;
	{
		std::lock_guard<std::mutex> lock(g_RenderedSetupCandidatesMutex);
		const RenderedSetupCandidate& entry = g_RenderedSetupCandidates[serial % RENDERED_SETUP_CANDIDATE_COUNT];
		if (entry.serial != serial || !entry.hasWorldToScreen) return;
		matrix = entry.worldToScreen;
	}

	std::lock_guard<std::mutex> lock(g_RenderedSetupWorldToScreenMutex);
	if (g_RenderedSetupWorldToScreenSerial.load(std::memory_order_relaxed) < serial) {
		g_RenderedSetupWorldToScreenMatrix = matrix;
		g_RenderedSetupWorldToScreenSerial.store(serial, std::memory_order_release);
	}
}

uint64_t RenderedSetup_OnSetupViewCandidate(float x, float y, float z, float pitch, float yaw, float roll, float fov, float deltaTime) {
	CameraTransform transform;
	transform.x = x;
	transform.y = y;
	transform.z = z;
	transform.pitch = pitch;
	transform.yaw = yaw;
	transform.roll = roll;
	transform.fov = fov;

	const uint64_t serial = 1 + g_RenderedSetupCandidateSerialCounter.fetch_add(1, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(g_RenderedSetupCandidatesMutex);
		RenderedSetupCandidate& entry = g_RenderedSetupCandidates[serial % RENDERED_SETUP_CANDIDATE_COUNT];
		entry.serial = serial;
		entry.transform = transform;
		entry.deltaTime = deltaTime;
		entry.hasWorldToScreen = false;
	}
	g_RenderedSetupLatestCandidateSerial.store(serial, std::memory_order_release);
	return serial;
}

uint64_t RenderedSetup_GetLatestCandidateSerial() {
	return g_RenderedSetupLatestCandidateSerial.load(std::memory_order_acquire);
}

void RenderedSetup_OnWorldToScreenCandidate(uint64_t setupSerial, const SOURCESDK::VMatrix& worldToScreenMatrix) {
	if (0 == setupSerial) return;

	{
		std::lock_guard<std::mutex> lock(g_RenderedSetupCandidatesMutex);
		RenderedSetupCandidate& entry = g_RenderedSetupCandidates[setupSerial % RENDERED_SETUP_CANDIDATE_COUNT];
		if (entry.serial != setupSerial) return;
		entry.worldToScreen = worldToScreenMatrix;
		entry.hasWorldToScreen = true;
	}

	if (setupSerial <= g_RenderedSetupConfirmedSerial.load(std::memory_order_acquire)) {
		RenderedSetup_TryPublishWorldToScreenForSerial(setupSerial);
	}
}

bool RenderedSetup_GetPublishedWorldToScreenMatrix(SOURCESDK::VMatrix& outWorldToScreenMatrix) {
	std::lock_guard<std::mutex> lock(g_RenderedSetupWorldToScreenMutex);
	if (0 == g_RenderedSetupWorldToScreenSerial.load(std::memory_order_acquire)) return false;
	outWorldToScreenMatrix = g_RenderedSetupWorldToScreenMatrix;
	return true;
}

void RenderedSetup_OnPlayerSetupViewRendered(uint64_t setupSerial) {
	if (0 == setupSerial) return;

	uint64_t current = g_RenderedSetupConfirmedSerial.load(std::memory_order_relaxed);
	while (current < setupSerial
		&& !g_RenderedSetupConfirmedSerial.compare_exchange_weak(current, setupSerial, std::memory_order_release, std::memory_order_relaxed)) {
	}

	RenderedSetup_TryPublishWorldToScreenForSerial(setupSerial);
	g_MirvImageDrawer.PublishAttachmentsForSetupSerial(setupSerial);
}

void RenderedSetup_OnBeforePresent() {
	const uint64_t renderedSerial = g_RenderedSetupConfirmedSerial.load(std::memory_order_acquire);
	if (0 == renderedSerial || renderedSerial == g_RenderedSetupLastPublishedSerial.load(std::memory_order_acquire)) return;

	RenderedSetupCandidate candidate;
	{
		std::lock_guard<std::mutex> lock(g_RenderedSetupCandidatesMutex);
		const RenderedSetupCandidate& entry = g_RenderedSetupCandidates[renderedSerial % RENDERED_SETUP_CANDIDATE_COUNT];
		if (entry.serial != renderedSerial) return;
		candidate = entry;
	}

	{
		std::lock_guard<std::mutex> lock(g_ObsCameraTransformMutex);
		if (g_ObsHasCurrentCameraTransform) {
			g_ObsPreviousCameraTransform = g_ObsCurrentCameraTransform;
			g_ObsHasPreviousCameraTransform = true;
		}
		g_ObsCurrentCameraTransform = candidate.transform;
		g_ObsCurrentCameraDeltaTime = candidate.deltaTime;
		g_ObsHasCurrentCameraTransform = true;
	}

	RenderedSetup_TryPublishWorldToScreenForSerial(renderedSerial);

	g_RenderedSetupLastPublishedSerial.store(renderedSerial, std::memory_order_release);
}

CameraTransform Obs_GetLastCameraTransform() {
	CameraTransform transform;
	transform.x = (float)g_MirvInputEx.LastCameraOrigin[0];
	transform.y = (float)g_MirvInputEx.LastCameraOrigin[1];
	transform.z = (float)g_MirvInputEx.LastCameraOrigin[2];
	transform.pitch = (float)g_MirvInputEx.LastCameraAngles[0];
	transform.yaw = (float)g_MirvInputEx.LastCameraAngles[1];
	transform.roll = (float)g_MirvInputEx.LastCameraAngles[2];
	transform.fov = (float)g_MirvInputEx.LastCameraFov;
	return transform;
}

CameraTransformSamples Obs_GetRecentCameraTransforms() {
	CameraTransformSamples samples;
	samples.current = Obs_GetLastCameraTransform();

	{
		std::lock_guard<std::mutex> lock(g_ObsCameraTransformMutex);
		if (g_ObsHasCurrentCameraTransform) {
			samples.current = g_ObsCurrentCameraTransform;
			samples.deltaTime = g_ObsCurrentCameraDeltaTime;
			if (g_ObsHasPreviousCameraTransform) {
				samples.previous = g_ObsPreviousCameraTransform;
				samples.hasPrevious = true;
			}
		}
	}

	return samples;
}

CON_COMMAND(mirv_input, "Input mode configuration.")
{
	g_MirvInputEx.m_MirvInput->ConCommand(args);
}

CON_COMMAND(mirv_udpdebug, "Enable/disable UDP input debug logging (mirv_udpdebug 0|1).")
{
	if (args->ArgC() < 2)
	{
		advancedfx::Message("mirv_udpdebug <0|1> - Current: %s\n",
			(g_pObsInput ? (g_pObsInput->GetPacketLoss(), "unknown") : "disabled"));
		advancedfx::Message("Usage: mirv_udpdebug 0 (off) or 1 (on)\n");
		return;
	}

	int value = atoi(args->ArgV(1));
	bool enable = value != 0;

	if (g_pObsInput)
	{
		g_pObsInput->SetDebug(enable);
		advancedfx::Message("mirv_udpdebug: %s\n", enable ? "ON" : "OFF");
	}
	else
	{
		advancedfx::Message("mirv_udpdebug: UDP input receiver not initialized.\n");
	}
}

WNDPROC g_NextWindProc;
static bool g_afxWindowProcSet = false;

LRESULT CALLBACK new_Afx_WindowProc(
	__in HWND hwnd,
	__in UINT uMsg,
	__in WPARAM wParam,
	__in LPARAM lParam
)
{
//	if (AfxHookSource::Gui::WndProcHandler(hwnd, uMsg, wParam, lParam))
//		return 0;

	switch(uMsg)
	{
	case WM_ACTIVATE:
		g_MirvInputEx.m_MirvInput->Supply_Focus(LOWORD(wParam) != 0);
		break;
	case WM_CHAR:
		if(g_MirvInputEx.m_MirvInput->Supply_CharEvent(wParam, lParam))
			return 0;
		break;
	case WM_KEYDOWN:
		if(g_MirvInputEx.m_MirvInput->Supply_KeyEvent(MirvInput::KS_DOWN, wParam, lParam))
			return 0;
		break;
	case WM_KEYUP:
		if(g_MirvInputEx.m_MirvInput->Supply_KeyEvent(MirvInput::KS_UP,wParam, lParam))
			return 0;
		break;
	case WM_LBUTTONDBLCLK:
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_MBUTTONDBLCLK:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_RBUTTONDBLCLK:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_MOUSEMOVE:
	case WM_MOUSEWHEEL:
		if (g_MirvInputEx.m_MirvInput->Supply_MouseEvent(uMsg, wParam, lParam))
			return 0;
		break;
	}
	return CallWindowProcW(g_NextWindProc, hwnd, uMsg, wParam, lParam);
}

// TODO: this is risky, actually we should track the hWnd maybe.
LONG_PTR WINAPI new_GetWindowLongPtrW(
  __in HWND hWnd,
  __in int  nIndex
)
{
	if(nIndex == GWLP_WNDPROC)
	{
		if(g_afxWindowProcSet)
		{
			return (LONG_PTR)g_NextWindProc;
		}
	}

	return GetWindowLongPtrW(hWnd, nIndex);
}

// TODO: this is risky, actually we should track the hWnd maybe.
LONG_PTR WINAPI new_SetWindowLongPtrW(
  __in HWND     hWnd,
  __in int      nIndex,
  __in LONG_PTR dwNewLong
)
{
	if(nIndex == GWLP_WNDPROC)
	{
		LONG lResult = SetWindowLongPtrW(hWnd, nIndex, (LONG_PTR)new_Afx_WindowProc);

		if(!g_afxWindowProcSet)
		{
			g_afxWindowProcSet = true;
		}
		else
		{
			lResult = (LONG_PTR)g_NextWindProc;
		}

		g_NextWindProc = (WNDPROC)dwNewLong;

		return lResult;
	}

	return SetWindowLongPtrW(hWnd, nIndex, dwNewLong);
}

BOOL WINAPI new_GetCursorPos(
	__out LPPOINT lpPoint
)
{
	BOOL result = GetCursorPos(lpPoint);

//	if (AfxHookSource::Gui::OnGetCursorPos(lpPoint))
//		return TRUE;

	g_MirvInputEx.m_MirvInput->Supply_GetCursorPos(lpPoint);

	return result;
}

BOOL WINAPI new_SetCursorPos(
	__in int X,
	__in int Y
)
{
//	if (AfxHookSource::Gui::OnSetCursorPos(X, Y))
//		return TRUE;

	BOOL result = SetCursorPos(X, Y);
	if(result) g_MirvInputEx.m_MirvInput->Supply_SetCursorPos(X, Y);
	return result;
}

HCURSOR WINAPI new_SetCursor(__in_opt HCURSOR hCursor)
{
//	HCURSOR result;

//	if (AfxHookSource::Gui::OnSetCursor(hCursor, result))
//		return result;

	return SetCursor(hCursor);
}

HWND WINAPI new_SetCapture(__in HWND hWnd)
{
//	HWND result;

//	if (AfxHookSource::Gui::OnSetCapture(hWnd, result))
//		return result;

	return SetCapture(hWnd);
}

BOOL WINAPI new_ReleaseCapture()
{
//	if (AfxHookSource::Gui::OnReleaseCapture())
//		return TRUE;

	return ReleaseCapture();
}


////////////////////////////////////////////////////////////////////////////////

CamPath g_CamPath;

class CMirvCampath_Time : public IMirvCampath_Time
{
public:
	virtual double GetTime() {
		// Can be paused time, we don't support that currently.
		return g_MirvTime.curtime_get();
	}
	virtual double GetCurTime() {
		return g_MirvTime.curtime_get();
	}
	virtual bool GetCurrentDemoTick(int& outTick) {
		return g_MirvTime.GetCurrentDemoTick(outTick);
	}
	virtual bool GetCurrentDemoTime(double& outDemoTime) {
		return g_MirvTime.GetCurrentDemoTime(outDemoTime);
	}
	virtual bool GetDemoTickFromDemoTime(double curTime, double time, int& outTick) {
		outTick = (int)round(time / g_MirvTime.interval_per_tick_get());
		return true;
	}
	virtual bool GetDemoTimeFromClientTime(double curTime, double time, double& outDemoTime) {
		double current_demo_time;
		if(GetCurrentDemoTime(current_demo_time)) {
			outDemoTime = time - (curTime - current_demo_time);
			return true;
		}
		return false;
	}
    virtual bool GetDemoTickFromClientTime(double curTime, double targetTime, int& outTick)
    {
        double demoTime;
        return GetDemoTimeFromClientTime(curTime, targetTime, demoTime) && GetDemoTickFromDemoTime(curTime, demoTime, outTick);
    }
} g_MirvCampath_Time;

class CMirvCampath_Camera : public IMirvCampath_Camera
{
public:
	virtual SMirvCameraValue GetCamera() {
		return SMirvCameraValue(			
			g_MirvInputEx.LastCameraOrigin[0],
			g_MirvInputEx.LastCameraOrigin[1],
			g_MirvInputEx.LastCameraOrigin[2],
			g_MirvInputEx.LastCameraAngles[0],
			g_MirvInputEx.LastCameraAngles[1],
			g_MirvInputEx.LastCameraAngles[2],
			g_MirvInputEx.LastCameraFov
		);
	}
} g_MirvCampath_Camera;

class CMirvCampath_Drawer : public IMirvCampath_Drawer
{
public:
	virtual bool GetEnabled() {
		return g_CampathDrawer.Draw_get();
	}
	virtual void SetEnabled(bool value) {
		g_CampathDrawer.Draw_set(value);
	}
	virtual bool GetDrawKeyframeAxis() {
		return g_CampathDrawer.GetDrawKeyframeAxis();
	}
	virtual void SetDrawKeyframeAxis(bool value) {
		g_CampathDrawer.SetDrawKeyframeAxis(value);
	}
	virtual bool GetDrawKeyframeCam() {
		return g_CampathDrawer.GetDrawKeyframeCam();
	}
	virtual void SetDrawKeyframeCam(bool value) {
		g_CampathDrawer.SetDrawKeyframeCam(value);
	}

	virtual float GetDrawKeyframeIndex() { return g_CampathDrawer.GetDrawKeyframeIndex(); }
	virtual void SetDrawKeyframeIndex(float value) { g_CampathDrawer.SetDrawKeyframeIndex(value); }

} g_MirvCampath_Drawer;

CON_COMMAND(mirv_campath, "camera paths")
{
	if (nullptr == g_pGlobals)
	{
		advancedfx::Warning("Error: Hooks not installed.\n");
		return;
	}

	MirvCampath_ConCommand(args, advancedfx::Message, advancedfx::Warning, &g_CamPath, &g_MirvCampath_Time, &g_MirvCampath_Camera, &g_MirvCampath_Drawer);
}

CON_COMMAND(mirv_playerpath, "player path visualization")
{
	g_PlayerPathDrawer.Console_Command(args);
}

double MirvCamIO_GetTimeFn(void) {
	return g_MirvTime.curtime_get();
}

CON_COMMAND(mirv_camio, "New camera motion data import / export.") {
	g_S2CamIO.Console_CamIO(args);
}

static std::chrono::steady_clock::time_point g_LastFreecamSpeedBroadcast = (std::chrono::steady_clock::time_point::min)();
static const std::chrono::milliseconds g_FreecamSpeedBroadcastInterval(150); // ~6-7 Hz

static void BroadcastFreecamSpeedIfNeeded() {
	if (!g_pFreecam || !g_pObsWebSocket) {
		return;
	}

	if (!g_pFreecam->IsEnabled() || !g_pObsWebSocket->IsActive()) {
		return;
	}

	if (!g_pFreecam->IsSpeedDirty()) {
		return;
	}

	auto now = std::chrono::steady_clock::now();
	if (g_LastFreecamSpeedBroadcast == (std::chrono::steady_clock::time_point::min)() ||
		now - g_LastFreecamSpeedBroadcast >= g_FreecamSpeedBroadcastInterval) {

		json payload{
			{"type", "freecam_speed"},
			{"speed", g_pFreecam->GetCurrentMoveSpeed()}
		};

		g_pObsWebSocket->BroadcastJson(payload.dump());
		g_LastFreecamSpeedBroadcast = now;
		g_pFreecam->ClearSpeedDirtyFlag();
	}
}

static Afx::Math::Quaternion SourceQuatToAfx(const SOURCESDK::Quaternion& q) {
	return Afx::Math::Quaternion(q.w, q.x, q.y, q.z);
}

static Afx::Math::Vector3 RotateVectorByQuat(const Afx::Math::Quaternion& q, const Afx::Math::Vector3& v) {
	Afx::Math::Quaternion vQuat(0.0, v.X, v.Y, v.Z);
	Afx::Math::Quaternion result = q * vQuat * q.Conjugate();
	return Afx::Math::Vector3(result.X, result.Y, result.Z);
}

static Afx::Math::Quaternion AxisAngleQuat(const Afx::Math::Vector3& axis, double radians) {
	Afx::Math::Vector3 n = axis;
	if (n.Length() <= 1.0e-9) {
		return Afx::Math::Quaternion(1.0, 0.0, 0.0, 0.0);
	}
	n.Normalize();
	const double half = 0.5 * radians;
	const double s = sin(half);
	return Afx::Math::Quaternion(cos(half), n.X * s, n.Y * s, n.Z * s);
}

static Afx::Math::Quaternion ApplyAxisRotation(
	const Afx::Math::Quaternion& q,
	double degrees,
	const Afx::Math::Vector3& axis,
	AttachmentCameraRotationBasis basis) {
	if (fabs(degrees) <= 1.0e-9) return q;
	const double radians = degrees * (M_PI / 180.0);
	const Afx::Math::Quaternion rot = AxisAngleQuat(axis, radians);
	return (basis == AttachmentCameraRotationBasis::World ? rot * q : q * rot).Normalized();
}

static Afx::Math::Quaternion ApplyEulerWithBasis(
	const Afx::Math::Quaternion& base,
	const Afx::Math::QEulerAngles& angles,
	AttachmentCameraRotationBasis basisPitch,
	AttachmentCameraRotationBasis basisYaw,
	AttachmentCameraRotationBasis basisRoll) {
	Afx::Math::Quaternion q = base;
	const Afx::Math::Vector3 axisPitch(0.0, 1.0, 0.0);
	const Afx::Math::Vector3 axisYaw(0.0, 0.0, 1.0);
	const Afx::Math::Vector3 axisRoll(1.0, 0.0, 0.0);
	q = ApplyAxisRotation(q, angles.Pitch, axisPitch, basisPitch);
	q = ApplyAxisRotation(q, angles.Yaw, axisYaw, basisYaw);
	q = ApplyAxisRotation(q, angles.Roll, axisRoll, basisRoll);
	return q.Normalized();
}

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

static double EaseCurve(double t, AttachmentCameraKeyframeEasingCurve curve, AttachmentCameraKeyframeEase mode) {
	if (t <= 0.0) return 0.0;
	if (t >= 1.0) return 1.0;

	switch (curve) {
	case AttachmentCameraKeyframeEasingCurve::Smoothstep:
		switch (mode) {
		case AttachmentCameraKeyframeEase::EaseIn:
			{
				const double k = 1.0 - t;
				return 1.0 - (k * k);
			}
		case AttachmentCameraKeyframeEase::EaseOut:
			return t * t;
		case AttachmentCameraKeyframeEase::EaseInOut:
		default:
			return t * t * (3.0 - 2.0 * t);
		}
	case AttachmentCameraKeyframeEasingCurve::Cubic:
		switch (mode) {
		case AttachmentCameraKeyframeEase::EaseIn:
			{
				const double k = 1.0 - t;
				return 1.0 - (k * k * k);
			}
		case AttachmentCameraKeyframeEase::EaseOut:
			return t * t * t;
		case AttachmentCameraKeyframeEase::EaseInOut:
		default:
			if (t < 0.5) {
				return 4.0 * t * t * t;
			}
			{
				const double k = -2.0 * t + 2.0;
				return 1.0 - (k * k * k) / 2.0;
			}
		}
	case AttachmentCameraKeyframeEasingCurve::Linear:
	default:
		return t;
	}
}

static AttachmentCameraKeyframeEasingCurve ToKeyframeCurve(AttachmentCameraTransitionEasing easing) {
	switch (easing) {
	case AttachmentCameraTransitionEasing::Linear:
		return AttachmentCameraKeyframeEasingCurve::Linear;
	case AttachmentCameraTransitionEasing::EaseInOutCubic:
		return AttachmentCameraKeyframeEasingCurve::Cubic;
	case AttachmentCameraTransitionEasing::Smoothstep:
	default:
		return AttachmentCameraKeyframeEasingCurve::Smoothstep;
	}
}

static bool ComputeAttachmentCameraTransform(
	const AttachmentCameraState& state,
	int controllerIndex,
	double animT,
	Afx::Math::Vector3& outOrigin,
	Afx::Math::Quaternion& outQuat,
	float& outFov) {
	if (!state.active) return false;

	auto pawn = GetPawnFromControllerIndex(controllerIndex);
	if (!pawn) return false;

	SOURCESDK::Vector attachmentOrigin;
	SOURCESDK::Quaternion attachmentAngles;

	const bool isPov = !state.useAttachmentIndex && state.attachmentName == "POV";
	if (isPov) {
		float eyeOrigin[3];
		float eyeAngles[3];
		pawn->GetRenderEyeOrigin(eyeOrigin);
		pawn->GetRenderEyeAngles(eyeAngles);
		attachmentOrigin.x = eyeOrigin[0];
		attachmentOrigin.y = eyeOrigin[1];
		attachmentOrigin.z = eyeOrigin[2];
		// Convert Euler angles (pitch, yaw, roll) to quaternion
		Afx::Math::QEulerAngles euler(eyeAngles[0], eyeAngles[1], eyeAngles[2]);
		Afx::Math::Quaternion q = Afx::Math::Quaternion::FromQREulerAngles(
			Afx::Math::QREulerAngles::FromQEulerAngles(euler)
		).Normalized();
		attachmentAngles.x = q.X;
		attachmentAngles.y = q.Y;
		attachmentAngles.z = q.Z;
		attachmentAngles.w = q.W;
	} else {
		uint8_t attachmentIdx = state.attachmentIndex;
		if (!state.useAttachmentIndex) {
			attachmentIdx = pawn->LookupAttachment(state.attachmentName.c_str());
		}
		if (!pawn->GetAttachment(attachmentIdx, attachmentOrigin, attachmentAngles)) return false;
	}

	Afx::Math::Quaternion baseQuat = SourceQuatToAfx(attachmentAngles).Normalized();
	Afx::Math::Quaternion baseQuatForRotation = baseQuat;
	if (state.rotationLockPitch || state.rotationLockYaw || state.rotationLockRoll) {
		Afx::Math::QEulerAngles baseAngles = baseQuat.ToQREulerAngles().ToQEulerAngles();
		if (state.rotationLockPitch) baseAngles.Pitch = 0.0;
		if (state.rotationLockYaw) baseAngles.Yaw = 0.0;
		if (state.rotationLockRoll) baseAngles.Roll = 0.0;
		baseQuatForRotation = Afx::Math::Quaternion::FromQREulerAngles(
			Afx::Math::QREulerAngles::FromQEulerAngles(baseAngles)
		).Normalized();
	}

	SOURCESDK::Vector deltaPos = { 0.0f, 0.0f, 0.0f };
	Afx::Math::QEulerAngles deltaAngles(0.0, 0.0, 0.0);
	float fov = state.fov;

	if (state.animation.enabled && !state.animation.keyframes.empty()) {
		double t = animT;
		if (t < 0.0) t = 0.0;

		const auto& keyframes = state.animation.keyframes;
		const AttachmentCameraKeyframe* k0 = &keyframes.front();
		const AttachmentCameraKeyframe* k1 = &keyframes.back();

		if (t <= keyframes.front().time) {
			k0 = k1 = &keyframes.front();
		} else if (t >= keyframes.back().time) {
			k0 = k1 = &keyframes.back();
		} else {
			for (size_t i = 1; i < keyframes.size(); ++i) {
				if (t < keyframes[i].time) {
					k0 = &keyframes[i - 1];
					k1 = &keyframes[i];
					break;
				}
			}
		}

		double alpha = 0.0;
		if (k0 != k1) {
			const double dt = k1->time - k0->time;
			alpha = dt > 1.0e-9 ? (t - k0->time) / dt : 0.0;
			if (alpha < 0.0) alpha = 0.0;
			if (alpha > 1.0) alpha = 1.0;
		}
		if (k0 != k1) {
			const bool easeOut = k0->easingCurve != AttachmentCameraKeyframeEasingCurve::Linear
				&& (k0->easingMode == AttachmentCameraKeyframeEase::EaseOut
					|| k0->easingMode == AttachmentCameraKeyframeEase::EaseInOut);
			const bool easeIn = k1->easingCurve != AttachmentCameraKeyframeEasingCurve::Linear
				&& (k1->easingMode == AttachmentCameraKeyframeEase::EaseIn
					|| k1->easingMode == AttachmentCameraKeyframeEase::EaseInOut);

			if (easeOut || easeIn) {
				const auto curve = easeIn ? k1->easingCurve : k0->easingCurve;
				const auto mode = easeIn && easeOut
					? AttachmentCameraKeyframeEase::EaseInOut
					: (easeIn ? AttachmentCameraKeyframeEase::EaseIn : AttachmentCameraKeyframeEase::EaseOut);
				alpha = EaseCurve(alpha, curve, mode);
			}
		}

		auto lerp = [](float a, float b, double t) -> float {
			return (float)(a + (b - a) * t);
		};

		deltaPos.x = lerp(k0->deltaPos.x, k1->deltaPos.x, alpha);
		deltaPos.y = lerp(k0->deltaPos.y, k1->deltaPos.y, alpha);
		deltaPos.z = lerp(k0->deltaPos.z, k1->deltaPos.z, alpha);

		Afx::Math::Quaternion q0 = Afx::Math::Quaternion::FromQREulerAngles(
			Afx::Math::QREulerAngles::FromQEulerAngles(k0->deltaAngles)
		).Normalized();
		Afx::Math::Quaternion q1 = Afx::Math::Quaternion::FromQREulerAngles(
			Afx::Math::QREulerAngles::FromQEulerAngles(k1->deltaAngles)
		).Normalized();
		Afx::Math::Quaternion deltaQuat = q0.Slerp(q1, (float)alpha).Normalized();
		deltaAngles = deltaQuat.ToQREulerAngles().ToQEulerAngles();

		const float f0 = k0->hasFov ? k0->fov : state.fov;
		const float f1 = k1->hasFov ? k1->fov : state.fov;
		fov = lerp(f0, f1, alpha);
	}

	Afx::Math::Quaternion combinedQuat = ApplyEulerWithBasis(
		baseQuatForRotation,
		state.offsetAngles,
		state.rotationBasisPitch,
		state.rotationBasisYaw,
		state.rotationBasisRoll);
	combinedQuat = ApplyEulerWithBasis(
		combinedQuat,
		deltaAngles,
		state.rotationBasisPitch,
		state.rotationBasisYaw,
		state.rotationBasisRoll);

	Afx::Math::Vector3 combinedOrigin(attachmentOrigin.x, attachmentOrigin.y, attachmentOrigin.z);
	Afx::Math::Vector3 offsetVec(
		state.offsetPos.x + deltaPos.x,
		state.offsetPos.y + deltaPos.y,
		state.offsetPos.z + deltaPos.z
	);
	const Afx::Math::Quaternion positionQuat = state.rotationReference == AttachmentCameraRotationReference::OffsetLocal
		? baseQuatForRotation
		: combinedQuat;
	combinedOrigin += RotateVectorByQuat(positionQuat, offsetVec);

	outOrigin = combinedOrigin;
	outQuat = combinedQuat;
	outFov = fov;

	return true;
}

static bool TryComputeAttachmentCamera(const AttachmentCameraState& state, Afx::Math::Vector3& outOrigin, Afx::Math::QEulerAngles& outAngles, float & outFov) {
	if (!state.active) return false;

	const double now = g_MirvTime.curtime_get();
	double animT = state.animation.enabled ? now - state.animation.startTime : 0.0;
	if (animT < 0.0) animT = 0.0;

	const bool hasTransition = state.animation.enabled
		&& state.animation.hasTransition
		&& state.animation.targetControllerIndex != -1;
	const double transitionStart = state.animation.transitionTime;
	const double transitionDuration = state.animation.transitionDuration;
	const double transitionEnd = transitionStart + transitionDuration;
	const bool hasBlend = hasTransition && transitionDuration > 0.0;

	double animEvalT = animT;
	if (hasBlend && animT >= transitionStart) {
		if (animT < transitionEnd) {
			animEvalT = transitionStart;
		} else {
			animEvalT = animT - transitionDuration;
		}
	}

	if (hasBlend && animT >= transitionStart && animT < transitionEnd) {
		Afx::Math::Vector3 origin0;
		Afx::Math::Vector3 origin1;
		Afx::Math::Quaternion quat0;
		Afx::Math::Quaternion quat1;
		float fov0 = state.fov;
		float fov1 = state.fov;

		if (!ComputeAttachmentCameraTransform(state, state.controllerIndex, animEvalT, origin0, quat0, fov0)) return false;
		if (!ComputeAttachmentCameraTransform(state, state.animation.targetControllerIndex, animEvalT, origin1, quat1, fov1)) return false;

		double alpha = (transitionDuration > 1.0e-9) ? (animT - transitionStart) / transitionDuration : 1.0;
		if (alpha < 0.0) alpha = 0.0;
		if (alpha > 1.0) alpha = 1.0;
		alpha = EaseCurve(alpha, ToKeyframeCurve(state.animation.transitionEasing), AttachmentCameraKeyframeEase::EaseInOut);

		auto lerpVec = [](const Afx::Math::Vector3& a, const Afx::Math::Vector3& b, double t) -> Afx::Math::Vector3 {
			return Afx::Math::Vector3(
				a.X + (b.X - a.X) * t,
				a.Y + (b.Y - a.Y) * t,
				a.Z + (b.Z - a.Z) * t
			);
		};

		auto lerp = [](float a, float b, double t) -> float {
			return (float)(a + (b - a) * t);
		};

		Afx::Math::Vector3 blendedOrigin = lerpVec(origin0, origin1, alpha);
		Afx::Math::Quaternion blendedQuat = quat0.Slerp(quat1, (float)alpha).Normalized();
		float blendedFov = lerp(fov0, fov1, alpha);

		outOrigin = blendedOrigin;
		outAngles = blendedQuat.ToQREulerAngles().ToQEulerAngles();
		outFov = blendedFov;
		return true;
	}

	int controllerIndex = state.controllerIndex;
	if (hasTransition) {
		if (hasBlend) {
			if (animT >= transitionEnd) {
				controllerIndex = state.animation.targetControllerIndex;
			}
		} else if (animT >= transitionStart) {
			controllerIndex = state.animation.targetControllerIndex;
		}
	}

	Afx::Math::Quaternion outQuat;
	if (!ComputeAttachmentCameraTransform(state, controllerIndex, animEvalT, outOrigin, outQuat, outFov)) return false;

	outAngles = outQuat.ToQREulerAngles().ToQEulerAngles();
	return true;
}

static bool GetCurrentSpectatedControllerIndex(int & outControllerIndex) {
    if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex || !g_ClientDll_GetSplitScreenPlayer)
        return false;

    CEntityInstance* localController = g_ClientDll_GetSplitScreenPlayer(0);
    if (!localController) return false;

    auto pawnHandle = localController->GetPlayerPawnHandle();
    if (!pawnHandle.IsValid()) return false;

    CEntityInstance* localPawn = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, pawnHandle.GetEntryIndex());
    if (!localPawn) return false;

    CEntityInstance* targetPawn = localPawn;

    auto observerHandle = localPawn->GetObserverTarget();
    if (observerHandle.IsValid()) {
        CEntityInstance* observedPawn = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, observerHandle.GetEntryIndex());
        if (observedPawn) targetPawn = observedPawn;
    }

    auto controllerHandle = targetPawn->GetPlayerControllerHandle();
    if (!controllerHandle.IsValid()) return false;

    outControllerIndex = controllerHandle.GetEntryIndex();
    return true;
}

CON_COMMAND(mirv_attach, "Attach camera to a player attachment") {
	if (2 <= args->ArgC() && 0 == _stricmp("stop", args->ArgV(1))) {
		g_AttachmentCamera.active = false;
		g_AttachmentCameraHadError = false;
		advancedfx::Message("mirv_attach: stopped.\n");
		return;
	}

	if (args->ArgC() < 10 || 0 != _stricmp("start", args->ArgV(1))) {
		advancedfx::Message(
			"mirv_attach start <playerControllerIndex|current> <attachmentIndex|attachmentName> <offX> <offY> <offZ> <pitch> <yaw> <roll> [fov]\n"
			"mirv_attach stop\n"
		);
		return;
	}

	AttachmentCameraState newState;
	newState.active = true;
	const char* controllerArg = args->ArgV(2);

	if (0 == _stricmp("current", controllerArg)) {
		int idx = -1;
		if (!GetCurrentSpectatedControllerIndex(idx)) {
			advancedfx::Warning("mirv_attach: could not resolve 'current' (no spectated/local controller).\n");
			return;
		}
		newState.controllerIndex = idx;
	} else {
		char* endPtr = nullptr;
		long controllerVal = strtol(controllerArg, &endPtr, 10);

		if (!(endPtr && *endPtr == '\0')) {
			advancedfx::Warning("mirv_attach: controller index must be a number or 'current'.\n");
			return;
		}

		newState.controllerIndex = (int)controllerVal;
	}

	const char* attachmentArg = args->ArgV(3);
	char* endPtr = nullptr;
	long attachmentVal = strtol(attachmentArg, &endPtr, 10);
	if (endPtr && *endPtr == '\0') {
		if (attachmentVal < 0 || 0xff < attachmentVal) {
			advancedfx::Warning("mirv_attach: attachment index must be between 0 and 255.\n");
			return;
		}
		newState.useAttachmentIndex = true;
		newState.attachmentIndex = (uint8_t)attachmentVal;
	} else {
		newState.useAttachmentIndex = false;
		newState.attachmentName = attachmentArg;
	}

	newState.offsetPos.x = (float)atof(args->ArgV(4));
	newState.offsetPos.y = (float)atof(args->ArgV(5));
	newState.offsetPos.z = (float)atof(args->ArgV(6));
	newState.offsetAngles = Afx::Math::QEulerAngles(
		atof(args->ArgV(7)),
		atof(args->ArgV(8)),
		atof(args->ArgV(9))
	);
	if (args->ArgC() >= 11) {
		newState.fov = (float)atof(args->ArgV(10));
	} else {
		newState.fov = 90.0f;
	}

	g_AttachmentCamera = newState;
	g_AttachmentCameraHadError = false;

	std::string attachmentDesc = newState.useAttachmentIndex ? std::to_string(newState.attachmentIndex) : newState.attachmentName;
	advancedfx::Message(
		"mirv_attach: start controller %d attachment %s (%s) offset pos [%.2f %.2f %.2f] angles [%.2f %.2f %.2f] fov [%.2f]\n",
		newState.controllerIndex,
		newState.useAttachmentIndex ? "index" : "name",
		attachmentDesc.c_str(),
		newState.offsetPos.x, newState.offsetPos.y, newState.offsetPos.z,
		(float)newState.offsetAngles.Pitch, (float)newState.offsetAngles.Yaw, (float)newState.offsetAngles.Roll,
		newState.fov
	);
}

static bool g_bViewOverriden = false;
static float g_fFovOverride = 90.0f;
static float * g_pFov = nullptr;
int g_iWidth = 1920;
int g_iHeight = 1080;
SOURCESDK::VMatrix g_WorldToScreenMatrix;

extern bool g_b_on_c_view_render_setup_view;

extern bool MirvFovOverride(float &fov);

bool CS2_Client_CSetupView_Trampoline_IsPlayingDemo(void *ThisCViewSetup) {
	if(!g_pEngineToClient) return false;

	bool originOrAnglesOverriden = false;

	float curTime = g_MirvTime.curtime_get(); //TODO: + m_PausedTime
	float absTime = g_MirvTime.absoluteframetime_get();

	int *pWidth = (int*)((unsigned char *)ThisCViewSetup + 0x434);
	int *pHeight = (int*)((unsigned char *)ThisCViewSetup + 0x43C);

	float *pFov = (float*)((unsigned char *)ThisCViewSetup + 0x498);
	float *pViewOrigin = (float*)((unsigned char *)ThisCViewSetup + 0x4a0);
	float *pViewAngles = (float*)((unsigned char *)ThisCViewSetup + 0x4b8);

	int width = *pWidth;
	int height = *pHeight;
	float Tx = pViewOrigin[0];
	float Ty = pViewOrigin[1];
	float Tz = pViewOrigin[2];
	float Rx = pViewAngles[0];
	float Ry = pViewAngles[1];
	float Rz = pViewAngles[2];
	float Fov = *pFov;

	//advancedfx::Message("Console: %i [%ix%i]\n", (g_pGameUIService->Con_IsVisible()?1:0),width,height);

	//advancedfx::Message("%f: (%f,%f,%f) (%f,%f,%f) [%f]\n",curTime,pViewOrigin[0],pViewOrigin[1],pViewOrigin[2],pViewAngles[0],pViewAngles[1],pViewAngles[2],*pFov);

	g_MirvInputEx.GameCameraOrigin[0] = Tx;
	g_MirvInputEx.GameCameraOrigin[1] = Ty;
	g_MirvInputEx.GameCameraOrigin[2] = Tz;
	g_MirvInputEx.GameCameraAngles[0] = Rx;
	g_MirvInputEx.GameCameraAngles[1] = Ry;
	g_MirvInputEx.GameCameraAngles[2] = Rz;
	g_MirvInputEx.GameCameraFov = Fov;

	ObsWebSocket_ProcessActions();

	if (g_CamPath.Enabled_get() && g_CamPath.CanEval())
	{
		double campathCurTime = curTime - g_CamPath.GetOffset();
		if(g_CamPath.GetHold()) {
			if(campathCurTime > g_CamPath.GetUpperBound()) campathCurTime = g_CamPath.GetUpperBound();
			else if(campathCurTime < g_CamPath.GetLowerBound()) campathCurTime = g_CamPath.GetLowerBound();
		}

		// no extrapolation:
		if (g_CamPath.GetLowerBound() <= campathCurTime && campathCurTime <= g_CamPath.GetUpperBound())
		{
			CamPathValue val = g_CamPath.Eval(campathCurTime);
			QEulerAngles ang = val.R.ToQREulerAngles().ToQEulerAngles();

			//Tier0_Msg("================",curTime);
			//Tier0_Msg("currenTime = %f",curTime);
			//Tier0_Msg("vCp = %f %f %f\n", val.X, val.Y, val.Z);

			originOrAnglesOverriden = true;

			Tx = (float)val.X;
			Ty = (float)val.Y;
			Tz = (float)val.Z;

			Rx = (float)ang.Pitch;
			Ry = (float)ang.Yaw;
			Rz = (float)ang.Roll;

			Fov = (float)val.Fov;
		}
	}

	if (g_S2CamIO.GetCamImport())
	{
		CamIO::CamData camData;

		if (g_S2CamIO.GetCamImport()->GetCamData(curTime, width, height, camData))
		{
			originOrAnglesOverriden = true;

			Tx = (float)camData.XPosition;
			Ty = (float)camData.YPosition;
			Tz = (float)camData.ZPosition;
			Rx = (float)camData.YRotation;
			Ry = (float)camData.ZRotation;
			Rz = (float)camData.XRotation;
			Fov = (float)camData.Fov;
		}
	}	

	if(MirvFovOverride(Fov)) originOrAnglesOverriden = true;

	if(g_MirvInputEx.m_MirvInput->Override(g_MirvInputEx.LastFrameTime, Tx,Ty,Tz,Rx,Ry,Rz,Fov)) originOrAnglesOverriden = true;

	// Observer input handling (freecam + spectator switching)
	InputState input = {};
	if (g_pObsInput) {
		g_pObsInput->GetInputState(input);
	}
	bool altDown = input.IsKeyDown(0x12);  // VK_MENU (Alt)

	// Handle spectator key presses (1-5 and 6-0 or Q/E/R/T/Z)
	if (g_pEngineToClient) {
		const uint8_t numberKeyVks[10] = { '1','2','3','4','5','6','7','8','9','0' };
		const uint8_t altKeyVks[5] = { 'Q','E','R','T','Z' };
		bool currentNumberKeys[10] = {};

		for (int i = 0; i < 10; ++i) {
			uint8_t vk = numberKeyVks[i];
			if (g_UseAltSpectatorBindings && i >= 5) {
				vk = altKeyVks[i - 5];
			}
			currentNumberKeys[i] = input.IsKeyDown(vk);

			// Detect key press (transition from false to true)
			if (currentNumberKeys[i] && !g_LastSpectatorKeyState[i]) {
				// Check if this key is mapped to a player
				if (g_SpectatorBindings[i] != -1) {
					std::string specCmd = "spec_mode 2; spec_player " + std::to_string(g_SpectatorBindings[i]);
					g_pEngineToClient->ExecuteClientCmd(0, specCmd.c_str(), true);
					
					g_PendingSpectatorSwitch = true;
					g_SpectatorSwitchTimeout = 15; // Max 15 frames timeout
				}
			}
			g_LastSpectatorKeyState[i] = currentNumberKeys[i];
		}
	}

	// Handle deferred freecam disable
	if (g_PendingSpectatorSwitch && g_ClientDll_GetSplitScreenPlayer && g_pEntityList && g_GetEntityFromIndex) {
		CEntityInstance* localPlayer = g_ClientDll_GetSplitScreenPlayer(0);
		if (localPlayer && localPlayer->IsPlayerController()) {
			auto pawnHandle = localPlayer->GetPlayerPawnHandle();
			if (pawnHandle.IsValid()) {
				int pawnIndex = pawnHandle.GetEntryIndex();
				if (pawnIndex >= 0) {
					CEntityInstance* pawn = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, pawnIndex);
					if (pawn) {
						uint8_t currentMode = pawn->GetObserverMode();
						bool switchComplete = (currentMode == 1 || currentMode == 2);
						if (switchComplete || --g_SpectatorSwitchTimeout <= 0) {
							g_AttachmentCamera.active = false;
							g_AttachmentCameraHadError = false;
							if(g_CamPath.Enabled_get()) g_CamPath.Enabled_set(false);
							if (g_pFreecam && g_pFreecam->IsEnabled()) g_pFreecam->SetEnabled(0);
							if (g_pNadeCam && g_pNadeCam->IsEnabled()) g_pNadeCam->SetEnabled(false, false);
							if (altDown) g_NadeCamSuppressUntilAltRelease = true;
							g_PendingSpectatorSwitch = false;
						}
					}
				}
			}
		}
	}

	bool freecamEnabled = g_pFreecam && g_pFreecam->IsEnabled();
	if (!altDown) g_NadeCamSuppressUntilAltRelease = false;
	if (freecamEnabled && !g_LastFreecamEnabled) {
		if (g_pNadeCam && g_pNadeCam->IsEnabled()) g_pNadeCam->SetEnabled(false, false);
		if (altDown) g_NadeCamSuppressUntilAltRelease = true;
	}
	g_LastFreecamEnabled = freecamEnabled;

	// Freecam controller for remote observing
	if (freecamEnabled) {
		// Update freecam every frame for smooth movement
		float deltaTime = (float)g_MirvInputEx.LastFrameTime;
		g_pFreecam->Update(input, deltaTime);

		const CameraTransform& cam = g_pFreecam->GetTransform();
		Tx = cam.x;
		Ty = cam.y;
		Tz = cam.z;
		Rx = cam.pitch;
		Ry = cam.yaw;
		Rz = cam.roll;
		Fov = cam.fov;
		originOrAnglesOverriden = true;
	}

	// Nadecam
	{
		// Toggle on Alt key hold
		if (altDown && !g_NadeCamSuppressUntilAltRelease && g_pNadeCam && g_pFreecam && !g_pFreecam->IsEnabled()) {
			if (!g_pNadeCam->IsEnabled()) g_pNadeCam->SetEnabled(true);
		} else if (g_pNadeCam && g_pNadeCam->IsEnabled()) g_pNadeCam->SetEnabled(false, !altDown);

		// Update NadeCam and apply transform if active
		if (g_pNadeCam && g_pNadeCam->IsEnabled()) {
			float deltaTime = (float)g_MirvInputEx.LastFrameTime;
			CameraTransform currentCam;
			currentCam.x = Tx;
			currentCam.y = Ty;
			currentCam.z = Tz;
			currentCam.pitch = Rx;
			currentCam.yaw = Ry;
			currentCam.roll = Rz;
			currentCam.fov = Fov;
			CameraTransform nadeCamTransform;
			if (g_pNadeCam->Update(deltaTime, curTime, currentCam, nadeCamTransform)) {
				Tx = nadeCamTransform.x;
				Ty = nadeCamTransform.y;
				Tz = nadeCamTransform.z;
				Rx = nadeCamTransform.pitch;
				Ry = nadeCamTransform.yaw;
				Rz = nadeCamTransform.roll;
				Fov = nadeCamTransform.fov;
				originOrAnglesOverriden = true;
			}
		}
	}

	// Camera attachment override
	if (g_AttachmentCamera.active) {
		Afx::Math::Vector3 attachedOrigin(0.0, 0.0, 0.0);
		Afx::Math::QEulerAngles attachedAngles(0.0, 0.0, 0.0);

	float attachedFov = 90.0f;
	if (g_AttachmentCamera.animation.enabled
		&& g_AttachmentCamera.animation.hasTransition
		&& g_AttachmentCamera.animation.targetControllerIndex != -1
		&& g_pEngineToClient) {

		const double now = g_MirvTime.curtime_get();
		const double animT = now - g_AttachmentCamera.animation.startTime;
		const double transitionStart = g_AttachmentCamera.animation.transitionTime;
		const double transitionDuration = g_AttachmentCamera.animation.transitionDuration;
		const double transitionEnd = transitionStart + transitionDuration;

		if (transitionDuration > 0.0) {
			if (!g_AttachmentCamera.animation.transitionMode4Applied && animT >= transitionStart) {
				g_pEngineToClient->ExecuteClientCmd(0, "spec_mode 4", true);
				g_AttachmentCamera.animation.transitionMode4Applied = true;
			}

			if (!g_AttachmentCamera.animation.transitionApplied && animT >= transitionEnd) {
				std::string cmd = "spec_mode 2; spec_player " + std::to_string(g_AttachmentCamera.animation.targetControllerIndex);
				g_pEngineToClient->ExecuteClientCmd(0, cmd.c_str(), true);
				g_AttachmentCamera.animation.transitionApplied = true;
			}
		} else if (!g_AttachmentCamera.animation.transitionApplied && animT >= transitionStart) {
			std::string cmd = "spec_mode 2; spec_player " + std::to_string(g_AttachmentCamera.animation.targetControllerIndex);
			g_pEngineToClient->ExecuteClientCmd(0, cmd.c_str(), true);
			g_AttachmentCamera.animation.transitionApplied = true;
		}
	}

	if (TryComputeAttachmentCamera(g_AttachmentCamera, attachedOrigin, attachedAngles, attachedFov)) {
		Tx = (float)attachedOrigin.X;
		Ty = (float)attachedOrigin.Y;
		Tz = (float)attachedOrigin.Z;
		Rx = (float)attachedAngles.Pitch;
		Ry = (float)attachedAngles.Yaw;
		Rz = (float)attachedAngles.Roll;
		Fov = attachedFov;
		originOrAnglesOverriden = true;
		g_AttachmentCameraHadError = false;
	} else if (!g_AttachmentCameraHadError) {
			advancedfx::Warning("mirv_attach: failed to resolve attachment for controller %d.\n", g_AttachmentCamera.controllerIndex);
			g_AttachmentCameraHadError = true;
		}
	}

	BroadcastFreecamSpeedIfNeeded();

	if(g_b_on_c_view_render_setup_view) {
		AfxHookSourceRsView currentView = {Tx,Ty,Tz,Rx,Ry,Rz,Fov};
		AfxHookSourceRsView gameView = {(float)g_MirvInputEx.GameCameraOrigin[0],(float)g_MirvInputEx.GameCameraOrigin[1],(float)g_MirvInputEx.GameCameraOrigin[2],(float)g_MirvInputEx.GameCameraAngles[0],(float)g_MirvInputEx.GameCameraAngles[1],(float)g_MirvInputEx.GameCameraAngles[2],(float)g_MirvInputEx.GameCameraFov};
		AfxHookSourceRsView lastView = {(float)g_MirvInputEx.LastCameraOrigin[0],(float)g_MirvInputEx.LastCameraOrigin[1],(float)g_MirvInputEx.LastCameraOrigin[2],(float)g_MirvInputEx.LastCameraAngles[0],(float)g_MirvInputEx.LastCameraAngles[1],(float)g_MirvInputEx.LastCameraAngles[2],(float)g_MirvInputEx.LastCameraFov};
		if(AfxHookSource2Rs_OnCViewRenderSetupView(
			curTime, absTime, (float)g_MirvInputEx.LastFrameTime,
			currentView, gameView, lastView,
			width,height
		)) {
			Tx = currentView.x;
			Ty = currentView.y;
			Tz = currentView.z;
			Rx = currentView.rx;
			Ry = currentView.ry;
			Rz = currentView.rz;
			Fov = currentView.fov;
			originOrAnglesOverriden = true;
		}		
	}

	if (g_S2CamIO.GetCamExport())
	{
		CamIO::CamData camData;

		camData.Time = curTime;
		camData.XPosition = Tx;
		camData.YPosition = Ty;
		camData.ZPosition = Tz;
		camData.YRotation = Rx;
		camData.ZRotation = Ry;
		camData.XRotation = Rz;
		camData.Fov = Fov;

		g_S2CamIO.GetCamExport()->WriteFrame(width, height, camData);
	}	

	if(originOrAnglesOverriden) {
		pViewOrigin[0] = Tx;
		pViewOrigin[1] = Ty;
		pViewOrigin[2] = Tz;

		pViewAngles[0] = Rx;
		pViewAngles[1] = Ry;
		pViewAngles[2] = Rz;

		*pFov = Fov;

		g_bViewOverriden = true;
		g_fFovOverride = Fov;
		g_pFov = pFov;
	} else {
		g_bViewOverriden = false;
	}

	g_iWidth = width;
	g_iHeight = height;

	RenderedSetup_OnSetupViewCandidate(Tx, Ty, Tz, Rx, Ry, Rz, Fov, absTime);

	g_MirvInputEx.LastCameraOrigin[0] = Tx;
	g_MirvInputEx.LastCameraOrigin[1] = Ty;
	g_MirvInputEx.LastCameraOrigin[2] = Tz;
	g_MirvInputEx.LastCameraAngles[0] = Rx;
	g_MirvInputEx.LastCameraAngles[1] = Ry;
	g_MirvInputEx.LastCameraAngles[2] = Rz;
	g_MirvInputEx.LastCameraFov = Fov;

	g_MirvInputEx.LastFrameTime = absTime;

	g_MirvInputEx.LastWidth = width;
	g_MirvInputEx.LastHeight = height;

	g_MirvInputEx.m_MirvInput->Supply_MouseFrameEnd();

	g_CurrentGameCamera.origin[0] = Tx;
	g_CurrentGameCamera.origin[1] = Ty;
	g_CurrentGameCamera.origin[2] = Tz;
	g_CurrentGameCamera.angles[0] = Rx;
	g_CurrentGameCamera.angles[1] = Ry;
	g_CurrentGameCamera.angles[2] = Rz;
	g_CurrentGameCamera.time = curTime;

	return g_pEngineToClient->IsPlayingDemo();
}

typedef void (__fastcall * Unk_Override_Fov_t)(void *param_1,int param_2);
Unk_Override_Fov_t g_Old_Unk_Override_Fov = nullptr;
void __fastcall New_Unk_Override_Fov(void *param_1,int param_2) {

	if(g_bViewOverriden) {
		float * pWeaponFov = g_pFov + 1;
		float oldFov = *g_pFov;
		float oldWeaponFov = *pWeaponFov;

		g_Old_Unk_Override_Fov(param_1,param_2);

		*g_pFov = Apply_FovScaling(g_MirvInputEx.LastWidth, g_MirvInputEx.LastHeight, oldFov, FovScaling_AlienSwarm);
		*pWeaponFov = Apply_FovScaling(g_MirvInputEx.LastWidth, g_MirvInputEx.LastHeight, oldWeaponFov, FovScaling_AlienSwarm);
	} else {
		g_Old_Unk_Override_Fov(param_1,param_2);
	}
}

/*size_t ofsProj = 0;

DirectX::XMMATRIX g_Mul = {
	{1,0,0,0},
	{0,1,0,0},
	{0,0,1,0},
	{0,0,0,1}
};

bool transpose = false;

CON_COMMAND(__mirv_t,"") {
	if(args->ArgC()>=2) transpose = 0 != atoi(args->ArgV(1));
}

CON_COMMAND(__mirv_o,"") {
	advancedfx::Message("=> %i\n",ofsProj);

	float b0 = ((ofsProj>>0) & 1) ? -1 : 1;
	float b1 = ((ofsProj>>1) & 1) ? -1 : 1;
	float b2 = ((ofsProj>>2) & 1) ? -1 : 1;

	switch((ofsProj>>3)%6) {
	default:
	case 0:
		// 0 1 2
		g_Mul = DirectX::XMMATRIX(
			b0, 0, 0, 0,
			0, b1, 0, 0,
			0, 0, b2, 0,
			0, 0, 0, 1
		);
		break;
	case 1:
		// 0 2 1
		g_Mul = DirectX::XMMATRIX(
			b0, 0, 0, 0,
			0, 0, b2, 0,
			0, b1, 0, 0,
			0, 0, 0, 1
		);
		break;
	case 2:
		// 1 0 2
		g_Mul = DirectX::XMMATRIX(
			0, b1, 0, 0,
			b0, 0, 0, 0,
			0, 0, b2, 0,
			0, 0, 0, 1
		);
		break;
	case 3:
		// 1 2 0
		// 0 1 2
		g_Mul = DirectX::XMMATRIX(
			0, b1, 0, 0,
			0, 0, b2, 0,
			b0, 0, 0, 0,
			0, 0, 0, 1
		);
		break;
	case 4:
		// 2 0 1
		g_Mul = DirectX::XMMATRIX(
			0, 0, b2, 0,
			b0, 0, 0, 0,
			0, b1, 0, 0,
			0, 0, 0, 1
		);		
		break;
	case 5:
		// 2 1 0
		g_Mul = DirectX::XMMATRIX(
			0, 0, b2, 0,
			0, b1, 0, 0,
			b0, 0, 0, 0,
			0, 0, 0, 1
		);		
		break;
	}


	ofsProj = (ofsProj + 1)%48;
}*/

typedef void (__fastcall * CViewRender_UnkMakeMatrix_t)(void* This);
CViewRender_UnkMakeMatrix_t g_Old_CViewRender_UnkMakeMatrix = nullptr;
void __fastcall New_CViewRender_UnkMakeMatrix(void* This) {
	g_Old_CViewRender_UnkMakeMatrix(This);
	//memcpy(g_WorldToScreenMatrix.m,(unsigned char*)This + 0x1b8,sizeof(g_WorldToScreenMatrix.m));


	/*DirectX::XMMATRIX * proj = (DirectX::XMMATRIX *)((unsigned char*)This + 0x298);
	DirectX::XMMATRIX result = g_Mul * *proj;
	if(transpose) result = DirectX::XMMatrixTranspose(result);

	g_WorldToScreenMatrix.m[0][0] = result(0,0);
	g_WorldToScreenMatrix.m[0][1] = result(0,1);
	g_WorldToScreenMatrix.m[0][2] = result(0,2);
	g_WorldToScreenMatrix.m[0][3] = result(0,3);
	g_WorldToScreenMatrix.m[1][0] = result(1,0);
	g_WorldToScreenMatrix.m[1][1] = result(1,1);
	g_WorldToScreenMatrix.m[1][2] = result(1,2);
	g_WorldToScreenMatrix.m[1][3] = result(1,3);
	g_WorldToScreenMatrix.m[2][0] = result(2,0);
	g_WorldToScreenMatrix.m[2][1] = result(2,1);
	g_WorldToScreenMatrix.m[2][2] = result(2,2);
	g_WorldToScreenMatrix.m[2][3] = result(2,3);
	g_WorldToScreenMatrix.m[3][0] = result(3,0);
	g_WorldToScreenMatrix.m[3][1] = result(3,1);
	g_WorldToScreenMatrix.m[3][2] = result(3,2);
	g_WorldToScreenMatrix.m[3][3] = result(3,3);*/

	float* proj = (float*)((unsigned char*)This + 0x218);
	SOURCESDK::VMatrix projectionMatrix;
	projectionMatrix.m[0][0] = proj[4 * 0 + 0];
	projectionMatrix.m[0][1] = proj[4 * 0 + 1];
	projectionMatrix.m[0][2] = proj[4 * 0 + 2];
	projectionMatrix.m[0][3] = proj[4 * 0 + 3];
	projectionMatrix.m[1][0] = proj[4 * 1 + 0];
	projectionMatrix.m[1][1] = proj[4 * 1 + 1];
	projectionMatrix.m[1][2] = proj[4 * 1 + 2];
	projectionMatrix.m[1][3] = proj[4 * 1 + 3];
	projectionMatrix.m[2][0] = proj[4 * 2 + 0];
	projectionMatrix.m[2][1] = proj[4 * 2 + 1];
	projectionMatrix.m[2][2] = proj[4 * 2 + 2];
	projectionMatrix.m[2][3] = proj[4 * 2 + 3];
	projectionMatrix.m[3][0] = proj[4 * 3 + 0];
	projectionMatrix.m[3][1] = proj[4 * 3 + 1];
	projectionMatrix.m[3][2] = proj[4 * 3 + 2];
	projectionMatrix.m[3][3] = proj[4 * 3 + 3];
	RenderSystemDX11_SupplyProjectionMatrix(projectionMatrix);

	proj = (float *)((unsigned char*)This + 0x298);
	SOURCESDK::VMatrix worldToScreenMatrix;
	worldToScreenMatrix.m[0][0] = proj[4*0+0];
	worldToScreenMatrix.m[0][1] = proj[4*0+1];
	worldToScreenMatrix.m[0][2] = proj[4*0+2];
	worldToScreenMatrix.m[0][3] = proj[4*0+3];
	worldToScreenMatrix.m[1][0] = proj[4*1+0];
	worldToScreenMatrix.m[1][1] = proj[4*1+1];
	worldToScreenMatrix.m[1][2] = proj[4*1+2];
	worldToScreenMatrix.m[1][3] = proj[4*1+3];
	worldToScreenMatrix.m[2][0] = proj[4*2+0];
	worldToScreenMatrix.m[2][1] = proj[4*2+1];
	worldToScreenMatrix.m[2][2] = proj[4*2+2];
	worldToScreenMatrix.m[2][3] = proj[4*2+3];
	worldToScreenMatrix.m[3][0] = proj[4*3+0];
	worldToScreenMatrix.m[3][1] = proj[4*3+1];
	worldToScreenMatrix.m[3][2] = proj[4*3+2];
	worldToScreenMatrix.m[3][3] = proj[4*3+3];
	g_WorldToScreenMatrix = worldToScreenMatrix;

	const uint64_t setupSerial = RenderedSetup_GetLatestCandidateSerial();
	if (setupSerial) {
		RenderedSetup_OnWorldToScreenCandidate(setupSerial, worldToScreenMatrix);
		g_MirvImageDrawer.UpdateAttachmentsForSetupSerial(setupSerial);
		if (setupSerial <= g_RenderedSetupConfirmedSerial.load(std::memory_order_acquire)) {
			g_MirvImageDrawer.PublishAttachmentsForSetupSerial(setupSerial);
		}
	} else {
		g_MirvImageDrawer.UpdateAttachments();
	}

	g_CampathDrawer.OnEngineThread_SetupViewDone();
	g_PlayerPathDrawer.OnEngineThread_SetupViewDone();
}

/*
class CCSGOVScriptGameSystem;
CCSGOVScriptGameSystem * g_pCCSGOVScriptGameSystem = nullptr;
typedef void (__fastcall * CCSGOVScriptGameSystem_UnkAddon_t)(CCSGOVScriptGameSystem *This); //:000
typedef unsigned long long int (__fastcall * CCSGOVScriptGameSystem_UnkLoadScriptFile_t)(CCSGOVScriptGameSystem *This, const char * pszFileName, bool bDebugPrint); //:008
CCSGOVScriptGameSystem_UnkAddon_t g_Old_CCSGOVScriptGameSystem_UnkAddon = nullptr;
CCSGOVScriptGameSystem_UnkLoadScriptFile_t g_Old_CCSGOVScriptGameSystem_UnkLoadScriptFile = nullptr;

void __fastcall New_CSGOVScriptGameSystem_UnkAddon(CCSGOVScriptGameSystem *This) {
	g_pCCSGOVScriptGameSystem = This;
	//advancedfx::Message("GOT IT\n");
	g_Old_CCSGOVScriptGameSystem_UnkAddon(This);
}

unsigned long long int __fastcall New_CCSGOVScriptGameSystem_UnkLoadScriptFile(CCSGOVScriptGameSystem *This, const char * pszFileName, bool bDebugPrint) {
	g_pCCSGOVScriptGameSystem = This;
	advancedfx::Message("LoadScriptFile: %s\n",pszFileName);
	return g_Old_CCSGOVScriptGameSystem_UnkLoadScriptFile(This, pszFileName, bDebugPrint);
}

CON_COMMAND(mirv_vscript_exec,"") {
	int argC = args->ArgC();
	const char * arg0 = args->ArgV(0);

	if(2 <= argC) {
		if(g_pCCSGOVScriptGameSystem) {
			g_Old_CCSGOVScriptGameSystem_UnkLoadScriptFile(g_pCCSGOVScriptGameSystem,args->ArgV(1),3 <= argC ? (0 != atoi(args->ArgV(2))) : true);
		} else advancedfx::Warning("Missing hooks.\n");
		return;
	}
	advancedfx::Message("%s <script_file_name> [<debug_print=0|1>]\n",arg0);
}*/

/*
typedef void (__fastcall * CViewRender_RenderView_t)(void* This, void * pViewSetup, void * pHudViewSetup, void * nClearFlags, void * whatToDraw);
CViewRender_RenderView_t g_Old_CViewRender_RenderView = nullptr;

enum ClearFlags_t
{
	VIEW_CLEAR_COLOR = 0x1,
	VIEW_CLEAR_DEPTH = 0x2,
	VIEW_CLEAR_FULL_TARGET = 0x4,
	VIEW_NO_DRAW = 0x8,
	VIEW_CLEAR_OBEY_STENCIL = 0x10, // Draws a quad allowing stencil test to clear through portals
	VIEW_CLEAR_STENCIL = 0x20,
};
void __fastcall New_CViewRender_RenderView(void* This, void * pViewSetup, void * pHudViewSetup, void * nClearFlags, void * whatToDraw) {
	return;
	g_Old_CViewRender_RenderView(This, pViewSetup, pHudViewSetup, nClearFlags, whatToDraw);
	//if((nClearFlags & VIEW_CLEAR_COLOR)&&(nClearFlags && VIEW_CLEAR_DEPTH)) {
		DrawCampath();
	//}
}*/

void HookClientDll(HMODULE clientDll) {
	static bool bFirstCall = true;
	if(!bFirstCall) return;
	bFirstCall = false;

	Afx::BinUtils::MemRange textRange = Afx::BinUtils::MemRange::FromEmpty();
	Afx::BinUtils::MemRange dataRange = Afx::BinUtils::MemRange::FromEmpty();
	{
		Afx::BinUtils::ImageSectionsReader sections((HMODULE)clientDll);
		if(!sections.Eof()) {
			textRange = sections.GetMemRange();
			sections.Next();
			if(!sections.Eof()){
				dataRange = sections.GetMemRange();
			}
		}
	}

	/*
		This is where it checks for engine->IsPlayingDemo() (and afterwards for cl_demoviewoverride (float))
		before under these conditions it is calling CalcDemoViewOverride, so this is in CViewRender::SetUpView:

       180898120 48 8b 0d        MOV        RCX,qword ptr [DAT_181e2d7a8]
                 81 56 59 01
       180898127 48 8b 01        MOV        RAX,qword ptr [RCX]
       18089812a ff 90 40        CALL       qword ptr [RAX + 0x148]
                 01 00 00
       180898130 0f 57 f6        XORPS      XMM6,XMM6
       180898133 84 c0           TEST       AL,AL
       180898135 74 63           JZ         LAB_18089819a
       180898137 ba ff ff        MOV        EDX,0xffffffff
                 ff ff

	*/
	{
		Afx::BinUtils::MemRange result = FindPatternString(textRange, "48 8b 0d ?? ?? ?? ?? 48 8b 01 ff 90 48 01 00 00 0f 57 ff 84 c0 74 63 ba ff ff ff ff");
																	  
		if (!result.IsEmpty()) {
			/*
				These are the top 16 bytes we change to:

180882cd6	4889f1               mov     rcx, rsi
			48b8???????????????? mov     rax, ???????????????? <-- here we load our hook's address
			ff10                 call    qword ptr [rax]
			90                   nop
			*/
			unsigned char asmCode[16]={
				0x48, 0x89, 0xf1,
				0x48, 0xb8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
				0xff, 0x10,
				0x90
			};
			static LPVOID ptr = CS2_Client_CSetupView_Trampoline_IsPlayingDemo;
			LPVOID ptrPtr = &ptr;
			memcpy(&asmCode[5], &ptrPtr, sizeof(LPVOID));

			MdtMemBlockInfos mbis;
			MdtMemAccessBegin((LPVOID)result.Start, 16, &mbis);
			memcpy((LPVOID)result.Start, asmCode, 16);
			MdtMemAccessEnd(&mbis);
		}
		else
			ErrorBox(MkErrStr(__FILE__, __LINE__));
	}	

	/*
		The FOV is overridden / computed a second time in the function called in
		CViewRender::SetUpView (see hook above on how to find it):
		
       180898360 49 8b cf        MOV        RCX,R15
       180898363 8b 10           MOV        EDX,dword ptr [RAX]
       180898365 e8 b6 fd        CALL       FUN_180888120 <-- we detour this function.                                   undefined FUN_180888120()
                 fe ff
       18089836a 4c 8b c7        MOV        R8,RDI
       18089836d 41 c6 87        MOV        byte ptr [R15 + 0x1330],0x0
                 30 13 00 
                 00 00

		void FUN_180888120(longlong *param_1,int param_2) { ... }
	*/
	// Commenting out this for now since it's now in the same function as above
	// {
	// 	Afx::BinUtils::MemRange result = FindPatternString(textRange, "48 8B C4 53 55 56 57 41 56 41 57");
	// 	if (!result.IsEmpty()) {
	// 		g_Old_Unk_Override_Fov = (Unk_Override_Fov_t)result.Start;
	// 		DetourTransactionBegin();
	// 		DetourUpdateThread(GetCurrentThread());
	// 		DetourAttach((PVOID*)&g_Old_Unk_Override_Fov, New_Unk_Override_Fov);
	// 		if(NO_ERROR != DetourTransactionCommit()) ErrorBox(MkErrStr(__FILE__, __LINE__));            
	// 	} else ErrorBox(MkErrStr(__FILE__, __LINE__));
	// }
/*
	if(void ** vtable = (void**)Afx::BinUtils::FindClassVtable(clientDll,".?AVCRenderingPipelineCsgo@@", 0, 0x0)) {
		g_Old_CViewRender_RenderView = (CViewRender_RenderView_t)vtable[0] ;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_Old_CViewRender_RenderView,New_CViewRender_RenderView);
        // doesn't work without error // DetourAttach(&(PVOID&)g_Old_CCSGOVScriptGameSystem_UnkLoadScriptFile, New_CCSGOVScriptGameSystem_UnkLoadScriptFile);
        if(NO_ERROR != DetourTransactionCommit()) ErrorBox(MkErrStr(__FILE__, __LINE__));
	} else ErrorBox(MkErrStr(__FILE__, __LINE__));*/

	if(!Hook_CGameEventManager((void*)clientDll)) ErrorBox(MkErrStr(__FILE__, __LINE__));
/*
	if(void ** vtable = (void**)Afx::BinUtils::FindClassVtable(clientDll,".?AVCCSGOVScriptGameSystem@@", 0, 0x10)) {
		g_Old_CCSGOVScriptGameSystem_UnkAddon = (CCSGOVScriptGameSystem_UnkAddon_t)vtable[0] ;
		g_Old_CCSGOVScriptGameSystem_UnkLoadScriptFile = (CCSGOVScriptGameSystem_UnkLoadScriptFile_t)vtable[5] ;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_Old_CCSGOVScriptGameSystem_UnkAddon,New_CSGOVScriptGameSystem_UnkAddon);
        // doesn't work without error // DetourAttach(&(PVOID&)g_Old_CCSGOVScriptGameSystem_UnkLoadScriptFile, New_CCSGOVScriptGameSystem_UnkLoadScriptFile);
        if(NO_ERROR != DetourTransactionCommit()) ErrorBox(MkErrStr(__FILE__, __LINE__));
		else AfxDetourPtr((PVOID *)&(vtable[7]), New_CCSGOVScriptGameSystem_UnkLoadScriptFile, (PVOID*)&g_Old_CCSGOVScriptGameSystem_UnkLoadScriptFile);
	} else ErrorBox(MkErrStr(__FILE__, __LINE__));*/

	if(void ** vtable = (void**)Afx::BinUtils::FindClassVtable(clientDll,".?AVCViewRender@@", 0, 0x0)) {
		g_Old_CViewRender_UnkMakeMatrix = (CViewRender_UnkMakeMatrix_t)vtable[4] ;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_Old_CViewRender_UnkMakeMatrix,New_CViewRender_UnkMakeMatrix);
        if(NO_ERROR != DetourTransactionCommit()) ErrorBox(MkErrStr(__FILE__, __LINE__));
	} else ErrorBox(MkErrStr(__FILE__, __LINE__));

	// client entity system related
	{
		// "Entities/Client Entity Count"
		auto unkFn = Afx::BinUtils::FindPatternString(textRange, "40 55 53 48 8d ac 24 ?? ?? ?? ?? 48 81 ec ?? ?? ?? ?? 48 8b 0d ?? ?? ?? ?? 33 d2 e8 ?? ?? ?? ??");
		if (!unkFn.IsEmpty()) {
			void * pEntityList = (void *)(unkFn.Start+18+7+*(int*)(unkFn.Start+18+3));
			void * pFnGetHighestEntityIterator = (void *)(unkFn.Start+27+5+*(int*)(unkFn.Start+27+1));

			// see near "no such entity %d\n" called with pEntityList and uint
            // or near "Format: ent_find_index <index>\n" called only with uint and there's pEntityList inside with uint
			auto fnGetEntityFromIndexMem = Afx::BinUtils::FindPatternString(textRange, "4c 8d 49 10 81 fa fe 7f 00 00");
			if (!fnGetEntityFromIndexMem.IsEmpty()) {
				auto pFnGetEntityFromIndex = (void*)(fnGetEntityFromIndexMem.Start);
				if(! Hook_ClientEntitySystem( pEntityList, pFnGetHighestEntityIterator, pFnGetEntityFromIndex )) ErrorBox(MkErrStr(__FILE__, __LINE__));

			} else ErrorBox(MkErrStr(__FILE__, __LINE__));

		} else ErrorBox(MkErrStr(__FILE__, __LINE__));

	}
	/*
	   GetSplitScreenPlayer(int): 
	   This function is called in GetLocalPlayerController script function in clientDll, 
	   go inside of function there and it's called with 0.

       1808541f0 40 53           PUSH       RBX
       1808541f2 48 83 ec 20     SUB        RSP,0x20
       1808541f6 8b 91 9c        MOV        EDX,dword ptr [RCX + 0x9c]
                 00 00 00
       1808541fc 48 8b d9        MOV        RBX,RCX
       1808541ff 83 fa ff        CMP        EDX,-0x1
       180854202 0f 84 27        JZ         LAB_18085432f
                 01 00 00
       180854208 4c 8b 0d        MOV        R9,qword ptr [DAT_1818b7d68]
                 59 3b 06 01
       18085420f 4d 85 c9        TEST       R9,R9

	*/
	{
		Afx::BinUtils::MemRange range_get_split_screen_player = Afx::BinUtils::FindPatternString(textRange, "48 83 EC ?? 83 F9 ?? 75 ?? 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 ?? 48 8B 01 FF 90 ?? ?? ?? ?? 8B 08 48 63 C1 48 8D 0D ?? ?? ?? ?? 48 8B 04 C1 48 83 C4 ?? C3");
		if(!range_get_split_screen_player.IsEmpty()) {
			Hook_GetSplitScreenPlayer((void*)range_get_split_screen_player.Start);
		} else ErrorBox(MkErrStr(__FILE__, __LINE__));
	}
}

SOURCESDK::CreateInterfaceFn g_AppSystemFactory = nullptr;
SOURCESDK::CS2::IMemAlloc *SOURCESDK::CS2::g_pMemAlloc = nullptr;
SOURCESDK::CS2::ICvar * SOURCESDK::CS2::cvar = nullptr;
SOURCESDK::CS2::ICvar * SOURCESDK::CS2::g_pCVar = nullptr;
void * g_pSceneSystem = nullptr;

typedef bool (__fastcall * CSceneSystem_WaitForRenderingToComplete_t)(void * This);
CSceneSystem_WaitForRenderingToComplete_t g_Old_CSceneSystem_WaitForRenderingToComplete = nullptr;

bool __fastcall New_CSceneSystem_WaitForRenderingToComplete(void * This) {
	bool result = g_Old_CSceneSystem_WaitForRenderingToComplete(This);
	//DrawCampath();
	return result;
}

typedef int(* CCS2_Client_Connect_t)(void* This, SOURCESDK::CreateInterfaceFn appSystemFactory);
CCS2_Client_Connect_t old_CCS2_Client_Connect;
int new_CCS2_Client_Connect(void* This, SOURCESDK::CreateInterfaceFn appSystemFactory) {
	static bool bFirstCall = true;

	if (bFirstCall) {
		bFirstCall = false;

		void * iface = NULL;

		if (SOURCESDK::CS2::g_pCVar = SOURCESDK::CS2::cvar = (SOURCESDK::CS2::ICvar*)appSystemFactory(SOURCESDK_CS2_CVAR_INTERFACE_VERSION, NULL)) {
		}
		else ErrorBox(MkErrStr(__FILE__, __LINE__));

		if (g_pEngineToClient = (SOURCESDK::CS2::ISource2EngineToClient*)appSystemFactory(SOURCESDK_CS2_ENGINE_TO_CLIENT_INTERFACE_VERSION, NULL)) {
		}
		else ErrorBox(MkErrStr(__FILE__, __LINE__));

		if (g_pGameUIService = (SOURCESDK::CS2::IGameUIService*)appSystemFactory(SOURCESDK_CS2_GAMEUISERVICE_INTERFACE_VERSION, NULL)) {
		}
		else ErrorBox(MkErrStr(__FILE__, __LINE__));

		if (g_pSceneSystem = (SOURCESDK::CS2::IGameUIService*)appSystemFactory("SceneSystem_002", NULL)) {
			Hook_SceneSystem_WaitForRenderingToComplete(g_pSceneSystem);
		}
		else ErrorBox(MkErrStr(__FILE__, __LINE__));
	}

	return old_CCS2_Client_Connect(This, appSystemFactory);
}

static void StringReplace(std::string& data, const std::string& toSearch, const std::string& replaceStr)
{
	size_t pos = data.find(toSearch);
	while (pos != std::string::npos)
	{
		data.replace(pos, toSearch.size(), replaceStr);
		pos = data.find(toSearch, pos + replaceStr.size());
	}
}

struct ConEntry_t
{
	ConEntry_t() : pszName(nullptr), defaultValue(nullptr), conVarType(SOURCESDK::CS2::EConVarType_Invalid), flags(0), pszDescription(nullptr)
	{
	}

	ConEntry_t(const char* pszName, const SOURCESDK::CS2::CVValue_t* defaultValue, SOURCESDK::CS2::EConVarType conVarType, SOURCESDK::int64 flags, const char* pszDescription)
		: pszName(pszName), defaultValue(defaultValue), conVarType(conVarType), flags(flags), pszDescription(pszDescription)
	{
	}

	const char* pszName;
	const SOURCESDK::CS2::CVValue_t* defaultValue;
	SOURCESDK::CS2::EConVarType conVarType;
	SOURCESDK::int64 flags;
	const char* pszDescription;
};

static std::string FormatCVValue(const SOURCESDK::CS2::CVValue_t& value, SOURCESDK::CS2::EConVarType type) {
	std::ostringstream oss;

	switch (type) {
	case SOURCESDK::CS2::EConVarType_Bool:
		return value.m_bValue ? "true" : "false";
	case SOURCESDK::CS2::EConVarType_Int16:
		oss << value.m_i16Value;
		break;
	case SOURCESDK::CS2::EConVarType_UInt16:
		oss << value.m_u16Value;
		break;
	case SOURCESDK::CS2::EConVarType_Int32:
		oss << value.m_i32Value;
		break;
	case SOURCESDK::CS2::EConVarType_UInt32:
		oss << value.m_u32Value;
		break;
	case SOURCESDK::CS2::EConVarType_Int64:
		oss << value.m_i64Value;
		break;
	case SOURCESDK::CS2::EConVarType_UInt64:
		oss << value.m_u64Value;
		break;
	case SOURCESDK::CS2::EConVarType_Float32:
		oss << value.m_flValue;
		break;
	case SOURCESDK::CS2::EConVarType_Float64:
		oss << value.m_dbValue;
		break;
	case SOURCESDK::CS2::EConVarType_String:
		oss << (value.m_szValue.Get() ? value.m_szValue.Get() : "");
		break;
	case SOURCESDK::CS2::EConVarType_Color:
		oss << value.m_clrValue.r() << " " << value.m_clrValue.g() << " "
			<< value.m_clrValue.b() << " " << value.m_clrValue.a();
		break;
	case SOURCESDK::CS2::EConVarType_Vector2:
		oss << value.m_vec2Value.x << " " << value.m_vec2Value.y;
		break;
	case SOURCESDK::CS2::EConVarType_Vector3:
		oss << value.m_vec3Value.x << " " << value.m_vec3Value.y << " " << value.m_vec3Value.z;
		break;
	case SOURCESDK::CS2::EConVarType_Vector4:
		oss << value.m_vec4Value.x << " " << value.m_vec4Value.y << " " << value.m_vec4Value.z
			<< " " << value.m_vec4Value.w;
		break;
	case SOURCESDK::CS2::EConVarType_Qangle:
		oss << value.m_angValue.x << " " << value.m_angValue.y << " " << value.m_angValue.z;
		break;
	default:
		return "(unknown type)";
	}

	return oss.str();
}

static std::string ConvarFlagsString(SOURCESDK::int64 unFlags)
{
	std::vector<std::string> flags;

	if (unFlags & SOURCESDK_CS2_FCVAR_DEVELOPMENTONLY)
		flags.push_back("devonly");
	if (unFlags & SOURCESDK_CS2_FCVAR_GAMEDLL)
		flags.push_back("sv");
	if (unFlags & SOURCESDK_CS2_FCVAR_CLIENTDLL)
		flags.push_back("cl");
	if (unFlags & SOURCESDK_CS2_FCVAR_HIDDEN)
		flags.push_back("hidden");
	if (unFlags & SOURCESDK_CS2_FCVAR_PROTECTED)
		flags.push_back("prot");
	if (unFlags & SOURCESDK_CS2_FCVAR_SPONLY)
		flags.push_back("sp");
	if (unFlags & SOURCESDK_CS2_FCVAR_ARCHIVE)
		flags.push_back("a");
	if (unFlags & SOURCESDK_CS2_FCVAR_NOTIFY)
		flags.push_back("nf");
	if (unFlags & SOURCESDK_CS2_FCVAR_USERINFO)
		flags.push_back("user");
	if (unFlags & SOURCESDK_CS2_FCVAR_UNLOGGED)
		flags.push_back("unlogged");
	if (unFlags & SOURCESDK_CS2_FCVAR_REPLICATED)
		flags.push_back("rep");
	if (unFlags & SOURCESDK_CS2_FCVAR_CHEAT)
		flags.push_back("cheat");
	if (unFlags & SOURCESDK_CS2_FCVAR_DEMO)
		flags.push_back("demo");
	if (unFlags & SOURCESDK_CS2_FCVAR_DONTRECORD)
		flags.push_back("norecord");
	if (unFlags & SOURCESDK_CS2_FCVAR_RELEASE)
		flags.push_back("release");
	if (unFlags & SOURCESDK_CS2_FCVAR_NOT_CONNECTED)
		flags.push_back("notconnected");
	if (unFlags & SOURCESDK_CS2_FCVAR_SERVER_CAN_EXECUTE)
		flags.push_back("server_can_execute");
	if (unFlags & SOURCESDK_CS2_FCVAR_SERVER_CANNOT_QUERY)
		flags.push_back("server_cannot_query");
	if (unFlags & SOURCESDK_CS2_FCVAR_CLIENTCMD_CAN_EXECUTE)
		flags.push_back("clientcmd_can_execute");

	std::string result;
	bool bFirst = true;

	for (auto& flag : flags)
	{
		if (bFirst)
			bFirst = false;
		else
			result += ", ";

		result += flag;
	}

	return result;
}

static std::string MarkdownEscape(const std::string& str)
{
	std::string escaped{ str };

	StringReplace(escaped, "\\", "\\\\");
	StringReplace(escaped, "<", "&lt;");
	StringReplace(escaped, ">", "&gt;");
	StringReplace(escaped, "[", "\\[");
	StringReplace(escaped, "]", "\\]");
	StringReplace(escaped, "\n", "<br>");
	StringReplace(escaped, "|", "\\|");

	return escaped;
}

static std::string GetGameDirectoryPath()
{
	char path[MAX_PATH] = {};
	if (g_H_ClientDll && 0 != GetModuleFileNameA(g_H_ClientDll, path, MAX_PATH))
	{
		std::string gamePath(path);
		for (int i = 0; i < 3; ++i)
		{
			auto pos = gamePath.find_last_of("\\/");
			if (pos == std::string::npos) break;
			gamePath.erase(pos);
		}
		return gamePath;
	}

	if (0 != GetModuleFileNameA(nullptr, path, MAX_PATH))
	{
		std::string exePath(path);
		auto pos = exePath.find_last_of("\\/");
		if (pos != std::string::npos)
			exePath.erase(pos);
		return exePath;
	}

	return ".";
}

CON_COMMAND(cvarlist_md, "List all convars/concmds in Markdown format. Format: [hidden]")
{
	if (!SOURCESDK::CS2::g_pCVar)
	{
		advancedfx::Warning("cvarlist_md: ICvar not available.\n");
		return;
	}

	std::map<std::string, ConEntry_t> allEntries;

	for (size_t i = 0; i < 65536; ++i)
	{
		SOURCESDK::CS2::CCmd* cmd = SOURCESDK::CS2::g_pCVar->GetCmd(i);
		if (nullptr == cmd) break;

		SOURCESDK::int64 nFlags = cmd->GetFlags();
		if (nFlags == 0x400) break;

		allEntries[cmd->GetName()] = ConEntry_t(cmd->GetName(), nullptr, SOURCESDK::CS2::EConVarType_Invalid, nFlags, cmd->GetHelpString());
	}

	for (size_t i = 0; i < 65536; ++i)
	{
		SOURCESDK::CS2::Cvar_s* cvar = SOURCESDK::CS2::g_pCVar->GetCvar(i);
		if (nullptr == cvar) break;

		allEntries[cvar->m_pszName] = ConEntry_t(cvar->m_pszName, cvar->m_defaultValue, cvar->m_eVarType, cvar->m_nFlags, cvar->m_pszHelpString);
	}

	bool bShowHidden = args->ArgC() > 1 && 0 == _stricmp(args->ArgV(1), "hidden");

	std::string outputPath = GetGameDirectoryPath();
	outputPath.append("\\cvarlist.md");

	std::ofstream file(outputPath, std::ios::trunc);
	if (!file.is_open())
	{
		advancedfx::Warning("cvarlist_md: could not open %s for writing.\n", outputPath.c_str());
		return;
	}

	file << "Name | Flags | Description\n";
	file << "---- | ----- | -----------\n";

	int nWritten = 0;
	for (auto& pair : allEntries)
	{
		const std::string& name = pair.first;
		const auto& cmd = pair.second;

		if (!bShowHidden && (cmd.flags & (SOURCESDK_CS2_FCVAR_DEVELOPMENTONLY | SOURCESDK_CS2_FCVAR_HIDDEN)))
			continue;

		auto flagsStr = ConvarFlagsString(cmd.flags);
		std::string helpText = cmd.pszDescription ? cmd.pszDescription : "";
		helpText = MarkdownEscape(helpText);

		file << name << " | " << flagsStr << " | ";

		if (cmd.defaultValue != nullptr && cmd.conVarType != SOURCESDK::CS2::EConVarType_Invalid)
		{
			auto defaultValue = MarkdownEscape(FormatCVValue(*cmd.defaultValue, cmd.conVarType));
			file << "Default: " << defaultValue << "<br>";
		}

		file << helpText << "\n";
		nWritten++;
	}

	file.close();

	advancedfx::Message("cvarlist_md wrote %d entries to %s\n", nWritten, outputPath.c_str());
}

CON_COMMAND(mirv_cvar_unhide_all, "Unlocks cmds and cvars.") {
	int total = 0;
	int nUnhidden = 0;
	for(size_t i = 0; i < 65536; i++ )
	{
		SOURCESDK::CS2::CCmd * cmd = SOURCESDK::CS2::g_pCVar->GetCmd(i);
		if(nullptr == cmd) break;
		int nFlags = cmd->GetFlags();
		if(nFlags == 0x400) break;
		total++;
		if(nFlags & (SOURCESDK_CS2_FCVAR_DEVELOPMENTONLY | SOURCESDK_CS2_FCVAR_HIDDEN)) {
//			fprintf(f1,"[+] %lli: 0x%08x: %s : %s\n", i, cmd->m_nFlags, cmd->m_pszName, cmd->m_pszHelpString);
			cmd->SetFlags(nFlags &= ~(SOURCESDK::int64)(SOURCESDK_CS2_FCVAR_DEVELOPMENTONLY | SOURCESDK_CS2_FCVAR_HIDDEN));
			nUnhidden++;
		} else {
//			fprintf(f1,"[ ] %lli: 0x%08x: %s : %s\n", i, cmd->m_nFlags, cmd->m_pszName, cmd->m_pszHelpString);
		}
	}
	advancedfx::Message("==== Cmds total: %i (Cmds unhidden: %i) ====\n",total,nUnhidden);

	total = 0;
	nUnhidden = 0;
	for(size_t i = 0; i < 65536; i++ )
	{
		SOURCESDK::CS2::Cvar_s * cvar = SOURCESDK::CS2::g_pCVar->GetCvar(i);
		if(nullptr == cvar) break;
		total++;
		if(cvar->m_nFlags & (SOURCESDK_CS2_FCVAR_DEVELOPMENTONLY | SOURCESDK_CS2_FCVAR_HIDDEN)) {
//			fprintf(f1,"[+] %lli: 0x%08x: %s : %s\n", i, cvar->m_nFlags, cvar->m_pszName, cvar->m_pszHelpString);
			cvar->m_nFlags &= ~(SOURCESDK::int64)(SOURCESDK_CS2_FCVAR_DEVELOPMENTONLY | SOURCESDK_CS2_FCVAR_HIDDEN);
			nUnhidden++;
		} else {
//			fprintf(f1,"[ ] %lli: 0x%08x: %s : %s\n", i, cvar->m_nFlags, cvar->m_pszName, cvar->m_pszHelpString);
		}
	}
	
	advancedfx::Message("==== Cvars total: %i (Cvars unhidden: %i) ====\n",total,nUnhidden);
}

CON_COMMAND(mirv_cvar_unlock_sv_cheats, "Unlocks sv_cheats on client (as much as possible).") {
	int total = 0;
	int nUnhidden = 0;
	for(size_t i = 0; i < 65536; i++ )
	{
		SOURCESDK::CS2::CCmd * cmd = SOURCESDK::CS2::g_pCVar->GetCmd(i);
		if(nullptr == cmd) break;
		int nFlags = cmd->GetFlags();
		if(nFlags == 0x400) break;
		total++;
		if(nFlags & (SOURCESDK_CS2_FCVAR_CHEAT)) {
//			fprintf(f1,"[+] %lli: 0x%08x: %s : %s\n", i, cmd->m_nFlags, cmd->m_pszName, cmd->m_pszHelpString);
			cmd->SetFlags(nFlags &= ~(SOURCESDK::int64)(SOURCESDK_CS2_FCVAR_CHEAT));
			nUnhidden++;
		} else {
//			fprintf(f1,"[ ] %lli: 0x%08x: %s : %s\n", i, cmd->m_nFlags, cmd->m_pszName, cmd->m_pszHelpString);
		}
	}
	advancedfx::Message("==== Cmds total: %i (Cmds unlocked: %i) ====\n",total,nUnhidden);

	total = 0;
	nUnhidden = 0;
	for(size_t i = 0; i < 65536; i++ )
	{
		SOURCESDK::CS2::Cvar_s * cvar = SOURCESDK::CS2::g_pCVar->GetCvar(i);
		if(nullptr == cvar) break;
		total++;
		if(cvar->m_nFlags & (SOURCESDK_CS2_FCVAR_CHEAT)) {
//			fprintf(f1,"[+] %lli: 0x%08x: %s : %s\n", i, cvar->m_nFlags, cvar->m_pszName, cvar->m_pszHelpString);
			cvar->m_nFlags &= ~(SOURCESDK::int64)(SOURCESDK_CS2_FCVAR_CHEAT);
			nUnhidden++;
		} else {
//			fprintf(f1,"[ ] %lli: 0x%08x: %s : %s\n", i, cvar->m_nFlags, cvar->m_pszName, cvar->m_pszHelpString);
		}
		if(0 == strcmp("sv_cheats",cvar->m_pszName)) {
			cvar->m_nFlags &= ~(SOURCESDK::int64)(SOURCESDK_CS2_FCVAR_REPLICATED| SOURCESDK_CS2_FCVAR_NOTIFY);
			cvar->m_nFlags |= SOURCESDK_CS2_FCVAR_CLIENTDLL;
			cvar->m_Value.m_bValue = true;			
		}
	}
	
	advancedfx::Message("==== Cvars total: %i (Cvars unlocked: %i) ====\n",total,nUnhidden);
}

typedef int(* CCS2_Client_Init_t)(void* This);
CCS2_Client_Init_t old_CCS2_Client_Init;
int new_CCS2_Client_Init(void* This) {
	int result = old_CCS2_Client_Init(This);

	if(!Hook_ClientEntitySystem2()) ErrorBox(MkErrStr(__FILE__, __LINE__));	

	// Connect to reshade addon if present:
	g_ReShadeAdvancedfx.Connect();

	WrpRegisterCommands();

	AfxHookSource2Rs_Engine_Init();

	// Initialize observer tools for remote control (safe to create threads here)
	advancedfx::Message("DEBUG: Starting observer tools initialization...\n");

	// Initialize Winsock for observer tools (required before creating sockets)
	WSADATA wsaData;
	int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (wsaResult != 0) {
		advancedfx::Message("WSAStartup failed for observer tools: %d\n", wsaResult);
	} else {
		advancedfx::Message("WSAStartup succeeded\n");
	}

	// Install GSI config on main thread before GSI servers spin up.
	static bool gsiInstalled = false;
	if (!gsiInstalled) {
		std::string gsiOverride;
		// Explicit target IP takes precedence (for remote GUI). Otherwise keep default loopback.
		if (int targetIdx = g_CommandLine->FindParam(L"-targetip")) {
			TryGetCommandLineIp(targetIdx, gsiOverride);
		}
		InstallGsiConfig(gsiOverride);
		gsiInstalled = true;
	}

	RegisterObsWebSocketHandlers();

	// Decide bind address for observer tools
	std::string bindAddress("127.0.0.1");
	const int lanIndex = g_CommandLine->FindParam(L"-lanserver");

	std::string overrideAddress;
	const bool hasOverride = (lanIndex > 0) && TryGetCommandLineIp(lanIndex, overrideAddress);

	if (hasOverride) {
		bindAddress = overrideAddress;
		advancedfx::Message("DEBUG: Using explicit observer bind address %s\n", bindAddress.c_str());
	} else if (lanIndex > 0) {
		auto detected = DetectIpv4Address(false);
		if (!detected.empty()) {
			bindAddress = detected;
			advancedfx::Message("DEBUG: Detected LAN observer bind address %s\n", bindAddress.c_str());
		}
	}

	advancedfx::Message("DEBUG: Observer tools binding to %s (WS:31338, UDP:31339)\n", bindAddress.c_str());

	// Initialize observer tools for remote control
	advancedfx::Message("DEBUG: Creating WebSocket server...\n");
	g_pObsWebSocket = new CObsWebSocketServer();
	if (g_pObsWebSocket->Start(31338, bindAddress)) {
		advancedfx::Message("Observer WebSocket server started on %s:31338\n", bindAddress.c_str());
		g_pObsWebSocket->SetCommandCallback(HandleObsWebSocketCommand);
	} else {
		advancedfx::Message("Failed to start Observer WebSocket server\n");
	}

	g_pObsInput = new CObsInputReceiver();
	if (g_pObsInput->Start(31339, bindAddress.c_str())) {
		advancedfx::Message("Observer UDP input receiver started on %s:31339\n", bindAddress.c_str());
	} else {
		advancedfx::Message("Failed to start Observer UDP input receiver\n");
	}

	g_pFreecam = new CFreecamController();
	advancedfx::Message("Freecam controller initialized\n");

	g_pNadeCam = new CNadeCam();
	advancedfx::Message("Grenade camera initialized\n");

	PrintInfo();

	HookSchemaSystem(g_H_SchemaSystem);

	if (g_pFileSystem) {
		std::string path(GetHlaeFolder());
		path.append("resources\\AfxHookSource2\\cs2");
		g_pFileSystem->AddSearchPath(path.c_str(), "GAME");

		const wchar_t* USRLOCALCSGO = _wgetenv(L"USRLOCALCSGO");
		if (nullptr != USRLOCALCSGO) {
			std::string USRLOCALCSGO_copy = "";
			WideStringToUTF8String(USRLOCALCSGO, USRLOCALCSGO_copy);
			if (USRLOCALCSGO_copy.size() > 0) g_pFileSystem->AddSearchPath(USRLOCALCSGO_copy.c_str(), "GAME");
		}
	}

	return result;
}

CON_COMMAND(__mirv_print_search_paths, "")
{
	g_pFileSystem->PrintSearchPaths();
}

typedef void(* CCS2_Client_Shutdown_t)(void* This);
CCS2_Client_Shutdown_t old_CCS2_Client_Shutdown;
void new_CCS2_Client_Shutdown(void* This) {
	AfxHookSource2Rs_Engine_Shutdown();

	old_CCS2_Client_Shutdown(This);
}


typedef void * (* CS2_Client_SetGlobals_t)(void* This, void * pGlobals);
CS2_Client_SetGlobals_t old_CS2_Client_SetGlobals;
void *  new_CS2_Client_SetGlobals(void* This, void * pGlobals) {

	g_pGlobals = (Cs2Gloabls_t)pGlobals;

	return old_CS2_Client_SetGlobals(This, pGlobals);
}

class CExecuteClientCmdForCommandSystem : public IExecuteClientCmdForCommandSystem {
public:
	virtual void ExecuteClientCmd(const char * value) {
		if(g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0,value,true);
	}
} g_ExecuteClientCmdForCommandSystem;

class CGetTickForCommandSystem : public IGetTickForCommandSystem {
public:
	virtual float GetTick() {
		float tick = 0;
		if(g_pEngineToClient) {
			if(SOURCESDK::CS2::IDemoFile * pDemoFile = g_pEngineToClient->GetDemoFile()) {
				tick = (float)pDemoFile->GetDemoTick() + g_MirvTime.interpolation_amount_get();
			}
		}
		return tick;
	}
} g_GetTickForCommandSystem;

class CGetTimeForCommandSystem : public IGetTimeForCommandSystem {
public:
	virtual float GetTime() {
		return g_MirvTime.curtime_get();
	}
} g_GetTimeForCommandSystem;

class CommandSystem g_CommandSystem(&g_ExecuteClientCmdForCommandSystem, &g_GetTickForCommandSystem, &g_GetTimeForCommandSystem);

CON_COMMAND(mirv_cmd, "Command system (for scheduling commands).")
{
	g_CommandSystem.Console_Command(args);
}

class CMirvSkip_GotoDemoTick : public IMirvSkip_GotoDemoTick {
	virtual void GotoDemoTick(int tick) {
        std::ostringstream oss;
        oss << "demo_gototick " << tick;
        if(g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0,oss.str().c_str(),true);		
	}
} g_MirvSkip_GotoDemoTick;

CON_COMMAND(mirv_skip, "for skipping through demos (uses demo_gototick)")
{
    MirvSkip_ConsoleCommand(args, &g_MirvCampath_Time, &g_MirvSkip_GotoDemoTick);
}

extern void resetDefaultCloudColors();
extern void resetCachedMaterials();

typedef void * (* CS2_Client_LevelInitPreEntity_t)(void* This, void * pUnk1, void * pUnk2);
CS2_Client_LevelInitPreEntity_t old_CS2_Client_LevelInitPreEntity;
void * new_CS2_Client_LevelInitPreEntity(void* This, void * pUnk1, void * pUnk2) {
	resetDefaultCloudColors();
	resetCachedMaterials();
	void * result = old_CS2_Client_LevelInitPreEntity(This, pUnk1, pUnk2);
	g_CommandSystem.OnLevelInitPreEntity();
	return result;
}

typedef void (* CS2_Client_FrameStageNotify_t)(void* This, SOURCESDK::CS2::ClientFrameStage_t curStage);

CS2_Client_FrameStageNotify_t old_CS2_Client_FrameStageNotify;

void  new_CS2_Client_FrameStageNotify(void* This, SOURCESDK::CS2::ClientFrameStage_t curStage) {
	
	AfxHookSource2Rs_Engine_RunJobQueue();

	/*
	// React to demo being paused / unpaused to work around Valve's new bandaid client time "fix":
	bool bIsDemoPaused = false;
	if(g_pEngineToClient) {
		if(SOURCESDK::CS2::IDemoFile * pDemoPlayer = g_pEngineToClient->GetDemoFile()) {
			if(pDemoPlayer->IsPlayingDemo())
				bIsDemoPaused = pDemoPlayer->IsDemoPaused();
		}
	}
	if(bIsDemoPaused != g_DemoPausedData.IsPaused) {
		if(bIsDemoPaused) {
			g_DemoPausedData.FirstPausedCurtime = g_MirvTime.curtime_get();
			g_DemoPausedData.FirstPausedInterpolationAmount = g_MirvTime.interpolation_amount_get();
			g_DemoPausedData.IsPaused = true;
		} else {
			g_DemoPausedData.IsPaused = false;
		}
	}*/

	// Work around demoui cursor sheningans:
	// - Always manually fetch cursor pos.
	// - Make room to move the cursor.
	// - Hide cursor (can do this way because SDL3 never hides it, uses invisible one instead).
	static bool bCursorHidden = false;
	if(g_MirvInputEx.m_MirvInput->IsActive()) {
		HWND hWnd = GetActiveWindow();
		if(NULL != hWnd) {
			if(!bCursorHidden) ShowCursor(FALSE);
			else {
				POINT point;
				if(GetCursorPos(&point)) {
					new_GetCursorPos(&point);
				}
			}
			RECT rect;
			if(GetClientRect(hWnd, &rect)) {
				POINT pt {(rect.left+rect.right)/2,(rect.top+rect.bottom)/2};
				if(ClientToScreen(hWnd,&pt)) {
					new_SetCursorPos(pt.x, pt.y);
				}
			}
		}
		bCursorHidden = true;
	}
	else if(bCursorHidden) {
		bCursorHidden = false;
		ShowCursor(TRUE);
	}	

	switch(curStage) {
	case SOURCESDK::CS2::FRAME_RENDER_PASS:
		g_CommandSystem.OnExecuteCommands();
		break;
	}

	AfxHookSource2Rs_Engine_OnClientFrameStageNotify(curStage, true);

	old_CS2_Client_FrameStageNotify(This, curStage);

	AfxHookSource2Rs_Engine_OnClientFrameStageNotify(curStage, false);

	AfxHookSource2Rs_Engine_RunJobQueue();
}


void CS2_HookClientDllInterface(void * iface)
{
	void ** vtable = *(void***)iface;

	AfxDetourPtr((PVOID *)&(vtable[0]), new_CCS2_Client_Connect, (PVOID*)&old_CCS2_Client_Connect);
	AfxDetourPtr((PVOID *)&(vtable[3]), new_CCS2_Client_Init, (PVOID*)&old_CCS2_Client_Init);
	AfxDetourPtr((PVOID *)&(vtable[4]), new_CCS2_Client_Shutdown, (PVOID*)&old_CCS2_Client_Shutdown);
	AfxDetourPtr((PVOID *)&(vtable[11]), new_CS2_Client_SetGlobals, (PVOID*)&old_CS2_Client_SetGlobals);
	AfxDetourPtr((PVOID *)&(vtable[35]), new_CS2_Client_LevelInitPreEntity, (PVOID*)&old_CS2_Client_LevelInitPreEntity);
	AfxDetourPtr((PVOID *)&(vtable[36]), new_CS2_Client_FrameStageNotify, (PVOID*)&old_CS2_Client_FrameStageNotify);
}

SOURCESDK::CreateInterfaceFn old_Client_CreateInterface = 0;

void* new_Client_CreateInterface(const char *pName, int *pReturnCode)
{
	static bool bFirstCall = true;

	void * pRet = old_Client_CreateInterface(pName, pReturnCode);

	if(bFirstCall)
	{
		bFirstCall = false;

		void * iface = NULL;
		
		if (iface = old_Client_CreateInterface(SOURCESDK_CS2_Source2Client_VERSION, NULL)) {
			CS2_HookClientDllInterface(iface);
		}
		else
		{
			ErrorBox("Could not get a supported VClient interface.");
		}
	}

	return pRet;
}

SOURCESDK::CreateInterfaceFn old_ResourceSystem_CreateInterface = 0;

void* new_ResourceSystem_CreateInterface(const char *pName, int *pReturnCode)
{
	static bool bFirstCall = true;
	void * pRet = old_ResourceSystem_CreateInterface (pName, pReturnCode);

	if(bFirstCall)
	{
		bFirstCall = false;
		if (!(g_pCResourceSystem = (CResourceSystem*)old_ResourceSystem_CreateInterface("ResourceSystem013", NULL))) ErrorBox("Could not get ResourceSystem013 interface.");
	}

	return pRet;
}

SOURCESDK::CreateInterfaceFn old_FileSystem_CreateInterface = 0;

void* new_FileSystem_CreateInterface(const char *pName, int *pReturnCode)
{
	static bool bFirstCall = true;
	void * pRet = old_FileSystem_CreateInterface (pName, pReturnCode);

	if(bFirstCall)
	{
		bFirstCall = false;
		if (!(g_pFileSystem = (SOURCESDK::CS2::IFileSystem*)old_FileSystem_CreateInterface("VFileSystem017", NULL))) ErrorBox("Could not get VFileSystem017 interface.");
	}

	return pRet;
}

FARPROC WINAPI new_tier0_GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
	FARPROC nResult;
	nResult = GetProcAddress(hModule, lpProcName);

	if (!nResult)
		return nResult;

	if (HIWORD(lpProcName))
	{
		if (!lstrcmp(lpProcName, "GetProcAddress"))
			return (FARPROC) &new_tier0_GetProcAddress;

		if (
			hModule == g_H_ClientDll
			&& !lstrcmp(lpProcName, "CreateInterface")
		) {
			old_Client_CreateInterface = (SOURCESDK::CreateInterfaceFn)nResult;
			return (FARPROC) &new_Client_CreateInterface;
		}

		if (
			hModule == g_H_ResourceSystemDll
			&& !lstrcmp(lpProcName, "CreateInterface")
		) {
			old_ResourceSystem_CreateInterface = (SOURCESDK::CreateInterfaceFn)nResult;
			return (FARPROC) &new_ResourceSystem_CreateInterface;
		}

		if (
			hModule == g_H_FileSystem_stdio
			&& !lstrcmp(lpProcName, "CreateInterface")
		) {
			old_FileSystem_CreateInterface = (SOURCESDK::CreateInterfaceFn)nResult;
			return (FARPROC) &new_FileSystem_CreateInterface;
		}
	}

	return nResult;
}

HMODULE WINAPI new_LoadLibraryA(LPCSTR lpLibFileName);
HMODULE WINAPI new_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
HMODULE WINAPI new_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);

HANDLE
WINAPI
new_CreateFileW(
	_In_ LPCWSTR lpFileName,
	_In_ DWORD dwDesiredAccess,
	_In_ DWORD dwShareMode,
	_In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
	_In_ DWORD dwCreationDisposition,
	_In_ DWORD dwFlagsAndAttributes,
	_In_opt_ HANDLE hTemplateFile
);

BOOL
WINAPI
new_CreateDirectoryW(
    _In_ LPCWSTR lpPathName,
    _In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes
    );

BOOL
WINAPI
new_GetFileAttributesExW(
    _In_ LPCWSTR lpFileName,
    _In_ GET_FILEEX_INFO_LEVELS fInfoLevelId,
    _Out_writes_bytes_(sizeof(WIN32_FILE_ATTRIBUTE_DATA)) LPVOID lpFileInformation
    );

CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD)> g_Import_tier0_KERNEL32_LoadLibraryExA("LoadLibraryExA", &new_LoadLibraryExA);
CAfxImportFuncHook<HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD)> g_Import_tier0_KERNEL32_LoadLibraryExW("LoadLibraryExW", &new_LoadLibraryExW);
CAfxImportFuncHook<FARPROC(WINAPI*)(HMODULE, LPCSTR)> g_Import_tier0_KERNEL32_GetProcAddress("GetProcAddress", &new_tier0_GetProcAddress);
CAfxImportFuncHook<HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE)> g_Import_tier0_KERNEL32_CreateFileW("CreateFileW", &new_CreateFileW);
CAfxImportFuncHook<BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES)> g_Import_tier0_KERNEL32_CreateDirectoryW("CreateDirectoryW", &new_CreateDirectoryW);
CAfxImportFuncHook<BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID)> g_Import_tier0_KERNEL32_GetFileAttributesExW("GetFileAttributesExW", &new_GetFileAttributesExW);

HANDLE WINAPI new_CreateFileW(
	_In_ LPCWSTR lpFileName,
	_In_ DWORD dwDesiredAccess,
	_In_ DWORD dwShareMode,
	_In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
	_In_ DWORD dwCreationDisposition,
	_In_ DWORD dwFlagsAndAttributes,
	_In_opt_ HANDLE hTemplateFile
)
{
	static bool bWasRecording = false; // allow startmovie wav-fixup by engine to get through one more time.
	if (AfxStreams_IsRcording() || bWasRecording) {
		std::wstring strFileName(lpFileName);
		for (auto& c : strFileName) c = std::tolower(c);
		if (StringEndsWithW(strFileName.c_str(), L"" ADVANCEDFX_STARTMOVIE_WAV_KEY ".wav")) {
			// Detours our wav to our folder.			
			bWasRecording = AfxStreams_IsRcording();
			std::wstring newPath(AfxStreams_GetTakeDir());
			newPath.append(L"\\audio.wav");
			return g_Import_tier0_KERNEL32_CreateFileW.TrueFunc(newPath.c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
		}
	}
	return g_Import_tier0_KERNEL32_CreateFileW.TrueFunc(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

BOOL
WINAPI
new_CreateDirectoryW(
    _In_ LPCWSTR lpPathName,
    _In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes
    ) {

	if (AfxStreams_IsRcording()) {
		// Do not create dummy movie folders while recording startmovie wav.
		std::wstring strMovieFolder(L"\\\\?\\");
		strMovieFolder.append(GetProcessFolderW());
		strMovieFolder.append(L"csgo\\movie\\");
		if(StringBeginsWithW(lpPathName,strMovieFolder.c_str())) return TRUE;
	}

	return g_Import_tier0_KERNEL32_CreateDirectoryW.TrueFunc(lpPathName,lpSecurityAttributes);
}

BOOL
WINAPI
new_GetFileAttributesExW(
    _In_ LPCWSTR lpFileName,
    _In_ GET_FILEEX_INFO_LEVELS fInfoLevelId,
    _Out_writes_bytes_(sizeof(WIN32_FILE_ATTRIBUTE_DATA)) LPVOID lpFileInformation
    ) {

	if (AfxStreams_IsRcording()) {
		std::wstring strFileName(lpFileName);
		for (auto& c : strFileName) c = std::tolower(c);
		if (StringEndsWithW(strFileName.c_str(), L"" ADVANCEDFX_STARTMOVIE_WAV_KEY ".wav")) {
			// Detours our wav to our folder.			
			std::wstring newPath(AfxStreams_GetTakeDir());
			newPath.append(L"\\audio.wav");
			return g_Import_tier0_KERNEL32_GetFileAttributesExW.TrueFunc(newPath.c_str(),fInfoLevelId,lpFileInformation);
		}
	}

	return g_Import_tier0_KERNEL32_GetFileAttributesExW.TrueFunc(lpFileName,fInfoLevelId,lpFileInformation);
}


CAfxImportDllHook g_Import_tier0_KERNEL32("KERNEL32.dll", CAfxImportDllHooks({
	&g_Import_tier0_KERNEL32_LoadLibraryExA
	, &g_Import_tier0_KERNEL32_LoadLibraryExW
	, &g_Import_tier0_KERNEL32_GetProcAddress
	, &g_Import_tier0_KERNEL32_CreateFileW
	, &g_Import_tier0_KERNEL32_CreateDirectoryW
	, &g_Import_tier0_KERNEL32_GetFileAttributesExW}));

CAfxImportsHook g_Import_tier0(CAfxImportsHooks({
	&g_Import_tier0_KERNEL32 }));

void CommonHooks()
{
	static bool bFirstRun = true;
	static bool bFirstTier0 = true;

	// do not use messageboxes here, there is some friggin hooking going on in between by the
	// Source engine.

	if (bFirstRun)
	{
		bFirstRun = false;
	}
}

CAfxImportFuncHook<HMODULE (WINAPI *)(LPCSTR)> g_Import_launcher_KERNEL32_LoadLibraryA("LoadLibraryA", &new_LoadLibraryA);
CAfxImportFuncHook<HMODULE (WINAPI *)(LPCSTR, HANDLE, DWORD)> g_Import_launcher_KERNEL32_LoadLibraryExA("LoadLibraryExA", &new_LoadLibraryExA);

CAfxImportDllHook g_Import_launcher_KERNEL32("KERNEL32.dll", CAfxImportDllHooks({
	&g_Import_launcher_KERNEL32_LoadLibraryA
	, &g_Import_launcher_KERNEL32_LoadLibraryExA }));

CAfxImportsHook g_Import_launcher(CAfxImportsHooks({
	&g_Import_launcher_KERNEL32 }));

CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR)> g_Import_filesystem_steam_KERNEL32_LoadLibraryA("LoadLibraryA", &new_LoadLibraryA);
CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD)> g_Import_filesystem_steam_KERNEL32_LoadLibraryExA("LoadLibraryExA", &new_LoadLibraryExA);

CAfxImportDllHook g_Import_filesystem_steam_KERNEL32("KERNEL32.dll", CAfxImportDllHooks({
	&g_Import_filesystem_steam_KERNEL32_LoadLibraryA
	, &g_Import_filesystem_steam_KERNEL32_LoadLibraryExA }));

CAfxImportsHook g_Import_filesystem_steam(CAfxImportsHooks({
	&g_Import_filesystem_steam_KERNEL32 }));


//CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR)> g_Import_engine2_KERNEL32_LoadLibraryA("LoadLibraryA", &new_LoadLibraryA);
//CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD)> g_Import_engine2_KERNEL32_LoadLibraryExA("LoadLibraryExA", &new_LoadLibraryExA);

//CAfxImportDllHook g_Import_engine2_KERNEL32("KERNEL32.dll", CAfxImportDllHooks({
//	&g_Import_engine2_KERNEL32_LoadLibraryA
//	, &g_Import_engine2_KERNEL32_LoadLibraryExA }));

void * New_SteamInternal_FindOrCreateUserInterface(void*pUser, const char * pIntervaceName);

CAfxImportFuncHook<void*(*)(void *, const char *)> g_Import_engine2_steam_api64_SteamInternal_FindOrCreateUserInterface("SteamInternal_FindOrCreateUserInterface", &New_SteamInternal_FindOrCreateUserInterface);

void * New_SteamInternal_FindOrCreateUserInterface(void*pUser, const char * pInterfaceName) {
	if(0 == strcmp(pInterfaceName,"STEAMREMOTESTORAGE_INTERFACE_VERSION016")) {
		if (int idx = g_CommandLine->FindParam(L"-afxDisableSteamStorage")) {
			return nullptr;
		}
	}
	
	return g_Import_engine2_steam_api64_SteamInternal_FindOrCreateUserInterface.GetTrueFuncValue()(pUser,pInterfaceName);
}

CAfxImportDllHook g_Import_engine2_steam_api64("steam_api64.dll", CAfxImportDllHooks({
	&g_Import_engine2_steam_api64_SteamInternal_FindOrCreateUserInterface }));

CAfxImportsHook g_Import_engine2(CAfxImportsHooks({
	//&g_Import_engine2_KERNEL32,
	&g_Import_engine2_steam_api64 }));

CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR)> g_Import_materialsystem2_KERNEL32_LoadLibraryA("LoadLibraryA", &new_LoadLibraryA);
CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD)> g_Import_materialsystem2_KERNEL32_LoadLibraryExA("LoadLibraryExA", &new_LoadLibraryExA);

CAfxImportDllHook g_Import_materialsystem2_KERNEL32("KERNEL32.dll", CAfxImportDllHooks({
	&g_Import_materialsystem2_KERNEL32_LoadLibraryA
	, &g_Import_materialsystem2_KERNEL32_LoadLibraryExA }));

CAfxImportsHook g_Import_materialsystem2(CAfxImportsHooks({
	&g_Import_materialsystem2_KERNEL32 }));


CAfxImportFuncHook<LONG_PTR(WINAPI*)(HWND, int)> g_Import_SDL3_USER32_GetWindowLongW("GetWindowLongPtrW", &new_GetWindowLongPtrW);
CAfxImportFuncHook<LONG_PTR(WINAPI*)(HWND, int, LONG_PTR)> g_Import_SDL3_USER32_SetWindowLongW("SetWindowLongPtrW", &new_SetWindowLongPtrW);
CAfxImportFuncHook<HCURSOR(WINAPI*)(HCURSOR)> g_Import_SDL3_USER32_SetCursor("SetCursor", &new_SetCursor);
CAfxImportFuncHook<HWND(WINAPI*)(HWND)> g_Import_SDL3_USER32_SetCapture("SetCapture", &new_SetCapture);
CAfxImportFuncHook<BOOL(WINAPI*)()> g_Import_SDL3_USER32_ReleaseCapture("ReleaseCapture", &new_ReleaseCapture);
CAfxImportFuncHook<BOOL(WINAPI*)(LPPOINT)> g_Import_SDL3_USER32_GetCursorPos("GetCursorPos", &new_GetCursorPos);
CAfxImportFuncHook<BOOL(WINAPI*)(int, int)> g_Import_SDL3_USER32_SetCursorPos("SetCursorPos", &new_SetCursorPos);


UINT
WINAPI
New_GetRawInputBuffer(
    _Out_writes_bytes_opt_(*pcbSize) PRAWINPUT pData,
    _Inout_ PUINT pcbSize,
    _In_ UINT cbSizeHeader);

CAfxImportFuncHook<UINT(WINAPI*)(_Out_writes_bytes_opt_(*pcbSize) PRAWINPUT, _Inout_ PUINT, _In_ UINT cbSizeHeader)> g_Import_SDL3_USER32_GetRawInputBuffer("GetRawInputBuffer", &New_GetRawInputBuffer);

UINT
WINAPI
New_GetRawInputBuffer(
    _Out_writes_bytes_opt_(*pcbSize) PRAWINPUT pData,
    _Inout_ PUINT pcbSize,
    _In_ UINT cbSizeHeader) {
	UINT result = g_Import_SDL3_USER32_GetRawInputBuffer.GetTrueFuncValue()(pData,pcbSize,cbSizeHeader);

	result = g_MirvInputEx.m_MirvInput->Supply_RawInputBuffer(result, pData,pcbSize,cbSizeHeader);

	return result;

}


UINT WINAPI New_GetRawInputData(
    _In_ HRAWINPUT hRawInput,
    _In_ UINT uiCommand,
    _Out_writes_bytes_to_opt_(*pcbSize, return) LPVOID pData,
    _Inout_ PUINT pcbSize,
    _In_ UINT cbSizeHeader);

CAfxImportFuncHook<UINT(WINAPI*)(_In_ HRAWINPUT, _In_ UINT, _Out_writes_bytes_to_opt_(*pcbSize, return) LPVOID pData,_Inout_ PUINT,_In_ UINT)> g_Import_SDL3_USER32_GetRawInputData("GetRawInputData", &New_GetRawInputData);

UINT WINAPI New_GetRawInputData(
    _In_ HRAWINPUT hRawInput,
    _In_ UINT uiCommand,
    _Out_writes_bytes_to_opt_(*pcbSize, return) LPVOID pData,
    _Inout_ PUINT pcbSize,
    _In_ UINT cbSizeHeader) {

	UINT result = g_Import_SDL3_USER32_GetRawInputData.GetTrueFuncValue()(hRawInput,uiCommand,pData,pcbSize,cbSizeHeader);

	result = g_MirvInputEx.m_MirvInput->Supply_RawInputData(result, hRawInput, uiCommand,pData,pcbSize,cbSizeHeader);

	return result;
}


CAfxImportDllHook g_Import_SDL3_USER32("USER32.dll", CAfxImportDllHooks({
	&g_Import_SDL3_USER32_GetWindowLongW,
	&g_Import_SDL3_USER32_SetWindowLongW,
	&g_Import_SDL3_USER32_SetCursor,
	&g_Import_SDL3_USER32_SetCapture,
	&g_Import_SDL3_USER32_ReleaseCapture,
	&g_Import_SDL3_USER32_GetCursorPos,
	&g_Import_SDL3_USER32_SetCursorPos,
	&g_Import_SDL3_USER32_GetRawInputData,
	&g_Import_SDL3_USER32_GetRawInputBuffer }));

CAfxImportsHook g_Import_SDL3(CAfxImportsHooks({
	&g_Import_SDL3_USER32 }));

CAfxImportFuncHook<LONG_PTR(WINAPI*)(HWND, int)> g_Import_inputsystem_USER32_GetWindowLongW("GetWindowLongPtrW", &new_GetWindowLongPtrW);
CAfxImportFuncHook<LONG_PTR(WINAPI*)(HWND, int, LONG_PTR)> g_Import_inputsystem_USER32_SetWindowLongW("SetWindowLongPtrW", &new_SetWindowLongPtrW);
CAfxImportFuncHook<HCURSOR(WINAPI*)(HCURSOR)> g_Import_inputsystem_USER32_SetCursor("SetCursor", &new_SetCursor);
CAfxImportFuncHook<HWND(WINAPI*)(HWND)> g_Import_inputsystem_USER32_SetCapture("SetCapture", &new_SetCapture);
CAfxImportFuncHook<BOOL(WINAPI*)()> g_Import_inputsystem_USER32_ReleaseCapture("ReleaseCapture", &new_ReleaseCapture);
CAfxImportFuncHook<BOOL(WINAPI*)(LPPOINT)> g_Import_inputsystem_USER32_GetCursorPos("GetCursorPos", &new_GetCursorPos);
CAfxImportFuncHook<BOOL(WINAPI*)(int, int)> g_Import_inputsystem_USER32_SetCursorPos("SetCursorPos", &new_SetCursorPos);

CAfxImportDllHook g_Import_inputsystem_USER32("USER32.dll", CAfxImportDllHooks({
	&g_Import_inputsystem_USER32_GetWindowLongW,
	&g_Import_inputsystem_USER32_SetWindowLongW,
	&g_Import_inputsystem_USER32_SetCursor,
	&g_Import_inputsystem_USER32_SetCapture,
	&g_Import_inputsystem_USER32_ReleaseCapture,
	&g_Import_inputsystem_USER32_GetCursorPos,
	&g_Import_inputsystem_USER32_SetCursorPos }));

CAfxImportsHook g_Import_inputsystem(CAfxImportsHooks({
	&g_Import_inputsystem_USER32 }));

//CAfxImportDllHook g_Import_client_steam_api64("steam_api64.dll", CAfxImportDllHooks({
//	&g_Import_client_steam_api64_SteamInternal_FindOrCreateUserInterface }));
//
//CAfxImportsHook g_Import_client(CAfxImportsHooks({
//&g_Import_client_steam_api64 }));

void LibraryHooksA(HMODULE hModule, LPCSTR lpLibFileName)
{
	CommonHooks();

	if(!hModule || !lpLibFileName)
		return;

#if 0
	static FILE *f1=NULL;

	if( !f1 ) f1=fopen("hlae_log_LibraryHooksA.txt","wb");
	fprintf(f1,"%s\n", lpLibFileName);
	fflush(f1);
#endif
}

advancedfx::Con_Printf_t Tier0_Message = nullptr;
advancedfx::Con_Printf_t Tier0_Warning = nullptr;
advancedfx::Con_DevPrintf_t Tier0_DevMessage = nullptr;
advancedfx::Con_DevPrintf_t Tier0_DevWarning = nullptr;

class CConsolePrint_Message : public IConsolePrint {
public:
	virtual void Print(const char * text) {
		Tier0_Message("%s", text);
	}
};

class CConsolePrint_Warning : public IConsolePrint {
public:
	virtual void Print(const char * text) {
		Tier0_Warning("%s", text);
	}
};

class CConsolePrint_DevMessage : public IConsolePrint {
public:
	CConsolePrint_DevMessage(int level)
	: m_Level(level) {

	}

	virtual void Print(const char * text) {
		Tier0_DevMessage(m_Level, "%s", text);
	}
private:
	int m_Level;
};

class CConsolePrint_DevWarning : public IConsolePrint {
public:
	CConsolePrint_DevWarning(int level)
	: m_Level(level) {

	}

	virtual void Print(const char * text) {
		Tier0_DevWarning(m_Level, "%s", text);
	}
private:
	int m_Level;
};

CConsolePrinter * g_ConsolePrinter = nullptr;

void My_Console_Message(const char* fmt, ...) {
	CConsolePrint_Message consolePrint;
	va_list args;
	va_start(args, fmt);
	g_ConsolePrinter->Print(&consolePrint, fmt, args);
	va_end(args);
}

void My_Console_Warning(const char* fmt, ...) {
	CConsolePrint_Warning consolePrint;
	va_list args;
	va_start(args, fmt);
	g_ConsolePrinter->Print(&consolePrint, fmt, args);
	va_end(args);
}

void My_Console_DevMessage(int level, const char* fmt, ...) {
	CConsolePrint_DevMessage consolePrint(level);
	va_list args;
	va_start(args, fmt);
	g_ConsolePrinter->Print(&consolePrint, fmt, args);
	va_end(args);
}

void My_Console_DevWarning(int level, const char* fmt, ...) {
	CConsolePrint_DevWarning consolePrint(level);
	va_list args;
	va_start(args, fmt);
	g_ConsolePrinter->Print(&consolePrint, fmt, args);
	va_end(args);
}

void LibraryHooksW(HMODULE hModule, LPCWSTR lpLibFileName)
{
	static bool bFirstTier0 = true;
	static bool bFirstClient = true;
	static bool bFirstEngine2 = true;
	static bool bFirstfilesystem_stdio = true;
	static bool bFirstMaterialsystem2 = true;
	static bool bFirstInputsystem = true;
	static bool bFirstSDL3 = true;
	static bool bFirstRenderSystemDX11 = true;
	static bool bFirstPanorama = true;
	static bool bFirstSchemaSystem = true;
	static bool bFirstSceneSystem = true;
	static bool bFirstResourceSystem = true;
	
	CommonHooks();

	if (!hModule || !lpLibFileName)
		return;

#if 0
	static FILE *f1 = NULL;

	if (!f1) f1 = fopen("hlae_log_LibraryHooksW.txt", "wb");
	fwprintf(f1, L"%s\n", lpLibFileName);
	fflush(f1);
#endif


	if(bFirstTier0 && StringEndsWithW( lpLibFileName, L"tier0.dll"))
	{
		bFirstTier0 = false;
		
		g_Import_tier0.Apply(hModule);

		SOURCESDK::CS2::g_pMemAlloc = *(SOURCESDK::CS2::IMemAlloc **)GetProcAddress(hModule, "g_pMemAlloc");

		if(Tier0_Message = (Tier0MsgFn)GetProcAddress(hModule, "Msg"))
			advancedfx::Message = My_Console_Message;
		else
			ErrorBox(MkErrStr(__FILE__, __LINE__));
		if(Tier0_Warning = (Tier0MsgFn)GetProcAddress(hModule, "Warning"))
			advancedfx::Warning = My_Console_Warning;
		else
			ErrorBox(MkErrStr(__FILE__, __LINE__));
		if(Tier0_DevMessage = (Tier0DevMsgFn)GetProcAddress(hModule, "DevMsg"))
			advancedfx::DevMessage = My_Console_DevMessage;
		else
			ErrorBox(MkErrStr(__FILE__, __LINE__));
		if(Tier0_DevWarning = (Tier0DevMsgFn)GetProcAddress(hModule, "DevWarning"))
			advancedfx::DevWarning = My_Console_DevWarning;
		else
			ErrorBox(MkErrStr(__FILE__, __LINE__));
	}
	else if(bFirstfilesystem_stdio && StringEndsWithW( lpLibFileName, L"filesystem_stdio.dll"))
	{
		bFirstfilesystem_stdio = false;

		g_H_FileSystem_stdio = hModule;

		org_AddSearchPath = (AddSearchPath_t)getVTableFn(hModule, 31, ".?AVCFileSystem_Stdio@@");
		if (0 == org_AddSearchPath) ErrorBox(MkErrStr(__FILE__, __LINE__));

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		
		DetourAttach(&(PVOID&)org_AddSearchPath, new_AddSearchPath);
		
		if(NO_ERROR != DetourTransactionCommit()) ErrorBox("Failed to detour filesystem_stdio functions.");
		
		// g_Import_filesystem_stdio.Apply(hModule);
	}
	else if(bFirstInputsystem && StringEndsWithW(lpLibFileName, L"inputsystem.dll"))
	{
		bFirstInputsystem = false;

		g_Import_inputsystem.Apply(hModule);
	}	
	else if(bFirstSDL3 && StringEndsWithW(lpLibFileName, L"SDL3.dll"))
	{
		bFirstSDL3 = false;

		g_Import_SDL3.Apply(hModule);
	}
	else if(bFirstEngine2 && StringEndsWithW( lpLibFileName, L"engine2.dll"))
	{
		bFirstEngine2 = false;

		g_h_engine2Dll = hModule;

		Addresses_InitEngine2Dll((AfxAddr)hModule);

		HookEngineDll(hModule);

		g_Import_engine2.Apply(hModule);

		Hook_Engine_RenderService();

		Hook_Engine__HostStateRequest_Start();
	}
	else if(bFirstSceneSystem && StringEndsWithW( lpLibFileName, L"scenesystem.dll"))
	{
		bFirstSceneSystem = false;

		Addresses_InitSceneSystemDll((AfxAddr)hModule);

		g_Import_SceneSystem.Apply(hModule);
		Hook_SceneSystem(hModule);
		HookSceneSystem(hModule);
	}
	else if(bFirstMaterialsystem2 && StringEndsWithW( lpLibFileName, L"materialsystem2.dll"))
	{
		bFirstMaterialsystem2 = false;

		HookMaterialSystem(hModule);

		// g_Import_materialsystem2.Apply(hModule);
	}
	else if(bFirstRenderSystemDX11 && StringEndsWithW( lpLibFileName, L"rendersystemdx11.dll"))
	{
		bFirstRenderSystemDX11 = false;

		Hook_RenderSystemDX11((void*)hModule);
	}
	else if(bFirstClient && StringEndsWithW(lpLibFileName, L"csgo\\bin\\win64\\client.dll"))
	{
		bFirstClient = false;

		g_H_ClientDll = hModule;

		Addresses_InitClientDll((AfxAddr)hModule);

		//if(!g_Import_client.Apply(hModule)) ErrorBox("client.dll steam_api64 hooks failed.");

		HookMirvColors(hModule);

		HookMirvCommands(hModule);

		HookViewmodel(hModule);

		HookDeathMsg(hModule);

		HookReplaceName(hModule);

		HookClientDll(hModule);

		Hook_ClientEntitySystem3(hModule);
	} 
	else if(bFirstPanorama && StringEndsWithW(lpLibFileName, L"panorama.dll"))
	{
		bFirstPanorama = false;
		g_Import_panorama.Apply(hModule);
		HookPanorama(hModule);
	}
	else if(bFirstSchemaSystem && StringEndsWithW(lpLibFileName, L"schemasystem.dll"))
	{
		bFirstSchemaSystem = false;
		g_H_SchemaSystem = hModule;
	}
	else if(bFirstResourceSystem && StringEndsWithW(lpLibFileName, L"resourcesystem.dll"))
	{
		bFirstResourceSystem = false;
		g_H_ResourceSystemDll = hModule;
	}
}

HMODULE WINAPI new_LoadLibraryA( LPCSTR lpLibFileName ) {
	HMODULE hRet = LoadLibraryA(lpLibFileName);

	LibraryHooksA(hRet, lpLibFileName);

	return hRet;
}

HMODULE WINAPI new_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
	HMODULE hRet = LoadLibraryExA(lpLibFileName, hFile, dwFlags);

	LibraryHooksA(hRet, lpLibFileName);

	return hRet;
}

HMODULE WINAPI new_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
	HMODULE hRet = LoadLibraryExW(lpLibFileName, hFile, dwFlags);

	LibraryHooksW(hRet, lpLibFileName);

	return hRet;
}

CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR)> g_Import_PROCESS_KERNEL32_LoadLibraryA("LoadLibraryA", &new_LoadLibraryA);
CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD)> g_Import_PROCESS_KERNEL32_LoadLibraryExA("LoadLibraryExA", &new_LoadLibraryExA);
CAfxImportFuncHook<HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD)> g_Import_PROCESS_KERNEL32_LoadLibraryExW("LoadLibraryExW", &new_LoadLibraryExW);

CAfxImportDllHook g_Import_PROCESS_KERNEL32("KERNEL32.dll", CAfxImportDllHooks({
	&g_Import_PROCESS_KERNEL32_LoadLibraryA
	, &g_Import_PROCESS_KERNEL32_LoadLibraryExA
	, &g_Import_PROCESS_KERNEL32_LoadLibraryExW }));

CAfxImportsHook g_Import_PROCESS(CAfxImportsHooks({
	&g_Import_PROCESS_KERNEL32 }));


advancedfx::CThreadPool * g_pThreadPool = nullptr;
advancedfx::CGrowingBufferPoolThreadSafe * g_pImageBufferPoolThreadSafe = nullptr;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved)
{
	switch (fdwReason) 
	{ 
		case DLL_PROCESS_ATTACH:
		{
			g_CommandLine = new advancedfx::CCommandLine();

			if(!g_CommandLine->FindParam(L"-insecure"))
			{
				ErrorBox("Please add -insecure to launch options, AfxHookSource2 will refuse to work without it!");

				HANDLE hproc = OpenProcess(PROCESS_TERMINATE, true, GetCurrentProcessId());
				TerminateProcess(hproc, 0);
				CloseHandle(hproc);
				
				do MessageBoxA(NULL, "Please terminate the game manually in the taskmanager!", "Cannot terminate, please help:", MB_OK | MB_ICONERROR);
				while (true);
			}

#if _DEBUG
			MessageBox(0,"DLL_PROCESS_ATTACH","MDT_DEBUG",MB_OK);
#endif
			g_Import_PROCESS.Apply(GetModuleHandle(NULL));

			if (!(g_Import_PROCESS_KERNEL32_LoadLibraryA.TrueFunc || g_Import_PROCESS_KERNEL32_LoadLibraryExA.TrueFunc || g_Import_PROCESS_KERNEL32_LoadLibraryExW.TrueFunc))
				ErrorBox();

			//
			// Remember we are not on the main program thread here,
			// instead we are on our own thread, so don't run
			// things here that would have problems with that.
			//

			size_t thread_pool_thread_count = advancedfx::CThreadPool::GetDefaultThreadCount();
			if (int idx = g_CommandLine->FindParam(L"-afxThreadPoolSize")) {
				if (idx + 1 < g_CommandLine->GetArgC()) {
					thread_pool_thread_count = (size_t)wcstoul( g_CommandLine->GetArgV(idx + 1), nullptr, 10);
				}
			}
			g_pThreadPool = new advancedfx::CThreadPool(thread_pool_thread_count);

			g_pImageBufferPoolThreadSafe = new advancedfx::CGrowingBufferPoolThreadSafe();

			g_ConsolePrinter = new CConsolePrinter();

			g_CampathDrawer.Begin();
			g_PlayerPathDrawer.Begin();

			if (int idx = g_CommandLine->FindParam(L"-afxFixNetCon")) {
				// https://github.com/ValveSoftware/csgo-osx-linux/issues/3603#issuecomment-2163695087

				WORD wVersionRequested;
    			WSADATA wsaData;
    			int err;

				wVersionRequested = MAKEWORD(2, 0);

				err = WSAStartup(wVersionRequested, &wsaData);
    			if (err != 0) {
					ErrorBox("WSAStartup failed");
			    }
			
			    if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 0) {
			        ErrorBox("Could not find a usable version of Winsock.dll");
        			WSACleanup();
				}		
			}

			break;
		}
		case DLL_PROCESS_DETACH:
		{
			// actually this gets called now.

			g_CampathDrawer.End();
			g_PlayerPathDrawer.End();

			g_S2CamIO.ShutDown();

			// Cleanup observer tools
			if (g_pNadeCam) {
				delete g_pNadeCam;
				g_pNadeCam = nullptr;
			}

			if (g_pFreecam) {
				delete g_pFreecam;
				g_pFreecam = nullptr;
			}

			if (g_pObsInput) {
				g_pObsInput->Stop();
				delete g_pObsInput;
				g_pObsInput = nullptr;
			}

			if (g_pObsWebSocket) {
				g_pObsWebSocket->Stop();
				delete g_pObsWebSocket;
				g_pObsWebSocket = nullptr;
			}

			delete g_ConsolePrinter;

			delete g_pImageBufferPoolThreadSafe;

			delete g_pThreadPool;

#ifdef _DEBUG
			_CrtDumpMemoryLeaks();
#endif

			break;
		}
		case DLL_THREAD_ATTACH:
		{
			break;
		}
		case DLL_THREAD_DETACH:
		{
			break;
		}
	}
	return TRUE;
}

