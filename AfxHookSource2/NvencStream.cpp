#include "stdafx.h"
#include "NvencStream.h"

#include "../deps/release/nvcodec/samples/NvEncoder/NvEncoderD3D11.h"
#include "../deps/release/nvcodec/samples/Utils/Logger.h"
#include "../deps/release/nvcodec/samples/Utils/NvCodecUtils.h"

#include <d3dcompiler.h>
#include <iostream>
#include <sstream>
#include <functional>
#include <array>

#pragma comment(lib, "ws2_32.lib")

// Simple fullscreen vertex shader - generates fullscreen triangle
static const char* g_FullscreenVS = R"(
struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VS_OUTPUT main(uint id : SV_VertexID) {
    VS_OUTPUT output;
    // Generate fullscreen triangle
    output.uv = float2((id << 1) & 2, id & 2);
    output.pos = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
)";

// Pixel shader to copy texture with format conversion
// D3D11 handles RGBA->BGRA layout conversion automatically
static const char* g_ConvertPS = R"(
Texture2D sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_TARGET {
    // D3D11 automatically handles memory layout conversion between
    // RGBA source texture and BGRA render target - no manual swapping needed
    return sourceTexture.Sample(sourceSampler, input.uv);
}
)";

simplelogger::Logger* g_NvencLogger = simplelogger::LoggerFactory::CreateConsoleLogger();

static bool HasStartCode(const uint8_t* data, size_t size)
{
    if (!data || size < 3) return false;
    for (size_t i = 0; i + 3 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return true;
        if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) return true;
    }
    return false;
}

static std::string Base64Encode(const std::vector<uint8_t>& input)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < input.size(); i += 3) {
        uint32_t triple = (input[i] << 16) | (input[i + 1] << 8) | input[i + 2];
        out.push_back(table[(triple >> 18) & 0x3F]);
        out.push_back(table[(triple >> 12) & 0x3F]);
        out.push_back(table[(triple >> 6) & 0x3F]);
        out.push_back(table[triple & 0x3F]);
    }
    if (i + 1 == input.size()) {
        uint32_t triple = (input[i] << 16);
        out.push_back(table[(triple >> 18) & 0x3F]);
        out.push_back(table[(triple >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    }
    else if (i + 2 == input.size()) {
        uint32_t triple = (input[i] << 16) | (input[i + 1] << 8);
        out.push_back(table[(triple >> 18) & 0x3F]);
        out.push_back(table[(triple >> 12) & 0x3F]);
        out.push_back(table[(triple >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

CNvencStream::CNvencStream()
    : m_bActive(false)
    , m_pDevice(nullptr)
    , m_pStagingTexture(nullptr)
    , m_pStagingRTV(nullptr)
    , m_pSourceSRV(nullptr)
    , m_pConvertShader(nullptr)
    , m_pFullscreenVS(nullptr)
    , m_pSamplerState(nullptr)
    , m_nWidth(0)
    , m_nHeight(0)
    , m_nEncodedFrames(0)
    , m_nDroppedFrames(0)
    , m_SourceFormat(DXGI_FORMAT_UNKNOWN)
    , m_EncoderFormat(DXGI_FORMAT_UNKNOWN)
    , m_bStreamingEnabled(false)
    , m_Socket(INVALID_SOCKET)
    , m_RtpSequence(0)
    , m_RtpTimestamp(0)
    , m_RtpSsrc(0x12345678) // Random SSRC
    , m_bSentKeyframe(false)
    , m_TargetFrameTime(std::chrono::microseconds(16667)) // 60fps = 16.667ms per frame
{
    // Initialize WinSock
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

CNvencStream::~CNvencStream()
{
    Stop();
    DisableStreaming();
    WSACleanup();
}

bool CNvencStream::Start(ID3D11Device* pDevice, uint32_t nWidth, uint32_t nHeight, const char* outputPath)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (m_bActive) {
        return false; // Already active
    }

    if (!pDevice || nWidth == 0 || nHeight == 0) {
        return false;
    }

    m_nWidth = nWidth;
    m_nHeight = nHeight;
    m_pDevice = pDevice;
    m_pDevice->AddRef();
    m_bSentKeyframe = false;
    m_RtpStartTime = std::chrono::steady_clock::now();
    m_LastIdrTime = m_RtpStartTime;
    m_LastEncodeTime = m_RtpStartTime;

    // Create debug log file
    m_pDebugLog = std::make_unique<std::ofstream>("nvenc_debug.txt", std::ios::out | std::ios::trunc);
    if (m_pDebugLog && m_pDebugLog->is_open()) {
        *m_pDebugLog << "=== NVENC Debug Log ===" << std::endl;
        *m_pDebugLog << "Resolution: " << nWidth << "x" << nHeight << std::endl;
        m_pDebugLog->flush();
    }

    // Save output path if provided
    if (outputPath && strlen(outputPath) > 0) {
        m_OutputPath = outputPath;
        m_pOutputFile = std::make_unique<std::ofstream>(m_OutputPath, std::ios::out | std::ios::binary);
        if (!m_pOutputFile->is_open()) {
            std::cerr << "Failed to open output file: " << m_OutputPath << std::endl;
            Cleanup();
            return false;
        }
    }

    // Initialize encoder
    if (!InitializeEncoder(pDevice, nWidth, nHeight)) {
        std::cerr << "Failed to initialize NVENC encoder" << std::endl;
        Cleanup();
        return false;
    }

    m_bActive = true;
    m_nEncodedFrames = 0;
    m_nDroppedFrames = 0;

    std::cout << "NVENC stream started: " << nWidth << "x" << nHeight << std::endl;
    if (!m_OutputPath.empty()) {
        std::cout << "  Output file: " << m_OutputPath << std::endl;
    }

    return true;
}

void CNvencStream::Stop()
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (!m_bActive) {
        return;
    }

    m_bActive = false;

    // Flush encoder
    if (m_pEncoder) {
        try {
            std::vector<NvEncOutputFrame> vPacket;
            m_pEncoder->EndEncode(vPacket);

            // Write remaining packets
            for (NvEncOutputFrame& packet : vPacket) {
                ProcessEncodedFrame(packet.frame);
            }

            m_pEncoder->DestroyEncoder();
        }
        catch (const std::exception& ex) {
            std::cerr << "Error during encoder shutdown: " << ex.what() << std::endl;
        }
    }

    // Close output file
    if (m_pOutputFile) {
        m_pOutputFile->close();
        m_pOutputFile.reset();
    }

    // Close debug log
    if (m_pDebugLog) {
        *m_pDebugLog << "=== Encoding stopped ===" << std::endl;
        *m_pDebugLog << "Total encoded frames: " << m_nEncodedFrames << std::endl;
        *m_pDebugLog << "Total dropped frames: " << m_nDroppedFrames << std::endl;
        m_pDebugLog->close();
        m_pDebugLog.reset();
    }

    Cleanup();
}

bool CNvencStream::EnableStreaming(const char* ipAddress, uint16_t port)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    // Close existing socket if any
    DisableStreaming();

    // Create UDP socket
    m_Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_Socket == INVALID_SOCKET) {
        std::cerr << "Failed to create UDP socket" << std::endl;
        return false;
    }

    // Setup destination address
    memset(&m_DestAddr, 0, sizeof(m_DestAddr));
    m_DestAddr.sin_family = AF_INET;
    m_DestAddr.sin_port = htons(port);
    inet_pton(AF_INET, ipAddress, &m_DestAddr.sin_addr);

    // Reset RTP sequence and timestamp
    m_RtpSequence = 0;
    m_RtpTimestamp = 0;
    m_RtpStartTime = std::chrono::steady_clock::now();
    m_LastIdrTime = m_RtpStartTime;

    m_bStreamingEnabled = true;
    WriteSdpFile(ipAddress, port);

    std::cout << "Network streaming enabled: " << ipAddress << ":" << port << std::endl;
    if (m_pDebugLog && m_pDebugLog->is_open()) {
        *m_pDebugLog << "Network streaming enabled: " << ipAddress << ":" << port << std::endl;
        if (!m_SdpPath.empty()) {
            *m_pDebugLog << "SDP written to: " << m_SdpPath << std::endl;
        }
        m_pDebugLog->flush();
    }

    return true;
}

void CNvencStream::DisableStreaming()
{
    if (m_Socket != INVALID_SOCKET) {
        closesocket(m_Socket);
        m_Socket = INVALID_SOCKET;
    }
    m_bStreamingEnabled = false;
}

bool CNvencStream::InitializeEncoder(ID3D11Device* pDevice, uint32_t nWidth, uint32_t nHeight)
{
    try {
        if (m_pDebugLog && m_pDebugLog->is_open()) {
            *m_pDebugLog << "Initializing encoder..." << std::endl;
            *m_pDebugLog << "NOTE: Using ARGB format (creates BGRA textures)" << std::endl;
            *m_pDebugLog << "      Will use staging texture for format conversion" << std::endl;
            m_pDebugLog->flush();
        }

        // Create NvEncoderD3D11 instance
        // NOTE: We use ARGB which creates BGRA textures (format 87)
        // We'll handle the RGBA->BGRA conversion manually using a staging texture
        m_pEncoder = std::make_unique<NvEncoderD3D11>(pDevice, nWidth, nHeight, NV_ENC_BUFFER_FORMAT_ARGB);

        // Setup encode parameters
        NV_ENC_INITIALIZE_PARAMS initializeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
        NV_ENC_CONFIG encodeConfig = { NV_ENC_CONFIG_VER };
        initializeParams.encodeConfig = &encodeConfig;

        // Create default encoder params with H.264 codec
        m_pEncoder->CreateDefaultEncoderParams(
            &initializeParams,
            NV_ENC_CODEC_H264_GUID,
            NV_ENC_PRESET_P3_GUID,  // Low latency preset
            NV_ENC_TUNING_INFO_LOW_LATENCY
        );

        // Customize settings for real-time streaming
        initializeParams.encodeWidth = nWidth;
        initializeParams.encodeHeight = nHeight;
        initializeParams.frameRateNum = 60;
        initializeParams.frameRateDen = 1;

        // Low latency settings
        encodeConfig.gopLength = NVENC_INFINITE_GOPLENGTH;  // All P frames for lowest latency
        encodeConfig.frameIntervalP = 1;

        // H.264 specific settings
        encodeConfig.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        encodeConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1;  // Repeat SPS/PPS for robustness

        // Rate control
        encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;  // Constant bitrate for streaming
        encodeConfig.rcParams.averageBitRate = 8000000;  // 8 Mbps (higher quality for 60fps)
        encodeConfig.rcParams.maxBitRate = 8000000;

        // Create the encoder
        m_pEncoder->CreateEncoder(&initializeParams);

        // Cache SPS/PPS from sequence parameters provided by the encoder
        std::vector<uint8_t> seqParams;
        m_pEncoder->GetSequenceParams(seqParams);
        ParseParameterSets(seqParams.data(), seqParams.size());

        // Create a staging texture for format conversion (RGBA -> BGRA)
        D3D11_TEXTURE2D_DESC stagingDesc = {};
        stagingDesc.Width = nWidth;
        stagingDesc.Height = nHeight;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // BGRA to match encoder
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_DEFAULT;
        stagingDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        stagingDesc.CPUAccessFlags = 0;

        HRESULT hr = pDevice->CreateTexture2D(&stagingDesc, nullptr, &m_pStagingTexture);
        if (FAILED(hr)) {
            if (m_pDebugLog && m_pDebugLog->is_open()) {
                *m_pDebugLog << "ERROR: Failed to create staging texture for format conversion" << std::endl;
                m_pDebugLog->flush();
            }
            return false;
        }

        // Create render target view for staging texture
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = 0;

        hr = pDevice->CreateRenderTargetView(m_pStagingTexture, &rtvDesc, &m_pStagingRTV);
        if (FAILED(hr)) {
            if (m_pDebugLog && m_pDebugLog->is_open()) {
                *m_pDebugLog << "ERROR: Failed to create render target view" << std::endl;
                m_pDebugLog->flush();
            }
            return false;
        }

        // Create sampler state for texture sampling
        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samplerDesc.MinLOD = 0;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

        hr = pDevice->CreateSamplerState(&samplerDesc, &m_pSamplerState);
        if (FAILED(hr)) {
            if (m_pDebugLog && m_pDebugLog->is_open()) {
                *m_pDebugLog << "ERROR: Failed to create sampler state" << std::endl;
                m_pDebugLog->flush();
            }
            return false;
        }

        // Compile and create vertex shader
        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* errorBlob = nullptr;
        hr = D3DCompile(
            g_FullscreenVS, strlen(g_FullscreenVS),
            "FullscreenVS", nullptr, nullptr,
            "main", "vs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS, 0,
            &vsBlob, &errorBlob
        );

        if (FAILED(hr)) {
            if (m_pDebugLog && m_pDebugLog->is_open()) {
                *m_pDebugLog << "ERROR: Failed to compile vertex shader" << std::endl;
                if (errorBlob) {
                    *m_pDebugLog << "  Shader error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
                }
                m_pDebugLog->flush();
            }
            if (errorBlob) errorBlob->Release();
            return false;
        }

        hr = pDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_pFullscreenVS);
        vsBlob->Release();

        if (FAILED(hr)) {
            if (m_pDebugLog && m_pDebugLog->is_open()) {
                *m_pDebugLog << "ERROR: Failed to create vertex shader" << std::endl;
                m_pDebugLog->flush();
            }
            return false;
        }

        // Compile and create pixel shader
        ID3DBlob* psBlob = nullptr;
        hr = D3DCompile(
            g_ConvertPS, strlen(g_ConvertPS),
            "ConvertPS", nullptr, nullptr,
            "main", "ps_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS, 0,
            &psBlob, &errorBlob
        );

        if (FAILED(hr)) {
            if (m_pDebugLog && m_pDebugLog->is_open()) {
                *m_pDebugLog << "ERROR: Failed to compile pixel shader" << std::endl;
                if (errorBlob) {
                    *m_pDebugLog << "  Shader error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
                }
                m_pDebugLog->flush();
            }
            if (errorBlob) errorBlob->Release();
            return false;
        }

        hr = pDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pConvertShader);
        psBlob->Release();

        if (FAILED(hr)) {
            if (m_pDebugLog && m_pDebugLog->is_open()) {
                *m_pDebugLog << "ERROR: Failed to create pixel shader" << std::endl;
                m_pDebugLog->flush();
            }
            return false;
        }

        if (m_pDebugLog && m_pDebugLog->is_open()) {
            *m_pDebugLog << "Encoder initialized successfully" << std::endl;
            *m_pDebugLog << "Staging texture and views created for RGBA->BGRA conversion" << std::endl;
            *m_pDebugLog << "Shaders compiled and created successfully" << std::endl;
            m_pDebugLog->flush();
        }
        // After m_pDevice is valid
        if (!m_pNoDepthState) {
            D3D11_DEPTH_STENCIL_DESC dsDesc = {};
            dsDesc.DepthEnable = FALSE;
            dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
            dsDesc.StencilEnable = FALSE;
            m_pDevice->CreateDepthStencilState(&dsDesc, &m_pNoDepthState);
        }

        if (!m_pFullscreenRS) {
            D3D11_RASTERIZER_DESC rsDesc = {};
            rsDesc.FillMode = D3D11_FILL_SOLID;
            rsDesc.CullMode = D3D11_CULL_NONE;
            rsDesc.FrontCounterClockwise = FALSE;
            rsDesc.DepthClipEnable = TRUE;
            rsDesc.ScissorEnable = FALSE;
            rsDesc.MultisampleEnable = FALSE;
            rsDesc.AntialiasedLineEnable = FALSE;
            m_pDevice->CreateRasterizerState(&rsDesc, &m_pFullscreenRS);
        }

        if (!m_pNoBlendState) {
            D3D11_BLEND_DESC bsDesc = {};
            bsDesc.AlphaToCoverageEnable = FALSE;
            bsDesc.IndependentBlendEnable = FALSE;

            D3D11_RENDER_TARGET_BLEND_DESC& rt = bsDesc.RenderTarget[0];
            rt.BlendEnable = FALSE;  // <- THIS is the important bit: ignore src alpha & game blend.
            rt.SrcBlend = D3D11_BLEND_ONE;
            rt.DestBlend = D3D11_BLEND_ZERO;
            rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_ZERO;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

            m_pDevice->CreateBlendState(&bsDesc, &m_pNoBlendState);
        }

        return true;
    }
    catch (const std::exception& ex) {
        if (m_pDebugLog && m_pDebugLog->is_open()) {
            *m_pDebugLog << "ERROR: Failed to initialize encoder: " << ex.what() << std::endl;
            m_pDebugLog->flush();
        }
        std::cerr << "Failed to initialize NVENC encoder: " << ex.what() << std::endl;
        return false;
    }
}

void CNvencStream::EncodeFrame(ID3D11DeviceContext* pContext, ID3D11Texture2D* pTexture)
{
    if (!m_bActive || !m_pEncoder || !pTexture) {
        return;
    }

    // Frame rate limiting: only encode at 60fps
    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - m_LastEncodeTime;
    if (elapsed < m_TargetFrameTime) {
        // Too soon, skip this frame
        return;
    }
    m_LastEncodeTime = now;

    std::lock_guard<std::mutex> lock(m_Mutex);

    try {
        // Debug: Check source texture format (only log first frame)
        static bool bLoggedFormat = false;
        if (!bLoggedFormat) {
            D3D11_TEXTURE2D_DESC srcDesc;
            pTexture->GetDesc(&srcDesc);
            m_SourceFormat = srcDesc.Format;

            if (m_pDebugLog && m_pDebugLog->is_open()) {
                *m_pDebugLog << "Source texture format: " << srcDesc.Format
                            << " (" << srcDesc.Width << "x" << srcDesc.Height << ")" << std::endl;

                // Common formats:
                // DXGI_FORMAT_B8G8R8A8_UNORM = 87
                // DXGI_FORMAT_R8G8B8A8_UNORM = 28
                if (srcDesc.Format == 87) {
                    *m_pDebugLog << "  -> BGRA (DXGI_FORMAT_B8G8R8A8_UNORM)" << std::endl;
                }
                else if (srcDesc.Format == 28) {
                    *m_pDebugLog << "  -> RGBA (DXGI_FORMAT_R8G8B8A8_UNORM)" << std::endl;
                }
                else {
                    *m_pDebugLog << "  -> Unknown/Other format" << std::endl;
                }
                m_pDebugLog->flush();
            }
            bLoggedFormat = true;
        }

        // Get the next input frame from encoder
        const NvEncInputFrame* encoderInputFrame = m_pEncoder->GetNextInputFrame();
        if (!encoderInputFrame) {
            m_nDroppedFrames++;
            if (m_pDebugLog && m_pDebugLog->is_open()) {
                *m_pDebugLog << "ERROR: Failed to get input frame from encoder" << std::endl;
                m_pDebugLog->flush();
            }
            return;
        }

        // Copy backbuffer to encoder input texture
        ID3D11Texture2D* pInputTexture = reinterpret_cast<ID3D11Texture2D*>(encoderInputFrame->inputPtr);

        // Debug: Check destination texture format
        static bool bLoggedDstFormat = false;
        if (!bLoggedDstFormat) {
            D3D11_TEXTURE2D_DESC dstDesc;
            pInputTexture->GetDesc(&dstDesc);
            m_EncoderFormat = dstDesc.Format;

            if (m_pDebugLog && m_pDebugLog->is_open()) {
                *m_pDebugLog << "Encoder input texture format: " << dstDesc.Format << std::endl;
                if (dstDesc.Format == 87) {
                    *m_pDebugLog << "  -> BGRA (DXGI_FORMAT_B8G8R8A8_UNORM)" << std::endl;
                }
                else if (dstDesc.Format == 28) {
                    *m_pDebugLog << "  -> RGBA (DXGI_FORMAT_R8G8B8A8_UNORM)" << std::endl;
                }
                else if (dstDesc.Format == 29) {
                    *m_pDebugLog << "  -> ARGB (DXGI_FORMAT_A8R8G8B8_UNORM - not standard!)" << std::endl;
                }

                // Check if formats match
                if (m_SourceFormat == m_EncoderFormat) {
                    *m_pDebugLog << "  ✓ Formats match! CopyResource will work correctly." << std::endl;
                }
                else {
                    *m_pDebugLog << "  ✗ WARNING: Format mismatch! CopyResource may fail or produce garbage!" << std::endl;
                    *m_pDebugLog << "    Source: " << m_SourceFormat << ", Encoder: " << m_EncoderFormat << std::endl;
                }
                m_pDebugLog->flush();
            }
            bLoggedDstFormat = true;
        }

        // Use shader to convert RGBA to BGRA
        if (m_pStagingTexture && m_pFullscreenVS && m_pConvertShader) {
            // Create shader resource view for source texture (once)
            if (!m_pSourceSRV) {
                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                D3D11_TEXTURE2D_DESC srcDesc;
                pTexture->GetDesc(&srcDesc);
                srvDesc.Format = srcDesc.Format;
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = 1;
                srvDesc.Texture2D.MostDetailedMip = 0;

                HRESULT hr = m_pDevice->CreateShaderResourceView(pTexture, &srvDesc, &m_pSourceSRV);
                if (FAILED(hr)) {
                    if (m_pDebugLog && m_pDebugLog->is_open()) {
                        *m_pDebugLog << "ERROR: Failed to create shader resource view" << std::endl;
                        m_pDebugLog->flush();
                    }
                    return;
                }

                if (m_pDebugLog && m_pDebugLog->is_open()) {
                    *m_pDebugLog << "Created shader resource view for format conversion" << std::endl;
                    m_pDebugLog->flush();
                }
            }

            // Save current render state
            ID3D11RenderTargetView* oldRTV = nullptr;
            ID3D11DepthStencilView* oldDSV = nullptr;
            pContext->OMGetRenderTargets(1, &oldRTV, &oldDSV);

            // Save old blend / depth-stencil / rasterizer state too
            ID3D11BlendState*        oldBlendState = nullptr;
            FLOAT                    oldBlendFactor[4] = { 0,0,0,0 };
            UINT                     oldSampleMask = 0xffffffff;
            pContext->OMGetBlendState(&oldBlendState, oldBlendFactor, &oldSampleMask);

            ID3D11DepthStencilState* oldDepthState = nullptr;
            UINT                     oldStencilRef = 0;
            pContext->OMGetDepthStencilState(&oldDepthState, &oldStencilRef);

            ID3D11RasterizerState*   oldRSState = nullptr;
            pContext->RSGetState(&oldRSState);

            // Set up our own clean render state for conversion
            pContext->OMSetRenderTargets(1, &m_pStagingRTV, nullptr);

            D3D11_VIEWPORT viewport = {};
            viewport.TopLeftX = 0.0f;
            viewport.TopLeftY = 0.0f;
            viewport.Width  = static_cast<float>(m_nWidth);
            viewport.Height = static_cast<float>(m_nHeight);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            pContext->RSSetViewports(1, &viewport);

            if (m_pNoBlendState) {
                FLOAT blendFactor[4] = { 0,0,0,0 };
                pContext->OMSetBlendState(m_pNoBlendState, blendFactor, 0xffffffff);
            }
            if (m_pNoDepthState) {
                pContext->OMSetDepthStencilState(m_pNoDepthState, 0);
            }
            if (m_pFullscreenRS) {
                pContext->RSSetState(m_pFullscreenRS);
            }

            pContext->VSSetShader(m_pFullscreenVS, nullptr, 0);
            pContext->PSSetShader(m_pConvertShader, nullptr, 0);
            pContext->PSSetShaderResources(0, 1, &m_pSourceSRV);
            pContext->PSSetSamplers(0, 1, &m_pSamplerState);

            pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            pContext->Draw(3, 0);

            // Restore render targets
            pContext->OMSetRenderTargets(1, &oldRTV, oldDSV);

            // Restore previous states
            pContext->OMSetBlendState(oldBlendState, oldBlendFactor, oldSampleMask);
            pContext->OMSetDepthStencilState(oldDepthState, oldStencilRef);
            pContext->RSSetState(oldRSState);

            // Release our references
            if (oldRTV)       oldRTV->Release();
            if (oldDSV)       oldDSV->Release();
            if (oldBlendState) oldBlendState->Release();
            if (oldDepthState) oldDepthState->Release();
            if (oldRSState)    oldRSState->Release();

            // Unbind SRV
            ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
            pContext->PSSetShaderResources(0, 1, nullSRV);


            // Now copy the converted staging texture (BGRA) to encoder input (BGRA) - formats match!
            pContext->CopyResource(pInputTexture, m_pStagingTexture);

            if (m_pDebugLog && m_pDebugLog->is_open() && m_nEncodedFrames == 0) {
                *m_pDebugLog << "Using pixel shader to convert RGBA->BGRA" << std::endl;
                *m_pDebugLog << "Colors should now be correct" << std::endl;
                m_pDebugLog->flush();
            }
        }
        else {
            // Fallback: direct copy (will fail with format mismatch)
            if (m_pDebugLog && m_pDebugLog->is_open() && m_nEncodedFrames == 0) {
                *m_pDebugLog << "WARNING: Shader conversion not available, using direct copy" << std::endl;
                m_pDebugLog->flush();
            }
            pContext->CopyResource(pInputTexture, pTexture);
        }

        // Flush to ensure copy completes before encoding
        pContext->Flush();

        // Encode the frame
        std::vector<NvEncOutputFrame> vPacket;
        NV_ENC_PIC_PARAMS picParams = {};
        NV_ENC_PIC_PARAMS* pPicParams = nullptr;
        auto nowForIdr = std::chrono::steady_clock::now();
        bool forceIdr = !m_bSentKeyframe || (nowForIdr - m_LastIdrTime) >= std::chrono::seconds(2);
        if (forceIdr) {
            picParams.version = NV_ENC_PIC_PARAMS_VER;
            picParams.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
            pPicParams = &picParams;
            m_LastIdrTime = nowForIdr; // remember we requested one
        }
        m_pEncoder->EncodeFrame(vPacket, pPicParams);

        // Process encoded packets
        for (NvEncOutputFrame& packet : vPacket) {
            ProcessEncodedFrame(packet.frame);
            m_nEncodedFrames++;

            // Debug: Log packet size for first few frames
            if (m_pDebugLog && m_pDebugLog->is_open()) {
                if (m_nEncodedFrames <= 5) {
                    *m_pDebugLog << "Frame " << m_nEncodedFrames
                                << " encoded, size: " << packet.frame.size() << " bytes" << std::endl;
                    m_pDebugLog->flush();
                }
                else if (m_nEncodedFrames == 10) {
                    *m_pDebugLog << "Continuing to encode... (stopping detailed logging)" << std::endl;
                    m_pDebugLog->flush();
                }
            }
        }
    }
    catch (const std::exception& ex) {
        if (m_pDebugLog && m_pDebugLog->is_open()) {
            *m_pDebugLog << "ERROR encoding frame: " << ex.what() << std::endl;
            m_pDebugLog->flush();
        }
        m_nDroppedFrames++;
    }
}

void CNvencStream::ProcessEncodedFrame(const std::vector<uint8_t>& frameData)
{
    // Write to output file if enabled
    if (m_pOutputFile && m_pOutputFile->is_open()) {
        m_pOutputFile->write(reinterpret_cast<const char*>(frameData.data()), frameData.size());
    }

    // Send over network if streaming enabled
    if (m_bStreamingEnabled && m_Socket != INVALID_SOCKET) {
        struct NalSlice {
            const uint8_t* data;
            size_t size;
            uint8_t type;
        };

        const uint8_t* data = frameData.data();
        size_t dataSize = frameData.size();
        size_t offset = 0;
        std::vector<NalSlice> nals;
        bool hasIdr = false;
        bool hasSpsInFrame = false;
        bool hasPpsInFrame = false;

        if (HasStartCode(data, dataSize)) {
            // Annex-B style (start codes)
            while (offset < dataSize) {
                size_t nalStart = offset;

                if (offset + 3 < dataSize &&
                    data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 0 && data[offset + 3] == 1) {
                    nalStart = offset + 4;
                }
                else if (offset + 2 < dataSize &&
                    data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1) {
                    nalStart = offset + 3;
                }
                else {
                    offset++;
                    continue;
                }

                // Find next start code or end of data
                size_t nalEnd = dataSize;
                for (size_t i = nalStart; i + 2 < dataSize; i++) {
                    if (data[i] == 0 && data[i + 1] == 0 &&
                        (data[i + 2] == 1 || (i + 3 < dataSize && data[i + 2] == 0 && data[i + 3] == 1))) {
                        nalEnd = i;
                        break;
                    }
                }

                size_t nalSize = nalEnd - nalStart;
                if (nalSize > 0) {
                    uint8_t type = data[nalStart] & 0x1F;
                    nals.push_back({ data + nalStart, nalSize, type });
                    if (type == 5) hasIdr = true;
                    if (type == 7) hasSpsInFrame = true;
                    if (type == 8) hasPpsInFrame = true;
                }

                offset = nalEnd;
            }
        } else {
            // Length-prefixed (AVCC) style
            while (offset + 4 <= dataSize) {
                uint32_t nalSize = (data[offset] << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3];
                offset += 4;
                if (offset + nalSize > dataSize) {
                    break;
                }

                if (nalSize > 0) {
                    uint8_t type = data[offset] & 0x1F;
                    nals.push_back({ data + offset, nalSize, type });
                    if (type == 5) hasIdr = true;
                    if (type == 7) hasSpsInFrame = true;
                    if (type == 8) hasPpsInFrame = true;
                }
                offset += nalSize;
            }
        }

        // Cache SPS/PPS when seen
        for (const auto& nal : nals) {
            if (nal.type == 7 || nal.type == 8) {
                CacheParameterSet(nal.type, nal.data, nal.size);
            }
        }

        // Prepend cached SPS/PPS before an IDR if not present in this access unit
        std::vector<NalSlice> finalNals;
        if (hasIdr) {
            if (!hasSpsInFrame && !m_Sps.empty()) {
                finalNals.push_back({ m_Sps.data(), m_Sps.size(), 7 });
            }
            if (!hasPpsInFrame && !m_Pps.empty()) {
                finalNals.push_back({ m_Pps.data(), m_Pps.size(), 8 });
            }
            m_bSentKeyframe = true;
            m_LastIdrTime = std::chrono::steady_clock::now();
        }
        finalNals.insert(finalNals.end(), nals.begin(), nals.end());

        // Ensure stream starts with SPS/PPS + IDR. If we haven't sent a keyframe yet, drop until we see an IDR.
        if (!m_bSentKeyframe) {
            if (!hasIdr) {
                if (m_pDebugLog && m_pDebugLog->is_open()) {
                    *m_pDebugLog << "Waiting for IDR before starting RTP stream, dropping non-IDR frame." << std::endl;
                    m_pDebugLog->flush();
                }
                return;
            }
            m_bSentKeyframe = true;
        }

        // Debug dump: write the reconstructed access unit as Annex-B to rtp_debug.h264
        {
            static bool s_DebugInit = false;
            static std::ofstream s_RtpDebug;
            if (!s_DebugInit) {
                s_RtpDebug.open("rtp_debug.h264", std::ios::binary | std::ios::trunc);
                s_DebugInit = true;
            }
            if (s_RtpDebug.is_open()) {
                static const uint8_t startCode[4] = { 0, 0, 0, 1 };
                for (const auto& nal : finalNals) {
                    s_RtpDebug.write(reinterpret_cast<const char*>(startCode), sizeof(startCode));
                    s_RtpDebug.write(reinterpret_cast<const char*>(nal.data), nal.size);
                }
                s_RtpDebug.flush();
            }
        }

        // Send with marker on last NAL
        if (!finalNals.empty()) {
            // Timestamp based on elapsed real time (90 kHz clock)
            auto now = std::chrono::steady_clock::now();
            auto elapsed90k = std::chrono::duration_cast<std::chrono::microseconds>(now - m_RtpStartTime).count() * 90 / 1000;
            m_RtpTimestamp = static_cast<uint32_t>(elapsed90k);

            for (size_t i = 0; i < finalNals.size(); ++i) {
                const auto& nal = finalNals[i];
                bool lastNal = (i + 1 == finalNals.size());
                SendRtpPacket(nal.data, nal.size, lastNal);
            }
        }
    }
}

void write_rtp_header(uint8_t* p, uint16_t seq, uint32_t ts, uint32_t ssrc, bool marker, uint8_t payloadType)
{
    p[0] = 0x80;                             // V=2, P=0, X=0, CC=0
    p[1] = (marker ? 0x80 : 0x00) | payloadType;

    p[2] = (uint8_t)(seq >> 8);
    p[3] = (uint8_t)(seq & 0xFF);

    p[4] = (uint8_t)(ts >> 24);
    p[5] = (uint8_t)(ts >> 16);
    p[6] = (uint8_t)(ts >> 8);
    p[7] = (uint8_t)(ts & 0xFF);

    p[8]  = (uint8_t)(ssrc >> 24);
    p[9]  = (uint8_t)(ssrc >> 16);
    p[10] = (uint8_t)(ssrc >> 8);
    p[11] = (uint8_t)(ssrc & 0xFF);
}

void CNvencStream::SendRtpPacket(const uint8_t* nalData, size_t nalSize, bool lastNalOfFrame)
{
    const size_t MAX_PAYLOAD = 1400; // MTU - headers

    if (nalSize <= MAX_PAYLOAD) {
        // Single NAL unit mode
        std::vector<uint8_t> packet(12 + nalSize);
        write_rtp_header(packet.data(), m_RtpSequence++, m_RtpTimestamp, m_RtpSsrc,
                        lastNalOfFrame, 96);
        memcpy(packet.data() + 12, nalData, nalSize);

        sendto(m_Socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
            reinterpret_cast<sockaddr*>(&m_DestAddr), sizeof(m_DestAddr));
    }
    else {
        // Fragmentation Unit (FU-A) mode for large NAL units
        uint8_t nalHeader = nalData[0];
        uint8_t nalType = nalHeader & 0x1F;
        const uint8_t* payload = nalData + 1;
        size_t payloadSize = nalSize - 1;
        size_t offset = 0;
        bool firstFragment = true;

        while (offset < payloadSize) {
            size_t fragmentSize = (std::min)(MAX_PAYLOAD - 2, payloadSize - offset); // -2 for FU indicator and header
            bool lastFragment = (offset + fragmentSize >= payloadSize);

            std::vector<uint8_t> packet(12 + 2 + fragmentSize);
            write_rtp_header(packet.data(), m_RtpSequence++, m_RtpTimestamp, m_RtpSsrc,
                            lastFragment && lastNalOfFrame, 96);

            packet[12] = (nalHeader & 0xE0) | 28; // FU-A indicator
            uint8_t fuHeader = nalType;
            if (firstFragment) fuHeader |= 0x80;  // S
            if (lastFragment) fuHeader |= 0x40;   // E
            packet[13] = fuHeader;

            memcpy(packet.data() + 14, payload + offset, fragmentSize);

            sendto(m_Socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                reinterpret_cast<sockaddr*>(&m_DestAddr), sizeof(m_DestAddr));

            offset += fragmentSize;
            firstFragment = false;
        }
    }
}

void CNvencStream::Cleanup()
{
    if (m_pSamplerState) {
        m_pSamplerState->Release();
        m_pSamplerState = nullptr;
    }

    if (m_pConvertShader) {
        m_pConvertShader->Release();
        m_pConvertShader = nullptr;
    }

    if (m_pFullscreenVS) {
        m_pFullscreenVS->Release();
        m_pFullscreenVS = nullptr;
    }

    if (m_pSourceSRV) {
        m_pSourceSRV->Release();
        m_pSourceSRV = nullptr;
    }

    if (m_pStagingRTV) {
        m_pStagingRTV->Release();
        m_pStagingRTV = nullptr;
    }

    if (m_pStagingTexture) {
        m_pStagingTexture->Release();
        m_pStagingTexture = nullptr;
    }

    if (m_pNoBlendState)      { m_pNoBlendState->Release();      m_pNoBlendState = nullptr; }
    if (m_pNoDepthState)      { m_pNoDepthState->Release();      m_pNoDepthState = nullptr; }
    if (m_pFullscreenRS)      { m_pFullscreenRS->Release();      m_pFullscreenRS = nullptr; }

    m_pEncoder.reset();

    if (m_pDevice) {
        m_pDevice->Release();
        m_pDevice = nullptr;
    }
    m_bSentKeyframe = false;
}

void CNvencStream::ParseParameterSets(const uint8_t* data, size_t size)
{
    if (!data || size == 0) return;

    size_t offset = 0;
    if (HasStartCode(data, size)) {
        // Annex-B style
        while (offset < size) {
            size_t nalStart = offset;

            if (offset + 3 < size &&
                data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 0 && data[offset + 3] == 1) {
                nalStart = offset + 4;
            }
            else if (offset + 2 < size &&
                data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1) {
                nalStart = offset + 3;
            }
            else {
                offset++;
                continue;
            }

            size_t nalEnd = size;
            for (size_t i = nalStart; i + 2 < size; i++) {
                if (data[i] == 0 && data[i + 1] == 0 &&
                    (data[i + 2] == 1 || (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1))) {
                    nalEnd = i;
                    break;
                }
            }

            size_t nalSize = nalEnd - nalStart;
            if (nalSize > 0) {
                uint8_t type = data[nalStart] & 0x1F;
                if (type == 7 || type == 8) {
                    CacheParameterSet(type, data + nalStart, nalSize);
                }
            }

            offset = nalEnd;
        }
    } else {
        // Length-prefixed (AVCC)
        while (offset + 4 <= size) {
            uint32_t nalSize = (data[offset] << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3];
            offset += 4;
            if (offset + nalSize > size) break;

            if (nalSize > 0) {
                uint8_t type = data[offset] & 0x1F;
                if (type == 7 || type == 8) {
                    CacheParameterSet(type, data + offset, nalSize);
                }
            }
            offset += nalSize;
        }
    }
}

void CNvencStream::WriteSdpFile(const char* ipAddress, uint16_t port)
{
    m_SdpPath.clear();
    if (!ipAddress || port == 0) {
        return;
    }

    // Minimal SDP so players like VLC know the dynamic payload (96) is H.264.
    m_SdpPath = "nvenc_stream.sdp";
    std::ofstream sdp(m_SdpPath, std::ios::out | std::ios::trunc);
    if (!sdp.is_open()) {
        if (m_pDebugLog && m_pDebugLog->is_open()) {
            *m_pDebugLog << "WARNING: Could not write SDP file." << std::endl;
            m_pDebugLog->flush();
        }
        return;
    }

    sdp << "v=0\n";
    sdp << "o=- 0 0 IN IP4 0.0.0.0\n";
    sdp << "s=HLAE NVENC Stream\n";
    sdp << "c=IN IP4 " << ipAddress << "\n";
    sdp << "t=0 0\n";
    sdp << "m=video " << port << " RTP/AVP 96\n";
    sdp << "a=rtpmap:96 H264/90000\n";

    std::string fmtp = "a=fmtp:96 packetization-mode=1";
    if (!m_Sps.empty() && !m_Pps.empty()) {
        fmtp += ";sprop-parameter-sets=" + Base64Encode(m_Sps) + "," + Base64Encode(m_Pps);
    }
    sdp << fmtp << "\n";
    sdp << "a=recvonly\n";
    sdp.close();

    if (m_pDebugLog && m_pDebugLog->is_open()) {
        *m_pDebugLog << "SDP file generated at " << m_SdpPath
                     << " (open this in your player)" << std::endl;
        m_pDebugLog->flush();
    }
}

void CNvencStream::CacheParameterSet(uint8_t nalType, const uint8_t* data, size_t size)
{
    if (!data || size == 0) return;
    if (nalType == 7) {
        m_Sps.assign(data, data + size);
    } else if (nalType == 8) {
        m_Pps.assign(data, data + size);
    }
}
