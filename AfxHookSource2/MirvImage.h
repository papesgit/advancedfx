#pragma once

#include <d3d11.h>
#define _XM_NO_INTRINSICS_
#include <DirectXMath.h>

#include <mutex>
#include <string>
#include <vector>

#include "../shared/AfxMath.h"

struct IDXGIKeyedMutex;

class CMirvImageDrawer
{
public:
	CMirvImageDrawer();
	~CMirvImageDrawer();

	void BeginDevice(ID3D11Device* device);
	void EndDevice();

	void OnRenderThread_Draw(
		ID3D11DeviceContext* pImmediateContext,
		const D3D11_VIEWPORT* pViewPort,
		ID3D11RenderTargetView* pRenderTargetView,
		ID3D11DepthStencilView* pDepthStencilView
	);

	void LoadImage(const char* name, const char* fileName);
	void UnloadImage(const char* name);
	void ListImages();

	void RegisterAtlas(const char* name, const char* handleStr, UINT width, UINT height, const char* formatStr, const char* alphaStr, bool keyedMutex);
	void UnregisterAtlas(const char* name);
	void ListAtlases();
	void SetAtlasRegion(const char* atlasName, const char* regionId, float u0, float v0, float u1, float v1, double defaultW, double defaultH);
	void RemoveAtlasRegion(const char* atlasName, const char* regionId);
	void UseAtlasRegion(const char* name, const char* atlasName, const char* regionId);
	void ProbeAtlas(const char* atlasName, int x, int y);

	void SetPosition(const char* name, double x, double y, double z);
	void SetAngles(const char* name, double pitch, double yaw, double roll);
	void SetScale(const char* name, double sx, double sy);
	void SetVisible(const char* name, bool value);
	void SetDepthTest(const char* name, bool value);
	void SetDepthWrite(const char* name, bool value);
	void SetAttachment(const char* name, int slot, bool useYaw, bool usePitch, bool useRoll, const char* attachmentName);
	void UpdateAttachments();

	enum class AtlasFormat {
		BGRA8,
		RGBA8
	};

	enum class AlphaMode {
		Premultiplied,
		Straight
	};

	struct AtlasRegionSnapshot {
		std::string id;
		float u0 = 0.0f;
		float v0 = 0.0f;
		float u1 = 1.0f;
		float v1 = 1.0f;
		double defaultW = 1.0;
		double defaultH = 1.0;
	};
	struct AtlasSnapshot {
		std::string name;
		HANDLE handle = nullptr;
		UINT width = 0;
		UINT height = 0;
		AtlasFormat format = AtlasFormat::BGRA8;
		AlphaMode alphaMode = AlphaMode::Premultiplied;
		bool keyedMutex = false;
		bool open = false;
		bool keyed = false;
		std::vector<AtlasRegionSnapshot> regions;
	};
	struct ImageSnapshot {
		std::string name;
		Afx::Math::Vector3 position = Afx::Math::Vector3(0.0, 0.0, 0.0);
		double pitch = 0.0;
		double yaw = 0.0;
		double roll = 0.0;
		double scaleX = 1.0;
		double scaleY = 1.0;
		bool visible = true;
		bool depthTest = true;
		bool depthWrite = true;
		bool useAtlas = false;
		std::string atlasName;
		std::string regionId;
		int attachSlot = -1;
		std::string attachAttachmentName;
		bool attachUseYaw = false;
		bool attachUsePitch = false;
		bool attachUseRoll = false;
	};
	void GetAtlasSnapshot(std::vector<AtlasSnapshot>& out);
	void GetImageSnapshot(std::vector<ImageSnapshot>& out);

private:
	struct Vertex
	{
		FLOAT x, y, z;
		DWORD diffuse;
		FLOAT t0u, t0v, t0w;
		FLOAT t1u, t1v, t1w;
		FLOAT t2u, t2v, t2w;
	};

	struct VS_CONSTANT_BUFFER
	{
		DirectX::XMFLOAT4X4 matrix;
		DirectX::XMFLOAT4 plane0;
		DirectX::XMFLOAT4 planeN;
		DirectX::XMFLOAT4 screenInfo;
	};

	struct ImageEntry
	{
		std::string name;
		std::wstring filePath;
		std::string atlasName;
		std::string regionId;
		Afx::Math::Vector3 position = Afx::Math::Vector3(0.0, 0.0, 0.0);
		double pitch = 0.0;
		double yaw = 0.0;
		double roll = 0.0;
		double scaleX = 1.0;
		double scaleY = 1.0;
		bool visible = true;
		bool depthTest = true;
		bool depthWrite = true;
		bool pendingLoad = false;
		int attachSlot = -1;
		std::string attachAttachmentName;
		bool attachUseYaw = false;
		bool attachUsePitch = false;
		bool attachUseRoll = false;
		bool attachValid = false;
		Afx::Math::Vector3 attachOrigin = Afx::Math::Vector3(0.0, 0.0, 0.0);
		double attachPitch = 0.0;
		double attachYaw = 0.0;
		double attachRoll = 0.0;

		ID3D11Texture2D* texture = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		UINT width = 0;
		UINT height = 0;
		float u0 = 0.0f;
		float v0 = 0.0f;
		float u1 = 1.0f;
		float v1 = 1.0f;
		bool useAtlas = false;
	};

	struct AtlasRegion
	{
		std::string id;
		float u0 = 0.0f;
		float v0 = 0.0f;
		float u1 = 1.0f;
		float v1 = 1.0f;
		double defaultW = 1.0;
		double defaultH = 1.0;
	};

	struct AtlasTexture
	{
		HANDLE handle = nullptr;
		ID3D11Texture2D* texture = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		IDXGIKeyedMutex* keyedMutexObj = nullptr;
		bool loggedOpenFail = false;
		bool loggedMutexFail = false;
	};

	struct AtlasEntry
	{
		std::string name;
		UINT width = 0;
		UINT height = 0;
		AtlasFormat format = AtlasFormat::BGRA8;
		AlphaMode alphaMode = AlphaMode::Premultiplied;
		bool keyedMutex = false;

		AtlasTexture texture;

		ID3D11Texture2D* localTexture = nullptr;
		ID3D11ShaderResourceView* localSrv = nullptr;
		DXGI_FORMAT localFormat = DXGI_FORMAT_UNKNOWN;
		UINT localWidth = 0;
		UINT localHeight = 0;
		bool localValid = false;

		std::vector<AtlasRegion> regions;
	};

	ImageEntry* FindImageLocked(const std::string& name);
	AtlasEntry* FindAtlasLocked(const std::string& name);
	AtlasRegion* FindAtlasRegionLocked(AtlasEntry& atlas, const std::string& regionId);
	void ReleaseImageResources(ImageEntry& entry);
	void ReleaseAtlasResources(AtlasEntry& entry);
	void EnsureLocalAtlasResources(AtlasEntry& entry, const D3D11_TEXTURE2D_DESC& desc);
	bool TryOpenSharedTexture(AtlasEntry& entry);
	bool AcquireAtlas(AtlasEntry& entry, DWORD key);
	void ReleaseAtlas(AtlasEntry& entry, DWORD key);

	bool LoadTextureFromFile(
		const std::wstring& filePath,
		ID3D11Texture2D** outTexture,
		ID3D11ShaderResourceView** outSrv,
		UINT* outWidth,
		UINT* outHeight
	);

	bool SetMatrixConstantBuffer();
	void EnsureDeviceResources();
	void DrawImages();
	void HandlePendingProbe();

	std::mutex m_Mutex;
	std::vector<ImageEntry> m_Images;
	std::vector<AtlasEntry> m_Atlases;
	bool m_ProbePending = false;
	std::string m_ProbeAtlasName;
	int m_ProbeX = 0;
	int m_ProbeY = 0;

	ID3D11Device* m_Device = nullptr;
	ID3D11DeviceContext* m_DeviceContext = nullptr;
	ID3D11DeviceContext* m_ImmediateContext = nullptr;
	const D3D11_VIEWPORT* m_pViewPort = nullptr;
	ID3D11RenderTargetView* m_Rtv = nullptr;
	ID3D11DepthStencilView* m_Dsv = nullptr;

	ID3D11DepthStencilState* m_DepthStateOn = nullptr;
	ID3D11DepthStencilState* m_DepthStateWrite = nullptr;
	ID3D11DepthStencilState* m_DepthStateOff = nullptr;
	ID3D11SamplerState* m_SamplerState = nullptr;
	ID3D11RasterizerState* m_RasterizerState = nullptr;
	ID3D11BlendState* m_BlendState = nullptr;
	ID3D11BlendState* m_BlendStatePremul = nullptr;
	ID3D11InputLayout* m_InputLayout = nullptr;
	ID3D11Buffer* m_ConstantBuffer = nullptr;
	ID3D11VertexShader* m_VertexShader = nullptr;
	ID3D11PixelShader* m_PixelShader = nullptr;
	ID3D11Buffer* m_VertexBuffer = nullptr;
};

extern CMirvImageDrawer g_MirvImageDrawer;
