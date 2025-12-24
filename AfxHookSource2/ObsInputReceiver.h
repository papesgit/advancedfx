#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <array>
#include <initializer_list>

/// Binary input packet v1 format (50 bytes) for ultra-low latency
#pragma pack(push, 1)
struct InputPacketV1 {
    uint32_t sequence;      // Packet sequence number
    int16_t mouseDx;        // Mouse delta X
    int16_t mouseDy;        // Mouse delta Y
    int8_t mouseWheel;      // Mouse wheel delta
    uint8_t mouseButtons;   // Button flags (L=1, R=2, M=4, X1=8, X2=16)
    uint8_t keyBitmap[32];  // 256-bit virtual-key bitmap (VK code -> bit)
    uint64_t timestamp;     // Microsecond timestamp
};

/// Binary input packet v2 format (versioned, includes analog axes)
struct InputPacketV2 {
    uint8_t version;       // Packet version (2)
    uint8_t flags;         // bit0 = analog enabled
    uint16_t reserved;     // alignment
    InputPacketV1 base;
    float analogLX;        // Left stick X [-1, 1]
    float analogLY;        // Left stick Y [-1, 1]
    float analogRY;        // Right stick Y [-1, 1]
    float analogRX;        // Right stick X [-1, 1] (sprint)
};
#pragma pack(pop)

/// Input state decoded from packets
struct InputState {
    static constexpr size_t kKeyBitmapSize = 32; // 256 bits

    int16_t mouseDx;
    int16_t mouseDy;
    int8_t mouseWheel;
    bool mouseLeft;
    bool mouseRight;
    bool mouseMiddle;
    bool mouseButton4;
    bool mouseButton5;
    bool analogEnabled;
    float analogLX;
    float analogLY;
    float analogRY;
    float analogRX;

    std::array<uint8_t, kKeyBitmapSize> keyBitmap;
    uint64_t timestamp;

    InputState()
        : mouseDx(0), mouseDy(0), mouseWheel(0)
        , mouseLeft(false), mouseRight(false), mouseMiddle(false)
        , mouseButton4(false), mouseButton5(false)
        , keyBitmap{}
        , timestamp(0)
        , analogEnabled(false)
        , analogLX(0.0f)
        , analogLY(0.0f)
        , analogRY(0.0f)
        , analogRX(0.0f)
    {}

    bool IsKeyDown(uint8_t virtualKey) const {
        if (virtualKey >= 256) {
            return false;
        }
        const size_t byteIndex = virtualKey >> 3;
        const uint8_t mask = static_cast<uint8_t>(1u << (virtualKey & 0x07));
        return (keyBitmap[byteIndex] & mask) != 0;
    }

    bool IsAnyKeyDown(std::initializer_list<uint8_t> virtualKeys) const {
        for (uint8_t vk : virtualKeys) {
            if (IsKeyDown(vk)) {
                return true;
            }
        }
        return false;
    }
};

/// UDP input receiver for low-latency freecam control
/// Target: <2ms latency, 240Hz update rate
class CObsInputReceiver {
public:
    CObsInputReceiver();
    ~CObsInputReceiver();

    /// Start UDP receiver on specified port
    /// @param port Port number (default: 31339)
    /// @param bindAddress Address to bind (default: 127.0.0.1)
    /// @return true if started successfully
    bool Start(uint16_t port = 31339, const char* bindAddress = "127.0.0.1");

    /// Stop UDP receiver
    void Stop();

    /// Check if receiver is active
    bool IsActive() const { return m_bActive; }

    /// Get latest input state (thread-safe)
    /// @param outState Output input state
    /// @return true if new data available since last call
    bool GetInputState(InputState& outState);

    /// Get packets per second
    uint32_t GetPacketsPerSecond() const { return m_PacketsPerSecond; }

    /// Get packet loss percentage
    float GetPacketLoss() const;

    /// Enable or disable debug logging
    void SetDebug(bool debug) { m_Debug = debug; }

private:
    void ReceiveThread();
    void DecodePacket(const InputPacketV1& packet, InputState& state);
    void DecodePacket(const InputPacketV2& packet, InputState& state);

    std::atomic<bool> m_bActive;
    SOCKET m_Socket;
    std::thread m_ReceiveThread;

    // Latest input state (double-buffered for lock-free reads)
    InputState m_CurrentState;
    std::mutex m_StateMutex;
    std::atomic<bool> m_bNewData;

    // Statistics
    std::atomic<uint32_t> m_PacketsPerSecond;
    std::atomic<uint32_t> m_LastSequence;
    std::atomic<uint32_t> m_PacketLossCount;
    std::atomic<uint32_t> m_TotalPackets;

    // Debug
    std::atomic<bool> m_Debug{ false };
};
