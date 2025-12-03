#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <d3d11.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <vector>
#include <fstream>
#include <string>
#include <chrono>

// Forward declarations
class NvEncoderD3D11;

/// NVENC encoder stream for real-time encoding of D3D11 backbuffer
/// This is separate from CAfxCapture and runs independently
class CNvencStream {
public:
    CNvencStream();
    ~CNvencStream();

    /// Start encoding with the given parameters
    /// @param pDevice D3D11 device
    /// @param nWidth Frame width
    /// @param nHeight Frame height
    /// @param outputPath Optional output file path for testing (empty = no file output)
    bool Start(ID3D11Device* pDevice, uint32_t nWidth, uint32_t nHeight, const char* outputPath = nullptr);

    /// Enable network streaming
    /// @param ipAddress Destination IP address
    /// @param port Destination port
    bool EnableStreaming(const char* ipAddress, uint16_t port);

    /// Disable network streaming
    void DisableStreaming();

    /// Stop encoding and cleanup
    void Stop();

    /// Check if encoder is active
    bool IsActive() const { return m_bActive; }

    /// Encode a frame
    /// @param pContext D3D11 device context
    /// @param pTexture Source texture (backbuffer)
    void EncodeFrame(ID3D11DeviceContext* pContext, ID3D11Texture2D* pTexture);

    /// Get statistics
    uint32_t GetEncodedFrameCount() const { return m_nEncodedFrames; }
    uint32_t GetDroppedFrameCount() const { return m_nDroppedFrames; }

    /// Get format info (for debugging)
    DXGI_FORMAT GetSourceFormat() const { return m_SourceFormat; }
    DXGI_FORMAT GetEncoderFormat() const { return m_EncoderFormat; }

private:
    void Cleanup();
    bool InitializeEncoder(ID3D11Device* pDevice, uint32_t nWidth, uint32_t nHeight);
    void ProcessEncodedFrame(const std::vector<uint8_t>& frameData);
    void SendRtpPacket(const uint8_t* nalData, size_t nalSize, bool lastNalOfFrame);
    void WriteSdpFile(const char* ipAddress, uint16_t port);
    void ParseParameterSets(const uint8_t* data, size_t size);
    void CacheParameterSet(uint8_t nalType, const uint8_t* data, size_t size);

    std::atomic<bool> m_bActive;
    std::mutex m_Mutex;

    // Encoder
    std::unique_ptr<NvEncoderD3D11> m_pEncoder;

    // D3D11 resources
    ID3D11Device* m_pDevice;
    ID3D11Texture2D* m_pStagingTexture; // For format conversion
    ID3D11RenderTargetView* m_pStagingRTV;
    ID3D11ShaderResourceView* m_pSourceSRV;
    ID3D11PixelShader* m_pConvertShader;
    ID3D11VertexShader* m_pFullscreenVS;
    ID3D11SamplerState* m_pSamplerState;
    ID3D11BlendState*         m_pNoBlendState       = nullptr;
    ID3D11DepthStencilState*  m_pNoDepthState       = nullptr;
    ID3D11RasterizerState*    m_pFullscreenRS       = nullptr;
    
    // Frame size
    uint32_t m_nWidth;
    uint32_t m_nHeight;

    // Statistics
    std::atomic<uint32_t> m_nEncodedFrames;
    std::atomic<uint32_t> m_nDroppedFrames;

    // Output file (for testing)
    std::unique_ptr<std::ofstream> m_pOutputFile;
    std::string m_OutputPath;

    // Debug log file
    std::unique_ptr<std::ofstream> m_pDebugLog;

    // Debug info (accessible after start)
    DXGI_FORMAT m_SourceFormat;
    DXGI_FORMAT m_EncoderFormat;

    // Network streaming
    std::atomic<bool> m_bStreamingEnabled;
    SOCKET m_Socket;
    sockaddr_in m_DestAddr;
    uint32_t m_RtpSequence;
    uint32_t m_RtpTimestamp;
    uint32_t m_RtpSsrc;
    std::string m_SdpPath;
    std::vector<uint8_t> m_Sps;
    std::vector<uint8_t> m_Pps;
    bool m_bSentKeyframe;
    std::chrono::steady_clock::time_point m_RtpStartTime;
    std::chrono::steady_clock::time_point m_LastIdrTime;

    // Frame rate limiting
    std::chrono::steady_clock::time_point m_LastEncodeTime;
    std::chrono::microseconds m_TargetFrameTime;
};
