#pragma once

#include <string>
#include <vector>

struct ObsPlayerFilterRule {
    std::string name;
    bool contains = false;
};

// Thread-safe replacement; binding refresh is queued separately on the game thread.
void SetSpectatorPlayerFilters(std::vector<ObsPlayerFilterRule> rules);

extern int g_SpectatorBindings[10];
extern bool g_LastSpectatorKeyState[10];
extern bool g_UseAltSpectatorBindings;
extern bool g_PendingSpectatorSwitch;
extern int g_SpectatorSwitchTimeout;

void RefreshSpectatorBindings();
void SetAltSpectatorBindings(bool enabled);
void SpectatorBindings_OnGameEvent(const char* eventName);
