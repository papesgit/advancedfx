#pragma once

#include <d3d11.h>
#define _XM_NO_INTRINSICS_
#include <DirectXMath.h>

#include <mutex>
#include <string>
#include <vector>

#include "../shared/AfxMath.h"

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

	void SetPosition(const char* name, double x, double y, double z);
	void SetAngles(const char* name, double pitch, double yaw, double roll);
	void SetScale(const char* name, double sx, double sy);
	void SetVisible(const char* name, bool value);
	void SetDepthTest(const char* name, bool value);

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
		Afx::Math::Vector3 position = Afx::Math::Vector3(0.0, 0.0, 0.0);
		double pitch = 0.0;
		double yaw = 0.0;
		double roll = 0.0;
		double scaleX = 1.0;
		double scaleY = 1.0;
		bool visible = true;
		bool depthTest = true;
		bool pendingLoad = false;

		ID3D11Texture2D* texture = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		UINT width = 0;
		UINT height = 0;
	};

	ImageEntry* FindImageLocked(const std::string& name);
	void ReleaseImageResources(ImageEntry& entry);

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

	std::mutex m_Mutex;
	std::vector<ImageEntry> m_Images;

	ID3D11Device* m_Device = nullptr;
	ID3D11DeviceContext* m_DeviceContext = nullptr;
	ID3D11DeviceContext* m_ImmediateContext = nullptr;
	const D3D11_VIEWPORT* m_pViewPort = nullptr;
	ID3D11RenderTargetView* m_Rtv = nullptr;
	ID3D11DepthStencilView* m_Dsv = nullptr;

	ID3D11DepthStencilState* m_DepthStateOn = nullptr;
	ID3D11DepthStencilState* m_DepthStateOff = nullptr;
	ID3D11SamplerState* m_SamplerState = nullptr;
	ID3D11RasterizerState* m_RasterizerState = nullptr;
	ID3D11BlendState* m_BlendState = nullptr;
	ID3D11InputLayout* m_InputLayout = nullptr;
	ID3D11Buffer* m_ConstantBuffer = nullptr;
	ID3D11VertexShader* m_VertexShader = nullptr;
	ID3D11PixelShader* m_PixelShader = nullptr;
	ID3D11Buffer* m_VertexBuffer = nullptr;
};

extern CMirvImageDrawer g_MirvImageDrawer;
