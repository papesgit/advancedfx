#include "ObsWebSocketProtocol.h"

#include <utility>

using json = nlohmann::json;

void CObsWebSocketProtocol::RegisterCommandHandler(const std::string& name, CommandHandler handler)
{
    m_CommandHandlers[name] = std::move(handler);
}

void CObsWebSocketProtocol::SetExecCommandHandler(ExecCommandHandler handler)
{
    m_ExecCommandHandler = std::move(handler);
}

void CObsWebSocketProtocol::SetCampathPlayHandler(CampathPlayHandler handler)
{
    m_CampathPlayHandler = std::move(handler);
}

void CObsWebSocketProtocol::HandleMessage(const std::string& message, const ResponseSender& respond) const
{
    if (!respond)
        return;

    const JsonResponder respondJson = [&respond](const json& payload) {
        respond(payload.dump());
    };

    json parsed;
    try
    {
        parsed = json::parse(message);
    }
    catch (const std::exception& ex)
    {
        respondJson(MakeError("invalid_json", ex.what()));
        return;
    }

    if (!parsed.is_object())
    {
        respondJson(MakeError("invalid_request", "Expected JSON object"));
        return;
    }

    const std::string type = parsed.value("type", "");

    if (type == "cmd" || type == "command")
    {
        const std::string commandName = parsed.value("name", "");
        if (commandName.empty())
        {
            respondJson(MakeError("missing_field", "Missing 'name' for command message"));
            return;
        }

        json args = json::object();
        auto argsIt = parsed.find("args");
        if (argsIt != parsed.end())
            args = *argsIt;

        auto handlerIt = m_CommandHandlers.find(commandName);
        if (handlerIt == m_CommandHandlers.end())
        {
            respondJson(MakeCommandError(commandName, "Unknown command"));
            return;
        }

        handlerIt->second(args, respondJson);
    }
    else if (type == "exec_cmd")
    {
        auto cmdIt = parsed.find("cmd");
        if (cmdIt == parsed.end() || !cmdIt->is_string())
        {
            respondJson(MakeError("missing_field", "exec_cmd requires 'cmd' string field"));
            return;
        }

        if (!m_ExecCommandHandler)
        {
            respondJson(MakeError("unsupported", "exec_cmd not supported on this build"));
            return;
        }

        m_ExecCommandHandler(cmdIt->get<std::string>(), respondJson);
    }
    else if (type == "campath_play")
    {
        auto cmdIt = parsed.find("cmd");
        if (cmdIt == parsed.end() || !cmdIt->is_string())
        {
            respondJson(MakeError("missing_field", "campath_play requires 'cmd' string field"));
            return;
        }

        if (!m_CampathPlayHandler)
        {
            respondJson(MakeError("unsupported", "campath_play not supported on this build"));
            return;
        }

        double offset = 0.0;
        auto offsetIt = parsed.find("offset");
        if (offsetIt != parsed.end() && offsetIt->is_number())
        {
            offset = offsetIt->get<double>();
        }

        m_CampathPlayHandler(cmdIt->get<std::string>(), offset, respondJson);
    }
    else
    {
        respondJson(MakeError("unknown_type", "Unsupported message type: " + type));
    }
}

json CObsWebSocketProtocol::MakeError(const std::string& code, const std::string& message) const
{
    return {
        {"type", "error"},
        {"code", code},
        {"message", message}
    };
}

json CObsWebSocketProtocol::MakeCommandError(const std::string& command, const std::string& message) const
{
    return {
        {"type", "command_result"},
        {"command", command},
        {"ok", false},
        {"error", message}
    };
}
