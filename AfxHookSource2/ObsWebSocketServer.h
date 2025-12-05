#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace ix {
    class WebSocketServer;
}

/// WebSocket server for HLAE Observer Tools communication
/// Handles JSON commands from GUI and broadcasts game state/events
class CObsWebSocketServer {
public:
    using ResponseSender = std::function<void(const std::string&)>;
    using CommandCallback = std::function<void(const std::string&, const ResponseSender&)>;

    CObsWebSocketServer();
    ~CObsWebSocketServer();

    /// Start WebSocket server on specified port
    /// @param port Port number (default: 31338)
    /// @return true if started successfully
    bool Start(uint16_t port = 31338);

    /// Stop WebSocket server
    void Stop();

    /// Check if server is running
    bool IsActive() const { return m_bActive; }

    /// Get number of connected clients
    int GetClientCount() const;

    /// Broadcast JSON message to all connected clients
    /// @param json JSON string to send
    void BroadcastJson(const std::string& json);

    /// Set callback for received commands
    /// @param callback Function to call when command received
    void SetCommandCallback(CommandCallback callback);

private:
    std::atomic<bool> m_bActive;
    std::shared_ptr<ix::WebSocketServer> m_Server;
    CommandCallback m_CommandCallback;
    mutable std::mutex m_ServerMutex;
};
