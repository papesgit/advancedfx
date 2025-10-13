// Minimal cs-hud radar integration helpers for a C++/DX11 overlay.
//
// Provides:
// - JSON loader for cs-hud radars.json
// - WIC image loader to ID3D11ShaderResourceView
// - World->radar mapping math (same as cs-hud)
// - Simple render helpers using ImGui draw lists

#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#endif

#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>

// ImGui types
#include "../third_party/imgui/imgui.h"

namespace Radar {

struct Vec2 { float x=0, y=0; };
struct Vec3 { float x=0, y=0, z=0; };

struct VerticalSection {
    std::string name;
    double min = -1e18;
    double max = +1e18;
};

struct RadarConfig {
    double pos_x = 0.0;
    double pos_y = 0.0;
    double scale = 1.0;
    std::string imagePath; // optional: absolute or relative path to radar image
    std::vector<VerticalSection> sections; // optional multi-floor
};

// Parse cs-hud radars.json into map: mapName -> RadarConfig.
// If imageBaseDir is provided and the JSON contains a radarImageUrl, the filename
// from the URL is appended to imageBaseDir to fill RadarConfig::imagePath.
bool LoadRadarsJson(const std::string& jsonPath,
                    std::unordered_map<std::string, RadarConfig>& outConfigs,
                    const std::string& imageBaseDir = "");

// Load an image file via WIC into an SRV; returns true on success.
bool LoadTextureWIC(ID3D11Device* device,
                    const std::wstring& filePath,
                    ID3D11ShaderResourceView** outSrv,
                    int* outWidth = nullptr,
                    int* outHeight = nullptr);

// cs-hud mapping: returns normalized UV in [0..1]
inline Vec2 WorldToUV(const Vec3& p, const RadarConfig& cfg) {
    float u = float((p.x - cfg.pos_x) / cfg.scale / 1024.0);
    float v = float((p.y - cfg.pos_y) / -cfg.scale / 1024.0);
    return { u, v };
}

// cs-hud yaw from 2D forward vector (degrees 0..360)
inline float YawFromForward(const Vec2& fwd) {
    float deg = std::atan2f(fwd.x, fwd.y) * 180.0f / 3.1415926535f;
    if (deg < 0) deg += 360.0f;
    return deg;
}

// Pick a level name from z and sections (mirrors cs-hud semantics)
inline std::string PickLevel(float z, const RadarConfig& cfg) {
    if (cfg.sections.empty()) return "default";
    int best = -1; double bestMin = -1e18;
    for (int i=0;i<(int)cfg.sections.size();++i) {
        const auto& s = cfg.sections[i];
        if (z > s.min && s.min > bestMin) { best = i; bestMin = s.min; }
    }
    return best >= 0 ? cfg.sections[best].name : std::string("default");
}

// Rolling average smoother (resets on teleport-like jumps)
struct RollingAvg3 {
    std::vector<Vec3> q;
    int maxN = 16;
    float teleportThresh = 128.0f;
    void clear() { q.clear(); }
    void push(const Vec3& v) {
        if (!q.empty()) {
            const Vec3& p = q.back();
            float dx=v.x-p.x, dy=v.y-p.y, dz=v.z-p.z;
            float d = std::sqrt(dx*dx+dy*dy+dz*dz);
            if (d > teleportThresh) q.clear();
        }
        q.push_back(v);
        if ((int)q.size() > maxN) q.erase(q.begin());
    }
    Vec3 avg() const {
        if (q.empty()) return {};
        double sx=0,sy=0,sz=0; for (auto& v: q){ sx+=v.x; sy+=v.y; sz+=v.z; }
        return { float(sx/q.size()), float(sy/q.size()), float(sz/q.size()) };
    }
};

struct Entity {
    int id = 0;      // stable ID for smoothing
    Vec3 pos;        // world position
    Vec2 fwd;        // 2D forward (x, y)
    ImU32 color = IM_COL32(255,255,255,255);
    bool smooth = true;
};

struct Context {
    RadarConfig cfg;
    ID3D11ShaderResourceView* srv = nullptr; // not owned; app owns lifetime
    int texW = 0, texH = 0;
    std::unordered_map<int, RollingAvg3> smooth; // per-entity
};

// Render radar background and entities.
void Render(ImDrawList* dl,
            const ImVec2& topLeft,
            const ImVec2& size,
            Context& ctx,
            const std::vector<Entity>& entities,
            float markerRadius = 7.0f,
            bool drawBorder = true,
            bool drawBackground = true);

} // namespace Radar
