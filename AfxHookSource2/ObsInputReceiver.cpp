#include "ObsInputReceiver.h"
#include <chrono>
#include "../shared/AfxConsole.h"
#include <algorithm>
#include <cstring>

namespace {
constexpr uint8_t kVkSpace = 0x20;
constexpr uint8_t kVkCtrl = 0x11;
constexpr uint8_t kVkShift = 0x10;
constexpr uint8_t kVkLeftCtrl = 0xA2;
constexpr uint8_t kVkRightCtrl = 0xA3;
constexpr uint8_t kVkLeftShift = 0xA0;
constexpr uint8_t kVkRightShift = 0xA1;
}

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

bool CObsInputReceiver::Start(uint16_t port, const char* bindAddress) {
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

    // Bind to port/address
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (!bindAddress || strlen(bindAddress) == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else {
        if (InetPtonA(AF_INET, bindAddress, &addr.sin_addr) != 1) {
            closesocket(m_Socket);
            m_Socket = INVALID_SOCKET;
            return false;
        }
    }

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
                m_CurrentState.mouseButton4 = packetState.mouseButton4;
                m_CurrentState.mouseButton5 = packetState.mouseButton5;

                // Latest key state bitmap
                m_CurrentState.keyBitmap = packetState.keyBitmap;

                m_CurrentState.timestamp = packetState.timestamp;
            }
            m_bNewData = true;

            if (m_Debug) {
                auto keyDown = [&packetState](uint8_t vk) {
                    return packetState.IsKeyDown(vk) ? "1" : "0";
                };
                auto keyDownAny = [&packetState](std::initializer_list<uint8_t> vks) {
                    return packetState.IsAnyKeyDown(vks) ? "1" : "0";
                };

                advancedfx::Message(
                    "mirv_udpdebug: seq=%u dx=%d dy=%d wheel=%d buttons=L%sR%sM%sX1%sX2%s keys=W%sA%sS%sD%sSpace%sCtrl%sShift%sQ%sE%s1%s2%s3%s4%s5%s6%s7%s8%s9%s0%s\n",
                    packet.sequence,
                    (int)packetState.mouseDx,
                    (int)packetState.mouseDy,
                    (int)packetState.mouseWheel,
                    packetState.mouseLeft ? "1" : "0",
                    packetState.mouseRight ? "1" : "0",
                    packetState.mouseMiddle ? "1" : "0",
                    packetState.mouseButton4 ? "1" : "0",
                    packetState.mouseButton5 ? "1" : "0",
                    keyDown('W'),
                    keyDown('A'),
                    keyDown('S'),
                    keyDown('D'),
                    keyDown(kVkSpace),
                    keyDownAny({kVkLeftCtrl, kVkRightCtrl, kVkCtrl}),
                    keyDownAny({kVkLeftShift, kVkRightShift, kVkShift}),
                    keyDown('Q'),
                    keyDown('E'),
                    keyDown('1'),
                    keyDown('2'),
                    keyDown('3'),
                    keyDown('4'),
                    keyDown('5'),
                    keyDown('6'),
                    keyDown('7'),
                    keyDown('8'),
                    keyDown('9'),
                    keyDown('0')
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
    state.mouseButton4 = (packet.mouseButtons & 0x08) != 0;
    state.mouseButton5 = (packet.mouseButtons & 0x10) != 0;

    // Copy keyboard bitmap
    std::copy(
        std::begin(packet.keyBitmap),
        std::end(packet.keyBitmap),
        state.keyBitmap.begin()
    );

    state.timestamp = packet.timestamp;
}
