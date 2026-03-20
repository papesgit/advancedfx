#pragma once

extern int g_SpectatorBindings[10];
extern bool g_LastSpectatorKeyState[10];
extern bool g_UseAltSpectatorBindings;
extern bool g_PendingSpectatorSwitch;
extern int g_SpectatorSwitchTimeout;

void RefreshSpectatorBindings();
void SetAltSpectatorBindings(bool enabled);
void SpectatorBindings_OnGameEvent(const char* eventName);
