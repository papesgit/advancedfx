#pragma once

#include "../deps/release/nlohmann/json.hpp"

#include <functional>
#include <string>
#include <unordered_map>

/// Lightweight router / validator for observer WebSocket messages.
/// Parses JSON, dispatches typed commands and provides helpers for replies.
class CObsWebSocketProtocol {
public:
    using ResponseSender = std::function<void(const std::string&)>;
    using JsonResponder = std::function<void(const nlohmann::json&)>;
    using CommandHandler = std::function<void(const nlohmann::json&, const JsonResponder&)>;
    using ExecCommandHandler = std::function<void(const std::string&, const JsonResponder&)>;
    using CampathPlayHandler = std::function<void(const std::string&, const JsonResponder&)>;

    /// Register a handler for a command (type == "command" / "cmd").
    void RegisterCommandHandler(const std::string& name, CommandHandler handler);

    /// Register the handler for exec_cmd messages.
    void SetExecCommandHandler(ExecCommandHandler handler);

    /// Register the handler for campath_play messages.
    void SetCampathPlayHandler(CampathPlayHandler handler);

    /// Parse and dispatch an incoming message.
    void HandleMessage(const std::string& message, const ResponseSender& respond) const;

private:
    nlohmann::json MakeError(const std::string& code, const std::string& message) const;
    nlohmann::json MakeCommandError(const std::string& command, const std::string& message) const;

    std::unordered_map<std::string, CommandHandler> m_CommandHandlers;
    ExecCommandHandler m_ExecCommandHandler;
    CampathPlayHandler m_CampathPlayHandler;
};
