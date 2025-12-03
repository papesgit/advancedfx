#include "ObsInputReceiver.h"
#include <chrono>
#include "../shared/AfxConsole.h"

// Key bit positions in keysDown bitmask
#define KEY_W     0
#define KEY_A     1
#define KEY_S     2
#define KEY_D     3
#define KEY_SPACE 4
#define KEY_CTRL  5
#define KEY_SHIFT 6
#define KEY_Q     7
#define KEY_E     8
#define KEY_1     9
#define KEY_2     10
#define KEY_3     11
#define KEY_4     12
#define KEY_5     13
#define KEY_6     14
#define KEY_7     15
#define KEY_8     16
#define KEY_9     17
#define KEY_0     18

CObsInputReceiver::CObsInputReceiver()
    : m_bActive(false)
    , m_Socket(INVALID_SOCKET)
    , m_bNewData(false)
    , m_PacketsPerSecond(0)
    , m_LastSequence(0)
    , m_PacketLossCount(0)
    , m_TotalPackets(0)
{
}

CObsInputReceiver::~CObsInputReceiver() {
    Stop();
}

bool CObsInputReceiver::Start(uint16_t port) {
    if (m_bActive) {
        return false;
    }

    // Initialize Winsock (may already be initialized)
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    // Create UDP socket
    m_Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_Socket == INVALID_SOCKET) {
        return false;
    }

    // Set reuse address
    int opt = 1;
    setsockopt(m_Socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    // Bind to port
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(m_Socket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(m_Socket);
        m_Socket = INVALID_SOCKET;
        return false;
    }

    // Set receive buffer size for low latency
    int recvBuf = 64 * 1024; // 64 KB
    setsockopt(m_Socket, SOL_SOCKET, SO_RCVBUF, (const char*)&recvBuf, sizeof(recvBuf));

    m_bActive = true;
    m_ReceiveThread = std::thread(&CObsInputReceiver::ReceiveThread, this);

    return true;
}

void CObsInputReceiver::Stop() {
    if (!m_bActive) {
        return;
    }

    m_bActive = false;

    if (m_Socket != INVALID_SOCKET) {
        closesocket(m_Socket);
        m_Socket = INVALID_SOCKET;
    }

    if (m_ReceiveThread.joinable()) {
        m_ReceiveThread.join();
    }
}

bool CObsInputReceiver::GetInputState(InputState& outState) {
    std::lock_guard<std::mutex> lock(m_StateMutex);
    outState = m_CurrentState;

    // Clear accumulated deltas after consumption (but keep key/button state)
    m_CurrentState.mouseDx = 0;
    m_CurrentState.mouseDy = 0;
    m_CurrentState.mouseWheel = 0;

    // Always return true so freecam updates every frame (keyboard state persists)
    // Mouse deltas are accumulated between calls, so every bit of movement is applied
    return true;
}

float CObsInputReceiver::GetPacketLoss() const {
    uint32_t total = m_TotalPackets.load();
    if (total == 0) {
        return 0.0f;
    }
    return (m_PacketLossCount.load() * 100.0f) / total;
}

void CObsInputReceiver::ReceiveThread() {
    InputPacket packet;
    sockaddr_in fromAddr;
    int fromLen = sizeof(fromAddr);

    uint32_t packetsThisSecond = 0;
    auto lastStatTime = std::chrono::steady_clock::now();

    while (m_bActive) {
        int bytesRead = recvfrom(
            m_Socket,
            (char*)&packet,
            sizeof(packet),
            0,
            (sockaddr*)&fromAddr,
            &fromLen
        );

        if (bytesRead == sizeof(InputPacket)) {
            // Decode packet
            InputState packetState;
            DecodePacket(packet, packetState);

            // Accumulate mouse deltas and update key/button state
            {
                std::lock_guard<std::mutex> lock(m_StateMutex);

                // Accumulate deltas between game frames
                m_CurrentState.mouseDx += packetState.mouseDx;
                m_CurrentState.mouseDy += packetState.mouseDy;
                m_CurrentState.mouseWheel += packetState.mouseWheel;

                // Latest button state
                m_CurrentState.mouseLeft = packetState.mouseLeft;
                m_CurrentState.mouseRight = packetState.mouseRight;
                m_CurrentState.mouseMiddle = packetState.mouseMiddle;

                // Latest key state
                m_CurrentState.keyW = packetState.keyW;
                m_CurrentState.keyA = packetState.keyA;
                m_CurrentState.keyS = packetState.keyS;
                m_CurrentState.keyD = packetState.keyD;
                m_CurrentState.keySpace = packetState.keySpace;
                m_CurrentState.keyCtrl = packetState.keyCtrl;
                m_CurrentState.keyShift = packetState.keyShift;
                m_CurrentState.keyQ = packetState.keyQ;
                m_CurrentState.keyE = packetState.keyE;
                m_CurrentState.key1 = packetState.key1;
                m_CurrentState.key2 = packetState.key2;
                m_CurrentState.key3 = packetState.key3;
                m_CurrentState.key4 = packetState.key4;
                m_CurrentState.key5 = packetState.key5;
                m_CurrentState.key6 = packetState.key6;
                m_CurrentState.key7 = packetState.key7;
                m_CurrentState.key8 = packetState.key8;
                m_CurrentState.key9 = packetState.key9;
                m_CurrentState.key0 = packetState.key0;

                m_CurrentState.timestamp = packetState.timestamp;
            }
            m_bNewData = true;

            if (m_Debug) {
                advancedfx::Message(
                    "mirv_udpdebug: seq=%u dx=%d dy=%d wheel=%d buttons=L%sR%sM%s keys=W%sA%sS%sD%sSpace%sCtrl%sShift%sQ%sE%s\n",
                    packet.sequence,
                    (int)packetState.mouseDx,
                    (int)packetState.mouseDy,
                    (int)packetState.mouseWheel,
                    packetState.mouseLeft ? "1" : "0",
                    packetState.mouseRight ? "1" : "0",
                    packetState.mouseMiddle ? "1" : "0",
                    packetState.keyW ? "1" : "0",
                    packetState.keyA ? "1" : "0",
                    packetState.keyS ? "1" : "0",
                    packetState.keyD ? "1" : "0",
                    packetState.keySpace ? "1" : "0",
                    packetState.keyCtrl ? "1" : "0",
                    packetState.keyShift ? "1" : "0",
                    packetState.keyQ ? "1" : "0",
                    packetState.keyE ? "1" : "0",
                    packetState.key1 ? "1" : "0",
                    packetState.key2 ? "1" : "0",
                    packetState.key3 ? "1" : "0",
                    packetState.key4 ? "1" : "0",
                    packetState.key5 ? "1" : "0",
                    packetState.key6 ? "1" : "0",
                    packetState.key7 ? "1" : "0",
                    packetState.key8 ? "1" : "0",
                    packetState.key9 ? "1" : "0",
                    packetState.key0 ? "1" : "0"
                );
            }

            // Track packet loss
            if (m_LastSequence != 0) {
                uint32_t expected = m_LastSequence + 1;
                if (packet.sequence != expected) {
                    // Packet loss detected
                    uint32_t lost = packet.sequence - expected;
                    m_PacketLossCount += lost;
                }
            }
            m_LastSequence = packet.sequence;
            m_TotalPackets++;

            // Update packets per second
            packetsThisSecond++;
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStatTime).count();
            if (elapsed >= 1000) {
                m_PacketsPerSecond = packetsThisSecond;
                packetsThisSecond = 0;
                lastStatTime = now;
            }
        } else if (bytesRead == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && m_bActive) {
                Sleep(1);
            }
        }
    }
}

void CObsInputReceiver::DecodePacket(const InputPacket& packet, InputState& state) {
    state.mouseDx = packet.mouseDx;
    state.mouseDy = packet.mouseDy;
    state.mouseWheel = packet.mouseWheel;

    // Decode mouse buttons
    state.mouseLeft = (packet.mouseButtons & 0x01) != 0;
    state.mouseRight = (packet.mouseButtons & 0x02) != 0;
    state.mouseMiddle = (packet.mouseButtons & 0x04) != 0;

    // Decode keyboard keys from bitmask
    state.keyW = (packet.keysDown & (1ULL << KEY_W)) != 0;
    state.keyA = (packet.keysDown & (1ULL << KEY_A)) != 0;
    state.keyS = (packet.keysDown & (1ULL << KEY_S)) != 0;
    state.keyD = (packet.keysDown & (1ULL << KEY_D)) != 0;
    state.keySpace = (packet.keysDown & (1ULL << KEY_SPACE)) != 0;
    state.keyCtrl = (packet.keysDown & (1ULL << KEY_CTRL)) != 0;
    state.keyShift = (packet.keysDown & (1ULL << KEY_SHIFT)) != 0;
    state.keyQ = (packet.keysDown & (1ULL << KEY_Q)) != 0;
    state.keyE = (packet.keysDown & (1ULL << KEY_E)) != 0;

    // Decode number keys for player switching
    state.key1 = (packet.keysDown & (1ULL << KEY_1)) != 0;
    state.key2 = (packet.keysDown & (1ULL << KEY_2)) != 0;
    state.key3 = (packet.keysDown & (1ULL << KEY_3)) != 0;
    state.key4 = (packet.keysDown & (1ULL << KEY_4)) != 0;
    state.key5 = (packet.keysDown & (1ULL << KEY_5)) != 0;
    state.key6 = (packet.keysDown & (1ULL << KEY_6)) != 0;
    state.key7 = (packet.keysDown & (1ULL << KEY_7)) != 0;
    state.key8 = (packet.keysDown & (1ULL << KEY_8)) != 0;
    state.key9 = (packet.keysDown & (1ULL << KEY_9)) != 0;
    state.key0 = (packet.keysDown & (1ULL << KEY_0)) != 0;

    state.timestamp = packet.timestamp;
}
