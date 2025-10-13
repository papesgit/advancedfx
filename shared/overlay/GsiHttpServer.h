// Minimal HTTP server to receive CS:GO/CS2 Game State Integration (GSI)
// POSTs at /gsi and map them to Hud::State.

#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <optional>

#include "Hud.h"
#include "third_party/nlohmann/json.hpp"

class GsiHttpServer {
public:
    GsiHttpServer();
    ~GsiHttpServer();

    // Start listening on 127.0.0.1:port
    bool Start(int port);
    void Stop();

    bool IsRunning() const { return running_.load(); }

    std::optional<Hud::State> TryGetHudState();

    // Minimal radar snapshot derived from last merged GSI (thread-safe).
    struct RadarPlayer {
        int   id = 0;              // stable id (hash of steam64)
        float pos[3] = {0,0,0};    // world position
        float fwd[2] = {0,0};      // forward XY (for yaw)
        int   teamSide = 0;        // 2=T, 3=CT
        int   observerSlot = -1;   // 0..9 if available
        bool  alive = true;        // health > 0
        bool  hasBomb = false;     // carrying C4
    };
    std::optional<std::vector<RadarPlayer>> TryGetRadarPlayers();

    struct RadarGrenade {
        enum Type { Smoke, Inferno } type;
        int   ownerSide = 0; // 2=T, 3=CT (best-effort)
        float pos[3] = {0,0,0}; // center for smoke; per-flame pos for inferno
    };
    std::optional<std::vector<RadarGrenade>> TryGetRadarGrenades();

    struct RadarBomb {
        bool  hasPosition = false;
        float pos[3] = {0,0,0};
        std::string state; // carried/dropped/planted/defusing/defused/exploded
    };
    std::optional<RadarBomb> TryGetRadarBomb();

private:
    void ThreadMain(int port);
    using SocketHandle = uintptr_t; // avoid winsock headers in this header
    bool HandleOneConnection(SocketHandle s);
    bool ReadHttpRequest(SocketHandle s, std::string& method, std::string& path, std::string& headers, std::string& body);
    void SendHttpResponse(SocketHandle s, int statusCode);
    void MergeGsiIntoState(const nlohmann::json& body);
    void RebuildHudStateFromGsi();
    void RebuildRadarSnapshotFromGsi();

private:
    std::thread thr_;
    std::atomic<bool> running_{false};
    std::mutex mtx_;
    // last full merged gsi state
    nlohmann::json gsi_;
    Hud::State hud_;

    // Cached radar snapshots built on POST, to avoid per-frame JSON parsing
    std::vector<RadarPlayer>   radar_players_;
    std::vector<RadarGrenade>  radar_grenades_;
    RadarBomb                  radar_bomb_;
};
