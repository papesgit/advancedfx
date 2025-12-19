#include "ObsSpectatorBindings.h"

#include "ClientEntitySystem.h"

#include "../shared/AfxConsole.h"

#include <algorithm>
#include <vector>

// Global spectator key bindings: slots 1-0 map to controller indices
// -1 = no player mapped to this key
int g_SpectatorBindings[10] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
bool g_LastSpectatorKeyState[10] = {false};
bool g_UseAltSpectatorBindings = false;
bool g_PendingSpectatorSwitch = false;
int g_SpectatorSwitchTimeout = 0; // Safety timeout

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

void SetAltSpectatorBindings(bool enabled) {
	if (g_UseAltSpectatorBindings == enabled) {
		return;
	}

	g_UseAltSpectatorBindings = enabled;
	for (int i = 0; i < 10; ++i) {
		g_LastSpectatorKeyState[i] = false;
	}
}
