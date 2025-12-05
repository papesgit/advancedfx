#include "ObsWebSocketServer.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <set>
#include <utility>

CObsWebSocketServer::CObsWebSocketServer()
    : m_bActive(false)
{
}

CObsWebSocketServer::~CObsWebSocketServer()
{
    Stop();
}

bool CObsWebSocketServer::Start(uint16_t port)
{
    if (m_bActive)
    {
        return false;
    }

    // Ensure IXWebSocket network system is initialized on Windows.
    static std::once_flag s_ixInitFlag;
    std::call_once(s_ixInitFlag, []() {
        ix::initNetSystem();
    });

    auto server = std::make_shared<ix::WebSocketServer>(static_cast<int>(port), "0.0.0.0");

    server->setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState> /*connectionState*/,
               ix::WebSocket& webSocket,
               const ix::WebSocketMessagePtr& msg)
        {
            if (!m_CommandCallback)
                return;

            if (msg->type == ix::WebSocketMessageType::Message)
            {
                auto responder = [&webSocket](const std::string& json) {
                    webSocket.sendText(json);
                };
                m_CommandCallback(msg->str, responder);
            }
        });

    if (!server->listenAndStart())
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_ServerMutex);
        m_Server = std::move(server);
    }

    m_bActive = true;
    return true;
}

void CObsWebSocketServer::Stop()
{
    if (!m_bActive)
    {
        return;
    }

    std::shared_ptr<ix::WebSocketServer> server;
    {
        std::lock_guard<std::mutex> lock(m_ServerMutex);
        server = std::move(m_Server);
    }

    m_bActive = false;

    if (server)
    {
        server->stop();
    }
}

int CObsWebSocketServer::GetClientCount() const
{
    std::lock_guard<std::mutex> lock(m_ServerMutex);
    if (!m_Server)
        return 0;

    return static_cast<int>(m_Server->getClients().size());
}

void CObsWebSocketServer::BroadcastJson(const std::string& json)
{
    std::shared_ptr<ix::WebSocketServer> server;
    {
        std::lock_guard<std::mutex> lock(m_ServerMutex);
        server = m_Server;
    }

    if (!server)
        return;

    auto clients = server->getClients();
    for (auto& client : clients)
    {
        if (client)
        {
            client->sendText(json);
        }
    }
}

void CObsWebSocketServer::SetCommandCallback(CommandCallback callback)
{
    m_CommandCallback = std::move(callback);
}
