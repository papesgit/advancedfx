#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>

/// Binary input packet format (26 bytes) for ultra-low latency
#pragma pack(push, 1)
struct InputPacket {
    uint32_t sequence;      // Packet sequence number
    int16_t mouseDx;        // Mouse delta X
    int16_t mouseDy;        // Mouse delta Y
    int8_t mouseWheel;      // Mouse wheel delta
    uint8_t mouseButtons;   // Button flags (L=1, R=2, M=4)
    uint64_t keysDown;      // Bitmask of keys currently down
    uint64_t timestamp;     // Microsecond timestamp
};
#pragma pack(pop)

/// Input state decoded from packets
struct InputState {
    int16_t mouseDx;
    int16_t mouseDy;
    int8_t mouseWheel;
    bool mouseLeft;
    bool mouseRight;
    bool mouseMiddle;

    // Key states (common keys for freecam)
    bool keyW;
    bool keyA;
    bool keyS;
    bool keyD;
    bool keySpace;
    bool keyCtrl;
    bool keyShift;
    bool keyQ;
    bool keyE;

    // Number keys for player switching
    bool key1, key2, key3, key4, key5;
    bool key6, key7, key8, key9, key0;

    uint64_t timestamp;

    InputState()
        : mouseDx(0), mouseDy(0), mouseWheel(0)
        , mouseLeft(false), mouseRight(false), mouseMiddle(false)
        , keyW(false), keyA(false), keyS(false), keyD(false)
        , keySpace(false), keyCtrl(false), keyShift(false)
        , keyQ(false), keyE(false)
        , key1(false), key2(false), key3(false), key4(false), key5(false)
        , key6(false), key7(false), key8(false), key9(false), key0(false)
        , timestamp(0)
    {}
};

/// UDP input receiver for low-latency freecam control
/// Target: <2ms latency, 240Hz update rate
class CObsInputReceiver {
public:
    CObsInputReceiver();
    ~CObsInputReceiver();

    /// Start UDP receiver on specified port
    /// @param port Port number (default: 31339)
    /// @return true if started successfully
    bool Start(uint16_t port = 31339);

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
    void DecodePacket(const InputPacket& packet, InputState& state);

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
