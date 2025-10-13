// Minimal native renderer for cs-hud-like overlays (top bar, sidebars, focused player)
// Draws with ImGui draw lists in a viewport rect.

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "third_party/imgui/imgui.h"

namespace Hud {

struct Weapon {
    std::string name; // unprefixed (e.g., "ak47") for display; optional
    bool isPrimary = false;
    bool isSecondary = false;
    bool isGrenade = false;
    bool isBomb = false;
    bool isActive = false;
    int  ammoClip = -1;
    int  ammoReserve = -1;
};

struct Player {
    int         id = 0;           // stable id
    std::string name;
    int         observerSlot = -1; // 0..9
    bool        isAlive = true;
    bool        isFocused = false;
    int         teamSide = 2;     // 2=T, 3=CT
    int         health = 100;
    int         armor = 0;
    bool        hasHelmet = false;
    bool        hasDefuser = false;
    bool        hasBomb = false;
    int         money = 0;
    int         kills = 0;
    int         deaths = 0;
    int         adr = 0;
    Weapon      primary, secondary;
    Weapon      active;                // best-effort active weapon (or primary/secondary fallback)
    std::vector<std::string> grenades; // standardized keys: flashbang, smokegrenade, molotov, hegrenade, decoy
};

struct Team {
    std::string name;
    int         side = 2; // 2=T, 3=CT
    int         score = 0;
    int         timeoutsLeft = 0;
    ImU32       color = IM_COL32(255,255,255,255);
};

struct RoundInfo {
    int         number = 1;     // 1-based
    std::string phase;          // "live", "freezetime", "over"
    float       timeLeft = 0.0f;// seconds
};

struct BombInfo {
    bool        isPlanted = false;
    bool        isDefusing = false;
    float       countdownSec = 0.0f;  // may be 0 near explosion
    std::string state;                 // planted/defusing/defused/exploded
};

struct State {
    Team              leftTeam;
    Team              rightTeam;
    std::vector<Player> leftPlayers;  // ordered by observerSlot
    std::vector<Player> rightPlayers; // ordered by observerSlot
    int               focusedPlayerId = -1;
    RoundInfo         round;
    BombInfo          bomb;
};

struct Viewport {
    ImVec2 min; // top-left
    ImVec2 size;
};

// Top bar in center (scores + clock)
void RenderTopBar(ImDrawList* dl, const Viewport& vp, const State& st);

// Sidebars at left/right (players summary)
void RenderSidebars(ImDrawList* dl, const Viewport& vp, const State& st);

// Focused player panel at bottom center
void RenderFocusedPlayer(ImDrawList* dl, const Viewport& vp, const State& st);

// Unified helper to draw all parts (ordering matters for layering)
inline void RenderAll(ImDrawList* dl, const Viewport& vp, const State& st) {
    RenderTopBar(dl, vp, st);
    RenderSidebars(dl, vp, st);
    RenderFocusedPlayer(dl, vp, st);
}

// Optional: setup for icon rendering (weapon/grenade/utility icons)
// Provide a D3D11 device and a base directory containing PNG icons, e.g.:
// resources/overlay/icons/{flashbang,smokegrenade,molotov,hegrenade,decoy,helmet,defuser,c4,ak47,...}.png
void SetIconDevice(void* d3d11DevicePtr);
void SetIconsDirectoryW(const std::wstring& dir);

} // namespace Hud
