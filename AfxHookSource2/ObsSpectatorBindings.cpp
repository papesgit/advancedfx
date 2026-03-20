#include "ObsSpectatorBindings.h"

#include "ClientEntitySystem.h"

#include "../shared/AfxConsole.h"

#include <algorithm>
#include <cstring>
#include <vector>

// Global spectator key bindings: slots 1-0 map to controller indices
// -1 = no player mapped to this key
int g_SpectatorBindings[10] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
bool g_LastSpectatorKeyState[10] = {false};
bool g_UseAltSpectatorBindings = false;
bool g_PendingSpectatorSwitch = false;
bool g_pendingSpectatorBindingsRefresh = false;
int g_SpectatorSwitchTimeout = 0; // Safety timeout

static bool StartsWithIgnoreCaseAscii(const char* s, const char* lit)
{
    for (; *lit; ++s, ++lit)
    {
        if (!*s) return false;
        char a = *s, b = *lit;
        if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// Returns how many bytes the separator consumes, or 0 if none
static int PipeLikeBytes(const unsigned char* p)
{
    if (!p || !*p) return 0;

    // ASCII '|'
    if (p[0] == 0x7C)
        return 1;

    // U+00A6 '¦'  (C2 A6)
    if (p[0] == 0xC2 && p[1] == 0xA6)
        return 2;

    // U+FF5C '｜' (EF BD 9C)
    if (p[0] == 0xEF && p[1] == 0xBD && p[2] == 0x9C)
        return 3;

    // U+2223 '∣' (E2 88 A3)
    if (p[0] == 0xE2 && p[1] == 0x88 && p[2] == 0xA3)
        return 3;

    return 0;
}

static bool IsCoachName(const char* name)
{
    if (!name || !*name)
        return false;

    // Must start with "coach"
    if (!StartsWithIgnoreCaseAscii(name, "coach"))
        return false;

    const unsigned char* after = (const unsigned char*)(name + 5);

    // Must be immediately followed by a pipe-like separator
    int sepLen = PipeLikeBytes(after);
    if (sepLen == 0)
        return false;

    // Require at least one character after the separator
    return after[sepLen] != '\0';
}

// Refresh spectator bindings for number keys 1-0
void RefreshSpectatorBindings() {
	if (!g_pEntityList || !g_GetEntityFromIndex || !g_GetHighestEntityIndex) {
		advancedfx::Warning("Cannot refresh bindings: Engine not ready\n");
		return;
	}

	// Collect all player controllers with their indices
	std::vector<int> ctControllers;
	std::vector<int> tControllers;

	int highestIndex = g_GetHighestEntityIndex(*g_pEntityList, false);
	for (int i = 0; i <= highestIndex; ++i) {
		CEntityInstance* ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, i);
		if (!ent) continue;

		// Check if it's a player controller (not pawn)
		if (!ent->IsPlayerController()) continue;

		// Get team (2 = T, 3 = CT)
		int team = ent->GetTeam();
		if (team != 2 && team != 3) continue;

		const char* name = ent->GetSanitizedPlayerName();
		if (!name || '\0' == name[0]) {
			name = ent->GetPlayerName();
		}
		if(IsCoachName(name)) {
			advancedfx::Message("Found coachname:");
			advancedfx::Message(name);
			continue;
		}

		// Controller index is the entity index itself
		int controllerIndex = i;

		// Add to appropriate team list
		if (team == 3) {
			ctControllers.push_back(controllerIndex);
		} else if (team == 2) {
			tControllers.push_back(controllerIndex);
		}
	}

	// Sort controller indices (lowest to highest)
	std::sort(ctControllers.begin(), ctControllers.end());
	std::sort(tControllers.begin(), tControllers.end());

	// Clear all bindings first
	for (int i = 0; i < 10; ++i) {
		g_SpectatorBindings[i] = -1;
	}

	// Map CT players to keys 1-5 (index 0-4)
	for (size_t i = 0; i < ctControllers.size() && i < 5; ++i) {
		g_SpectatorBindings[i] = ctControllers[i];
		advancedfx::Message("Key %d -> CT controller %d\n", i+1, ctControllers[i]);
	}

	// Map T players to keys 6-0 (index 5-9)
	for (size_t i = 0; i < tControllers.size() && i < 5; ++i) {
		g_SpectatorBindings[i + 5] = tControllers[i];
		if (g_UseAltSpectatorBindings) {
			static const char* kAltLabels[5] = { "Q", "E", "R", "T", "Z" };
			advancedfx::Message("Key %s -> T controller %d\n", kAltLabels[i], tControllers[i]);
		} else {
			advancedfx::Message("Key %d -> T controller %d\n", (i+6) % 10, tControllers[i]);
		}
	}

	advancedfx::Message("Spectator bindings refreshed: %zu CT, %zu T\n", ctControllers.size(), tControllers.size());
}

void SpectatorBindings_OnGameEvent(const char* eventName) {
    if (!eventName) return;

    if (0 == _stricmp(eventName, "player_team")) {
        g_pendingSpectatorBindingsRefresh = true;
		return;
    }
    if (0 == _stricmp(eventName, "player_spawn")) {
        if (g_pendingSpectatorBindingsRefresh) {
            g_pendingSpectatorBindingsRefresh = false;
            RefreshSpectatorBindings();
        }
        return;
    }
}

void SetAltSpectatorBindings(bool enabled) {
	if (g_UseAltSpectatorBindings == enabled) {
		return;
	}

	g_UseAltSpectatorBindings = enabled;
	for (int i = 0; i < 10; ++i) {
		g_LastSpectatorKeyState[i] = false;
	}
}
