#pragma once

#include "ObsWebSocketProtocol.h"

#include <string>

void RegisterObsWebSocketHandlers();
void HandleObsWebSocketCommand(const std::string& jsonMessage, const CObsWebSocketProtocol::ResponseSender& respond);
